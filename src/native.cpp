// Native imports: `import c name`, `import cxx name` (`import++ name`), `import rust name`, and
// `import zig name`. The source file is scanned for functions whose parameter and result types
// map to Flywheel (integers -> int, floating point -> double, bool, strings -> string, void).
// A generated shim includes the source and exposes each function as a C symbol with a uniform
// ABI (int64_t, double, const char *, malloc'd char * results); the compiler calls the shims.
// Functions with other types (pointers, structs, templates, generics) are not exposed.
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace fw {
namespace {
namespace fs = std::filesystem;
std::string readText(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
// Splits source into identifiers, numbers, string/char literals (as "" and ''), and punctuation;
// comments and (for C and C++) preprocessor lines are dropped.
std::vector<std::string> tokenize(const std::string &s, const std::string &language) {
    std::vector<std::string> out;
    bool lineStart = true;
    for (size_t p = 0; p < s.size();) {
        char c = s[p];
        if (c == '\n') {
            lineStart = true;
            ++p;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++p;
            continue;
        }
        if ((language == "c" || language == "cxx") && lineStart && c == '#') {
            while (p < s.size() && s[p] != '\n') {
                if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] == '\n')
                    ++p;
                ++p;
            }
            continue;
        }
        lineStart = false;
        if (s.compare(p, 2, "//") == 0 || (language == "zig" && s.compare(p, 2, "\\\\") == 0)) {
            p = s.find('\n', p);
            if (p == std::string::npos)
                break;
            if (language == "zig" && s.compare(p, 2, "\\\\") != 0)
                out.push_back(""); // keeps multiline strings from merging tokens
            continue;
        }
        if (s.compare(p, 2, "/*") == 0) {
            auto e = s.find("*/", p + 2);
            p = e == std::string::npos ? s.size() : e + 2;
            continue;
        }
        if (c == '"' || (c == '\'' && language != "rust")) {
            size_t q = p + 1;
            while (q < s.size() && s[q] != c && s[q] != '\n') {
                if (s[q] == '\\')
                    ++q;
                ++q;
            }
            // C++ raw strings R"(...)" are rare in function signatures; skipped as plain text.
            out.push_back(std::string(2, c));
            p = q + 1;
            continue;
        }
        if (c == '\'' && language == "rust") {
            // A lifetime ('a) or a char literal ('x', '\n').
            if (p + 2 < s.size() && (s[p + 2] == '\'' || s[p + 1] == '\\')) {
                auto e = s.find('\'', p + 2);
                p = e == std::string::npos ? s.size() : e + 1;
                out.push_back("''");
                continue;
            }
            size_t q = p + 1;
            while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '_'))
                ++q;
            out.push_back(s.substr(p, q - p));
            p = q;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
            static_cast<unsigned char>(c) >= 0x80) {
            size_t q = p;
            while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '_' ||
                                    static_cast<unsigned char>(s[q]) >= 0x80))
                ++q;
            out.push_back(s.substr(p, q - p));
            p = q;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t q = p;
            while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '.'))
                ++q;
            out.push_back(s.substr(p, q - p));
            p = q;
            continue;
        }
        for (const char *multi : {"::", "->", "...", "=>"})
            if (s.compare(p, std::strlen(multi), multi) == 0) {
                out.push_back(multi);
                p += std::strlen(multi);
                goto next;
            }
        out.push_back(std::string(1, c));
        ++p;
    next:;
    }
    return out;
}
std::string join(const std::vector<std::string> &tokens, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to; ++i) {
        bool word = !tokens[i].empty() &&
                    (std::isalnum(static_cast<unsigned char>(tokens[i][0])) || tokens[i][0] == '_');
        bool previousWord = !out.empty() && (std::isalnum(static_cast<unsigned char>(out.back())) ||
                                             out.back() == '_');
        if (word && previousWord)
            out += ' ';
        out += tokens[i];
    }
    return out;
}
// Splits tokens [from, to) at top-level commas.
std::vector<std::pair<size_t, size_t>> commaSplit(const std::vector<std::string> &t, size_t from,
                                                  size_t to) {
    std::vector<std::pair<size_t, size_t>> parts;
    int depth = 0;
    size_t start = from;
    for (size_t i = from; i < to; ++i) {
        if (t[i] == "(" || t[i] == "[" || t[i] == "{" || t[i] == "<")
            ++depth;
        else if (t[i] == ")" || t[i] == "]" || t[i] == "}" || t[i] == ">")
            --depth;
        else if (t[i] == "," && depth == 0) {
            parts.emplace_back(start, i);
            start = i + 1;
        }
    }
    if (start < to)
        parts.emplace_back(start, to);
    return parts;
}
size_t matching(const std::vector<std::string> &t, size_t open) {
    std::string a = t[open], z = a == "(" ? ")" : a == "{" ? "}" : a == "[" ? "]" : ">";
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i] == a)
            ++depth;
        else if (t[i] == z && --depth == 0)
            return i;
    }
    return t.size();
}
// --- C and C++ -----------------------------------------------------------------------------
// The Flywheel type of a C or C++ type ("" when unsupported). `owned` reports a std::string.
std::string cType(std::vector<std::string> words, bool cxx, bool parameter, std::string &kind) {
    std::vector<std::string> kept;
    for (auto &w : words)
        if (w != "const" && w != "volatile" && w != "register" && w != "restrict" &&
            w != "__restrict" && w != "__restrict__" && w != "inline" && w != "static" &&
            w != "extern" && w != "constexpr" && w != "typename" && w != "\"\"" && w != "noexcept")
            kept.push_back(w);
    auto text = join(kept, 0, kept.size());
    if (text.rfind("std::", 0) == 0 && text.find("string") == std::string::npos)
        text = text.substr(5);
    static const std::set<std::string> integers = {"int",
                                                   "long",
                                                   "short",
                                                   "unsigned",
                                                   "signed",
                                                   "long long",
                                                   "unsigned int",
                                                   "signed int",
                                                   "unsigned long",
                                                   "long int",
                                                   "unsigned long long",
                                                   "long long int",
                                                   "unsigned long int",
                                                   "short int",
                                                   "unsigned short",
                                                   "unsigned short int",
                                                   "signed char",
                                                   "unsigned char",
                                                   "signed long",
                                                   "signed short",
                                                   "int8_t",
                                                   "int16_t",
                                                   "int32_t",
                                                   "int64_t",
                                                   "uint8_t",
                                                   "uint16_t",
                                                   "uint32_t",
                                                   "uint64_t",
                                                   "size_t",
                                                   "ssize_t",
                                                   "ptrdiff_t",
                                                   "intptr_t",
                                                   "uintptr_t",
                                                   "intmax_t",
                                                   "uintmax_t",
                                                   "long unsigned",
                                                   "long unsigned int"};
    if (integers.count(text)) {
        kind = "int";
        return "int";
    }
    if (text == "float" || text == "double" || text == "long double") {
        kind = "real";
        return "double";
    }
    if (text == "bool" || text == "_Bool") {
        kind = "bool";
        return "bool";
    }
    if (text == "void") {
        kind = "void";
        return parameter ? "" : "void";
    }
    if (text == "char*") {
        bool constant = std::find(words.begin(), words.end(), "const") != words.end();
        kind = constant ? "cstr" : "mutable-cstr";
        return "string";
    }
    if (cxx && (text == "std::string" || text == "string" || text == "std::string_view" ||
                text == "string_view" || text == "std::string&" || text == "string&")) {
        bool reference = text.back() == '&';
        bool constant = std::find(words.begin(), words.end(), "const") != words.end();
        if (reference && (!constant || !parameter))
            return ""; // a mutable reference cannot bind a temporary
        kind = text.find("view") != std::string::npos ? "view" : "std";
        return "string";
    }
    return "";
}
void scanC(const std::string &source, bool cxx, ForeignModule &m) {
    auto t = tokenize(source, cxx ? "cxx" : "c");
    size_t start = 0; // first token of the current top-level declaration
    std::function<void(size_t, size_t)> scope = [&](size_t from, size_t to) {
        start = from;
        for (size_t i = from; i < to; ++i) {
            if (t[i] == ";" || t[i] == "}") {
                start = i + 1;
                continue;
            }
            if (t[i] == "(") {
                // `[ret] name ( params ) [qualifiers] {` at this level defines a function.
                auto close = matching(t, i);
                if (close >= to)
                    return;
                size_t body = close + 1;
                while (body < to && (t[body] == "const" || t[body] == "noexcept" ||
                                     t[body] == "override" || t[body] == "final"))
                    ++body;
                if (body < to && t[body] == "{" && i > start) {
                    auto name = t[i - 1];
                    bool qualified = i >= 2 && t[i - 2] == "::";
                    std::vector<std::string> head(t.begin() + static_cast<ptrdiff_t>(start),
                                                  t.begin() + static_cast<ptrdiff_t>(i - 1));
                    bool templated = std::find(head.begin(), head.end(), "template") != head.end();
                    bool word = std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_';
                    std::vector<std::string> returnWords;
                    for (size_t k = 0; k < head.size(); ++k) {
                        if (head[k] == "[" && k + 1 < head.size() && head[k + 1] == "[") {
                            while (k < head.size() && head[k] != "]")
                                ++k;
                            ++k; // [[attributes]]
                            continue;
                        }
                        if (head[k] == "__attribute__" || head[k] == "__declspec")
                            break;
                        returnWords.push_back(head[k]);
                    }
                    std::string kind;
                    ForeignFunction f;
                    f.name = name;
                    f.result =
                        word && !qualified && !templated && name != "main" && name != "operator"
                            ? cType(returnWords, cxx, false, kind)
                            : "";
                    f.nativeResult = kind;
                    bool ok = !f.result.empty();
                    auto params = commaSplit(t, i + 1, close);
                    if (params.size() == 1 && params[0].second - params[0].first == 1 &&
                        t[params[0].first] == "void")
                        params.clear();
                    for (auto [a, z] : params) {
                        if (!ok)
                            break;
                        for (size_t k = a; k < z; ++k)
                            if (t[k] == "=") {
                                z = k; // default argument
                                break;
                            }
                        std::vector<std::string> words(t.begin() + static_cast<ptrdiff_t>(a),
                                                       t.begin() + static_cast<ptrdiff_t>(z));
                        std::string paramKind;
                        auto type = cType(words, cxx, true, paramKind);
                        if (type.empty() && words.size() > 1) {
                            words.pop_back(); // the parameter name
                            type = cType(words, cxx, true, paramKind);
                        }
                        if (type.empty())
                            ok = false;
                        f.params.push_back(type);
                        f.nativeParams.push_back(paramKind + "|" + join(words, 0, words.size()));
                    }
                    if (ok)
                        m.functions.push_back(std::move(f));
                    auto end = matching(t, body);
                    i = end;
                    start = end + 1;
                    continue;
                }
                i = close;
                continue;
            }
            if (t[i] == "{") {
                auto end = matching(t, i);
                // extern "C" { ... } blocks are transparent; namespaces, classes, and
                // initializers are skipped.
                bool externBlock = i >= 2 && t[i - 1] == "\"\"" && t[i - 2] == "extern";
                if (externBlock) {
                    auto saved = end;
                    scope(i + 1, std::min(end, to));
                    i = saved;
                } else
                    i = end;
                start = i + 1;
            }
        }
    };
    scope(0, t.size());
}
std::string cShim(const ForeignModule &m, const std::string &path, bool cxx) {
    std::string s = cxx ? "#include <cstdint>\n#include <cstdlib>\n#include <cstring>\n#include "
                          "<exception>\n#include <string>\n#include <string_view>\n"
                        : "#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n"
                          "#include <stdbool.h>\n";
    auto generic = fs::path(path).generic_string();
    std::string escaped;
    for (char c : generic)
        escaped += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
    s += "#include \"" + escaped + "\"\n";
    std::string linkage = cxx ? "extern \"C\" " : "";
    s += linkage + "void fw_foreign_fail(const char *message);\n";
    s += "static char *fw_shim_copy(const char *text, size_t length) {\n"
         "    char *copy = (char *)malloc(length + 1);\n"
         "    if (copy) {\n        memcpy(copy, text, length);\n        copy[length] = 0;\n    }\n"
         "    return copy;\n}\n";
    if (cxx)
        s += "static char *fw_shim_out(std::string_view text) { return fw_shim_copy(text.data(), "
             "text.size()); }\nstatic char *fw_shim_out(const char *text) { return text ? "
             "fw_shim_copy(text, std::strlen(text)) : fw_shim_copy(\"\", 0); }\n";
    else
        s += "static char *fw_shim_out(const char *text) { return text ? fw_shim_copy(text, "
             "strlen(text)) : fw_shim_copy(\"\", 0); }\n";
    auto abi = [](const std::string &type) {
        return type == "int" || type == "bool" ? "int64_t"
               : type == "double"              ? "double"
               : type == "string"              ? "char *"
                                               : "void";
    };
    for (auto &f : m.functions) {
        std::string params, args;
        for (size_t i = 0; i < f.params.size(); ++i) {
            auto a = "a" + std::to_string(i);
            auto &native = f.nativeParams[i];
            auto kind = native.substr(0, native.find('|'));
            auto spelled = native.substr(native.find('|') + 1);
            std::string unconst;
            {
                auto words = tokenize(spelled, cxx ? "cxx" : "c");
                std::vector<std::string> kept;
                for (auto &w : words)
                    if (w != "const" && w != "&")
                        kept.push_back(w);
                unconst = join(kept, 0, kept.size());
            }
            params += std::string(params.empty() ? "" : ", ") +
                      (f.params[i] == "string" ? "const char *" : abi(f.params[i])) + " " + a;
            std::string arg;
            if (kind == "bool")
                arg = a + " != 0";
            else if (kind == "int" || kind == "real")
                arg = "(" + unconst + ")" + a;
            else if (kind == "cstr")
                arg = a;
            else if (kind == "mutable-cstr")
                arg = "(char *)" + a;
            else if (kind == "view")
                arg = "std::string_view(" + a + ")";
            else
                arg = "std::string(" + a + ")";
            args += (args.empty() ? "" : ", ") + arg;
        }
        auto invocation = f.name + "(" + args + ")";
        std::string body;
        if (f.result == "void")
            body = invocation + ";";
        else if (f.result == "bool")
            body = "return (" + invocation + ") ? 1 : 0;";
        else if (f.result == "string")
            body = "return fw_shim_out(" + invocation + ");";
        else
            body = std::string("return (") + abi(f.result) + ")(" + invocation + ");";
        s += linkage + abi(f.result) + " " + f.symbol + "(" +
             (params.empty() && !cxx ? "void" : params) + ") {\n";
        if (cxx) {
            s += "    try {\n        " + body +
                 "\n    } catch (const std::exception &e) {\n"
                 "        fw_foreign_fail(e.what());\n    } catch (...) {\n"
                 "        fw_foreign_fail(\"C++ exception\");\n    }\n";
            if (f.result != "void")
                s += std::string("    return ") + (f.result == "string" ? "nullptr" : "0") + ";\n";
        } else
            s += "    " + body + "\n";
        s += "}\n";
    }
    return s;
}
// --- Rust ----------------------------------------------------------------------------------
std::string rustType(const std::vector<std::string> &words, bool parameter, std::string &kind) {
    std::vector<std::string> kept;
    for (auto &w : words)
        if (!(w.size() > 1 && w[0] == '\'')) // lifetimes
            kept.push_back(w);
    auto text = join(kept, 0, kept.size());
    static const std::set<std::string> integers = {"i8",  "i16", "i32", "i64",  "i128",  "u8",
                                                   "u16", "u32", "u64", "u128", "isize", "usize"};
    if (integers.count(text)) {
        kind = "int";
        return "int";
    }
    if (text == "f32" || text == "f64") {
        kind = "real";
        return "double";
    }
    if (text == "bool") {
        kind = "bool";
        return "bool";
    }
    if (text == "&str" || text == "String" || text == "&String") {
        if (parameter && text == "&String")
            kind = "string-ref";
        else
            kind = text == "String" ? "owned" : "str";
        return "string";
    }
    if (!parameter && (text.empty() || text == "()")) {
        kind = "void";
        return "void";
    }
    return "";
}
void scanRust(const std::string &source, ForeignModule &m) {
    auto t = tokenize(source, "rust");
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == "{") {
            i = matching(t, i); // impl blocks, nested modules, and bodies
            continue;
        }
        if (t[i] != "fn" || i + 2 >= t.size())
            continue;
        // Visibility: `pub` (or `pub(crate)`) before `fn`, possibly with `extern "C"`.
        size_t k = i;
        if (k >= 2 && t[k - 1] == "\"\"" && t[k - 2] == "extern")
            k -= 2;
        bool visible = (k >= 1 && t[k - 1] == "pub") ||
                       (k >= 4 && t[k - 1] == ")" && t[k - 3] == "(" && t[k - 4] == "pub");
        bool unsafeFn = k >= 1 && t[k - 1] == "unsafe";
        auto name = t[i + 1];
        size_t open = i + 2;
        if (!visible || unsafeFn || t[open] != "(") {
            continue; // generic (`fn f<T>`) or private functions are not exposed
        }
        auto close = matching(t, open);
        size_t body = close + 1;
        std::vector<std::string> result;
        if (body < t.size() && t[body] == "->")
            for (++body; body < t.size() && t[body] != "{" && t[body] != "where"; ++body)
                result.push_back(t[body]);
        if (body >= t.size() || t[body] != "{")
            continue;
        ForeignFunction f;
        f.name = name;
        std::string kind;
        f.result = name == "main" ? "" : rustType(result, false, kind);
        f.nativeResult = kind;
        bool ok = !f.result.empty();
        for (auto [a, z] : commaSplit(t, open + 1, close)) {
            size_t colon = a;
            while (colon < z && t[colon] != ":")
                ++colon;
            if (colon == z) {
                ok = false; // self
                break;
            }
            std::vector<std::string> words(t.begin() + static_cast<ptrdiff_t>(colon + 1),
                                           t.begin() + static_cast<ptrdiff_t>(z));
            std::string paramKind;
            auto type = rustType(words, true, paramKind);
            if (type.empty()) {
                ok = false;
                break;
            }
            f.params.push_back(type);
            f.nativeParams.push_back(paramKind + "|" + join(words, 0, words.size()));
        }
        if (ok)
            m.functions.push_back(std::move(f));
        i = matching(t, body);
    }
}
std::string rustShim(const ForeignModule &m, const std::string &path) {
    auto module = "fw_module_" + m.key;
    std::string s;
    std::string escaped;
    for (char c : fs::path(path).generic_string())
        escaped += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
    s += "#[path = \"" + escaped + "\"]\nmod " + module + ";\n";
    for (auto &f : m.functions) {
        std::string params, args;
        for (size_t i = 0; i < f.params.size(); ++i) {
            auto a = "a" + std::to_string(i);
            auto kind = f.nativeParams[i].substr(0, f.nativeParams[i].find('|'));
            auto spelled = f.nativeParams[i].substr(f.nativeParams[i].find('|') + 1);
            auto abi = f.params[i] == "string"   ? "*const c_char"
                       : f.params[i] == "double" ? "f64"
                                                 : "i64";
            params += (params.empty() ? "" : ", ") + a + ": " + abi;
            std::string arg;
            if (kind == "bool")
                arg = a + " != 0";
            else if (kind == "int" || kind == "real")
                arg = a + " as " + spelled;
            else if (kind == "str")
                arg = "fw_in(" + a + ")";
            else if (kind == "owned")
                arg = "fw_in(" + a + ").to_string()";
            else
                arg = "&fw_in(" + a + ").to_string()";
            args += (args.empty() ? "" : ", ") + arg;
        }
        auto abiResult = f.result == "string"   ? " -> *mut c_char"
                         : f.result == "double" ? " -> f64"
                         : f.result == "void"   ? ""
                                                : " -> i64";
        auto convert = f.result == "void"   ? std::string("")
                       : f.result == "bool" ? std::string("if r { 1 } else { 0 }")
                       : f.result == "string"
                           ? std::string(f.nativeResult == "owned" ? "fw_out(&r)" : "fw_out(r)")
                       : f.result == "double" ? std::string("r as f64")
                                              : std::string("r as i64");
        auto failed = f.result == "void"     ? "()"
                      : f.result == "string" ? "std::ptr::null_mut()"
                      : f.result == "double" ? "0.0"
                                             : "0";
        s += "#[no_mangle]\npub extern \"C\" fn " + f.symbol + "(" + params + ")" + abiResult +
             " {\n    fw_quiet();\n    match "
             "std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| " +
             module + "::" + f.name + "(" + args + "))) {\n        Ok(r) => { " +
             (f.result == "void" ? "let _ = r;" : convert) +
             " }\n        Err(e) => { fw_panic(e); " + failed + " }\n    }\n}\n";
    }
    return s;
}
const char *rustPrelude =
    "#![allow(dead_code, unused_imports, unused_variables, unused_mut, non_snake_case)]\n"
    "use std::os::raw::c_char;\n"
    "extern \"C\" {\n    fn fw_foreign_fail(message: *const c_char);\n"
    "    fn malloc(size: usize) -> *mut u8;\n}\n"
    "fn fw_in(p: *const c_char) -> &'static str {\n"
    "    if p.is_null() { return \"\"; }\n"
    "    unsafe { std::ffi::CStr::from_ptr(p) }.to_str().unwrap_or(\"\")\n}\n"
    "fn fw_out(s: &str) -> *mut c_char {\n"
    "    unsafe {\n        let p = malloc(s.len() + 1);\n        if p.is_null() { return p as *mut "
    "c_char; }\n"
    "        std::ptr::copy_nonoverlapping(s.as_ptr(), p, s.len());\n"
    "        *p.add(s.len()) = 0;\n        p as *mut c_char\n    }\n}\n"
    "// Panics become Flywheel NativeErrors, so the default hook's message is not printed.\n"
    "fn fw_quiet() {\n    static ONCE: std::sync::Once = std::sync::Once::new();\n"
    "    ONCE.call_once(|| std::panic::set_hook(Box::new(|_| {})));\n}\n"
    "fn fw_panic(e: Box<dyn std::any::Any + Send>) {\n"
    "    let m = if let Some(s) = e.downcast_ref::<&str>() { s.to_string() }\n"
    "        else if let Some(s) = e.downcast_ref::<String>() { s.clone() }\n"
    "        else { \"Rust panic\".to_string() };\n"
    "    let c = std::ffi::CString::new(m.replace('\\0', \" \")).unwrap();\n"
    "    unsafe { fw_foreign_fail(c.as_ptr()) }\n}\n";
