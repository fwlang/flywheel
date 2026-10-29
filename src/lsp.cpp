// A small Language Server Protocol implementation: diagnostics from the compiler's semantic
// check and whole-document formatting. Messages are JSON-RPC over stdin/stdout.
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <filesystem>
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
struct Server {
    std::map<std::string, std::string> documents; // uri -> text
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
            compile(program, path, Options{});
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
        if (method == "initialize")
            reply("{\"capabilities\":{\"textDocumentSync\":1,\"documentFormattingProvider\":true},"
                  "\"serverInfo\":{\"name\":\"flywheel\",\"version\":\"0.1.0\"}}");
        else if (method == "shutdown")
            reply("null");
        else if (method == "exit")
            return false;
        else if (method == "textDocument/didOpen" && document()) {
            auto uri = document()->get("uri")->text();
            documents[uri] = document()->get("text")->text();
            diagnose(uri);
        } else if (method == "textDocument/didChange" && document()) {
            auto uri = document()->get("uri")->text();
            if (auto changes = params->get("contentChanges"))
                if (auto list = std::get_if<std::vector<JsonPtr>>(&changes->value);
                    list && !list->empty())
                    documents[uri] = list->back()->get("text")->text();
            diagnose(uri);
        } else if (method == "textDocument/didSave" && document())
            diagnose(document()->get("uri")->text());
        else if (method == "textDocument/didClose" && document()) {
            auto uri = document()->get("uri")->text();
            documents.erase(uri);
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
