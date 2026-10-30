// A small Language Server Protocol implementation: diagnostics from the compiler's semantic
// check and whole-document formatting. Messages are JSON-RPC over stdin/stdout.
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <iostream>
#include <memory>
#include <sstream>
#include <variant>

namespace fw {
namespace {
struct Json;
using JsonPtr = std::shared_ptr<Json>;
struct Json {
    std::variant<std::nullptr_t, bool, double, std::string, std::vector<JsonPtr>,
                 std::map<std::string, JsonPtr>>
        value;
    const Json *get(const std::string &key) const {
        auto object = std::get_if<std::map<std::string, JsonPtr>>(&value);
        if (!object)
            return nullptr;
        auto it = object->find(key);
        return it == object->end() ? nullptr : it->second.get();
    }
    std::string text() const {
        auto s = std::get_if<std::string>(&value);
        return s ? *s : "";
    }
};
class Reader {
    const std::string &s;
    size_t p = 0;
    void space() {
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
            ++p;
    }
    std::string string() {
        std::string out;
        ++p;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') {
                out += c;
                continue;
            }
            char e = s[p++];
            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                unsigned v = std::stoul(s.substr(p, 4), nullptr, 16);
                p += 4;
                if (v >= 0xd800 && v <= 0xdbff && s.compare(p, 2, "\\u") == 0) {
                    unsigned low = std::stoul(s.substr(p + 2, 4), nullptr, 16);
                    p += 6;
                    v = 0x10000 + ((v - 0xd800) << 10) + (low - 0xdc00);
                }
                if (v < 0x80)
                    out += char(v);
                else if (v < 0x800) {
                    out += char(0xc0 | (v >> 6));
                    out += char(0x80 | (v & 63));
                } else if (v < 0x10000) {
                    out += char(0xe0 | (v >> 12));
                    out += char(0x80 | ((v >> 6) & 63));
                    out += char(0x80 | (v & 63));
                } else {
                    out += char(0xf0 | (v >> 18));
                    out += char(0x80 | ((v >> 12) & 63));
                    out += char(0x80 | ((v >> 6) & 63));
                    out += char(0x80 | (v & 63));
                }
                break;
            }
            default:
                out += e;
            }
        }
        ++p;
        return out;
    }

  public:
    explicit Reader(const std::string &s) : s(s) {}
    JsonPtr value() {
        space();
        auto out = std::make_shared<Json>();
        if (p >= s.size())
            throw std::runtime_error("unexpected end of JSON");
        char c = s[p];
        if (c == '{') {
            ++p;
            std::map<std::string, JsonPtr> object;
            space();
            if (s[p] == '}')
                ++p;
            else
                for (;;) {
                    space();
                    auto key = string();
                    space();
                    ++p; // ':'
                    object[key] = value();
                    space();
                    if (s[p++] == '}')
                        break;
                }
            out->value = std::move(object);
        } else if (c == '[') {
            ++p;
            std::vector<JsonPtr> array;
            space();
            if (s[p] == ']')
                ++p;
            else
                for (;;) {
                    array.push_back(value());
                    space();
                    if (s[p++] == ']')
                        break;
                }
            out->value = std::move(array);
        } else if (c == '"')
            out->value = string();
        else if (s.compare(p, 4, "true") == 0) {
            p += 4;
            out->value = true;
        } else if (s.compare(p, 5, "false") == 0) {
            p += 5;
            out->value = false;
        } else if (s.compare(p, 4, "null") == 0) {
            p += 4;
            out->value = nullptr;
        } else {
            size_t used = 0;
            out->value = std::stod(s.substr(p), &used);
            p += used;
        }
        return out;
    }
};
std::string quote(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 32) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        } else
            out += char(c);
    }
    return out + '"';
}
// The raw JSON text of a request id (number or string), echoed in the response.
std::string idText(const Json *id) {
    if (!id)
        return "null";
    if (auto n = std::get_if<double>(&id->value)) {
        std::ostringstream out;
        out << static_cast<long long>(*n);
        return out.str();
    }
    return quote(id->text());
}
void send(const std::string &body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}
std::string pathOf(const std::string &uri) {
    std::string path = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            out += static_cast<char>(std::stoi(path.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else
            out += path[i];
    }
#ifdef _WIN32
    if (out.size() > 2 && out[0] == '/' && out[2] == ':')
        out.erase(0, 1);
#endif
    return out;
}
std::string canonical(const std::string &path) {
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : result.string();
}
std::string range(int line, int column, int endLine, int endColumn) {
    return "{\"start\":{\"line\":" + std::to_string(line) + ",\"character\":" +
           std::to_string(column) + "},\"end\":{\"line\":" + std::to_string(endLine) +
           ",\"character\":" + std::to_string(endColumn) + "}}";
}
// A syntactic index of a source file for navigation: declarations from the parser and
// identifier tokens from the lexer (including identifiers inside string interpolations).
struct Symbol {
    std::string name, kind, container; // kind: function, class, interface, enum, field,
                                        // method, member, variable, parameter, import
    int line = 0, column = 0;           // the name token, 1-based (byte column)
    std::string signature;              // the declaration's source line
    std::string file;
};
struct Scope {
    int line = 0;          // where the function or method starts
    std::string owner;     // class of a method
    std::vector<Symbol> locals;
};
struct FileIndex {
    std::string path, text;
    std::vector<std::string> lines;
    std::vector<Token> tokens;
    std::vector<Symbol> globals;            // top-level declarations
    std::map<std::string, std::vector<Symbol>> members; // class/enum -> fields, methods, members
    std::vector<Scope> scopes;              // functions and methods, by start line
    std::map<std::string, std::string> imports; // alias -> module name
    std::set<std::string> wildcards, privateNames;
    bool parsed = false;
};
std::string trimmed(const std::string &line) {
    auto a = line.find_first_not_of(" \t");
    if (a == std::string::npos)
        return "";
    auto b = line.find_last_not_of(" \t\r");
    return line.substr(a, b - a + 1);
}
// Identifier tokens of a source, adding those inside `{...}` of string literals.
std::vector<Token> navigationTokens(const std::string &text) {
    std::vector<Token> out;
    std::vector<Token> tokens;
    try {
        tokens = lex(text);
    } catch (const Error &) {
        return out;
    }
    for (auto &t : tokens) {
        out.push_back(t);
        if (t.kind != "string")
            continue;
        for (size_t i = 0; i < t.text.size(); ++i) {
            if (t.text[i] == '{' && i + 1 < t.text.size() && t.text[i + 1] == '{') {
                ++i;
                continue;
            }
            // `{` before whitespace is a literal brace, not an interpolation.
            if (t.text[i] != '{' || i + 1 == t.text.size() ||
                std::isspace(static_cast<unsigned char>(t.text[i + 1])))
                continue;
            size_t depth = 1, j = i + 1;
            while (j < t.text.size() && depth) {
                depth += t.text[j] == '{' ? 1 : t.text[j] == '}' ? -1 : 0;
                ++j;
            }
            try {
                for (auto inner : lex(t.text.substr(i + 1, j - i - 2))) {
                    if (inner.kind == "eof" || inner.kind == "nl")
                        continue;
                    inner.loc.line = t.loc.line;
                    inner.loc.column = t.loc.column + 1 + static_cast<int>(i) + inner.loc.column;
                    out.push_back(inner);
                }
            } catch (const Error &) {
            }
            i = j - 1;
        }
    }
    return out;
}
class Navigator {
    std::map<std::string, std::string> overlay; // canonical path -> open document text
    std::map<std::string, FileIndex> files;
    std::string canonicalPath(const std::string &path) {
        if (path.rfind("/<flywheel>/", 0) == 0)
            return path;
        std::error_code ec;
        auto result = std::filesystem::weakly_canonical(path, ec);
        return ec ? path : result.string();
    }
    // The first identifier token `name` at or after `loc`.
    std::pair<int, int> nameAt(FileIndex &f, const std::string &name, Location loc) {
        for (auto &t : f.tokens)
            if (t.kind == "id" && t.text == name &&
                (t.loc.line > loc.line || (t.loc.line == loc.line && t.loc.column >= loc.column)))
                return {t.loc.line, t.loc.column};
        return {loc.line, loc.column};
    }
    Symbol symbol(FileIndex &f, const std::string &name, const std::string &kind, Location loc,
                  const std::string &container = "") {
        Symbol s;
        s.name = name;
        s.kind = kind;
        s.container = container;
        s.file = f.path;
        std::tie(s.line, s.column) = nameAt(f, name, loc);
        if (s.line >= 1 && s.line <= static_cast<int>(f.lines.size()))
            s.signature = trimmed(f.lines[s.line - 1]);
        return s;
    }
    void locals(FileIndex &f, Scope &scope, const std::vector<SP> &body) {
        for (auto &sp : body) {
            auto &st = *sp;
            if (st.kind == "declare" && st.name.rfind("$", 0) != 0)
                scope.locals.push_back(symbol(f, st.name, "variable", st.loc));
            if (st.kind == "for") {
                scope.locals.push_back(symbol(f, st.name, "variable", st.loc));
                if (!st.op.empty())
                    scope.locals.push_back(symbol(f, st.op, "variable", st.loc));
            }
            for (size_t i = 0; i < st.catchNames.size(); ++i)
                if (!st.catchNames[i].empty())
                    scope.locals.push_back(symbol(f, st.catchNames[i], "variable", st.loc));
            locals(f, scope, st.body);
            locals(f, scope, st.other);
            locals(f, scope, st.finalizer);
            for (auto &c : st.catches)
                locals(f, scope, c);
            std::function<void(const Expr &)> lambdas = [&](const Expr &e) {
                if (e.lambda) {
                    for (auto &param : e.lambda->params)
                        scope.locals.push_back(symbol(f, param.name, "parameter", param.loc));
                    locals(f, scope, e.lambda->body);
                }
                for (auto &a : e.args)
                    if (a)
                        lambdas(*a);
            };
            if (st.expr)
                lambdas(*st.expr);
            if (st.target)
                lambdas(*st.target);
        }
    }
    void function(FileIndex &f, const Function &fn, const std::string &owner) {
        Scope scope;
        scope.line = fn.loc.line;
        scope.owner = owner;
        for (auto &param : fn.params)
            scope.locals.push_back(symbol(f, param.name, "parameter", param.loc));
        locals(f, scope, fn.body);
        f.scopes.push_back(std::move(scope));
    }

  public:
    void open(const std::string &path, const std::string &text) {
        overlay[canonicalPath(path)] = text;
        files.erase(canonicalPath(path));
    }
    void close(const std::string &path) {
        overlay.erase(canonicalPath(path));
        files.erase(canonicalPath(path));
    }
    FileIndex *index(const std::string &rawPath) {
        auto path = canonicalPath(rawPath);
        auto existing = files.find(path);
        if (existing != files.end())
            return existing->second.parsed ? &existing->second : nullptr;
        auto &f = files[path];
        f.path = path;
        if (auto it = overlay.find(path); it != overlay.end())
            f.text = it->second;
        else if (path.rfind("/<flywheel>/lib/", 0) == 0) {
            auto name = std::filesystem::path(path).stem().string();
            if (!libraryModules().count(name))
                return nullptr;
            f.text = libraryModules().at(name);
        } else {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return nullptr;
            std::stringstream buffer;
            buffer << in.rdbuf();
            f.text = buffer.str();
        }
        std::istringstream lines(f.text);
        std::string line;
        while (std::getline(lines, line))
            f.lines.push_back(line);
        f.tokens = navigationTokens(f.text);
        Program program;
        try {
            program = Parser(lex(f.text)).parse();
        } catch (const Error &) {
            // Keep the tokens so that text-based navigation still works while editing.
            f.parsed = true;
            return &f;
        }
        f.parsed = true;
        f.imports = program.imports;
        f.wildcards = program.wildcards;
        f.privateNames = program.privateNames;
        for (auto &[alias, name] : program.imports)
            f.globals.push_back(symbol(f, alias, "import", program.importLocations.at(alias)));
        for (auto &fn : program.functions) {
            f.globals.push_back(symbol(f, fn.name, "function", fn.loc));
            function(f, fn, "");
        }
        for (auto &c : program.classes) {
            if (!c.variantOf.empty()) {
                auto member = c.name.substr(c.variantOf.size() + 1);
                auto &list = f.members[c.variantOf];
                list.push_back(symbol(f, member, "member", c.loc, c.variantOf));
                for (auto &field : c.fields)
                    f.members[c.name].push_back(symbol(f, field.name, "field", field.loc, c.name));
                continue;
            }
            f.globals.push_back(symbol(f, c.name, c.isInterface ? "interface" : "class", c.loc));
            for (auto &field : c.fields)
                f.members[c.name].push_back(symbol(f, field.name, "field", field.loc, c.name));
            for (auto &m : c.methods) {
                if (m.name.rfind("operator", 0) == 0)
                    continue;
                f.members[c.name].push_back(symbol(f, m.name, "method", m.loc, c.name));
                function(f, m, c.name);
            }
        }
        for (auto &en : program.enums) {
            f.globals.push_back(symbol(f, en.name, "enum", en.loc));
            auto loc = en.loc;
            for (auto &m : en.members) {
                auto s = symbol(f, m, "member", loc, en.name);
                f.members[en.name].push_back(s);
                loc = {s.line, s.column + 1, ""};
            }
        }
        for (auto &[name, e] : program.components)
            f.globals.push_back(symbol(f, name, "component", e->loc));
        for (auto &st : program.body)
            if (st->kind == "declare" && st->name.rfind("$", 0) != 0)
                f.globals.push_back(symbol(f, st->name, st->op == "const" ? "constant" : "variable", st->loc));
        std::sort(f.scopes.begin(), f.scopes.end(), [](auto &a, auto &b) { return a.line < b.line; });
        return &f;
    }
    // The file a module import refers to, as the compiler would resolve it ("" if native).
    std::string modulePath(FileIndex &f, const std::string &module) {
        auto relative = module;
        std::replace(relative.begin(), relative.end(), '.', '/');
        auto directory = std::filesystem::path(f.path).parent_path();
        for (auto extension : {".fw", ".fly"}) {
            auto candidate = directory / (relative + extension);
            if (overlay.count(canonicalPath(candidate.string())) || std::filesystem::is_regular_file(candidate))
                return canonicalPath(candidate.string());
        }
        try {
            auto packages = packageRoots(f.path);
            auto head = module.substr(0, module.find('.'));
            if (packages.count(head)) {
                auto rest = module.find('.') == std::string::npos ? packageMain(packages.at(head), head)
                                                                  : relative.substr(head.size() + 1) + ".fw";
                return canonicalPath((std::filesystem::path(packages.at(head)) / rest).string());
            }
        } catch (const std::exception &) {
        }
        if (libraryModules().count(module))
            return "/<flywheel>/lib/" + module + ".fw";
        return "";
    }
    const Token *tokenAt(FileIndex &f, int line, int column, size_t *position = nullptr) {
        for (size_t i = 0; i < f.tokens.size(); ++i) {
            auto &t = f.tokens[i];
            if (t.loc.line == line && t.kind == "id" && column >= t.loc.column &&
                column <= t.loc.column + static_cast<int>(t.text.size())) {
                if (position)
                    *position = i;
                return &t;
            }
        }
        return nullptr;
    }
    const Scope *scopeAt(FileIndex &f, int line) {
        const Scope *found = nullptr;
        for (auto &s : f.scopes)
            if (s.line <= line)
                found = &s;
        // A function's scope ends where the next top-level declaration begins.
        if (found)
            for (auto &g : f.globals)
                if (g.line > found->line && g.line <= line && g.kind != "import")
                    return nullptr;
        return found;
    }
    std::optional<Symbol> member(FileIndex &f, const std::string &type, const std::string &name) {
        auto it = f.members.find(type);
        if (it != f.members.end())
            for (auto &m : it->second)
                if (m.name == name)
                    return m;
        return std::nullopt;
    }
    std::optional<Symbol> global(FileIndex &f, const std::string &name, int depth = 0) {
        for (auto &g : f.globals)
            if (g.name == name && g.kind != "import")
                return g;
        if (depth < 3)
            for (auto &alias : f.wildcards)
                if (auto module = index(modulePath(f, f.imports.at(alias))))
                    if (auto found = global(*module, name, depth + 1); found && !module->privateNames.count(name))
                        return found;
        for (auto &g : f.globals)
            if (g.name == name)
                return g;
        return std::nullopt;
    }
    // The declaration that the identifier at (line, column) refers to.
    std::optional<Symbol> definition(const std::string &path, int line, int column) {
        auto f = index(path);
        if (!f)
            return std::nullopt;
        size_t i;
        auto t = tokenAt(*f, line, column, &i);
        if (!t)
            return std::nullopt;
        auto name = t->text;
        auto scope = scopeAt(*f, line);
        bool afterDot = i >= 1 && f->tokens[i - 1].text == "." && f->tokens[i - 1].loc.line == t->loc.line;
        if (i >= 1 && f->tokens[i - 1].text == "inst" && scope && !scope->owner.empty())
            return member(*f, scope->owner, name);
        if (afterDot && i >= 2) {
            auto &receiver = f->tokens[i - 2];
            if (receiver.kind == "id" && f->imports.count(receiver.text)) {
                if (auto module = index(modulePath(*f, f->imports.at(receiver.text))))
                    return global(*module, name);
                return std::nullopt;
            }
            if (receiver.kind == "id" && f->members.count(receiver.text))
                if (auto found = member(*f, receiver.text, name))
                    return found;
            // An instance member: look for a field or method with this name in any class.
            for (auto &[type, list] : f->members)
                for (auto &m : list)
                    if (m.name == name && m.kind != "member")
                        return m;
            for (auto &[alias, module] : f->imports)
                if (auto other = index(modulePath(*f, module)))
                    for (auto &[type, list] : other->members)
                        for (auto &m : list)
                            if (m.name == name && m.kind != "member")
                                return m;
            return std::nullopt;
        }
        if (scope) {
            const Symbol *best = nullptr;
            for (auto &l : scope->locals)
                if (l.name == name && (l.line < line || (l.line == line && l.column <= column)))
                    if (!best || l.line > best->line || (l.line == best->line && l.column > best->column))
                        best = &l;
            if (best)
                return *best;
        }
        return global(*f, name);
    }
    // Every identifier token (in open documents and the declaring file) that refers to `target`.
    std::vector<Symbol> references(const Symbol &target) {
        std::set<std::string> paths;
        for (auto &[path, text] : overlay)
            paths.insert(path);
        paths.insert(target.file);
        std::vector<Symbol> out;
        for (auto &path : paths) {
            auto f = index(path);
            if (!f)
                continue;
            for (auto &t : f->tokens) {
                if (t.kind != "id" || t.text != target.name)
                    continue;
                auto d = definition(path, t.loc.line, t.loc.column);
                if (d && d->file == target.file && d->line == target.line && d->column == target.column) {
                    Symbol s = target;
                    s.file = path;
                    s.line = t.loc.line;
                    s.column = t.loc.column;
                    out.push_back(s);
                }
            }
        }
        return out;
    }
    // Documentation comment lines directly above a declaration.
    std::string documentation(const Symbol &s) {
        auto f = index(s.file);
        if (!f)
            return "";
        std::vector<std::string> comment;
        for (int n = s.line - 1; n >= 1; --n) {
            auto line = trimmed(f->lines[n - 1]);
            if (line.empty() || line[0] != '#')
                break;
            line = line.substr(1);
            if (!line.empty() && line[0] == ' ')
                line = line.substr(1);
            comment.insert(comment.begin(), line);
        }
        std::string out;
        for (auto &l : comment)
            out += l + "\n";
        return out;
    }
    // Names visible at a position, for completion: (label, kind).
    std::vector<std::pair<std::string, std::string>> completions(const std::string &path, int line,
                                                                 int column) {
        std::vector<std::pair<std::string, std::string>> out;
        auto f = index(path);
        if (!f)
            return out;
        // The text before the cursor decides between member and plain completion.
        std::string prefix = line >= 1 && line <= static_cast<int>(f->lines.size())
                                 ? f->lines[line - 1].substr(0, std::max(0, column - 1))
                                 : "";
        size_t end = prefix.size();
        while (end > 0 && (std::isalnum(static_cast<unsigned char>(prefix[end - 1])) || prefix[end - 1] == '_'))
            --end;
        auto before = prefix.substr(0, end);
        if (!before.empty() && before.back() == '.') {
            size_t start = before.size() - 1;
            while (start > 0 && (std::isalnum(static_cast<unsigned char>(before[start - 1])) || before[start - 1] == '_'))
                --start;
            auto receiver = before.substr(start, before.size() - 1 - start);
            if (f->imports.count(receiver)) {
                auto module = f->imports.at(receiver);
                if (auto other = index(modulePath(*f, module))) {
                    for (auto &g : other->globals)
                        if (g.kind != "import" && !other->privateNames.count(g.name))
                            out.push_back({g.name, g.kind});
                } else
                    for (auto &name : nativeMembers(module))
                        out.push_back({name, "function"});
                return out;
            }
            if (f->members.count(receiver)) {
                for (auto &m : f->members.at(receiver))
                    out.push_back({m.name, m.kind});
                return out;
            }
            std::set<std::string> seen;
            for (auto &[type, list] : f->members)
                for (auto &m : list)
                    if (m.kind != "member" && seen.insert(m.name).second)
                        out.push_back({m.name, m.kind});
            for (auto name : {"length", "push", "pop", "map", "filter", "reduce", "contains",
                              "split", "join", "keys", "values", "upper", "lower", "trim",
                              "replace", "starts_with", "ends_with", "find", "slice", "sort",
                              "freeze", "is_frozen"})
                if (seen.insert(name).second)
                    out.push_back({name, "method"});
            return out;
        }
        auto scope = scopeAt(*f, line);
        if (trimmed(before).size() >= 4 && before.substr(before.find_last_not_of(" ") - 3, 4) == "inst" &&
            scope && !scope->owner.empty()) {
            for (auto &m : f->members[scope->owner])
                out.push_back({m.name, m.kind});
            return out;
        }
        std::set<std::string> seen;
        if (scope)
            for (auto &l : scope->locals)
                if (l.line <= line && seen.insert(l.name).second)
                    out.push_back({l.name, l.kind});
        for (auto &g : f->globals)
            if (seen.insert(g.name).second)
                out.push_back({g.name, g.kind});
        for (auto keyword : {"function", "class", "interface", "enum", "var", "const", "if",
                             "then", "else", "end", "while", "for", "in", "do", "return",
                             "match", "case", "try", "catch", "finally", "throw", "defer",
                             "import", "private", "readonly", "inst", "true", "false", "null",
                             "and", "or", "not", "print", "returns", "break", "continue"})
            if (seen.insert(keyword).second)
                out.push_back({keyword, "keyword"});
        return out;
    }
    static std::vector<std::string> nativeMembers(const std::string &module) {
        static const std::map<std::string, std::vector<std::string>> table = {
            {"fs", {"exists", "is_dir", "is_file", "read", "write", "append", "try_read", "size",
                    "list", "walk", "mkdir", "copy", "move", "remove", "remove_all", "join",
                    "basename", "dirname", "extension", "stem", "absolute", "normalize", "cwd",
                    "temp_dir"}},
            {"time", {"monotonic", "sleep", "now", "format", "format_local", "parse", "date",
                      "local_date", "from_date"}},
            {"regex", {"matches", "contains", "find", "find_all", "groups", "replace", "split"}},
            {"json", {"parse", "encode", "decode", "stringify", "get", "at", "has", "length",
                      "keys", "items", "is_null", "kind", "as_string", "as_int", "as_double",
                      "as_bool"}},
            {"math", {"sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "hypot",
                      "exp", "log", "log10", "log2", "pow", "floor", "ceil", "round", "trunc",
                      "abs", "min", "max", "clamp", "random", "random_int", "seed", "is_nan",
                      "is_finite", "PI", "TAU", "E", "INF", "NAN"}},
            {"os", {"env", "args", "exit", "run", "process", "pipeline", "shell", "command"}},
            {"io", {"write", "write_error", "read_line", "read_all", "open"}},
            {"net", {"resolve", "connect", "listen"}},
            {"thread", {"spawn", "channel", "mutex", "cores"}},
            {"web", {"serve", "route", "static", "tls", "text", "html", "json", "redirect",
                     "workers", "wait", "get", "post", "put", "patch", "delete", "head",
                     "fetch"}},
            {"html", {"render", "parse", "query", "query_all", "get_by_id", "closest",
                      "matches", "children", "text_content", "inner_html", "outer_html"}},
            {"testing", {"assert", "assert_equal", "fail", "expect_error", "live_objects"}},
            {"crypto", {"sha256", "sha1", "md5", "sha256_bytes", "hmac_sha256", "hmac_sha1",
                        "random_bytes", "random_hex", "random_int", "token", "secure_equal"}},
            {"encoding", {"base64_encode", "base64_decode", "base64url_encode",
                          "base64url_decode", "hex_encode", "hex_decode", "url_encode",
                          "url_decode", "form_encode", "form_decode"}},
            {"uuid", {"v4", "v7", "is_valid"}},
            {"compress", {"gzip", "gunzip", "zlib", "unzlib", "deflate", "inflate", "crc32",
                          "zip", "unzip"}},
            {"sqlite", {"open"}},
            {"toml", {"parse"}},
            {"yaml", {"parse"}},
            {"gc", {"collect", "stats"}},
            {"collections", {"range"}}};
        auto it = table.find(module);
        return it == table.end() ? std::vector<std::string>{} : it->second;
    }
    // Top-level declarations and class members, for the outline view.
    std::vector<Symbol> outline(const std::string &path) {
        std::vector<Symbol> out;
        auto f = index(path);
        if (!f)
            return out;
        for (auto &g : f->globals)
            if (g.kind != "import") {
                out.push_back(g);
                if (f->members.count(g.name))
                    for (auto &m : f->members.at(g.name))
                        out.push_back(m);
            }
        std::stable_sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.line < b.line; });
        return out;
    }
};
std::string uriOf(const std::string &path) {
    std::string out = "file://";
#ifdef _WIN32
    out += "/";
#endif
    for (unsigned char c : path) {
        if (std::isalnum(c) || std::strchr("/-_.~:", c))
            out += static_cast<char>(c);
        else if (c == '\\')
            out += '/';
        else {
            char buffer[4];
            std::snprintf(buffer, sizeof buffer, "%%%02X", c);
            out += buffer;
        }
    }
    return out;
}
// LSP positions count UTF-16 code units; the index uses 1-based byte columns.
int byteColumn(const std::string &line, int utf16) {
    size_t i = 0;
    for (int units = 0; i < line.size() && units < utf16;) {
        auto c = static_cast<unsigned char>(line[i]);
        size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        units += width == 4 ? 2 : 1;
        i += width;
    }
    return static_cast<int>(i) + 1;
}
int utf16Column(const std::string &line, int byte) {
    int units = 0;
    for (size_t i = 0; i + 1 < static_cast<size_t>(byte) && i < line.size();) {
        auto c = static_cast<unsigned char>(line[i]);
        size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        units += width == 4 ? 2 : 1;
        i += width;
    }
    return units;
}
struct Server {
    std::map<std::string, std::string> documents; // uri -> text
    Navigator navigator;
    std::string lineText(const std::string &path, int line) {
        auto f = navigator.index(path);
        if (!f || line < 1 || line > static_cast<int>(f->lines.size()))
            return "";
        return f->lines[line - 1];
    }
    std::string location(const Symbol &s) {
        auto text = lineText(s.file, s.line);
        int start = utf16Column(text, s.column);
        return "{\"uri\":" + quote(uriOf(s.file)) + ",\"range\":" +
               range(s.line - 1, start, s.line - 1, start + utf16Column(s.name, static_cast<int>(s.name.size()) + 1)) + "}";
    }
    void diagnose(const std::string &uri) {
        std::string diagnostics;
        auto path = pathOf(uri);
        std::map<std::string, std::string> overlay;
        for (auto &[openUri, text] : documents)
            overlay[canonical(pathOf(openUri))] = text;
        auto add = [&](int line, int column, const std::string &message) {
            line = std::max(0, line - 1);
            column = std::max(0, column - 1);
            diagnostics += (diagnostics.empty() ? "" : ",") + std::string("{\"range\":") +
                           range(line, column, line, column + 1) +
                           ",\"severity\":1,\"source\":\"flywheel\",\"message\":" +
                           quote(message) + "}";
        };
        try {
            auto program = loadProgram(path, overlay);
            Options options;
            options.entry = "check"; // like `flywheel check`: markup may be returned at top level
            compile(program, path, options);
        } catch (const Error &e) {
            if (e.loc.file.empty() || canonical(e.loc.file) == canonical(path))
                add(e.loc.line, e.loc.column, e.what());
            else
                add(1, 1, "in " + e.loc.file + ":" + std::to_string(e.loc.line) + ": " + e.what());
        } catch (const std::exception &e) {
            add(1, 1, e.what());
        }
        send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
             "\"uri\":" +
             quote(uri) + ",\"diagnostics\":[" + diagnostics + "]}}");
    }
    // Returns false after `exit`.
    bool handle(const Json &message) {
        auto method = message.get("method") ? message.get("method")->text() : "";
        auto id = message.get("id");
        auto params = message.get("params");
        auto reply = [&](const std::string &result) {
            send("{\"jsonrpc\":\"2.0\",\"id\":" + idText(id) + ",\"result\":" + result + "}");
        };
        auto document = [&]() -> const Json * {
            return params ? params->get("textDocument") : nullptr;
        };
        // The document position of a request, as (path, 1-based line, 1-based byte column).
        auto position = [&](std::string &path, int &line, int &column) {
            auto p = params ? params->get("position") : nullptr;
            if (!document() || !p)
                return false;
            path = pathOf(document()->get("uri")->text());
            line = static_cast<int>(std::get<double>(p->get("line")->value)) + 1;
            column = byteColumn(lineText(path, line),
                                static_cast<int>(std::get<double>(p->get("character")->value)));
            return true;
        };
        std::string path;
        int line = 0, column = 0;
        if (method == "initialize")
            reply("{\"capabilities\":{\"textDocumentSync\":1,\"documentFormattingProvider\":true,"
                  "\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,"
                  "\"renameProvider\":true,\"documentSymbolProvider\":true,"
                  "\"completionProvider\":{\"triggerCharacters\":[\".\"]}},"
                  "\"serverInfo\":{\"name\":\"flywheel\",\"version\":\"" FW_VERSION "\"}}");
        else if (method == "shutdown")
            reply("null");
        else if (method == "exit")
            return false;
        else if (method == "textDocument/didOpen" && document()) {
            auto uri = document()->get("uri")->text();
            documents[uri] = document()->get("text")->text();
            navigator.open(pathOf(uri), documents[uri]);
            diagnose(uri);
        } else if (method == "textDocument/didChange" && document()) {
            auto uri = document()->get("uri")->text();
            if (auto changes = params->get("contentChanges"))
                if (auto list = std::get_if<std::vector<JsonPtr>>(&changes->value);
                    list && !list->empty())
                    documents[uri] = list->back()->get("text")->text();
            navigator.open(pathOf(uri), documents[uri]);
            diagnose(uri);
        } else if (method == "textDocument/didSave" && document())
            diagnose(document()->get("uri")->text());
        else if (method == "textDocument/didClose" && document()) {
            auto uri = document()->get("uri")->text();
            documents.erase(uri);
            navigator.close(pathOf(uri));
            send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
                 "\"uri\":" +
                 quote(uri) + ",\"diagnostics\":[]}}");
        } else if (method == "textDocument/formatting" && document()) {
            auto &text = documents[document()->get("uri")->text()];
            std::string edits = "[]";
            try {
                auto formatted = format(text);
                if (formatted != text) {
                    int lines = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
                    edits = "[{\"range\":" + range(0, 0, lines + 1, 0) +
                            ",\"newText\":" + quote(formatted) + "}]";
                }
            } catch (const std::exception &) {
            }
            reply(edits);
        } else if (method == "textDocument/definition" && position(path, line, column)) {
            auto found = navigator.definition(path, line, column);
            reply(found && found->file.rfind("/<flywheel>/", 0) != 0 ? location(*found) : "null");
        } else if (method == "textDocument/hover" && position(path, line, column)) {
            auto found = navigator.definition(path, line, column);
            if (!found) {
                reply("null");
                return true;
            }
            auto text = "```flywheel\n" + found->signature + "\n```";
            auto docs = navigator.documentation(*found);
            if (!docs.empty())
                text += "\n\n" + docs;
            if (!found->container.empty())
                text += "\n\n*" + found->kind + " of " + found->container + "*";
            reply("{\"contents\":{\"kind\":\"markdown\",\"value\":" + quote(text) + "}}");
        } else if (method == "textDocument/references" && position(path, line, column)) {
            std::string out;
            if (auto found = navigator.definition(path, line, column))
                for (auto &r : navigator.references(*found))
                    out += (out.empty() ? "" : ",") + location(r);
            reply("[" + out + "]");
        } else if (method == "textDocument/rename" && position(path, line, column)) {
            auto newName = params->get("newName") ? params->get("newName")->text() : "";
            auto found = navigator.definition(path, line, column);
            bool valid = !newName.empty() && (std::isalpha(static_cast<unsigned char>(newName[0])) || newName[0] == '_');
            for (unsigned char c : newName)
                valid = valid && (std::isalnum(c) || c == '_' || c >= 0x80);
            if (!found || !valid || found->file.rfind("/<flywheel>/", 0) == 0) {
                send("{\"jsonrpc\":\"2.0\",\"id\":" + idText(id) +
                     ",\"error\":{\"code\":-32602,\"message\":" +
                     quote(!found ? "nothing to rename here" : !valid ? "invalid name" : "library names cannot be renamed") + "}}");
                return true;
            }
            std::map<std::string, std::string> edits;
            for (auto &r : navigator.references(*found)) {
                auto text = lineText(r.file, r.line);
                int start = utf16Column(text, r.column);
                auto &list = edits[uriOf(r.file)];
                list += (list.empty() ? "" : ",") + std::string("{\"range\":") +
                        range(r.line - 1, start, r.line - 1, start + utf16Column(r.name, static_cast<int>(r.name.size()) + 1)) +
                        ",\"newText\":" + quote(newName) + "}";
            }
            std::string changes;
            for (auto &[uri, list] : edits)
                changes += (changes.empty() ? "" : ",") + quote(uri) + ":[" + list + "]";
            reply("{\"changes\":{" + changes + "}}");
        } else if (method == "textDocument/completion" && position(path, line, column)) {
            static const std::map<std::string, int> kinds = {
                {"method", 2},    {"function", 3}, {"field", 5},     {"variable", 6},
                {"parameter", 6}, {"class", 7},    {"interface", 8}, {"import", 9},
                {"enum", 13},     {"keyword", 14}, {"member", 20},   {"constant", 21},
                {"component", 7}};
            std::string items;
            for (auto &[label, kind] : navigator.completions(path, line, column)) {
                auto k = kinds.count(kind) ? kinds.at(kind) : 1;
                items += (items.empty() ? "" : ",") + std::string("{\"label\":") + quote(label) +
                         ",\"kind\":" + std::to_string(k) + ",\"detail\":" + quote(kind) + "}";
            }
            reply("{\"isIncomplete\":false,\"items\":[" + items + "]}");
        } else if (method == "textDocument/documentSymbol" && document()) {
            static const std::map<std::string, int> kinds = {
                {"class", 5},     {"method", 6},  {"field", 8},     {"enum", 10},
                {"interface", 11}, {"function", 12}, {"variable", 13}, {"constant", 14},
                {"member", 22},   {"component", 5}};
            std::string out;
            for (auto &sym : navigator.outline(pathOf(document()->get("uri")->text()))) {
                out += (out.empty() ? "" : ",") + std::string("{\"name\":") + quote(sym.name) +
                       ",\"kind\":" + std::to_string(kinds.count(sym.kind) ? kinds.at(sym.kind) : 13) +
                       ",\"location\":" + location(sym) +
                       (sym.container.empty() ? "" : ",\"containerName\":" + quote(sym.container)) + "}";
            }
            reply("[" + out + "]");
        } else if (id && !method.empty())
            send("{\"jsonrpc\":\"2.0\",\"id\":" + idText(id) +
                 ",\"error\":{\"code\":-32601,\"message\":\"method not supported\"}}");
        return true;
    }
};
} // namespace
int languageServer() {
    std::ios::sync_with_stdio(false);
    Server server;
    std::string line;
    while (true) {
        size_t length = 0;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                break;
            if (line.rfind("Content-Length:", 0) == 0)
                length = std::stoul(line.substr(15));
        }
        if (!std::cin || !length)
            return 0;
        std::string body(length, '\0');
        std::cin.read(body.data(), static_cast<std::streamsize>(length));
        try {
            auto message = Reader(body).value();
            if (!server.handle(*message))
                return 0;
        } catch (const std::exception &e) {
            std::cerr << "flywheel lsp: " << e.what() << '\n';
        }
    }
}
} // namespace fw