// --- Zig -----------------------------------------------------------------------------------
std::string zigType(std::vector<std::string> words, bool parameter, std::string &kind) {
    bool errorUnion = !parameter && !words.empty() && words[0] == "!";
    if (errorUnion)
        words.erase(words.begin());
    auto text = join(words, 0, words.size());
    std::string type;
    static const std::set<std::string> integers = {
        "i8",     "i16",     "i32",     "i64",      "u8",         "u16",
        "u32",    "u64",     "isize",   "usize",    "c_int",      "c_uint",
        "c_long", "c_ulong", "c_short", "c_ushort", "c_longlong", "c_ulonglong"};
    if (integers.count(text))
        kind = "int", type = "int";
    else if (text == "f16" || text == "f32" || text == "f64" || text == "c_longdouble")
        kind = "real", type = "double";
    else if (text == "bool")
        kind = "bool", type = "bool";
    else if (text == "[]const u8" || text == "[:0]const u8")
        kind = "slice", type = "string";
    else if (text == "[*:0]const u8")
        kind = "cstr", type = "string";
    else if (text == "void" && !parameter)
        kind = "void", type = "void";
    if (errorUnion && !type.empty())
        kind = "!" + kind;
    return type;
}
void scanZig(const std::string &source, ForeignModule &m) {
    auto t = tokenize(source, "zig");
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == "{") {
            i = matching(t, i);
            continue;
        }
        if (t[i] != "fn" || i + 2 >= t.size() || i == 0 ||
            (t[i - 1] != "pub" && t[i - 1] != "export"))
            continue;
        auto name = t[i + 1];
        if (t[i + 2] != "(")
            continue;
        auto close = matching(t, i + 2);
        size_t body = close + 1;
        std::vector<std::string> result;
        for (; body < t.size() && t[body] != "{"; ++body)
            result.push_back(t[body]);
        if (body >= t.size())
            continue;
        ForeignFunction f;
        f.name = name;
        std::string kind;
        f.result = name == "main" ? "" : zigType(result, false, kind);
        f.nativeResult = kind;
        bool ok = !f.result.empty();
        for (auto [a, z] : commaSplit(t, i + 3, close)) {
            size_t colon = a;
            while (colon < z && t[colon] != ":")
                ++colon;
            if (colon == z || t[a] == "comptime") {
                ok = false;
                break;
            }
            std::vector<std::string> words(t.begin() + static_cast<ptrdiff_t>(colon + 1),
                                           t.begin() + static_cast<ptrdiff_t>(z));
            std::string paramKind;
            auto type = zigType(words, true, paramKind);
            if (type.empty()) {
                ok = false;
                break;
            }
            f.params.push_back(type);
            f.nativeParams.push_back(paramKind + "|" + join(words, 0, words.size()));
        }
        if (ok)
            m.functions.push_back(std::move(f));
        i = matching(t, body);
    }
}
std::string zigShim(const ForeignModule &m) {
    std::string s = "const std = @import(\"std\");\nconst user = @import(\"user\");\n"
                    "extern fn fw_foreign_fail(message: [*:0]const u8) void;\n"
                    "extern fn malloc(size: usize) ?[*]u8;\n"
                    "fn fw_out(text: []const u8) ?[*]u8 {\n"
                    "    const p = malloc(text.len + 1) orelse return null;\n"
                    "    @memcpy(p[0..text.len], text);\n    p[text.len] = 0;\n    return p;\n}\n";
    for (auto &f : m.functions) {
        std::string params, args;
        for (size_t i = 0; i < f.params.size(); ++i) {
            auto a = "a" + std::to_string(i);
            auto kind = f.nativeParams[i].substr(0, f.nativeParams[i].find('|'));
            auto abi = f.params[i] == "string"   ? "[*:0]const u8"
                       : f.params[i] == "double" ? "f64"
                                                 : "i64";
            params += (params.empty() ? "" : ", ") + a + ": " + abi;
            std::string arg = kind == "bool"    ? a + " != 0"
                              : kind == "int"   ? "@intCast(" + a + ")"
                              : kind == "real"  ? "@floatCast(" + a + ")"
                              : kind == "slice" ? "std.mem.span(" + a + ")"
                                                : a;
            args += (args.empty() ? "" : ", ") + arg;
        }
        auto kind = f.nativeResult;
        bool errorUnion = !kind.empty() && kind[0] == '!';
        if (errorUnion)
            kind = kind.substr(1);
        auto abiResult = f.result == "string"   ? "?[*]u8"
                         : f.result == "double" ? "f64"
                         : f.result == "void"   ? "void"
                                                : "i64";
        auto failed = f.result == "void"     ? "return;"
                      : f.result == "string" ? "return null;"
                                             : "return 0;";
        std::string call = "user." + f.name + "(" + args + ")";
        if (errorUnion)
            call += " catch |err| {\n        fw_foreign_fail(@errorName(err));\n        " +
                    std::string(failed) + "\n    }";
        std::string body;
        if (f.result == "void")
            body = "    " + call + ";\n";
        else {
            body = "    const r = " + call + ";\n";
            if (kind == "bool")
                body += "    return @as(i64, @intFromBool(r));\n";
            else if (kind == "int")
                body += "    return @intCast(r);\n";
            else if (kind == "real")
                body += "    return @floatCast(r);\n";
            else if (kind == "cstr")
                body += "    return fw_out(std.mem.span(r));\n";
            else
                body += "    return fw_out(r);\n";
        }
        s += "export fn " + f.symbol + "(" + params + ") " + abiResult + " {\n" + body + "}\n";
    }
    return s;
}
// --- Building --------------------------------------------------------------------------------
int runTool(const std::string &program, const std::vector<std::string> &args, std::string &output) {
    llvm::SmallString<128> log;
    if (llvm::sys::fs::createTemporaryFile("flywheel-native", "log", log))
        throw std::runtime_error("cannot create a temporary file");
    std::vector<llvm::StringRef> refs{program};
    for (auto &a : args)
        refs.emplace_back(a);
    std::optional<llvm::StringRef> redirects[] = {std::nullopt, llvm::StringRef(log),
                                                  llvm::StringRef(log)};
    std::string error;
    int status = llvm::sys::ExecuteAndWait(program, refs, std::nullopt, redirects, 0, 0, &error);
    output = error.empty() ? readText(std::string(log)) : error;
    llvm::sys::fs::remove(log);
    return status;
}
std::string requireTool(const std::vector<std::string> &names, const std::string &language) {
    for (auto &name : names)
        if (auto found = llvm::sys::findProgramByName(name))
            return *found;
    throw std::runtime_error("importing " + language + " needs `" + names[0] + "` on PATH");
}
void compileTool(const std::string &program, const std::vector<std::string> &args,
                 const std::string &language, const std::string &source) {
    std::string output;
    if (runTool(program, args, output) != 0)
        throw std::runtime_error("cannot compile " + language + " source " + source + ":\n" +
                                 output);
}
} // namespace
ForeignModule scanForeign(const std::string &language, const std::string &path) {
    ForeignModule m;
    m.language = language;
    m.source = path;
    auto text = readText(path);
    m.key = contentHash(path).substr(0, 10);
    if (language == "c" || language == "cxx")
        scanC(text, language == "cxx", m);
    else if (language == "rust")
        scanRust(text, m);
    else
        scanZig(text, m);
    static const std::set<std::string> reserved = {
        "import", "as",    "var",      "const",    "class",     "function", "component",  "returns",
        "return", "if",    "then",     "else",     "while",     "do",       "for",        "in",
        "end",    "break", "continue", "try",      "catch",     "finally",  "throw",      "inst",
        "attr",   "true",  "false",    "null",     "public",    "private",  "void",       "weak",
        "and",    "or",    "not",      "enum",     "interface", "extends",  "implements", "match",
        "case",   "super", "is",       "elements", "defer"};
    std::set<std::string> seen;
    std::vector<ForeignFunction> kept;
    for (auto &f : m.functions)
        if (!reserved.count(f.name) && seen.insert(f.name).second) {
            f.symbol = "fw_" + language + "_" + m.key + "_" + f.name;
            kept.push_back(std::move(f));
        }
    m.functions = std::move(kept);
    if (m.functions.empty())
        throw std::runtime_error(
            "no importable functions in " + path +
            " (functions need int, floating-point, bool, string, or void parameters and results" +
            (language == "rust"  ? ", and `pub`"
             : language == "zig" ? ", and `pub`"
                                 : "") +
            ")");
    if (language == "c" || language == "cxx")
        m.shim = cShim(m, path, language == "cxx");
    else if (language == "rust")
        m.shim = rustShim(m, path);
    else
        m.shim = zigShim(m);
    m.key = contentHash(text + m.shim).substr(0, 16);
    return m;
}
ForeignBuild buildForeign(const std::vector<ForeignModule> &modules, bool shared) {
    ForeignBuild build;
    if (modules.empty())
        return build;
    auto root = fs::path(cacheDirectory("native"));
#ifdef _WIN32
    std::string sharedSuffix = ".dll", staticSuffix = ".lib", objectSuffix = ".obj";
#elif defined(__APPLE__)
    std::string sharedSuffix = ".dylib", staticSuffix = ".a", objectSuffix = ".o";
#else
    std::string sharedSuffix = ".so", staticSuffix = ".a", objectSuffix = ".o";
#endif
    // Rust modules share one crate so the standard library is linked once.
    std::string rustCrate = rustPrelude, rustKey;
    std::vector<const ForeignModule *> rust;
    for (auto &m : modules) {
        if (m.language == "rust") {
            rust.push_back(&m);
            rustCrate += m.shim;
            continue;
        }
        auto directory = root / (m.language + "-" + m.key);
        fs::create_directories(directory);
        auto output =
            directory / ((shared ? "lib" : "shim") + (shared                ? sharedSuffix
                                                      : m.language == "zig" ? staticSuffix
                                                                            : objectSuffix));
        build.inputs.push_back(output.string());
        if (m.language == "zig")
            build.flags.push_back("-lc");
        if (fs::exists(output))
            continue;
        auto partial = output.string() + ".partial";
        if (m.language == "zig") {
            auto shim = directory / "shim.zig";
            std::ofstream(shim, std::ios::binary) << m.shim;
            auto zig = requireTool({"zig"}, "Zig");
            std::vector<std::string> args = {"build-lib"};
            if (shared)
                args.push_back("-dynamic");
            args.insert(args.end(),
                        {"-O", "ReleaseSafe", "-lc", "-fPIC", "--dep", "user",
                         "-Mroot=" + shim.string(), "-Muser=" + m.source, "-femit-bin=" + partial});
            compileTool(zig, args, "Zig", m.source);
        } else {
            bool cxx = m.language == "cxx";
            auto shim = directory / (cxx ? "shim.cpp" : "shim.c");
            std::ofstream(shim, std::ios::binary) << m.shim;
            std::vector<std::string> args;
            auto rename = "main=fw_foreign_main_" + m.key;
#ifdef _MSC_VER
            args = {"/nologo",     "/O2",
                    "/MD",         "/EHsc",
                    "/c",          cxx ? "/TP" : "/TC",
                    // MSVC defaults to C++14 and C89-era C; the shim needs std::string_view.
                    cxx ? "/std:c++17" : "/std:c11",
                    "/D" + rename, "/I" + fs::path(m.source).parent_path().string(),
                    shim.string(), "/Fo:" + partial};
#else
            args = {"-O2", "-fPIC", "-D" + rename,
                    "-I" + fs::path(m.source).parent_path().string()};
            if (!cxx)
                args.insert(args.begin(), {"-x", "c"});
            args.push_back(shim.string());
            if (shared) {
#ifdef __APPLE__
                // fw_* runtime symbols resolve against the host process at load time.
                args.insert(args.end(), {"-undefined", "dynamic_lookup"});
#endif
                args.insert(args.end(), {"-shared", "-o", partial});
            }
            else
                args.insert(args.end(), {"-c", "-o", partial});
#endif
            compileTool(FW_CXX, args, cxx ? "C++" : "C", m.source);
        }
        fs::rename(partial, output);
    }
    if (!rust.empty()) {
        rustKey = contentHash(rustCrate).substr(0, 16);
        auto directory = root / ("rust-" + rustKey);
        fs::create_directories(directory);
#ifdef _WIN32
        auto library = directory / (shared ? "shim.dll" : "shim.lib");
#elif defined(__APPLE__)
        auto library = directory / (shared ? "libshim.dylib" : "libshim.a");
#else
        auto library = directory / (shared ? "libshim.so" : "libshim.a");
#endif
        auto flagsFile = directory / "link-flags.txt";
        build.inputs.push_back(library.string());
        if (!fs::exists(library)) {
            auto shim = directory / "shim.rs";
            std::ofstream(shim, std::ios::binary) << rustCrate;
            auto rustc = requireTool({"rustc"}, "Rust");
            auto partial = library.string() + ".partial";
            std::vector<std::string> args = {
                "--edition",    "2021",         "--crate-name",
                "fw_shim",      "--crate-type", shared ? "cdylib" : "staticlib",
                "-C",           "opt-level=2",  "-C",
                "panic=unwind", "--print",      "native-static-libs",
                "-o",           partial,        shim.string()};
            std::string output;
            if (runTool(rustc, args, output) != 0) {
                std::string sources;
                for (auto *m : rust)
                    sources += (sources.empty() ? "" : ", ") + m->source;
                throw std::runtime_error("cannot compile Rust source " + sources + ":\n" + output);
            }
            // "note: native-static-libs: -lgcc_s -lutil ..." lists what a staticlib needs.
            std::string flags;
            auto at = output.find("native-static-libs:");
            if (at != std::string::npos) {
                auto end = output.find('\n', at);
                flags = output.substr(at + 19,
                                      end == std::string::npos ? std::string::npos : end - at - 19);
            }
            std::ofstream(flagsFile) << flags;
            fs::rename(partial, library);
        }
        if (!shared) {
            std::ifstream in(flagsFile);
            std::string flag;
            while (in >> flag)
#ifdef __APPLE__
                // The C++ driver links libSystem already; ld warns about a second copy.
                if (flag != "-lSystem" &&
                    std::find(build.flags.begin(), build.flags.end(), flag) == build.flags.end())
#else
                if (std::find(build.flags.begin(), build.flags.end(), flag) == build.flags.end())
#endif
                    build.flags.push_back(flag);
        }
    }
    return build;
}
} // namespace fw
