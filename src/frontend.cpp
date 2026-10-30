#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

namespace fw {
namespace {
bool start(unsigned char c) {
    return std::isalpha(c) || c == '_';
}
bool part(unsigned char c) {
    return std::isalnum(c) || c == '_';
}
// Decodes the UTF-8 scalar at p (source is validated before tokenization).
unsigned scalarAt(const std::string &s, size_t p, size_t &length) {
    auto c = static_cast<unsigned char>(s[p]);
    length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    unsigned value = length == 1 ? c : c & (0x3f >> (length - 1));
    for (size_t i = 1; i < length && p + i < s.size(); ++i)
        value = (value << 6) | (static_cast<unsigned char>(s[p + i]) & 63);
    return value;
}
// Identifiers may use non-ASCII letters. Punctuation, symbol, and emoji blocks are rejected;
// spellings are compared by exact UTF-8 bytes (no normalization).
bool letterScalar(unsigned v) {
    if (v < 0x80)
        return std::isalpha(static_cast<int>(v)) || v == '_';
    return !(v <= 0xbf || v == 0xd7 || v == 0xf7 || (v >= 0x2000 && v <= 0x2bff) ||
             (v >= 0x3000 && v <= 0x303f) || (v >= 0xfe30 && v <= 0xfe4f) ||
             (v >= 0xff00 && v <= 0xff20) || (v >= 0x1f000 && v <= 0x1faff));
}
size_t identifierEnd(const std::string &s, size_t p) {
    while (p < s.size()) {
        size_t length;
        auto v = scalarAt(s, p, length);
        if (!(letterScalar(v) || (v < 0x80 && std::isdigit(static_cast<int>(v)))))
            break;
        p += length;
    }
    return p;
}
// A tag header also starts an argument after a callable, where '<' could otherwise
// be a comparison. Inspect only the header prefix, never search ahead for a closing
// tag: a later markup statement must not change how 'a <b' is tokenized.
bool markupArgumentStart(const std::string &source, size_t p) {
    ++p; // '<'
    if (p == source.size())
        return false;
    if (source[p] == '>')
        return true; // fragment
    if (!start(source[p]))
        return false;
    auto name = [&] {
        while (p < source.size() &&
               (part(source[p]) || source[p] == '-' || source[p] == ':' || source[p] == '.'))
            ++p;
    };
    auto space = [&] {
        while (p < source.size() && std::isspace(static_cast<unsigned char>(source[p])))
            ++p;
    };
    name();
    space();
    if (p == source.size())
        return false;
    if (source[p] == '>' || source.compare(p, 2, "/>") == 0)
        return true;
    if (!start(source[p]))
        return false;
    name();
    space();
    return p < source.size() && source[p] == '=';
}
size_t quotedEnd(const std::string &s, size_t p);
size_t braceEnd(const std::string &s, size_t p) {
    int depth = 1;
    for (++p; p < s.size(); ++p) {
        if (s[p] == '"')
            p = quotedEnd(s, p);
        else if (s[p] == '{')
            ++depth;
        else if (s[p] == '}' && --depth == 0)
            return p;
    }
    throw Error({}, "unterminated interpolation");
}
size_t quotedEnd(const std::string &s, size_t p) {
    for (++p; p < s.size(); ++p) {
        if (s[p] == '\\') {
            ++p;
            continue;
        }
        if (s[p] == '{' && p + 1 < s.size() && s[p + 1] == '{') {
            ++p;
            continue;
        }
        if (s[p] == '{') {
            p = braceEnd(s, p);
            continue;
        }
        if (s[p] == '"')
            return p;
    }
    throw Error({}, "unterminated string");
}
size_t markupEnd(const std::string &s, size_t p) {
    int depth = 0;
    for (; p < s.size(); ++p) {
        if (s[p] == '{' && p + 1 < s.size() && s[p + 1] == '{') {
            ++p;
            continue;
        }
        if (s[p] == '{') {
            p = braceEnd(s, p);
            continue;
        }
        if (s[p] != '<')
            continue;
        bool closing = p + 1 < s.size() && s[p + 1] == '/';
        size_t q = p + 1;
        for (; q < s.size() && s[q] != '>'; ++q) {
            if (s[q] == '"')
                q = quotedEnd(s, q);
            else if (s[q] == '{')
                q = braceEnd(s, q);
        }
        if (q == s.size())
            break;
        if (closing)
            --depth;
        else if (q == 0 || s[q - 1] != '/')
            ++depth;
        if (depth == 0)
            return q;
        p = q;
    }
    throw Error({}, "unterminated markup literal");
}
EP node(std::string kind, std::string text, Location l) {
    auto e = std::make_unique<Expr>();
    e->kind = std::move(kind);
    e->text = std::move(text);
    e->loc = l;
    return e;
}
int precedence(const std::string &s) {
    if (s == "or")
        return 1;
    if (s == "and")
        return 2;
    if (s == "==" || s == "!=")
        return 3;
    if (s == "<" || s == ">" || s == "<=" || s == ">=" || s == "in" || s == "is")
        return 4;
    if (s == "as")
        return 9;
    if (s == "??")
        return 5;
    if (s == ".." || s == "..<")
        return 6;
    if (s == "+" || s == "-")
        return 7;
    if (s == "*" || s == "/" || s == "//" || s == "%")
        return 8;
    return 0;
}
bool comparison(const std::string &s) {
    return s == "==" || s == "!=" || s == "<" || s == ">" || s == "<=" || s == ">=";
}
void utf8(std::string &out, unsigned value, Location l) {
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        throw Error(l, "invalid Unicode scalar");
    if (value < 0x80)
        out += char(value);
    else if (value < 0x800) {
        out += char(0xc0 | (value >> 6));
        out += char(0x80 | (value & 63));
    } else if (value < 0x10000) {
        out += char(0xe0 | (value >> 12));
        out += char(0x80 | ((value >> 6) & 63));
        out += char(0x80 | (value & 63));
    } else {
        out += char(0xf0 | (value >> 18));
        out += char(0x80 | ((value >> 12) & 63));
        out += char(0x80 | ((value >> 6) & 63));
        out += char(0x80 | (value & 63));
    }
}
std::string entities(const std::string &text, Location l) {
    std::string out;
    for (size_t p = 0; p < text.size(); ++p) {
        if (text[p] != '&') {
            out += text[p];
            continue;
        }
        auto end = text.find(';', p);
        if (end == std::string::npos)
            throw Error(l, "markup '&' must begin an entity (use &amp;)");
        auto name = text.substr(p + 1, end - p - 1);
        if (name == "amp")
            out += '&';
        else if (name == "lt")
            out += '<';
        else if (name == "gt")
            out += '>';
        else if (name == "quot")
            out += '"';
        else if (name == "apos")
            out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            try {
                size_t used = 0;
                bool hex = name.size() > 2 && name[1] == 'x';
                auto digits = name.substr(hex ? 2 : 1);
                auto v = std::stoul(digits, &used, hex ? 16 : 10);
                if (used != digits.size())
                    throw std::runtime_error("entity");
                utf8(out, unsigned(v), l);
            } catch (const Error &) {
                throw;
            } catch (...) {
                throw Error(l, "invalid numeric entity");
            }
        } else
            throw Error(l, "unknown markup entity '&" + name + ";'");
        p = end;
    }
    return out;
}
} // namespace
std::vector<Token> lex(const std::string &source) {
    // Validate UTF-8 before tokenization; identifier spelling is ASCII in this prototype.
    for (size_t i = 0; i < source.size();) {
        auto c = static_cast<unsigned char>(source[i++]);
        if (c < 0x80)
            continue;
        unsigned n = c >= 0xc2 && c <= 0xdf   ? 1
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xf0 && c <= 0xf4 ? 3
                                              : 99;
        if (n == 99 || i + n > source.size())
            throw Error({}, "source is not valid UTF-8");
        unsigned scalar = c & ((1u << (6 - n)) - 1);
        unsigned count = n;
        while (n--) {
            auto d = static_cast<unsigned char>(source[i++]);
            if ((d & 0xc0) != 0x80)
                throw Error({}, "source is not valid UTF-8");
            scalar = (scalar << 6) | (d & 63);
        }
        if ((count == 2 && scalar < 0x800) || (count == 3 && scalar < 0x10000) ||
            scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            throw Error({}, "source is not valid UTF-8");
    }
    std::vector<Token> out;
    Location loc;
    size_t p = 0;
    int group = 0;
    bool expected = true;
    auto advance = [&](size_t end) {
        while (p < end) {
            if (source[p++] == '\n') {
                ++loc.line;
                loc.column = 1;
            } else
                ++loc.column;
        }
    };
    auto emit = [&](std::string kind, std::string text, size_t end) {
        bool spaced = p == 0 || std::isspace(static_cast<unsigned char>(source[p - 1]));
        out.push_back({std::move(text), std::move(kind), loc, group, spaced});
        advance(end);
    };
    while (p < source.size()) {
        char c = source[p];
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(p + 1);
            continue;
        }
        if (c == '#') {
            auto e = source.find('\n', p);
            advance(e == std::string::npos ? source.size() : e);
            continue;
        }
        if (c == '\\') {
            // Explicit continuation: a backslash ending the line joins it with the next one.
            auto e = source.find_first_not_of(" \t\r", p + 1);
            if (e != std::string::npos && source[e] == '#')
                e = source.find('\n', e);
            if (e == std::string::npos || source[e] != '\n')
                throw Error(loc, "'\\' continues a line only at the end of the line");
            advance(e + 1);
            continue;
        }
        if (c == '\n' || c == ';') {
            // A line ending in a binary operator, comma, or '=' continues on the next line.
            if (c == '\n' && !group && !out.empty()) {
                auto &last = out.back();
                static const std::set<std::string> joins = {
                    "+",  "-",  "*",  "/",  "//", "%",  "==", "!=", "<=", ">=", "=",  "+=",
                    "-=", "*=", "/=", "//=", "%=", "..", "..<", ",",  "=>", "??"};
                bool wildcard = last.text == "*" && out.size() > 1 &&
                                (out[out.size() - 2].text == "." || out[out.size() - 2].text == ":");
                if ((last.kind == "op" && joins.count(last.text) && !wildcard) ||
                    (last.kind == "id" && (last.text == "and" || last.text == "or"))) {
                    advance(p + 1);
                    continue;
                }
            }
            // Grouped newlines are kept with their depth; the parser ignores them except
            // inside a lambda body that starts at that depth.
            if (!group)
                expected = true;
            emit("nl", "\n", p + 1);
            continue;
        }
        if (c == '\'') {
            // Character literal: one Unicode scalar or one string escape.
            size_t e = p + 1;
            if (e < source.size() && source[e] == '\\') {
                if (e + 1 < source.size() && source[e + 1] == 'u') {
                    auto close = source.find('}', e);
                    e = close == std::string::npos ? source.size() : close + 1;
                } else
                    e += 2;
            } else if (e < source.size() && source[e] != '\'' && source[e] != '\n') {
                size_t length;
                scalarAt(source, e, length);
                e += length;
            }
            if (e >= source.size() || source[e] != '\'' || e == p + 1)
                throw Error(loc, "character literals hold exactly one character, such as 'a'");
            emit("char", source.substr(p + 1, e - p - 1), e + 1);
            expected = false;
            continue;
        }
        if (c == '"') {
            auto end = quotedEnd(source, p);
            emit("string", source.substr(p + 1, end - p - 1), end + 1);
            expected = false;
            continue;
        }
        // '<' directly after an identifier (no space) opens type arguments, never markup.
        bool adjacent = p > 0 && !out.empty() && out.back().kind == "id" &&
                        (part(static_cast<unsigned char>(source[p - 1])) ||
                         static_cast<unsigned char>(source[p - 1]) >= 0x80);
        bool genericType = adjacent || (!out.empty() && out.back().kind == "id" &&
                                        (out.back().text == "array" || out.back().text == "tuple" ||
                                         out.back().text == "map" || out.back().text == "set" ||
                                         out.back().text == "Result" || out.back().text == "weak"));
        if (c == '<' && !genericType && (expected || markupArgumentStart(source, p)) &&
            p + 1 < source.size() && (start(source[p + 1]) || source[p + 1] == '>')) {
            auto end = markupEnd(source, p);
            emit("markup", source.substr(p, end - p + 1), end + 1);
            expected = false;
            continue;
        }
        size_t scalarLength;
        if (letterScalar(scalarAt(source, p, scalarLength))) {
            size_t e = identifierEnd(source, p);
            auto s = source.substr(p, e - p);
            emit("id", s, e);
            expected = s == "return" || s == "then" || s == "do" || s == "and" || s == "or" ||
                       s == "not" || s == "in";
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t e = p;
            while (e < source.size() && std::isdigit(static_cast<unsigned char>(source[e])))
                ++e;
            bool real = false;
            if (e + 1 < source.size() && source[e] == '.' &&
                std::isdigit(static_cast<unsigned char>(source[e + 1]))) {
                real = true;
                ++e;
                while (e < source.size() && std::isdigit(static_cast<unsigned char>(source[e])))
                    ++e;
            }
            if (e < source.size() && (source[e] == 'e' || source[e] == 'E')) {
                real = true;
                ++e;
                if (e < source.size() && (source[e] == '+' || source[e] == '-'))
                    ++e;
                auto begin = e;
                while (e < source.size() && std::isdigit(static_cast<unsigned char>(source[e])))
                    ++e;
                if (e == begin)
                    throw Error(loc, "expected exponent digits");
            }
            emit(real ? "real" : "int", source.substr(p, e - p), e);
            expected = false;
            continue;
        }
        std::string op(1, c);
        if (source.compare(p, 3, "//=") == 0)
            op = "//=";
        else if (p + 1 < source.size()) {
            auto two = source.substr(p, 2);
            if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "+=" ||
                two == "-=" || two == "*=" || two == "/=" || two == "%=" || two == "//" ||
                two == ".." || two == "=>" || two == "??")
                op = two;
        }
        if (op == ".." && p + 2 < source.size() && source[p + 2] == '<')
            op = "..<";
        if (std::string("+-*/%=!<>()[]{}.,?:").find(c) == std::string::npos)
            throw Error(loc, "invalid character");
        if (c == '(' || c == '[' || c == '{')
            ++group;
        if (c == ')' || c == ']' || c == '}') {
            if (--group < 0)
                throw Error(loc, "unmatched closing delimiter");
        }
        emit("op", op, p + op.size());
        expected = c != ')' && c != ']' && c != '}';
    }
    if (group)
        throw Error(loc, "unclosed delimiter");
    out.push_back({"", "eof", loc});
    return out;
}
size_t Parser::index(size_t n) {
    int active = bodyDepths.empty() ? 0 : bodyDepths.back();
    size_t i = pos;
    while (true) {
        while (i + 1 < tokens.size() && tokens[i].kind == "nl" && tokens[i].depth != active)
            ++i;
        if (n == 0 || i + 1 >= tokens.size())
            return i;
        --n;
        ++i;
    }
}
Token &Parser::peek(size_t n) {
    return tokens[index(n)];
}
Token Parser::take() {
    auto i = index(0);
    auto t = tokens[i];
    pos = t.kind == "eof" ? i : i + 1;
    return t;
}
bool Parser::accept(const std::string &s) {
    if (peek().text == s && peek().kind != "string" && peek().kind != "markup") {
        take();
        return true;
    }
    return false;
}
Token Parser::expect(const std::string &s) {
    if (peek().text != s || peek().kind == "string" || peek().kind == "markup")
        throw Error(peek().loc, "expected '" + s + "', got '" + peek().text + "'");
    return take();
}
Token Parser::identifier() {
    auto t = take();
    static const std::set<std::string> reserved = {
        "import",  "as",      "var",  "const", "class",    "function", "component",
        "returns", "return",  "if",   "then",  "else",     "while",    "do",
        "for",     "in",      "end",  "break", "continue", "try",      "catch",
        "finally", "throw",   "inst", "attr",  "true",     "false",    "null",
        "public",  "private", "void", "weak",  "and",      "or",       "not",
        "enum",    "interface", "extends", "implements", "match", "case", "super", "is",
        "elements"};
    if (t.kind != "id" || reserved.count(t.text))
        throw Error(t.loc, "expected identifier");
    return t;
}
void Parser::newlines() {
    while (peek().kind == "nl")
        take();
}
void Parser::finish() {
    if (peek().kind != "nl" && peek().kind != "eof")
        throw Error(peek().loc, "expected end of statement, got '" + peek().text + "'");
    newlines();
}
std::string Parser::type() {
    std::string r;
    if (accept("[")) {
        r = "[" + type();
        while (accept(","))
            r += "," + type();
        expect("]");
        r += "]";
    } else if (accept("(")) {
        // Parentheses group a type, so suffixes can apply to a function type:
        // (function() returns int)[] is an array of functions.
        r = type();
        expect(")");
    } else if (peek().text == "function" && peek(1).text == "(") {
        // function(A, B) returns R is stored as function<R,A,B>.
        take();
        take();
        std::vector<std::string> params;
        if (!accept(")")) {
            do {
                params.push_back(type());
            } while (accept(","));
            expect(")");
        }
        r = "function<" + (accept("returns") ? type() : std::string("void"));
        for (auto &param : params)
            r += "," + param;
        return r + ">";
    } else {
        auto t = take();
        if (t.kind != "id")
            throw Error(t.loc, "expected type");
        r = t.text;
        while (accept("."))
            r += "." + identifier().text;
        if (accept("<")) {
            r += "<" + type();
            while (accept(","))
                r += "," + type();
            expect(">");
            r += ">";
        }
    }
    while (true) {
        if (accept("?"))
            r += '?';
        else if (accept("[")) {
            expect("]");
            r += "[]";
        } else
            break;
    }
    return r;
}
std::string Parser::markupName() {
    auto first = take();
    if (first.kind != "id")
        throw Error(first.loc, "expected a tag or attribute name");
    std::string name = first.text;
    // Names continue across '-', ':' and identifier tokens written without spaces.
    while (!peek().spaced && (peek().text == "-" || peek().text == ":") &&
           !tokens[index(1)].spaced && tokens[index(1)].kind == "id") {
        name += take().text;
        name += take().text;
    }
    return name;
}
std::vector<std::string> Parser::typeParameters() {
    std::vector<std::string> names;
    if (!accept("<"))
        return names;
    do {
        names.push_back(identifier().text);
    } while (accept(","));
    expect(">");
    return names;
}
bool Parser::typedDeclaration() {
    if (peek().text == "inst" || peek().text == "attr" || peek().text == "super")
        return false;
    size_t i = pos;
    try {
        type();
        bool yes = peek().kind == "id" && peek(1).text == "=";
        pos = i;
        return yes;
    } catch (...) {
        pos = i;
        return false;
    }
}
EP Parser::stringExpr(const Token &t) {
    auto result = node("interpolate", "", t.loc);
    std::string text;
    auto flush = [&] {
        if (!text.empty()) {
            result->args.push_back(node("string", text, t.loc));
            text.clear();
        }
    };
    for (size_t p = 0; p < t.text.size(); ++p) {
        char c = t.text[p];
        if (c == '\\') {
            if (++p == t.text.size())
                throw Error(t.loc, "incomplete escape");
            switch (t.text[p]) {
            case 'n':
                text += '\n';
                break;
            case 'r':
                text += '\r';
                break;
            case 't':
                text += '\t';
                break;
            case '\\':
                text += '\\';
                break;
            case '"':
                text += '"';
                break;
            case 'u': {
                if (++p == t.text.size() || t.text[p] != '{')
                    throw Error(t.loc, "expected \\u{HEX}");
                auto e = t.text.find('}', p);
                if (e == std::string::npos)
                    throw Error(t.loc, "incomplete Unicode escape");
                auto hex = t.text.substr(p + 1, e - p - 1);
                try {
                    size_t n;
                    auto v = std::stoul(hex, &n, 16);
                    if (n != hex.size() || v > 0x10ffff)
                        throw std::runtime_error("scalar");
                    utf8(text, unsigned(v), t.loc);
                } catch (...) {
                    throw Error(t.loc, "invalid Unicode escape");
                }
                p = e;
                break;
            }
            default:
                throw Error(t.loc, "unknown string escape");
            }
            continue;
        }
        if ((c == '{' || c == '}') && p + 1 < t.text.size() && t.text[p + 1] == c) {
            text += c;
            ++p;
            continue;
        }
        if (c == '{') {
            flush();
            auto e = braceEnd(t.text, p);
            Parser sub(lex(t.text.substr(p + 1, e - p - 1)));
            result->args.push_back(sub.expression());
            if (sub.peek().kind != "eof")
                throw Error(t.loc, "invalid interpolation expression");
            p = e;
        } else if (c == '}')
            throw Error(t.loc, "literal '}' must be doubled");
        else
            text += c;
    }
    flush();
    if (result->args.empty())
        return node("string", "", t.loc);
    if (result->args.size() == 1 && result->args[0]->kind == "string")
        return std::move(result->args[0]);
    return result;
}
EP Parser::markup() {
    auto token = take();
    const auto &s = token.text;
    size_t p = 0;
    auto space = [&] {
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
            ++p;
    };
    auto name = [&]() {
        size_t begin = p;
        while (p < s.size() && (part(s[p]) || s[p] == '-' || s[p] == ':' || s[p] == '.'))
            ++p;
        if (begin == p)
            throw Error(token.loc, "expected markup name");
        return s.substr(begin, p - begin);
    };
    auto embedded = [&]() {
        auto end = braceEnd(s, p);
        Parser sub(lex(s.substr(p + 1, end - p - 1)));
        auto value = sub.expression();
        if (sub.peek().kind != "eof")
            throw Error(token.loc, "invalid markup interpolation");
        p = end + 1;
        return value;
    };
    std::function<EP()> element = [&]() -> EP {
        if (p >= s.size() || s[p++] != '<')
            throw Error(token.loc, "expected opening tag");
        std::string tag;
        if (p < s.size() && s[p] != '>')
            tag = name();
        auto e = node("element", tag, token.loc);
        space();
        while (p < s.size() && s[p] != '>' && s[p] != '/') {
            auto a = name();
            if (std::find(e->names.begin(), e->names.end(), a) != e->names.end())
                throw Error(token.loc, "duplicate attribute '" + a + "'");
            space();
            if (p == s.size() || s[p++] != '=')
                throw Error(token.loc, "attributes require '='");
            space();
            EP value;
            if (p < s.size() && s[p] == '"') {
                auto end = quotedEnd(s, p);
                value = stringExpr({s.substr(p + 1, end - p - 1), "string", token.loc});
                if (value->kind == "string")
                    value->text = entities(value->text, token.loc);
                else if (value->kind == "interpolate")
                    for (auto &part : value->args)
                        if (part->kind == "string")
                            part->text = entities(part->text, token.loc);
                p = end + 1;
            } else if (p < s.size() && s[p] == '{')
                value = embedded();
            else
                throw Error(token.loc, "attribute value requires quotes or {expression}");
            e->names.push_back(a);
            e->args.push_back(std::move(value));
            space();
        }
        if (p < s.size() && s[p] == '/') {
            ++p;
            if (p == s.size() || s[p++] != '>')
                throw Error(token.loc, "expected '/>'");
            return e;
        }
        if (p == s.size() || s[p++] != '>')
            throw Error(token.loc, "expected '>'");
        std::string text;
        auto flush = [&] {
            if (!text.empty()) {
                bool indentation = text.find('\n') != std::string::npos &&
                                   text.find_first_not_of(" \r\n\t") == std::string::npos;
                if (!indentation)
                    e->args.push_back(node("string", entities(text, token.loc), token.loc));
                text.clear();
            }
        };
        while (p < s.size()) {
            if (s.compare(p, 2, "</") == 0) {
                flush();
                p += 2;
                std::string close;
                if (p < s.size() && s[p] != '>')
                    close = name();
                space();
                if (p == s.size() || s[p++] != '>')
                    throw Error(token.loc, "expected closing '>'");
                if (close != tag)
                    throw Error(token.loc, "mismatched closing tag: expected </" + tag +
                                               ">, got </" + close + ">");
                return e;
            }
            if (s[p] == '<') {
                flush();
                e->args.push_back(element());
            } else if ((s[p] == '{' || s[p] == '}') && p + 1 < s.size() && s[p + 1] == s[p]) {
                text += s[p];
                p += 2;
            } else if (s[p] == '{') {
                flush();
                e->args.push_back(embedded());
            } else if (s[p] == '}')
                throw Error(token.loc, "literal '}' must be doubled");
            else
                text += s[p++];
        }
        throw Error(token.loc, "missing closing tag");
    };
    auto e = element();
    if (p != s.size())
        throw Error(token.loc, "unexpected text after markup");
    return e;
}
EP Parser::prefix() {
    auto t = peek();
    EP e;
    if (t.kind == "markup")
        e = markup();
    else if (t.kind == "string") {
        take();
        e = stringExpr(t);
    } else if (t.kind == "int" || t.kind == "real") {
        take();
        e = node(t.kind, t.text, t.loc);
    } else if (t.kind == "char") {
        take();
        auto text = stringExpr({t.text, "string", t.loc});
        if (text->kind != "string" || text->text.empty())
            throw Error(t.loc, "character literals cannot interpolate");
        size_t length;
        scalarAt(text->text, 0, length);
        if (length != text->text.size())
            throw Error(t.loc, "character literals hold exactly one character, such as 'a'");
        e = node("char", text->text, t.loc);
    } else if (accept("true") || accept("false"))
        e = node("bool", t.text, t.loc);
    else if (accept("null"))
        e = node("null", "", t.loc);
    else if (accept("-") || accept("+") || accept("not")) {
        e = node("unary", t.text, t.loc);
        e->args.push_back(expression(t.text == "not" ? 2 : 9));
        return e;
    } else if (accept("(")) {
        e = expression();
        expect(")");
        e->grouped = true;
    } else if (accept("[")) {
        e = node("array", "", t.loc);
        if (!accept("]")) {
            do {
                e->args.push_back(expression());
            } while (accept(","));
            expect("]");
        }
    } else if (accept("{")) {
        // {k: v, ...} is a map literal and {a, b, ...} a set literal; {} is an empty map or set.
        e = node("map", "", t.loc);
        if (!accept("}")) {
            e->args.push_back(expression());
            if (!accept(":"))
                e->kind = "set";
            else
                e->args.push_back(expression());
            while (accept(",")) {
                e->args.push_back(expression());
                if (e->kind == "map") {
                    expect(":");
                    e->args.push_back(expression());
                }
            }
            expect("}");
        }
    } else if (accept("inst") || accept("attr")) {
        e = node(t.text, identifier().text, t.loc);
    } else if (accept("super")) {
        e = node("super", "", t.loc);
    } else if (t.kind == "id" && t.text == "function" && peek(1).text == "(") {
        e = lambda();
    } else if (t.kind == "id" && t.text == "weak" && peek(1).text == "(") {
        take();
        e = node("name", "weak", t.loc);
    } else if (t.kind == "id") {
        static const std::set<std::string> structural = {
            "function", "class", "component", "import",   "public", "private", "returns",
            "return",   "if",    "then",      "else",     "while",  "do",      "for",
            "in",       "end",   "break",     "continue", "try",    "catch",   "finally",
            "throw",    "as",    "void",      "weak",     "and",    "or",      "enum",
            "interface", "extends", "implements", "match", "case",  "is"};
        if (structural.count(t.text))
            throw Error(t.loc, "unexpected keyword '" + t.text + "' in expression");
        take();
        static const std::set<std::string> builtinGenerics = {"array", "tuple", "map",
                                                              "set",   "Result", "weak"};
        e = node("name", t.text + typeArguments(builtinGenerics.count(t.text) > 0), t.loc);
    } else
        throw Error(t.loc, "expected expression, got '" + t.text + "'");
    while (true) {
        if (accept(".")) {
            auto m = identifier();
            auto n = node("member", m.text + typeArguments(), m.loc);
            n->args.push_back(std::move(e));
            e = std::move(n);
        } else if (accept("(")) {
            auto n = node("call", "", t.loc);
            n->args.push_back(std::move(e));
            if (!accept(")")) {
                do {
                    n->args.push_back(expression());
                } while (accept(","));
                expect(")");
            }
            e = std::move(n);
        } else if (accept("[")) {
            auto n = node("index", "", t.loc);
            n->args.push_back(std::move(e));
            n->args.push_back(expression());
            expect("]");
            e = std::move(n);
        } else
            break;
    }
    return e;
}
// `name<T, U>` written without a space before '<' supplies type arguments. Parsing is speculative:
// `i<n do` is still a comparison because `n do` is not a type argument list.
std::string Parser::typeArguments(bool allowSpace) {
    if (peek().text != "<" || (peek().spaced && !allowSpace) || peek().kind != "op")
        return "";
    auto saved = pos;
    try {
        take();
        std::string out = "<" + type();
        while (accept(","))
            out += "," + type();
        expect(">");
        return out + ">";
    } catch (const Error &) {
        pos = saved;
        return "";
    }
}
EP Parser::expression(int level) {
    auto e = prefix();
    while ((peek().kind == "op" || peek().kind == "id") && precedence(peek().text) > level) {
        if (peek().text == "is" || peek().text == "as") {
            // x is Type tests a runtime class; x as Type is a checked downcast.
            auto op = take();
            auto n = node(op.text, type(), op.loc);
            n->args.push_back(std::move(e));
            e = std::move(n);
            continue;
        }
        auto op = take();
        // a < b <= c chains as a < b and b <= c, evaluating b once.
        if (comparison(op.text) && !e->grouped && (e->kind == "binary" || e->kind == "chain") &&
            comparison(e->kind == "chain" ? e->names.back() : e->text) &&
            precedence(e->kind == "chain" ? e->names.back() : e->text) == precedence(op.text)) {
            if (e->kind == "binary") {
                e->kind = "chain";
                e->names.push_back(e->text);
            }
            e->names.push_back(op.text);
            e->args.push_back(expression(precedence(op.text)));
            continue;
        }
        auto n = node("binary", op.text, op.loc);
        n->args.push_back(std::move(e));
        n->args.push_back(expression(precedence(op.text)));
        e = std::move(n);
    }
    return e;
}
EP Parser::outer() {
    // A known callable followed by a unary sign starts an outer call, not subtraction.
    bool callable = peek().text == "print";
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
        if ((tokens[i].text == "function" || tokens[i].text == "class") &&
            tokens[i + 1].text == peek().text)
            callable = true;
    EP e;
    if (callable && (peek(1).text == "-" || peek(1).text == "+" || peek(1).text == "not" ||
                     peek(1).text == "[")) {
        auto t = take();
        e = node("name", t.text, t.loc);
    } else
        e = expression();
    auto k = peek().kind;
    if ((e->kind == "name" || e->kind == "member") &&
        (k == "id" || k == "int" || k == "real" || k == "string" || k == "char" || k == "markup" ||
         peek().text == "[" || (callable && (peek().text == "-" || peek().text == "+")))) {
        auto n = node("call", "", e->loc);
        n->args.push_back(std::move(e));
        do {
            n->args.push_back(expression());
        } while (accept(","));
        const auto &last = *n->args.back();
        if ((last.kind == "name" || last.kind == "member") &&
            (peek().kind == "id" || peek().kind == "int" || peek().kind == "real" ||
             peek().kind == "string" || peek().kind == "char" || peek().kind == "markup"))
            throw Error(peek().loc, "nested calls require parentheses around their arguments");
        e = std::move(n);
    }
    e->outer = true;
    return e;
}
std::vector<SP> Parser::block() {
    std::vector<SP> out;
    newlines();
    while (peek().kind != "eof" && peek().text != "end" && peek().text != "else" &&
           peek().text != "catch" && peek().text != "finally" && peek().text != "case")
        out.push_back(statement());
    return out;
}
SP Parser::statement() {
    auto s = std::make_unique<Stmt>();
    s->loc = peek().loc;
    if (accept("throw")) {
        s->kind = "throw";
        s->expr = expression();
        finish();
        return s;
    }
    if (accept("try")) {
        s->kind = "try";
        finish();
        s->body = block();
        bool catchAll = false;
        while (accept("catch")) {
            if (catchAll)
                throw Error(peek().loc, "catch-all must be the last catch clause");
            auto saved = pos;
            std::string t, name;
            try {
                t = type();
                name = identifier().text;
            } catch (const Error &) {
                pos = saved;
                t = "Error";
                name = identifier().text;
            }
            if (t == "Error")
                catchAll = true;
            s->catchTypes.push_back(t);
            s->catchNames.push_back(name);
            finish();
            s->catches.push_back(block());
        }
        if (accept("finally")) {
            finish();
            s->finalizer = block();
        }
        if (s->catches.empty() && s->finalizer.empty())
            throw Error(s->loc, "try requires catch or finally");
        expect("end");
        finish();
        return s;
    }
    if (accept("if")) {
        s->kind = "if";
        s->expr = expression();
        expect("then");
        finish();
        s->body = block();
        if (accept("else")) {
            if (peek().text == "if") {
                s->other.push_back(statement());
                return s;
            }
            finish();
            s->other = block();
        }
        expect("end");
        finish();
        return s;
    }
    if (accept("match")) {
        s->kind = "match";
        s->expr = expression();
        finish();
        while (accept("case")) {
            // A type pattern is `Type name`; anything else is a list of values to compare.
            auto saved = pos;
            std::string patternType, binding;
            try {
                patternType = type();
                binding = identifier().text;
                if (peek().text != "then" && peek().text != "if")
                    throw Error(peek().loc, "pattern");
            } catch (const Error &) {
                pos = saved;
                patternType.clear();
                binding.clear();
            }
            std::vector<EP> values;
            if (patternType.empty())
                do {
                    values.push_back(expression());
                } while (accept(","));
            s->catchTypes.push_back(patternType);
            s->catchNames.push_back(binding);
            s->patterns.push_back(std::move(values));
            s->guards.push_back(accept("if") ? expression() : nullptr);
            expect("then");
            finish();
            s->catches.push_back(block());
        }
        if (accept("else")) {
            s->op = "else";
            finish();
            s->other = block();
        }
        if (s->catches.empty() && s->op.empty())
            throw Error(s->loc, "match requires at least one case");
        expect("end");
        finish();
        return s;
    }
    if (accept("while")) {
        s->kind = "while";
        s->expr = expression();
        expect("do");
        finish();
        s->body = block();
        expect("end");
        finish();
        return s;
    }
    if (accept("for")) {
        s->kind = "for";
        if (peek().text == "(")
            throw Error(
                peek().loc,
                "use 'for item in items do'; parentheses are not allowed around the binding");
        s->name = identifier().text;
        if (accept(","))
            s->op = identifier().text; // for key, value in map / for index, item in array
        expect("in");
        s->expr = expression();
        expect("do");
        finish();
        s->body = block();
        expect("end");
        finish();
        return s;
    }
    if (accept("return")) {
        s->kind = "return";
        if (peek().kind != "nl" && peek().kind != "eof")
            s->expr = expression();
        finish();
        return s;
    }
    if (accept("break") || accept("continue")) {
        s->kind = tokens[pos - 1].text;
        finish();
        return s;
    }
    bool isconst = accept("const"), inferred = accept("var");
    if (isconst || inferred || typedDeclaration()) {
        s->kind = "declare";
        s->op = isconst ? "const" : "var";
        if (!inferred && peek(1).text != "=")
            s->type = type();
        s->name = identifier().text;
        expect("=");
        s->expr = outer();
        finish();
        return s;
    }
    s->target = outer();
    if (peek().text == "=" || peek().text == "+=" || peek().text == "-=" || peek().text == "*=" ||
        peek().text == "/=" || peek().text == "//=" || peek().text == "%=") {
        s->kind = "assign";
        s->op = take().text;
        s->expr = outer();
    } else
        s->kind = "expr";
    finish();
    return s;
}
EP Parser::lambda() {
    auto t = expect("function");
    auto f = std::make_shared<Function>();
    f->loc = t.loc;
    f->name = "lambda";
    expect("(");
    if (!accept(")")) {
        do {
            Param p;
            p.loc = peek().loc;
            p.type = type();
            p.name = identifier().text;
            if (peek().text == "=")
                throw Error(peek().loc, "lambda parameters cannot have defaults");
            f->params.push_back(std::move(p));
        } while (accept(","));
        expect(")");
    }
    if (accept("returns"))
        f->result = type();
    if (accept("=>")) {
        auto r = std::make_unique<Stmt>();
        r->kind = "return";
        r->loc = peek().loc;
        r->expr = expression();
        f->body.push_back(std::move(r));
    } else {
        bodyDepths.push_back(t.depth);
        try {
            finish();
            f->body = block();
            expect("end");
        } catch (...) {
            bodyDepths.pop_back();
            throw;
        }
        bodyDepths.pop_back();
    }
    auto e = node("lambda", "", t.loc);
    e->lambda = std::move(f);
    return e;
}
Function Parser::function(const std::string &owner, bool signatureOnly) {
    auto t = expect("function");
    Function f;
    f.loc = t.loc;
    f.owner = owner;
    f.name = identifier().text;
    f.typeParams = typeParameters();
    if (accept("(")) {
        if (!accept(")")) {
            do {
                Param p;
                p.loc = peek().loc;
                p.type = type();
                p.name = identifier().text;
                if (accept("="))
                    p.value = expression();
                f.params.push_back(std::move(p));
            } while (accept(","));
            expect(")");
        }
    } else if (peek().text != "returns" && peek().kind != "nl" && peek().kind != "eof") {
        throw Error(peek().loc, "function declarations with parameters require parentheses");
    }
    if (accept("returns"))
        f.result = type();
    finish();
    if (signatureOnly) {
        f.abstract = true;
        if (f.result.empty())
            f.result = "void";
        return f;
    }
    f.body = block();
    expect("end");
    finish();
    return f;
}
Program Parser::parse() {
    Program p;
    newlines();
    while (peek().kind != "eof") {
        bool hidden = accept("private"), visible = !hidden && accept("public");
        size_t nf = p.functions.size(), nc = p.classes.size(), nb = p.body.size(),
               ne = p.enums.size();
        std::set<std::string> components;
        for (auto &[name, e] : p.components) {
            (void)e;
            components.insert(name);
        }
        if ((hidden || visible) && peek().text == "import")
            throw Error(peek().loc, "visibility applies to declarations, not imports");
        if (accept("import")) {
            auto name = identifier().text;
            bool wildcard = false;
            while (accept(".")) {
                if (accept("*")) {
                    wildcard = true;
                    break;
                }
                name += "." + identifier().text;
            }
            std::string alias = name.substr(
                name.find_last_of('.') == std::string::npos ? 0 : name.find_last_of('.') + 1);
            if (accept("as")) {
                if (wildcard)
                    throw Error(peek().loc, "wildcard imports cannot have an alias");
                alias = identifier().text;
            }
            if (wildcard)
                p.wildcards.insert(alias);
            if (wildcard && name == "html")
                p.html = true;
            if (wildcard && name == "svg")
                p.svg = true;
            if (p.imports.count(alias))
                throw Error(peek().loc, "duplicate import '" + alias + "'");
            p.imports[alias] = name;
            p.importOrder.push_back(alias);
            p.importLocations[alias] = tokens[pos - 1].loc;
            finish();
        } else if (peek().text == "function" && peek(1).text != "(")
            p.functions.push_back(function());
        else if (accept("enum")) {
            Enum en;
            en.loc = peek().loc;
            en.name = identifier().text;
            finish();
            while (peek().text != "end" && peek().kind != "eof") {
                auto member = identifier();
                if (std::find(en.members.begin(), en.members.end(), member.text) != en.members.end())
                    throw Error(member.loc, "duplicate enum member '" + member.text + "'");
                en.members.push_back(member.text);
                if (!accept(","))
                    finish();
                newlines();
            }
            if (en.members.empty())
                throw Error(en.loc, "enum requires at least one member");
            expect("end");
            finish();
            p.enums.push_back(std::move(en));
        } else if (accept("elements")) {
            ElementFamily family;
            family.loc = peek().loc;
            family.name = identifier().text;
            finish();
            while (peek().text != "end" && peek().kind != "eof") {
                auto location = peek().loc;
                auto tag = markupName();
                FamilyTag info;
                if (accept("void"))
                    info.isVoid = true;
                if (accept(":")) {
                    if (accept("*"))
                        info.anyAttribute = true;
                    else
                        do {
                            info.attributes.insert(markupName());
                        } while (accept(","));
                }
                if (!family.tags.emplace(tag, std::move(info)).second)
                    throw Error(location, "duplicate tag '" + tag + "' in element family");
                finish();
            }
            expect("end");
            finish();
            p.families.push_back(std::move(family));
        } else if (peek().text == "class" || peek().text == "interface") {
            Class c;
            c.isInterface = take().text == "interface";
            c.loc = peek().loc;
            c.name = identifier().text;
            c.typeParams = typeParameters();
            if (!c.isInterface && accept("extends"))
                c.base = type();
            if (accept(c.isInterface ? "extends" : "implements"))
                do {
                    c.interfaces.push_back(type());
                } while (accept(","));
            finish();
            while (peek().text != "end" && peek().kind != "eof") {
                if (peek().text == "function" && peek(1).text != "(")
                    c.methods.push_back(function(c.name, c.isInterface));
                else if (c.isInterface)
                    throw Error(peek().loc, "interfaces declare only function signatures");
                else {
                    Param field;
                    field.loc = peek().loc;
                    field.type = type();
                    field.name = identifier().text;
                    if (accept("="))
                        field.value = expression();
                    c.fields.push_back(std::move(field));
                    finish();
                }
            }
            expect("end");
            finish();
            p.classes.push_back(std::move(c));
        } else if (accept("component")) {
            auto n = identifier();
            expect("=");
            if (p.components.count(n.text))
                throw Error(n.loc, "duplicate component");
            p.components[n.text] = expression();
            finish();
        } else
            p.body.push_back(statement());
        std::string declared;
        if (p.functions.size() > nf)
            declared = p.functions.back().name;
        else if (p.classes.size() > nc)
            declared = p.classes.back().name;
        else if (p.enums.size() > ne)
            declared = p.enums.back().name;
        else if (p.body.size() > nb && p.body.back()->kind == "declare")
            declared = p.body.back()->name;
        else
            for (auto &[name, e] : p.components)
                if (!components.count(name))
                    declared = name;
        if ((hidden || visible) && declared.empty())
            throw Error(peek().loc, "visibility requires a declaration");
        if (hidden)
            p.privateNames.insert(declared);
    }
    return p;
}
EP clone(const Expr &e) {
    auto out = std::make_unique<Expr>();
    out->kind = e.kind;
    out->text = e.text;
    out->grouped = e.grouped;
    out->outer = e.outer;
    out->loc = e.loc;
    out->names = e.names;
    for (auto &a : e.args)
        out->args.push_back(a ? clone(*a) : nullptr);
    if (e.lambda)
        out->lambda = std::make_shared<Function>(clone(*e.lambda));
    return out;
}
namespace {
std::vector<SP> cloneBody(const std::vector<SP> &body) {
    std::vector<SP> out;
    for (auto &s : body)
        out.push_back(clone(*s));
    return out;
}
std::vector<Param> cloneParams(const std::vector<Param> &params) {
    std::vector<Param> out;
    for (auto &p : params)
        out.push_back({p.name, p.type, p.value ? clone(*p.value) : nullptr, p.loc});
    return out;
}
} // namespace
SP clone(const Stmt &s) {
    auto out = std::make_unique<Stmt>();
    out->kind = s.kind;
    out->name = s.name;
    out->type = s.type;
    out->op = s.op;
    out->loc = s.loc;
    out->expr = s.expr ? clone(*s.expr) : nullptr;
    out->target = s.target ? clone(*s.target) : nullptr;
    out->body = cloneBody(s.body);
    out->other = cloneBody(s.other);
    out->finalizer = cloneBody(s.finalizer);
    out->catchTypes = s.catchTypes;
    out->catchNames = s.catchNames;
    for (auto &c : s.catches)
        out->catches.push_back(cloneBody(c));
    for (auto &values : s.patterns) {
        std::vector<EP> copy;
        for (auto &v : values)
            copy.push_back(clone(*v));
        out->patterns.push_back(std::move(copy));
    }
    for (auto &g : s.guards)
        out->guards.push_back(g ? clone(*g) : nullptr);
    return out;
}
Function clone(const Function &f) {
    Function out;
    out.name = f.name;
    out.result = f.result;
    out.owner = f.owner;
    out.loc = f.loc;
    out.params = cloneParams(f.params);
    out.body = cloneBody(f.body);
    out.abstract = f.abstract;
    out.typeParams = f.typeParams;
    return out;
}
Class clone(const Class &c) {
    Class out;
    out.name = c.name;
    out.base = c.base;
    out.loc = c.loc;
    out.fields = cloneParams(c.fields);
    for (auto &m : c.methods)
        out.methods.push_back(clone(m));
    out.interfaces = c.interfaces;
    out.typeParams = c.typeParams;
    out.isInterface = c.isInterface;
    return out;
}
} // namespace fw
