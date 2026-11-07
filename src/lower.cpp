// Reactive variables and negated aliases, lowered after parsing into ordinary Flywheel code.
//
//   reactive bool alive = true     a variable with a hidden watcher list, __fwr_w_alive
//   alive = false                  the assignment, then every watcher of `alive` runs
//   computed bool dead = not alive a variable that a watcher on `alive` recomputes (eagerly)
//   when dead do ... end           a closure, registered with each reactive or computed
//                                  variable its condition reads, that runs the body when the
//                                  condition becomes true (and re-arms when it becomes false)
//   bool enabled/disabled = true   `disabled` reads `not enabled`; `disabled = x` assigns
//                                  `enabled = not x`
//
// Names resolve lexically (functions see top-level declarations; locals, parameters, loop
// variables, and catch bindings shadow them), so the lowering agrees with the compiler's scopes.
// A `when` or `computed` inside a block unregisters its watchers when the block ends; at the top
// level they stay registered for the whole program.
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fw {
namespace {
struct Binding {
    enum Kind { Plain, Reactive, Computed, Alias } kind = Plain;
    std::string target; // Alias: the bool variable it negates
};
EP make(const std::string &kind, const std::string &text, Location loc) {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->text = text;
    e->loc = loc;
    return e;
}
void relocate(Expr &e, Location loc);
void relocate(std::vector<SP> &body, Location loc) {
    for (auto &s : body) {
        s->loc = loc;
        if (s->expr)
            relocate(*s->expr, loc);
        if (s->target)
            relocate(*s->target, loc);
        relocate(s->body, loc);
        relocate(s->other, loc);
        relocate(s->finalizer, loc);
        for (auto &c : s->catches)
            relocate(c, loc);
    }
}
void relocate(Expr &e, Location loc) {
    e.loc = loc;
    for (auto &a : e.args)
        if (a)
            relocate(*a, loc);
    if (e.lambda) {
        e.lambda->loc = loc;
        relocate(e.lambda->body, loc);
    }
}
// Parses generated Flywheel statements, placed at `loc` for diagnostics.
std::vector<SP> generated(const std::string &text, Location loc) {
    auto program = Parser(lex(text)).parse();
    relocate(program.body, loc);
    return std::move(program.body);
}
const char *watcherType = "array<function() returns void>";

class Lowerer {
    std::vector<std::map<std::string, Binding>> scopes;
    int counter = 0;

    Binding *find(const std::string &name) {
        for (auto i = scopes.rbegin(); i != scopes.rend(); ++i)
            if (auto it = i->find(name); it != i->end())
                return &it->second;
        return nullptr;
    }
    static std::string watchers(const std::string &name) {
        return "__fwr_w_" + name;
    }
    // Runs every watcher of `name`; a copy, so watchers may unregister while it runs.
    std::vector<SP> notify(const std::string &name, Location loc) {
        return generated("for __fwr_f in " + watchers(name) + ".copy() do\n    __fwr_f()\nend\n",
                         loc);
    }
    // Reactive and computed variables read by an expression, in order of first use.
    void dependencies(Expr &e, std::vector<std::string> &out) {
        if (e.kind == "name")
            if (auto b = find(e.text);
                b && (b->kind == Binding::Reactive || b->kind == Binding::Computed) &&
                std::find(out.begin(), out.end(), e.text) == out.end())
                out.push_back(e.text);
        for (auto &a : e.args)
            if (a)
                dependencies(*a, out);
        if (e.lambda) {
            std::function<void(std::vector<SP> &)> walk = [&](std::vector<SP> &body) {
                for (auto &s : body) {
                    if (s->expr)
                        dependencies(*s->expr, out);
                    if (s->target)
                        dependencies(*s->target, out);
                    walk(s->body);
                    walk(s->other);
                }
            };
            walk(e.lambda->body);
        }
    }
    void expression(EP &e) {
        if (!e)
            return;
        if (e->kind == "name")
            if (auto b = find(e->text); b && b->kind == Binding::Alias) {
                auto negated = make("unary", "not", e->loc);
                auto original = make("name", b->target, e->loc);
                original->grouped = true;
                negated->args.push_back(std::move(original));
                negated->grouped = true;
                e = std::move(negated);
                return;
            }
        for (auto &a : e->args)
            expression(a);
        if (e->lambda)
            function(*e->lambda);
    }
    void function(Function &f) {
        scopes.emplace_back();
        for (auto &p : f.params) {
            expression(p.value);
            scopes.back()[p.name] = {};
        }
        block(f.body, false);
        scopes.pop_back();
    }
    void bind(const std::string &name, Binding b) {
        scopes.back()[name] = std::move(b);
    }
    // After a `when` or `computed` inside a block, the rest of the block runs in a try whose
    // finally removes the watchers again.
    void unregisterAtEnd(std::vector<SP> &list, size_t after, const std::string &closure,
                         const std::vector<std::string> &deps, Location loc) {
        std::string text;
        for (auto &d : deps)
            text += watchers(d) + ".remove(" + closure + ")\n";
        auto t = std::make_unique<Stmt>();
        t->kind = "try";
        t->loc = loc;
        t->finalizer = generated(text, loc);
        for (size_t j = after; j < list.size(); ++j)
            t->body.push_back(std::move(list[j]));
        list.resize(after);
        list.push_back(std::move(t));
    }
    std::vector<std::string> requireDependencies(Expr &e, const std::string &what, Location loc) {
        std::vector<std::string> deps;
        dependencies(e, deps);
        if (deps.empty())
            throw Error(loc, what + " must read a reactive or computed variable, such as "
                                    "`reactive bool alive = true`");
        return deps;
    }

  public:
    void block(std::vector<SP> &list, bool topLevel) {
        scopes.emplace_back();
        for (size_t i = 0; i < list.size(); ++i) {
            auto &s = *list[i];
            if (s.kind == "declare") {
                expression(s.expr);
                if (s.op == "computed") {
                    // computed T y = expr: y is recomputed (and its watchers run) whenever a
                    // variable that expr reads changes. Assignments to y are rejected.
                    auto deps = requireDependencies(*s.expr, "a computed value", s.loc);
                    auto n = std::to_string(++counter);
                    auto update = "__fwr_update" + n;
                    std::string text = std::string(watcherType) + " " + watchers(s.name) +
                                       " = []\nvar " + update +
                                       " = function() returns void\n    " + s.name +
                                       " = __fwr_value\nend\n";
                    for (auto &d : deps)
                        text += watchers(d) + ".push(" + update + ")\n";
                    auto extra = generated(text, s.loc);
                    // The update closure assigns the expression, then notifies y's watchers.
                    auto &body = extra[1]->expr->lambda->body;
                    body[0]->expr = clone(*s.expr);
                    for (auto &st : notify(s.name, s.loc))
                        body.push_back(std::move(st));
                    s.op = "var";
                    bind(s.name, {Binding::Computed, ""});
                    if (!s.alias.empty())
                        bind(s.alias, {Binding::Alias, s.name});
                    s.alias.clear();
                    size_t at = i + 1;
                    for (auto &st : extra)
                        list.insert(list.begin() + static_cast<ptrdiff_t>(at++), std::move(st));
                    if (!topLevel)
                        unregisterAtEnd(list, at, update, deps, list[i]->loc);
                    i = at - 1;
                    continue;
                }
                bool reactive = s.op == "reactive";
                if (reactive)
                    s.op = "var";
                bind(s.name, {reactive ? Binding::Reactive : Binding::Plain, ""});
                if (!s.alias.empty())
                    bind(s.alias, {Binding::Alias, s.name});
                s.alias.clear();
                if (reactive) {
                    auto extra =
                        generated(std::string(watcherType) + " " + watchers(s.name) + " = []\n", s.loc);
                    list.insert(list.begin() + static_cast<ptrdiff_t>(i + 1), std::move(extra[0]));
                    ++i;
                }
                continue;
            }
            if (s.kind == "assign") {
                expression(s.expr);
                if (s.target->kind != "name") {
                    expression(s.target);
                    continue;
                }
                auto b = find(s.target->text);
                if (b && b->kind == Binding::Alias) {
                    if (s.op != "=")
                        throw Error(s.loc, "'" + s.target->text + "' is a negated alias of '" +
                                               b->target + "'; assign it with =");
                    auto negated = make("unary", "not", s.expr->loc);
                    s.expr->grouped = true;
                    negated->args.push_back(std::move(s.expr));
                    s.expr = std::move(negated);
                    s.target->text = b->target;
                    b = find(b->target);
                }
                if (b && b->kind == Binding::Computed)
                    throw Error(s.loc, "cannot assign to computed value '" + s.target->text +
                                           "'; it follows its expression");
                if (b && b->kind == Binding::Reactive) {
                    auto extra = notify(s.target->text, s.loc);
                    list.insert(list.begin() + static_cast<ptrdiff_t>(i + 1), std::move(extra[0]));
                    ++i;
                }
                continue;
            }
            if (s.kind == "when") {
                // when cond do body end: a check closure that runs body on each false -> true
                // change of cond, registered with every reactive variable cond reads, and run
                // once now (so a condition that is already true runs immediately).
                expression(s.expr);
                auto deps = requireDependencies(*s.expr, "a 'when' condition", s.loc);
                scopes.emplace_back(); // the body runs inside the check closure
                block(s.body, false);
                scopes.pop_back();
                auto n = std::to_string(++counter);
                auto armed = "__fwr_armed" + n, check = "__fwr_check" + n;
                std::string text = "var " + armed + " = true\nvar " + check +
                                   " = function() returns void\n"
                                   "    if __fwr_condition then\n"
                                   "        if " + armed + " then\n"
                                   "            " + armed + " = false\n"
                                   "        end\n"
                                   "    else\n"
                                   "        " + armed + " = true\n"
                                   "    end\n"
                                   "end\n";
                for (auto &d : deps)
                    text += watchers(d) + ".push(" + check + ")\n";
                text += check + "()\n";
                auto loc = s.loc;
                auto extra = generated(text, loc);
                auto &test = *extra[1]->expr->lambda->body[0];
                test.expr = std::move(s.expr);
                for (auto &st : s.body)
                    test.body[0]->body.push_back(std::move(st));
                list.erase(list.begin() + static_cast<ptrdiff_t>(i));
                size_t at = i;
                for (auto &st : extra)
                    list.insert(list.begin() + static_cast<ptrdiff_t>(at++), std::move(st));
                bind(armed, {});
                bind(check, {});
                if (!topLevel)
                    unregisterAtEnd(list, at, check, deps, loc);
                i = at - 1;
                continue;
            }
            if (s.kind == "for") {
                expression(s.expr);
                scopes.emplace_back();
                bind(s.name, {});
                if (!s.op.empty())
                    bind(s.op, {});
                block(s.body, false);
                scopes.pop_back();
                continue;
            }
            if (s.kind == "try") {
                block(s.body, false);
                for (size_t c = 0; c < s.catches.size(); ++c) {
                    scopes.emplace_back();
                    if (c < s.catchNames.size())
                        bind(s.catchNames[c], {});
                    block(s.catches[c], false);
                    scopes.pop_back();
                }
                block(s.finalizer, false);
                continue;
            }
            if (s.kind == "match") {
                expression(s.expr);
                for (size_t c = 0; c < s.catches.size(); ++c) {
                    scopes.emplace_back();
                    if (c < s.catchNames.size() && !s.catchNames[c].empty())
                        bind(s.catchNames[c], {});
                    // Bare names in patterns bind values; patterns themselves are left alone.
                    std::function<void(Expr &)> names = [&](Expr &p) {
                        if (p.kind == "name")
                            bind(p.text, {});
                        for (auto &a : p.args)
                            if (a)
                                names(*a);
                    };
                    if (c < s.patterns.size())
                        for (auto &p : s.patterns[c])
                            names(*p);
                    if (c < s.guards.size())
                        expression(s.guards[c]);
                    block(s.catches[c], false);
                    scopes.pop_back();
                }
                block(s.other, false);
                continue;
            }
            expression(s.expr);
            expression(s.target);
            block(s.body, false);
            block(s.other, false);
            block(s.finalizer, false);
        }
        scopes.pop_back();
    }
    void program(Program &p) {
        // Top-level declarations are visible in every function, whatever their position.
        std::map<std::string, Binding> top;
        bool any = false;
        std::function<bool(std::vector<SP> &)> uses = [&](std::vector<SP> &body) {
            for (auto &s : body)
                if ((s->kind == "declare" && (s->op == "reactive" || s->op == "computed" ||
                                              !s->alias.empty())) ||
                    s->kind == "when" || uses(s->body) || uses(s->other) || uses(s->finalizer))
                    return true;
            return false;
        };
        std::function<bool(Expr &)> lambdaUses = [&](Expr &e) {
            for (auto &a : e.args)
                if (a && lambdaUses(*a))
                    return true;
            return e.lambda && uses(e.lambda->body);
        };
        any = uses(p.body);
        for (auto &f : p.functions)
            any = any || uses(f.body);
        for (auto &c : p.classes)
            for (auto &m : c.methods)
                any = any || uses(m.body);
        if (!any) {
            // Lambdas nested in expressions may still declare them.
            std::function<bool(std::vector<SP> &)> deep = [&](std::vector<SP> &body) {
                for (auto &s : body)
                    if ((s->expr && lambdaUses(*s->expr)) || (s->target && lambdaUses(*s->target)) ||
                        deep(s->body) || deep(s->other) || deep(s->finalizer))
                        return true;
                return false;
            };
            any = deep(p.body);
            for (auto &f : p.functions)
                any = any || deep(f.body);
            for (auto &c : p.classes)
                for (auto &m : c.methods)
                    any = any || deep(m.body);
        }
        if (!any)
            return;
        for (auto &s : p.body)
            if (s->kind == "declare") {
                top[s->name] = {s->op == "reactive"   ? Binding::Reactive
                                : s->op == "computed" ? Binding::Computed
                                                      : Binding::Plain,
                                ""};
                if (!s->alias.empty())
                    top[s->alias] = {Binding::Alias, s->name};
            }
        scopes.push_back(top);
        for (auto &f : p.functions)
            function(f);
        for (auto &c : p.classes)
            for (auto &m : c.methods)
                function(m);
        scopes.pop_back();
        scopes.push_back(top);
        block(p.body, true);
        scopes.pop_back();
    }
};
} // namespace
void lowerReactive(Program &program) {
    Lowerer().program(program);
}
} // namespace fw
