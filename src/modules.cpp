#include "flywheel/frontend.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace fw {
namespace {
namespace fs = std::filesystem;
const std::set<std::string> native = {"desktop", "core",    "html",        "fs",   "math",
                                      "os",      "time",    "json",        "web",  "testing",
                                      "collections", "io",  "net",         "regex", "thread",
                                      "gc",      "svg",     "crypto",      "encoding", "uuid",
                                      "compress", "sqlite", "toml", "yaml"};
// Types that native modules export, spelled module.type in source.
const std::map<std::string, std::set<std::string>> nativeTypes = {
    {"json", {"value"}},
    {"desktop", {"window", "Window"}},
    {"web", {"request", "response"}},
    {"os", {"process", "result"}},
    {"io", {"file"}},
    {"net", {"socket", "listener"}},
    {"thread", {"task", "mutex", "channel"}},
    {"time", {"datetime"}},
    {"sqlite", {"database"}}};
struct Unit {
    Program program;
    std::string path, prefix;
    std::map<std::string, std::string> own, names, tags;
    std::map<std::string, Unit *> modules;
    std::map<std::string, std::string> builtins;
    bool html = false, svg = false;
    std::set<std::string> familyTags; // element family tags visible in this module
};
// Embedded library modules load from this virtual directory.
const fs::path libraryDirectory = fs::path("/") / "<flywheel>" / "lib";
class Loader {
    fs::path root;
    std::map<std::string, std::string> packages; // package name -> directory
    std::set<std::string> variants; // qualified Enum.Member classes of enums with data
    const std::map<std::string, std::string> *overlay = nullptr;
    std::map<std::string, std::unique_ptr<Unit>> units;
    std::vector<std::string> stack;
    Program merged;
    std::string exported(Unit &u, const std::string &name, Location loc) {
        if (u.program.privateNames.count(name))
            throw Error(loc, "'" + name + "' is private in module '" + u.path + "'");
        auto it = u.own.find(name);
        if (it == u.own.end())
            throw Error(loc, "module '" + u.path + "' has no exported name '" + name + "'");
        return it->second;
    }
    std::string resolve(Unit &u, std::string name, Location loc) {
        auto dot = name.find('.');
        if (dot != std::string::npos) {
            auto it = u.modules.find(name.substr(0, dot));
            if (it != u.modules.end())
                return exported(*it->second, name.substr(dot + 1), loc);
            if (u.builtins.count(name.substr(0, dot))) {
                auto module = u.program.imports.at(name.substr(0, dot));
                auto member = name.substr(dot + 1);
                auto types = nativeTypes.find(module);
                if (types != nativeTypes.end() && types->second.count(member))
                    return module + "." + (member == "Window" ? "window" : member);
            }
        }
        auto it = u.names.find(name);
        return it == u.names.end() ? name : it->second;
    }
    std::string type(Unit &u, const std::string &source, Location loc) {
        std::string out;
        auto word = [&](size_t i) {
            auto c = static_cast<unsigned char>(source[i]);
            return std::isalnum(c) || c == '_' || c >= 0x80;
        };
        for (size_t i = 0; i < source.size();) {
            auto c = static_cast<unsigned char>(source[i]);
            if (std::isalpha(c) || c == '_' || c >= 0x80) {
                size_t start = i++;
                while (i < source.size() && (word(i) || source[i] == '.'))
                    ++i;
                out += resolve(u, source.substr(start, i - start), loc);
            } else
                out += source[i++];
        }
        return out;
    }
    void expression(Unit &u, Expr &e, const std::set<std::string> &locals) {
        e.loc.file = u.path;
        // Type arguments on members (json.decode<User>, thread.channel<Job>) name local types.
        if (e.kind == "member")
            if (auto open = e.text.find('<'); open != std::string::npos)
                e.text = e.text.substr(0, open) + type(u, e.text.substr(open), e.loc);
        if (e.kind == "member" && e.args[0]->kind == "name" && !locals.count(e.args[0]->text)) {
            auto it = u.modules.find(e.args[0]->text);
            if (it != u.modules.end()) {
                // Type arguments (module.Box<int>) are resolved in the importing module.
                auto open = e.text.find('<');
                auto arguments =
                    open == std::string::npos ? "" : type(u, e.text.substr(open), e.loc);
                e.text = exported(*it->second, e.text.substr(0, open), e.loc) + arguments;
                e.kind = "name";
                e.args.clear();
                return;
            }
        }
        if (e.kind == "name" && !locals.count(e.text)) {
            auto nativeIt = u.builtins.find(e.text);
            if (nativeIt != u.builtins.end())
                e.text = nativeIt->second;
            else if (u.modules.count(e.text))
                throw Error(e.loc, "module namespace requires a member name");
            else if (e.text.find('<') != std::string::npos)
                e.text = type(u, e.text, e.loc);
            else
                e.text = resolve(u, e.text, e.loc);
        }
        if (e.kind == "is" || e.kind == "as")
            e.text = type(u, e.text, e.loc);
        if (e.kind == "element" && !e.text.empty()) {
            auto dot = e.text.find('.');
            if (dot != std::string::npos && u.modules.count(e.text.substr(0, dot))) {
                auto &dependency = *u.modules.at(e.text.substr(0, dot));
                auto name = e.text.substr(dot + 1);
                auto qualified = exported(dependency, name, e.loc);
                if (!dependency.program.components.count(name))
                    throw Error(e.loc, "exported name '" + name + "' is not a component");
                e.text = qualified;
            } else if (u.tags.count(e.text))
                e.text = u.tags.at(e.text);
            else if (!u.html && !u.svg && !u.familyTags.count(e.text))
                throw Error(e.loc,
                            "unknown tag '" + e.text + "'; import html.* or declare a component");
        }
        for (auto &a : e.args)
            expression(u, *a, locals);
        // Enum.Member of an enum with data names the variant class.
        if (e.kind == "member" && e.args[0]->kind == "name" &&
            variants.count(e.args[0]->text + "." + e.text)) {
            e.text = e.args[0]->text + "." + e.text;
            e.kind = "name";
            e.args.clear();
        }
        if (e.lambda) {
            // Parameters and enclosing locals shadow module names inside the body.
            auto inner = locals;
            auto &f = *e.lambda;
            f.loc.file = u.path;
            f.result = type(u, f.result, f.loc);
            for (auto &param : f.params) {
                param.loc.file = u.path;
                param.type = type(u, param.type, param.loc);
                inner.insert(param.name);
            }
            statements(u, f.body, inner);
        }
    }
    // A match pattern. Destructuring patterns (`Shape.Circle(r)`, `[x, y]`) bind their bare
    // names (other than `_`) instead of reading them.
    void pattern(Unit &u, Expr &e, const std::set<std::string> &locals,
                 std::set<std::string> &bound) {
        size_t first = 0;
        if (e.kind == "call" && !e.args.empty()) {
            expression(u, *e.args[0], locals);
            first = 1;
            if (e.args[0]->kind != "name" || !variants.count(e.args[0]->text)) {
                for (size_t i = 1; i < e.args.size(); ++i)
                    expression(u, *e.args[i], locals);
                e.loc.file = u.path;
                return;
            }
        } else if (e.kind != "array") {
            expression(u, e, locals);
            return;
        }
        e.loc.file = u.path;
        for (size_t i = first; i < e.args.size(); ++i) {
            auto &a = *e.args[i];
            if (a.kind == "name" && a.text.find('<') == std::string::npos &&
                a.text.find('.') == std::string::npos) {
                a.loc.file = u.path;
                a.kind = "bind";
                if (a.text != "_" && !bound.insert(a.text).second)
                    throw Error(a.loc, "'" + a.text + "' is bound twice in one pattern");
            } else
                pattern(u, a, locals, bound);
        }
    }
    void statements(Unit &u, std::vector<SP> &body, std::set<std::string> locals,
                    bool global = false) {
        for (auto &sp : body) {
            auto &s = *sp;
            s.loc.file = u.path;
            s.type = type(u, s.type, s.loc);
            if (s.expr)
                expression(u, *s.expr, locals);
            if (s.target)
                expression(u, *s.target, locals);
            auto inner = locals;
            if (s.kind == "for") {
                inner.insert(s.name);
                if (!s.op.empty())
                    inner.insert(s.op);
            }
            statements(u, s.body, inner);
            statements(u, s.other, locals);
            statements(u, s.finalizer, locals);
            std::vector<std::set<std::string>> bound(s.patterns.size());
            for (size_t i = 0; i < s.patterns.size(); ++i)
                for (auto &v : s.patterns[i])
                    pattern(u, *v, locals, bound[i]);
            for (size_t i = 0; i < s.catches.size(); ++i) {
                s.catchTypes[i] = type(u, s.catchTypes[i], s.loc);
                auto catchLocals = locals;
                catchLocals.insert(s.catchNames[i]);
                if (i < bound.size())
                    catchLocals.insert(bound[i].begin(), bound[i].end());
                if (i < s.guards.size() && s.guards[i])
                    expression(u, *s.guards[i], catchLocals);
                statements(u, s.catches[i], catchLocals);
            }
            if (s.kind == "declare") {
                if (global)
                    s.name = u.own.at(s.name);
                else
                    locals.insert(s.name);
            }
            if (s.kind == "destructure" && global)
                s.name = u.own.at(s.name);
        }
    }
    void function(Unit &u, Function &f) {
        f.loc.file = u.path;
        std::set<std::string> locals;
        f.result = type(u, f.result, f.loc);
        for (auto &[name, bound] : f.typeBounds)
            bound = type(u, bound, f.loc);
        for (auto &param : f.params) {
            param.loc.file = u.path;
            param.type = type(u, param.type, param.loc);
            if (param.value)
                expression(u, *param.value, {});
            locals.insert(param.name);
        }
        statements(u, f.body, locals);
        if (f.owner.empty())
            f.name = u.own.at(f.name);
        else
            f.owner = u.own.at(f.owner);
    }
    Unit &load(fs::path path) {
        auto canonical = fs::weakly_canonical(path).string();
        if (std::find(stack.begin(), stack.end(), canonical) != stack.end()) {
            std::string message = "import cycle: ";
            for (auto &s : stack)
                message += s + " -> ";
            throw Error({1, 1, canonical}, message + canonical);
        }
        if (units.count(canonical))
            return *units.at(canonical);
        std::ostringstream text;
        bool library = path.parent_path() == libraryDirectory;
        if (auto it = overlay ? overlay->find(canonical) : decltype(overlay->end()){};
            overlay && it != overlay->end())
            text << it->second;
        else if (library)
            text << libraryModules().at(path.stem().string());
        else {
            std::ifstream input(canonical, std::ios::binary);
            if (!input)
                throw Error({1, 1, canonical}, "cannot read module");
            text << input.rdbuf();
        }
        auto owned = std::make_unique<Unit>();
        auto &u = *owned;
        u.path = canonical;
        u.prefix = "$m" + std::to_string(units.size()) + "$";
        try {
            u.program = Parser(lex(text.str())).parse();
        } catch (Error &e) {
            e.loc.file = canonical;
            throw;
        }
        units[canonical] = std::move(owned);
        // Imported modules' top-level statements run before the main file's, so a top-level
        // `return` there would end the whole program.
        if (!stack.empty()) {
            std::function<void(const std::vector<SP> &)> noReturn = [&](const auto &body) {
                for (auto &s : body) {
                    if (s->kind == "return")
                        throw Error({s->loc.line, s->loc.column, canonical},
                                    "return is only valid inside a function or at the top level "
                                    "of the main file");
                    noReturn(s->body);
                    noReturn(s->other);
                    noReturn(s->finalizer);
                    for (auto &c : s->catches)
                        noReturn(c);
                }
            };
            noReturn(u.program.body);
        }
        stack.push_back(canonical);
        auto add = [&](const std::string &name, Location loc) {
            if (u.own.count(name) || u.program.imports.count(name))
                throw Error(loc, "duplicate declaration or import '" + name + "'");
            u.own[name] = u.prefix + name;
        };
        for (auto &f : u.program.functions)
            add(f.name, f.loc);
        for (auto &c : u.program.classes) {
            add(c.name, c.loc);
            if (!c.variantOf.empty())
                variants.insert(u.prefix + c.name);
        }
        for (auto &en : u.program.enums)
            add(en.name, en.loc);
        for (auto &[name, e] : u.program.components)
            add(name, e->loc);
        for (auto &s : u.program.body)
            if (s->kind == "declare")
                add(s->name, s->loc);
        u.names = u.own;
        for (auto &[name, body] : u.program.components) {
            (void)body;
            u.tags[name] = u.own.at(name);
        }
        for (auto &alias : u.program.importOrder) {
            auto &name = u.program.imports.at(alias);
            auto importLoc = u.program.importLocations.at(alias);
            importLoc.file = u.path;
            auto relative = name;
            std::replace(relative.begin(), relative.end(), '.',
                         static_cast<char>(fs::path::preferred_separator));
            fs::path found;
            // Library modules import only other library and native modules.
            std::vector<fs::path> bases;
            if (!library)
                bases = {path.parent_path(), root};
            for (auto base : bases) {
                for (auto extension : {".fw", ".fly"}) {
                    auto candidate = base / (relative + extension);
                    if (fs::is_regular_file(candidate)) {
                        // A file never resolves an import to itself: `math.fw` importing `math`
                        // means the native library, and any other self-import is an error.
                        if (fs::weakly_canonical(candidate).string() == u.path) {
                            if (native.count(name))
                                continue;
                            throw Error(importLoc, "module '" + name + "' imports itself");
                        }
                        found = candidate;
                        break;
                    }
                }
                if (!found.empty())
                    break;
            }
            // Packages from the project manifest: `import pkg` loads the package's main module,
            // `import pkg.part` its part.fw.
            auto head = name.substr(0, name.find('.'));
            if (found.empty() && !library && packages.count(head)) {
                fs::path directory = packages.at(head);
                auto rest = name.find('.') == std::string::npos ? "" : relative.substr(head.size() + 1);
                for (auto extension : {".fw", ".fly"}) {
                    auto candidate =
                        directory / (rest.empty() ? packageMain(directory.string(), head)
                                                  : rest + extension);
                    if (fs::is_regular_file(candidate)) {
                        found = candidate;
                        break;
                    }
                }
                if (found.empty())
                    throw Error(importLoc, "package '" + head + "' has no module '" + name + "'");
            }
            if (found.empty() && libraryModules().count(name))
                found = libraryDirectory / (name + ".fw");
            if (!found.empty()) {
                auto &dependency = load(found);
                u.modules[alias] = &dependency;
                for (auto &family : dependency.program.families)
                    for (auto &[tag, info] : family.tags)
                        u.familyTags.insert(tag);
                if (u.program.wildcards.count(alias))
                    for (auto &[publicName, qualified] : dependency.own) {
                        if (dependency.program.privateNames.count(publicName))
                            continue;
                        if (u.names.count(publicName) || u.program.imports.count(publicName))
                            throw Error(importLoc,
                                        "wildcard import collision for '" + publicName + "'");
                        u.names[publicName] = qualified;
                        if (dependency.program.components.count(publicName))
                            u.tags[publicName] = qualified;
                    }
            } else if (native.count(name)) {
                if (u.program.wildcards.count(alias) && name != "html" && name != "svg")
                    throw Error(importLoc,
                                "native wildcard imports are supported only for html and svg");
                auto qualified = "$native$" + name;
                u.builtins[alias] = qualified;
                merged.imports[qualified] = name;
                if (name == "html" && u.program.wildcards.count(alias))
                    u.html = true;
                if (name == "svg" && u.program.wildcards.count(alias))
                    u.svg = true;
            } else
                throw Error(
                    importLoc,
                    "module '" + name +
                        "' not found or not implemented (searched local .fw and .fly files)");
        }
        for (auto &family : u.program.families)
            for (auto &[tag, info] : family.tags)
                u.familyTags.insert(tag);
        for (auto &f : u.program.functions)
            function(u, f);
        for (auto &en : u.program.enums) {
            en.loc.file = u.path;
            en.name = u.own.at(en.name);
        }
        for (auto &c : u.program.classes) {
            c.loc.file = u.path;
            if (!c.base.empty())
                c.base = type(u, c.base, c.loc);
            for (auto &[name, bound] : c.typeBounds)
                bound = type(u, bound, c.loc);
            for (auto &v : c.variants)
                v = u.own.at(v);
            if (!c.variantOf.empty())
                c.variantOf = u.own.at(c.variantOf);
            for (auto &i : c.interfaces)
                i = type(u, i, c.loc);
            for (auto &field : c.fields) {
                field.loc.file = u.path;
                field.type = type(u, field.type, field.loc);
                if (field.value)
                    expression(u, *field.value, {});
            }
            for (auto &f : c.methods)
                function(u, f);
            c.name = u.own.at(c.name);
        }
        for (auto &[name, e] : u.program.components) {
            expression(u, *e, {});
            merged.components[u.own.at(name)] = std::move(e);
        }
        statements(u, u.program.body, {}, true);
        for (auto &f : u.program.functions)
            merged.functions.push_back(std::move(f));
        for (auto &c : u.program.classes)
            merged.classes.push_back(std::move(c));
        for (auto &en : u.program.enums)
            merged.enums.push_back(std::move(en));
        for (auto &s : u.program.body)
            merged.body.push_back(std::move(s));
        merged.html = merged.html || u.html;
        merged.svg = merged.svg || u.svg;
        for (auto &family : u.program.families)
            merged.families.push_back(family);
        stack.pop_back();
        return u;
    }

  public:
    explicit Loader(const std::map<std::string, std::string> *overlay = nullptr)
        : overlay(overlay) {}
    Program run(const std::string &file) {
        root = fs::absolute(file).parent_path();
        packages = packageRoots(fs::absolute(file).string());
        load(fs::absolute(file));
        return std::move(merged);
    }
};
} // namespace
Program loadProgram(const std::string &filename) {
    return Loader().run(filename);
}
Program loadProgram(const std::string &filename,
                    const std::map<std::string, std::string> &overlay) {
    return Loader(&overlay).run(filename);
}
} // namespace fw
