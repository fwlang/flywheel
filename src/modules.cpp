#include "flywheel/frontend.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstring>
#include <sstream>

namespace fw {
namespace {
namespace fs = std::filesystem;
const std::set<std::string> native = {"desktop", "core",    "html",        "fs",   "math",
                                      "os",      "time",    "json",        "web",  "testing",
                                      "collections", "io",  "net",         "regex", "thread",
                                      "gc",      "svg",     "crypto",      "encoding", "uuid",
                                      "compress", "sqlite", "toml", "yaml", "flywheel"};
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
    // Unit namespaces this module declares (hidden names unqualified), and the modules whose
    // namespaces it sees: its imports, and the units library behind `import time`.
    std::vector<UnitNamespace> unitExports;
    std::vector<Unit *> unitModules;
    std::map<std::string, std::string> ambiguousUnits; // namespace -> "a and b"
    std::vector<UnitNamespace> visibleUnits; // own and imported, for unit literals
};
// The hidden names a unit namespace's lowering generates (src/units.cpp).
std::vector<std::string> unitNames(const std::string &ns) {
    std::vector<std::string> out;
    for (auto suffix : {"int", "double", "names", "factor", "finer", "num", "den", "mul"})
        out.push_back("__fwu_" + ns + "__" + suffix);
    return out;
}
// Embedded library modules load from this virtual directory.
const fs::path libraryDirectory = fs::path("/") / "<flywheel>" / "lib";
// The URL's directory, with a trailing '/': "https://x.org/a/b.fw" -> "https://x.org/a/".
std::string urlDirectory(const std::string &url) {
    auto end = url.find_first_of("?#");
    auto path = url.substr(0, end);
    auto slash = path.rfind('/');
    auto scheme = path.find("://");
    if (slash == std::string::npos || slash < scheme + 3)
        return path + "/";
    return path.substr(0, slash + 1);
}
// An import string that names a file or URL rather than a dotted module name.
bool pathImport(const std::string &name) {
    if (name.find_first_of("/\\:") != std::string::npos)
        return true;
    for (auto extension : {".fw", ".fly"})
        if (name.size() > std::strlen(extension) &&
            name.compare(name.size() - std::strlen(extension), std::strlen(extension), extension) == 0)
            return true;
    return false;
}
// Source file extensions of natively imported languages, in search order.
std::vector<std::string> foreignExtensions(const std::string &language) {
    if (language == "c")
        return {".c", ".h"};
    if (language == "cxx")
        return {".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".h"};
    if (language == "rust")
        return {".rs"};
    return {".zig"};
}
std::string languageName(const std::string &language) {
    return language == "cxx" ? "C++" : language == "c" ? "C" : language == "rust" ? "Rust" : "Zig";
}
class Loader {
    fs::path root;
    std::string rootUrl; // the entry file's URL directory when it was named by URL
    std::map<std::string, std::string> packages; // package name -> directory
    std::map<std::string, std::string> downloads; // URL -> contents, fetched once per load
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
        if (name.rfind("__fwu_", 0) == 0 && !u.names.count(name)) {
            auto inner = name.substr(6);
            auto ns = inner.substr(0, inner.rfind("__"));
            if (auto it = u.ambiguousUnits.find(ns); it != u.ambiguousUnits.end())
                throw Error(loc, "unit namespace '" + ns + "' is ambiguous: modules " + it->second +
                                     " both declare it; qualify it, such as float[" +
                                     it->second.substr(0, it->second.find(' ')) + "." + ns + "]");
        }
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
    // 5km, 15time.s: the unit must belong to a namespace the module declares or imports.
    // The literal is then spelled with its namespace's hidden class (`$m2$__fwu_time__int.s`), so
    // the compiler finds that namespace even when other modules have one of the same name; the
    // module's own namespaces come first.
    void unitLiteral(Unit &u, Expr &e) {
        auto text = e.text, unit = text;
        auto spell = [&](const UnitNamespace &ns, const std::string &unitName) {
            auto it = u.names.find(ns.intClass);
            if (it != u.names.end())
                e.text = it->second + "." + unitName;
        };
        auto own = [&](const UnitNamespace &ns) {
            return &ns < u.visibleUnits.data() + u.program.unitNamespaces.size();
        };
        if (auto dot = text.find('.'); dot != std::string::npos) {
            auto name = text.substr(0, dot), rest = text.substr(dot + 1);
            const UnitNamespace *found = nullptr;
            int count = 0;
            for (auto &ns : u.visibleUnits)
                if (ns.name == name) {
                    if (own(ns) && !found) {
                        found = &ns;
                        count = 1;
                        break;
                    }
                    found = found ? found : &ns;
                    ++count;
                }
            if (found) {
                if (count == 1 && rest.find('.') == std::string::npos)
                    spell(*found, rest);
                return; // the unit itself is checked by the compiler
            }
            unit = name; // 15s.value
        }
        std::vector<const UnitNamespace *> candidates, mine;
        for (auto &ns : u.visibleUnits)
            if (std::find(ns.units.begin(), ns.units.end(), unit) != ns.units.end()) {
                candidates.push_back(&ns);
                if (own(ns))
                    mine.push_back(&ns);
            }
        if (!mine.empty())
            candidates = mine;
        if (candidates.size() == 1 && text.find('.') == std::string::npos)
            spell(*candidates[0], unit);
        if (!candidates.empty())
            return;
        std::string where;
        for (auto &[path, other] : units)
            for (auto &ns : other->unitExports)
                if (where.empty() &&
                    std::find(ns.units.begin(), ns.units.end(), unit) != ns.units.end())
                    where = path.rfind((libraryDirectory / "").string(), 0) == 0
                                ? fs::path(path).stem().string()
                                : path;
        if (where == "_time")
            where = "time";
        throw Error(e.loc, "unknown unit '" + unit + "' after " + e.args[0]->text +
                               (where.empty() ? "" : "; import " + where + ", which declares it"));
    }
    void expression(Unit &u, Expr &e, const std::set<std::string> &locals) {
        e.loc.file = u.path;
        if (e.kind == "unitlit")
            unitLiteral(u, e);
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
                // `catch "Name"` matches by name at run time; it is not a type reference.
                if (s.catchTypes[i].rfind("\"", 0) != 0)
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
    // A native C, C++, Rust, or Zig source: its functions become a module of foreign functions.
    Unit &loadForeign(const std::string &language, const std::string &file, Location loc) {
        auto canonical = fs::weakly_canonical(fs::path(file)).string();
        auto key = language + ":" + canonical;
        if (units.count(key))
            return *units.at(key);
        ForeignModule module;
        try {
            module = scanForeign(language, canonical);
        } catch (const std::exception &e) {
            throw Error(loc, e.what());
        }
        auto owned = std::make_unique<Unit>();
        auto &u = *owned;
        u.path = canonical;
        u.prefix = "$m" + std::to_string(units.size()) + "$";
        units[key] = std::move(owned);
        for (auto &f : module.functions) {
            if (u.own.count(f.name))
                continue; // overloads: the first definition wins
            u.own[f.name] = u.prefix + f.name;
            Function fn;
            fn.name = u.prefix + f.name;
            fn.result = f.result;
            fn.loc = {1, 1, canonical};
            fn.foreign = f.symbol;
            for (size_t i = 0; i < f.params.size(); ++i) {
                Param param;
                param.name = "arg" + std::to_string(i);
                param.type = f.params[i];
                param.loc = fn.loc;
                fn.params.push_back(std::move(param));
            }
            merged.functions.push_back(std::move(fn));
        }
        merged.foreign.push_back(std::move(module));
        return u;
    }
    // Finds the source file of `import <language> name`: a dotted name searches the importing
    // file's directory and the entry directory with the language's extensions; a string names a
    // path or URL (downloaded to the cache).
    std::string foreignSource(Unit &u, const std::string &language, const std::string &name,
                              Location loc) {
        bool remoteUnit = isUrl(u.path);
        if (isUrl(name) || (remoteUnit && pathImport(name))) {
            auto url = isUrl(name) ? name : urlDirectory(u.path) + name;
            if (auto local = fileUrlPath(url); !local.empty())
                return local;
            try {
                return cacheDownload(url, fetch(url));
            } catch (const std::exception &e) {
                throw Error(loc, e.what());
            }
        }
        auto relative = name;
        bool hasExtension = false;
        for (auto &extension : foreignExtensions(language))
            if (relative.size() > extension.size() &&
                relative.compare(relative.size() - extension.size(), extension.size(), extension) == 0)
                hasExtension = true;
        if (!pathImport(name) && !hasExtension)
            std::replace(relative.begin(), relative.end(), '.', '/');
        std::vector<std::string> extensions = {""};
        if (!hasExtension)
            extensions = foreignExtensions(language);
        std::vector<fs::path> bases;
        if (fs::path(relative).is_absolute())
            bases = {fs::path()};
        else if (remoteUnit) {
            // Remote modules find native sources next to them by URL.
            for (auto &extension : extensions) {
                auto url = urlDirectory(u.path) + relative + extension;
                std::string contents;
                if (tryFetchUrl(url, contents))
                    return cacheDownload(url, contents);
            }
        } else
            bases = {fs::path(u.path).parent_path(), root};
        for (auto &base : bases)
            for (auto &extension : extensions) {
                auto candidate = base.empty() ? fs::path(relative + extension)
                                              : base / (relative + extension);
                if (fs::is_regular_file(candidate))
                    return candidate.string();
            }
        throw Error(loc, languageName(language) + " source for '" + name + "' not found (searched " +
                             (hasExtension ? relative : relative + foreignExtensions(language)[0]) +
                             (remoteUnit ? " next to " + u.path : " next to the importing file") +
                             ")");
    }
    const std::string &fetch(const std::string &url) {
        auto it = downloads.find(url);
        if (it == downloads.end())
            it = downloads.emplace(url, fetchUrl(url)).first;
        return it->second;
    }
    static std::string canonicalLocation(const std::string &location) {
        return isUrl(location) ? location : fs::weakly_canonical(fs::path(location)).string();
    }
    // Lowers the module's units once its imports are loaded: imported namespaces become visible
    // by name (int[time]), and unitconv may relate them.
    template <class Add> void units_(Unit &u, Add &add) {
        std::vector<UnitNamespace> imported;
        std::map<std::string, std::vector<std::pair<Unit *, std::string>>> byName;
        std::set<Unit *> seen;
        for (auto *dependency : u.unitModules) {
            if (!seen.insert(dependency).second)
                continue;
            std::string module = dependency->path;
            for (auto &[alias, unit] : u.modules)
                if (unit == dependency)
                    module = alias;
            for (auto &ns : dependency->unitExports)
                byName[ns.name].push_back({dependency, module});
        }
        std::set<std::string> own;
        for (auto &ns : u.program.unitNamespaces)
            own.insert(ns.name);
        for (auto &ns : u.program.unitNamespaces)
            ns.key = u.prefix + "__fwu_" + ns.name + "__int";
        for (auto &[name, sources] : byName) {
            bool bare = !own.count(name) && sources.size() == 1;
            if (!own.count(name) && sources.size() > 1)
                u.ambiguousUnits[name] = sources[0].second + " and " + sources[1].second;
            for (auto &[dependency, module] : sources) {
                bool aliased = u.modules.count(module) && u.modules.at(module) == dependency;
                if (!bare && !aliased)
                    continue;
                for (auto &ns : dependency->unitExports)
                    if (ns.name == name) {
                        // Reached bare when unique, else through the import (metric.length).
                        auto copy = ns;
                        copy.module = aliased ? module : "";
                        copy.ref = bare ? "" : module + ".";
                        imported.push_back(copy);
                    }
                if (bare)
                    for (auto &hidden : unitNames(name))
                        if (dependency->own.count(hidden))
                            u.names[hidden] = dependency->own.at(hidden);
            }
        }
        try {
            lowerUnits(u.program, imported, true);
        } catch (Error &e) {
            e.loc.file = u.path;
            throw;
        }
        // A namespace of the module hides imported ones of the same name.
        u.visibleUnits = u.program.unitNamespaces;
        for (auto *dependency : seen)
            for (auto &ns : dependency->unitExports)
                if (!own.count(ns.name))
                    u.visibleUnits.push_back(ns);
        for (auto &c : u.program.classes)
            if (c.name.rfind("__fwu_", 0) == 0 && !u.own.count(c.name)) {
                add(c.name, c.loc);
                u.names[c.name] = u.own.at(c.name);
            }
        for (auto &s : u.program.body)
            if (s->kind == "declare" && s->name.rfind("__fwu_", 0) == 0 && !u.own.count(s->name)) {
                add(s->name, s->loc);
                u.names[s->name] = u.own.at(s->name);
            }
    }
    Unit &load(const std::string &location) {
        bool remote = isUrl(location);
        fs::path path = remote ? fs::path() : fs::path(location);
        auto canonical = remote ? location : fs::weakly_canonical(path).string();
        if (std::find(stack.begin(), stack.end(), canonical) != stack.end()) {
            std::string message = "import cycle: ";
            for (auto &s : stack)
                message += s + " -> ";
            throw Error({1, 1, canonical}, message + canonical);
        }
        if (units.count(canonical))
            return *units.at(canonical);
        std::ostringstream text;
        bool library = !remote && path.parent_path() == libraryDirectory;
        if (auto it = overlay ? overlay->find(canonical) : decltype(overlay->end()){};
            overlay && it != overlay->end())
            text << it->second;
        else if (remote) {
            try {
                text << fetch(location);
            } catch (const std::exception &e) {
                throw Error({1, 1, canonical}, e.what());
            }
        } else if (library)
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
            Parser parser(lex(text.str()));
            parser.deferUnits = true;
            u.program = parser.parse();
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
            if (auto kind = u.program.importKinds.find(alias); kind != u.program.importKinds.end()) {
                if (library)
                    throw Error(importLoc, "library modules cannot import native sources");
                auto &dependency =
                    loadForeign(kind->second, foreignSource(u, kind->second, name, importLoc),
                                importLoc);
                u.modules[alias] = &dependency;
                continue;
            }
            std::string found;
            if (pathImport(name)) {
                // A file path or URL: relative paths start at the importing file (or its URL).
                if (library)
                    throw Error(importLoc, "library modules cannot import files");
                std::string target = name;
                if (!isUrl(name) && remote && !fs::path(name).is_absolute())
                    target = urlDirectory(location) + name;
                if (auto local = fileUrlPath(target); !local.empty())
                    target = local;
                std::vector<std::string> extensions = {""};
                auto stem = target.substr(0, target.find_first_of("?#"));
                if (fs::path(stem).extension().empty())
                    extensions = {".fw", ".fly"};
                for (auto &extension : extensions) {
                    auto candidate = target + extension;
                    if (isUrl(candidate)) {
                        std::string contents;
                        if (downloads.count(candidate) || tryFetchUrl(candidate, contents)) {
                            downloads.emplace(candidate, contents);
                            found = candidate;
                            break;
                        }
                        continue;
                    }
                    std::vector<fs::path> bases = {fs::path()};
                    if (!fs::path(candidate).is_absolute())
                        bases = {path.parent_path(), root};
                    for (auto &base : bases) {
                        auto file = base.empty() ? fs::path(candidate) : base / candidate;
                        if (fs::is_regular_file(file)) {
                            found = file.string();
                            break;
                        }
                    }
                    if (!found.empty())
                        break;
                }
                if (found.empty())
                    throw Error(importLoc, "cannot find module file '" + name + "'");
                if (canonicalLocation(found) == u.path)
                    throw Error(importLoc, "module '" + name + "' imports itself");
            }
            auto relative = name;
            std::replace(relative.begin(), relative.end(), '.',
                         static_cast<char>(fs::path::preferred_separator));
            // Library modules import only other library and native modules.
            std::vector<fs::path> bases;
            if (!library && !remote && found.empty())
                bases = {path.parent_path(), root};
            // A module loaded by URL finds its sibling modules by URL (the entry's directory
            // too), except names of library and native modules, which are never fetched.
            if (remote && found.empty() && !native.count(name) && !libraryModules().count(name) &&
                !packages.count(name.substr(0, name.find('.')))) {
                auto slashed = name;
                std::replace(slashed.begin(), slashed.end(), '.', '/');
                std::vector<std::string> directories = {urlDirectory(location)};
                if (!rootUrl.empty() && rootUrl != directories[0])
                    directories.push_back(rootUrl);
                for (auto &directory : directories) {
                    for (auto extension : {".fw", ".fly"}) {
                        auto candidate = directory + slashed + extension;
                        std::string contents;
                        if (candidate != u.path &&
                            (downloads.count(candidate) || tryFetchUrl(candidate, contents))) {
                            downloads.emplace(candidate, contents);
                            found = candidate;
                            break;
                        }
                    }
                    if (!found.empty())
                        break;
                }
            }
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
                        found = candidate.string();
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
                        found = candidate.string();
                        break;
                    }
                }
                if (found.empty())
                    throw Error(importLoc, "package '" + head + "' has no module '" + name + "'");
            }
            if (found.empty() && libraryModules().count(name))
                found = (libraryDirectory / (name + ".fw")).string();
            if (!found.empty()) {
                auto &dependency = load(found);
                u.modules[alias] = &dependency;
                u.unitModules.push_back(&dependency);
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
            } else if (native.count(name) && !pathImport(name)) {
                if (u.program.wildcards.count(alias) && name != "html" && name != "svg")
                    throw Error(importLoc,
                                "native wildcard imports are supported only for html and svg");
                auto qualified = "$native$" + name;
                u.builtins[alias] = qualified;
                // `import time` also brings the time unit namespace (stdlib/fw/_time.fw).
                if (name == "time")
                    u.unitModules.push_back(&load((libraryDirectory / "_time.fw").string()));
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
        units_(u, add);
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
        u.unitExports = u.program.unitNamespaces;
        for (auto &ns : u.program.unitNamespaces) {
            ns.loc.file = u.path;
            for (auto *name : {&ns.intClass, &ns.realClass})
                if (!name->empty())
                    *name = u.own.at(*name);
            merged.unitNamespaces.push_back(std::move(ns));
        }
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
        if (isUrl(file) && fileUrlPath(file).empty()) {
            // A program named by URL: its imports resolve by URL; packages from the working
            // directory.
            rootUrl = urlDirectory(file);
            root = fs::current_path();
            packages = packageRoots((fs::current_path() / "main.fw").string());
            load(file);
            return std::move(merged);
        }
        auto local = isUrl(file) ? fileUrlPath(file) : file;
        root = fs::absolute(local).parent_path();
        packages = packageRoots(fs::absolute(local).string());
        load(fs::absolute(local).string());
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
