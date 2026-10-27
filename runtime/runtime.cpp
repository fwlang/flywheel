#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
[[noreturn]] void fail(const std::string &message) {
    std::cerr << "Flywheel runtime error: " << message << '\n';
    std::exit(1);
}
struct Json {
    enum Kind { Null, Boolean, Number, Text, List, Dictionary } kind = Null;
    std::string text;
    std::vector<std::shared_ptr<Json>> items;
    std::map<std::string, std::shared_ptr<Json>> fields;
};
struct Object;
void retain(Object *);
void release(Object *);
struct Slot {
    int64_t integer = 0;
    double real = 0;
    Object *object = nullptr;
    Slot() = default;
    Slot(const Slot &s) : integer(s.integer), real(s.real), object(s.object) {
        retain(object);
    }
    Slot(Slot &&s) noexcept
        : integer(s.integer), real(s.real), object(std::exchange(s.object, nullptr)) {}
    Slot &operator=(const Slot &s) {
        if (this != &s) {
            retain(s.object);
            release(object);
            object = s.object;
            integer = s.integer;
            real = s.real;
        }
        return *this;
    }
    ~Slot() {
        release(object);
    }
};
struct Object {
    std::atomic<size_t> refs{1};
    enum Kind { String, Array, Instance, Element, JsonValue } kind;
    std::string text;
    std::shared_ptr<Json> json;
    std::vector<Slot> slots;
    std::vector<std::pair<std::string, std::string>> attributes;
    explicit Object(Kind k) : kind(k) {}
};
void retain(Object *p) {
    if (p)
        p->refs.fetch_add(1, std::memory_order_relaxed);
}
void release(Object *p) {
    if (p && p->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete p;
}
struct Frame {
    std::vector<Object *> owned;
    std::vector<Object **> roots;
    void sweep() {
        for (auto i = owned.rbegin(); i != owned.rend(); ++i)
            release(*i);
        owned.clear();
    }
    ~Frame() {
        sweep();
        for (auto root : roots) {
            release(*root);
            *root = nullptr;
        }
    }
};
Object *own(Frame *f, Object *p) {
    f->owned.push_back(p);
    return p;
}
Object *string(Frame *f, std::string s) {
    auto p = new Object(Object::String);
    p->text = std::move(s);
    return own(f, p);
}
std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
        }
    }
    return out;
}
void render(Object *p, std::string &out) {
    if (!p)
        return;
    if (p->kind == Object::String) {
        out += escape(p->text);
        return;
    }
    if (p->kind != Object::Element)
        fail("expected markup node");
    bool fragment = p->text.empty();
    if (!fragment) {
        out += '<';
        out += p->text;
        for (auto &[name, value] : p->attributes) {
            out += ' ';
            out += name;
            out += "=\"";
            out += escape(value);
            out += '"';
        }
        out += '>';
    }
    for (auto &c : p->slots)
        render(c.object, out);
    static const std::string voids =
        " area base br col embed hr img input link meta param source track wbr ";
    if (!fragment && voids.find(" " + p->text + " ") == std::string::npos) {
        out += "</";
        out += p->text;
        out += '>';
    }
}
Slot &slot(Object *p, int64_t index) {
    if (!p || index < 0 || static_cast<uint64_t>(index) >= p->slots.size())
        fail("index out of bounds");
    return p->slots[static_cast<size_t>(index)];
}
} // namespace
extern "C" {
void *fw_frame() {
    return new Frame;
}
void fw_sweep(Frame *f) {
    f->sweep();
}
void fw_root(Frame *f, Object **address) {
    f->roots.push_back(address);
}
void fw_store(Object **address, Object *value) {
    retain(value);
    release(*address);
    *address = value;
}
void fw_drop(Frame *f) {
    delete f;
}
void fw_retain(Object *p) {
    retain(p);
}
Object *fw_keep(Frame *f, Object *p) {
    retain(p);
    return own(f, p);
}
Object *fw_adopt(Frame *f, Object *p) {
    return own(f, p);
}
Object *fw_literal(Frame *f, const char *s, int64_t length) {
    return string(f, std::string(s, static_cast<size_t>(length)));
}
Object *fw_concat(Frame *f, Object *a, Object *b) {
    return string(f, a->text + b->text);
}
Object *fw_int_string(Frame *f, int64_t n) {
    return string(f, std::to_string(n));
}
Object *fw_uint_string(Frame *f, uint64_t n) {
    return string(f, std::to_string(n));
}
Object *fw_real_string(Frame *f, double n) {
    std::ostringstream s;
    s << std::setprecision(15) << n;
    return string(f, s.str());
}
Object *fw_bool_string(Frame *f, int64_t n) {
    return string(f, n ? "true" : "false");
}
void fw_print(Object *p) {
    std::cout << p->text << '\n';
}
int64_t fw_equal(Object *a, Object *b) {
    if (!a || !b)
        return a == b;
    return a->kind == Object::String && b->kind == Object::String ? a->text == b->text : a == b;
}
int64_t fw_length(Object *p) {
    if (!p)
        fail("null length");
    return static_cast<int64_t>(p->kind == Object::String ? p->text.size() : p->slots.size());
}
Object *fw_new(Frame *f, int64_t count, int64_t kind) {
    if (count < 0)
        fail("negative object size");
    auto p = new Object(static_cast<Object::Kind>(kind));
    p->slots.resize(static_cast<size_t>(count));
    return own(f, p);
}
int64_t fw_get_int(Object *p, int64_t index) {
    return slot(p, index).integer;
}
double fw_get_real(Object *p, int64_t index) {
    return slot(p, index).real;
}
Object *fw_get_object(Object *p, int64_t index) {
    return slot(p, index).object;
}
void fw_set_int(Object *p, int64_t index, int64_t value) {
    slot(p, index).integer = value;
}
void fw_set_real(Object *p, int64_t index, double value) {
    slot(p, index).real = value;
}
void fw_set_object(Object *p, int64_t index, Object *value) {
    auto &s = slot(p, index);
    retain(value);
    release(s.object);
    s.object = value;
}
Object *fw_element(Frame *f, Object *tag) {
    auto p = new Object(Object::Element);
    p->text = tag->text;
    return own(f, p);
}
void fw_child(Object *p, Object *child) {
    Slot s;
    s.object = child;
    retain(child);
    p->slots.push_back(std::move(s));
}
void fw_attribute(Object *p, Object *name, Object *value) {
    p->attributes.emplace_back(name->text, value->text);
}
Object *fw_render(Frame *f, Object *p) {
    std::string out;
    render(p, out);
    return string(f, std::move(out));
}
Object *fw_tag(Frame *f, Object *p) {
    return string(f, p->text);
}
Object *fw_children(Frame *f, Object *p) {
    auto a = new Object(Object::Array);
    a->slots = p->slots;
    return own(f, a);
}
int64_t fw_exists(Object *path) {
    std::error_code ec;
    bool yes = std::filesystem::exists(
        std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
        ec);
    if (ec)
        fail(ec.message());
    return yes;
}
Object *fw_read(Frame *f, Object *path) {
    std::ifstream stream(
        std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
        std::ios::binary);
    if (!stream)
        fail("cannot read '" + path->text + "'");
    std::ostringstream out;
    out << stream.rdbuf();
    if (stream.bad())
        fail("read failed");
    return string(f, out.str());
}
void fw_write(Object *path, Object *data) {
    std::ofstream stream(
        std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
        std::ios::binary);
    if (!stream)
        fail("cannot write '" + path->text + "'");
    stream.write(data->text.data(), static_cast<std::streamsize>(data->text.size()));
    if (!stream)
        fail("write failed");
}
void fw_remove(Object *path) {
    std::error_code ec;
    std::filesystem::remove(
        std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
        ec);
    if (ec)
        fail(ec.message());
}
Object *fw_env(Frame *f, Object *name) {
    auto p = std::getenv(name->text.c_str());
    if (!p)
        fail("environment variable is not set");
    return string(f, p);
}
double fw_clock() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void fw_sleep(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0)
        fail("invalid sleep duration");
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}
double fw_sqrt(double n) {
    return std::sqrt(n);
}
double fw_sin(double n) {
    return std::sin(n);
}
double fw_cos(double n) {
    return std::cos(n);
}
double fw_pow(double a, double b) {
    return std::pow(a, b);
}
void fw_assert(int64_t ok, Object *message) {
    if (!ok)
        fail("assertion failed: " + message->text);
}
void fw_panic(const char *message) {
    fail(message);
}
}
extern "C" {
int64_t fw_contains_int(Object *p, int64_t item) {
    for (auto &s : p->slots)
        if (s.integer == item)
            return 1;
    return 0;
}
int64_t fw_contains_real(Object *p, double item) {
    for (auto &s : p->slots)
        if (s.real == item)
            return 1;
    return 0;
}
int64_t fw_contains_object(Object *p, Object *item) {
    for (auto &s : p->slots)
        if (fw_equal(s.object, item))
            return 1;
    return 0;
}
}
namespace {
class JsonParser {
    const std::string &source;
    size_t pos = 0;
    [[noreturn]] void error() {
        fail("invalid JSON at byte " + std::to_string(pos));
    }
    void space() {
        while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\r' ||
                                       source[pos] == '\n' || source[pos] == '\t'))
            ++pos;
    }
    bool accept(char c) {
        space();
        if (pos < source.size() && source[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }
    unsigned hex4() {
        unsigned n = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos == source.size())
                error();
            char c = source[pos++];
            unsigned d = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
            if (d == 16)
                error();
            n = n * 16 + d;
        }
        return n;
    }
    void append(std::string &s, unsigned n) {
        if (n < 0x80)
            s += char(n);
        else if (n < 0x800) {
            s += char(0xc0 | (n >> 6));
            s += char(0x80 | (n & 63));
        } else if (n < 0x10000) {
            s += char(0xe0 | (n >> 12));
            s += char(0x80 | ((n >> 6) & 63));
            s += char(0x80 | (n & 63));
        } else {
            s += char(0xf0 | (n >> 18));
            s += char(0x80 | ((n >> 12) & 63));
            s += char(0x80 | ((n >> 6) & 63));
            s += char(0x80 | (n & 63));
        }
    }
    std::string text() {
        if (!accept('"'))
            error();
        std::string result;
        while (pos < source.size()) {
            unsigned char c = source[pos++];
            if (c == '"')
                return result;
            if (c < 0x20)
                error();
            if (c != '\\') {
                result += char(c);
                continue;
            }
            if (pos == source.size())
                error();
            switch (source[pos++]) {
            case '"':
                result += '"';
                break;
            case '\\':
                result += '\\';
                break;
            case '/':
                result += '/';
                break;
            case 'b':
                result += '\b';
                break;
            case 'f':
                result += '\f';
                break;
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            case 'u': {
                unsigned n = hex4();
                if (n >= 0xd800 && n <= 0xdbff) {
                    if (pos + 2 > source.size() || source.substr(pos, 2) != "\\u")
                        error();
                    pos += 2;
                    unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff)
                        error();
                    n = 0x10000 + ((n - 0xd800) << 10) + (low - 0xdc00);
                } else if (n >= 0xdc00 && n <= 0xdfff)
                    error();
                append(result, n);
                break;
            }
            default:
                error();
            }
        }
        error();
    }
    std::shared_ptr<Json> value(int depth) {
        if (depth > 256)
            fail("JSON nesting exceeds 256");
        space();
        if (pos == source.size())
            error();
        auto result = std::make_shared<Json>();
        char c = source[pos];
        if (c == '"') {
            result->kind = Json::Text;
            result->text = text();
            return result;
        }
        if (accept('{')) {
            result->kind = Json::Dictionary;
            if (accept('}'))
                return result;
            do {
                auto key = text();
                if (!accept(':'))
                    error();
                auto v = value(depth + 1);
                if (!result->fields.emplace(key, v).second)
                    fail("duplicate JSON object key '" + key + "'");
            } while (accept(','));
            if (!accept('}'))
                error();
            return result;
        }
        if (accept('[')) {
            result->kind = Json::List;
            if (accept(']'))
                return result;
            do {
                result->items.push_back(value(depth + 1));
            } while (accept(','));
            if (!accept(']'))
                error();
            return result;
        }
        for (auto word : {"null", "true", "false"}) {
            size_t length = std::strlen(word);
            if (source.compare(pos, length, word) == 0) {
                pos += length;
                result->kind = word[0] == 'n' ? Json::Null : Json::Boolean;
                result->text = word;
                return result;
            }
        }
        size_t begin = pos;
        if (source[pos] == '-')
            ++pos;
        if (pos == source.size())
            error();
        if (source[pos] == '0')
            ++pos;
        else {
            if (source[pos] < '1' || source[pos] > '9')
                error();
            while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos])))
                ++pos;
        }
        if (pos < source.size() && source[pos] == '.') {
            ++pos;
            size_t digits = pos;
            while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos])))
                ++pos;
            if (digits == pos)
                error();
        }
        if (pos < source.size() && (source[pos] == 'e' || source[pos] == 'E')) {
            ++pos;
            if (pos < source.size() && (source[pos] == '+' || source[pos] == '-'))
                ++pos;
            size_t digits = pos;
            while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos])))
                ++pos;
            if (digits == pos)
                error();
        }
        result->kind = Json::Number;
        result->text = source.substr(begin, pos - begin);
        return result;
    }

  public:
    explicit JsonParser(const std::string &s) : source(s) {}
    std::shared_ptr<Json> parse() {
        auto result = value(0);
        space();
        if (pos != source.size())
            error();
        return result;
    }
};
std::string jsonQuote(const std::string &s) {
    std::string out = "\"";
    const char *hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += char(c);
    }
    return out + '"';
}
std::string jsonRender(const Json &j) {
    switch (j.kind) {
    case Json::Null:
        return "null";
    case Json::Boolean:
    case Json::Number:
        return j.text;
    case Json::Text:
        return jsonQuote(j.text);
    case Json::List: {
        std::string out = "[";
        for (auto &v : j.items) {
            if (out.size() > 1)
                out += ',';
            out += jsonRender(*v);
        }
        return out + "]";
    }
    case Json::Dictionary: {
        std::string out = "{";
        for (auto &[key, v] : j.fields) {
            if (out.size() > 1)
                out += ',';
            out += jsonQuote(key) + ":" + jsonRender(*v);
        }
        return out + "}";
    }
    }
    fail("invalid JSON value");
}
Object *jsonObject(Frame *frame, std::shared_ptr<Json> json) {
    auto p = new Object(Object::JsonValue);
    p->json = std::move(json);
    return own(frame, p);
}
Json &requireJson(Object *p) {
    if (!p || p->kind != Object::JsonValue || !p->json)
        fail("expected JSON value");
    return *p->json;
}
} // namespace
extern "C" {
Object *fw_json_parse(Frame *frame, Object *text) {
    return jsonObject(frame, JsonParser(text->text).parse());
}
Object *fw_json_stringify(Frame *frame, Object *value) {
    return string(frame, jsonRender(requireJson(value)));
}
Object *fw_json_kind(Frame *frame, Object *value) {
    static const char *names[] = {"null", "bool", "number", "string", "array", "object"};
    return string(frame, names[requireJson(value).kind]);
}
int64_t fw_json_has(Object *value, Object *key) {
    auto &j = requireJson(value);
    if (j.kind != Json::Dictionary)
        fail("expected JSON object");
    return j.fields.count(key->text) != 0;
}
Object *fw_json_get(Frame *frame, Object *value, Object *key) {
    auto &j = requireJson(value);
    if (j.kind != Json::Dictionary)
        fail("expected JSON object");
    auto it = j.fields.find(key->text);
    if (it == j.fields.end())
        fail("missing JSON field '" + key->text + "'");
    return jsonObject(frame, it->second);
}
Object *fw_json_string(Frame *frame, Object *value) {
    auto &j = requireJson(value);
    if (j.kind != Json::Text)
        fail("expected JSON string");
    return string(frame, j.text);
}
int64_t fw_json_int(Object *value) {
    auto &j = requireJson(value);
    if (j.kind != Json::Number)
        fail("expected JSON integer");
    int64_t n = 0;
    auto [end, ec] = std::from_chars(j.text.data(), j.text.data() + j.text.size(), n);
    if (ec != std::errc{} || end != j.text.data() + j.text.size())
        fail("JSON integer out of range or non-integral");
    return n;
}
double fw_json_real(Object *value) {
    auto &j = requireJson(value);
    if (j.kind != Json::Number)
        fail("expected JSON number");
    double n;
    try {
        n = std::stod(j.text);
    } catch (...) {
        fail("JSON number out of range");
    }
    if (!std::isfinite(n))
        fail("JSON number out of range");
    return n;
}
int64_t fw_json_bool(Object *value) {
    auto &j = requireJson(value);
    if (j.kind != Json::Boolean)
        fail("expected JSON boolean");
    return j.text == "true";
}
}
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
namespace {
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
void closeSocket(Socket s) {
    closesocket(s);
}
} // namespace
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
using Socket = int;
constexpr Socket invalidSocket = -1;
void closeSocket(Socket s) {
    close(s);
}
} // namespace
#endif
extern "C" void fw_serve(Object *route, Object *(*handler)(), int64_t port) {
    if (port < 1 || port > 65535)
        fail("HTTP port must be in 1..65535");
    if (route->text.empty() || route->text[0] != '/')
        fail("HTTP route must start with '/'");
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        fail("socket initialization failed");
#endif
    Socket server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == invalidSocket)
        fail("cannot create HTTP socket");
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
               sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        closeSocket(server);
        fail("cannot bind HTTP port " + std::to_string(port));
    }
    if (listen(server, 16) != 0) {
        closeSocket(server);
        fail("cannot listen on HTTP socket");
    }
    for (;;) {
        Socket client = accept(server, nullptr, nullptr);
        if (client == invalidSocket)
            continue;
#ifdef _WIN32
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout));
#else
        timeval timeout{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
        int noSignal = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
#endif
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            int n = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
            if (n <= 0)
                break;
            request.append(buffer, static_cast<size_t>(n));
        }
        std::string status = "400 Bad Request", body = "Bad Request",
                    content = "text/plain; charset=utf-8";
        auto first = request.find(' '),
             second = first == std::string::npos ? first : request.find(' ', first + 1);
        if (request.find("\r\n\r\n") != std::string::npos && first != std::string::npos &&
            second != std::string::npos) {
            auto method = request.substr(0, first);
            auto path = request.substr(first + 1, second - first - 1);
            auto query = path.find('?');
            if (query != std::string::npos)
                path.resize(query);
            if (method != "GET") {
                status = "405 Method Not Allowed";
                body = "Method Not Allowed";
            } else if (path != route->text) {
                status = "404 Not Found";
                body = "Not Found";
            } else {
                Object *result = handler();
                body.clear();
                render(result, body);
                release(result);
                status = "200 OK";
                content = "text/html; charset=utf-8";
            }
        }
        std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: " + content +
                               "\r\nContent-Length: " + std::to_string(body.size()) +
                               "\r\nConnection: close\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            int n = static_cast<int>(
                send(client, response.data() + sent,
                     static_cast<int>(std::min<size_t>(response.size() - sent, 65536)), flags));
            if (n <= 0)
                break;
            sent += static_cast<size_t>(n);
        }
        closeSocket(client);
    }
}
extern "C" {
int64_t fw_parse_int(Object *s) {
    int64_t n;
    auto [end, ec] = std::from_chars(s->text.data(), s->text.data() + s->text.size(), n);
    if (ec != std::errc{} || end != s->text.data() + s->text.size())
        fail("invalid or out-of-range integer string");
    return n;
}
uint64_t fw_parse_uint(Object *s) {
    uint64_t n;
    auto [end, ec] = std::from_chars(s->text.data(), s->text.data() + s->text.size(), n);
    if (ec != std::errc{} || end != s->text.data() + s->text.size())
        fail("invalid or out-of-range unsigned integer string");
    return n;
}
double fw_parse_real(Object *s) {
    double n;
    auto [end, ec] = std::from_chars(s->text.data(), s->text.data() + s->text.size(), n);
    if (ec != std::errc{} || end != s->text.data() + s->text.size() || !std::isfinite(n))
        fail("invalid or out-of-range real string");
    return n;
}
}
