#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
struct NativeFailure {
    std::string type, message;
};
[[noreturn]] void fail(const std::string &message, const std::string &type = "RuntimeError") {
    throw NativeFailure{type, message};
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
    enum Kind { Integer, Unsigned, Real, Boolean, Reference } kind = Integer;
    int64_t integer = 0;
    double real = 0;
    Object *object = nullptr;
    Slot() = default;
    Slot(const Slot &s) : kind(s.kind), integer(s.integer), real(s.real), object(s.object) {
        retain(object);
    }
    Slot(Slot &&s) noexcept
        : kind(s.kind), integer(s.integer), real(s.real), object(std::exchange(s.object, nullptr)) {
    }
    Slot &operator=(const Slot &s) {
        if (this != &s) {
            retain(s.object);
            release(object);
            object = s.object;
            kind = s.kind;
            integer = s.integer;
            real = s.real;
        }
        return *this;
    }
    ~Slot() {
        release(object);
    }
};
std::atomic<int64_t> liveObjects{0};
// Shared by an object and its weak references; cleared when the object is destroyed.
struct WeakCell {
    Object *target = nullptr;
};
// Hash lookup for map keys. Positions index the insertion-ordered keys/slots vectors.
struct MapIndex {
    std::unordered_map<int64_t, size_t> integers;
    std::unordered_map<std::string, size_t> strings;
    size_t count = 0;
};
struct DesktopState {
    std::string title;
    int width = 960, height = 640;
    bool running = false;
};
struct Object {
    std::atomic<size_t> refs{1};
    enum Kind {
        String,
        Array,
        Instance,
        Element,
        JsonValue,
        Map,
        ErrorValue,
        Tuple,
        Result,
        DesktopWindow,
        Weak,
        Closure
    } kind;
    std::unique_ptr<DesktopState> desktop;
    std::string text;
    std::shared_ptr<Json> json;
    std::vector<Slot> slots, keys;
    bool objectKeys = false;
    Slot::Kind keyKind = Slot::Integer;
    std::string errorType, typeName;
    std::vector<std::string> fieldNames;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::unique_ptr<MapIndex> index;
    // weakCell belongs to a weak reference's target; link is the cell a Weak object reads.
    std::shared_ptr<WeakCell> weakCell, link;
    void *code = nullptr; // Closure entry point; captured values live in slots.
    explicit Object(Kind k) : kind(k) {
        ++liveObjects;
    }
    ~Object() {
        if (weakCell)
            weakCell->target = nullptr;
        --liveObjects;
    }
};
void retain(Object *p) {
    if (p)
        p->refs.fetch_add(1, std::memory_order_relaxed);
}
void release(Object *p) {
    if (p && p->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete p;
}
struct PendingError {
    std::string type;
    Object *value = nullptr;
    void clear() {
        release(value);
        value = nullptr;
        type.clear();
    }
    void set(const std::string &t, Object *v) {
        retain(v);
        clear();
        type = t;
        value = v;
    }
    ~PendingError() {
        clear();
    }
};
thread_local PendingError pendingError;
void recordFailure(const std::string &type, const std::string &message) {
    auto p = new Object(Object::ErrorValue);
    p->text = message;
    p->errorType = type;
    pendingError.set(type, p);
    release(p);
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
        fail("index out of bounds", "IndexError");
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
    try {
        return string(f, std::string(s, static_cast<size_t>(length)));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_concat(Frame *f, Object *a, Object *b) {
    try {
        return string(f, a->text + b->text);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_int_string(Frame *f, int64_t n) {
    try {
        return string(f, std::to_string(n));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_uint_string(Frame *f, uint64_t n) {
    try {
        return string(f, std::to_string(n));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_real_string(Frame *f, double n) {
    try {
        std::ostringstream s;
        s << std::setprecision(15) << n;
        return string(f, s.str());
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_bool_string(Frame *f, int64_t n) {
    try {
        return string(f, n ? "true" : "false");
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
void fw_print(Object *p) {
    try {
        std::cout << p->text << '\n';
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
int64_t fw_equal(Object *a, Object *b) {
    try {
        if (!a || !b)
            return a == b;
        return a->kind == Object::String && b->kind == Object::String ? a->text == b->text : a == b;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_length(Object *p) {
    try {
        if (!p)
            fail("null length");
        return static_cast<int64_t>(p->kind == Object::String ? p->text.size() : p->slots.size());
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_new(Frame *f, int64_t count, int64_t kind) {
    try {
        if (count < 0)
            fail("negative object size");
        auto p = new Object(static_cast<Object::Kind>(kind));
        p->slots.resize(static_cast<size_t>(count));
        return own(f, p);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_get_int(Object *p, int64_t index) {
    try {
        return slot(p, index).integer;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
double fw_get_real(Object *p, int64_t index) {
    try {
        return slot(p, index).real;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_get_object(Object *p, int64_t index) {
    try {
        return slot(p, index).object;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
void fw_set_int(Object *p, int64_t index, int64_t value, int64_t kind) {
    try {
        auto &target = slot(p, index);
        target.integer = value;
        target.kind = static_cast<Slot::Kind>(kind);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
void fw_set_real(Object *p, int64_t index, double value) {
    try {
        auto &target = slot(p, index);
        target.real = value;
        target.kind = Slot::Real;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
void fw_set_object(Object *p, int64_t index, Object *value) {
    try {
        auto &s = slot(p, index);
        retain(value);
        release(s.object);
        s.object = value;
        s.kind = Slot::Reference;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
Object *fw_element(Frame *f, Object *tag) {
    try {
        auto p = new Object(Object::Element);
        p->text = tag->text;
        return own(f, p);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
void fw_child(Object *p, Object *child) {
    try {
        Slot s;
        s.object = child;
        s.kind = Slot::Reference;
        retain(child);
        p->slots.push_back(std::move(s));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
void fw_attribute(Object *p, Object *name, Object *value) {
    try {
        p->attributes.emplace_back(name->text, value->text);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
Object *fw_render(Frame *f, Object *p) {
    try {
        std::string out;
        render(p, out);
        return string(f, std::move(out));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_tag(Frame *f, Object *p) {
    try {
        return string(f, p->text);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_children(Frame *f, Object *p) {
    try {
        auto a = new Object(Object::Array);
        a->slots = p->slots;
        return own(f, a);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_exists(Object *path) {
    try {
        std::error_code ec;
        bool yes = std::filesystem::exists(
            std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
            ec);
        if (ec)
            fail("file operation failed: " + ec.message(), "FileError");
        return yes;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_read(Frame *f, Object *path) {
    try {
        std::ifstream stream(
            std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
            std::ios::binary);
        if (!stream)
            fail("cannot read '" + path->text + "'", "FileError");
        std::ostringstream out;
        out << stream.rdbuf();
        if (stream.bad())
            fail("read failed", "FileError");
        return string(f, out.str());
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
void fw_write(Object *path, Object *data) {
    try {
        std::ofstream stream(
            std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
            std::ios::binary);
        if (!stream)
            fail("cannot write '" + path->text + "'", "FileError");
        stream.write(data->text.data(), static_cast<std::streamsize>(data->text.size()));
        if (!stream)
            fail("write failed", "FileError");
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
void fw_remove(Object *path) {
    try {
        std::error_code ec;
        std::filesystem::remove(
            std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t *>(path->text.data()), path->text.size())),
            ec);
        if (ec)
            fail("file operation failed: " + ec.message(), "FileError");
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
Object *fw_env(Frame *f, Object *name) {
    try {
        auto p = std::getenv(name->text.c_str());
        if (!p)
            fail("environment variable is not set");
        return string(f, p);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
double fw_clock() {
    try {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
void fw_sleep(double seconds) {
    try {
        if (!std::isfinite(seconds) || seconds < 0)
            fail("invalid sleep duration", "NumericError");
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
double fw_sqrt(double n) {
    try {
        return std::sqrt(n);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
double fw_sin(double n) {
    try {
        return std::sin(n);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
double fw_cos(double n) {
    try {
        return std::cos(n);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
double fw_pow(double a, double b) {
    try {
        return std::pow(a, b);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
void fw_assert(int64_t ok, Object *message) {
    try {
        if (!ok)
            fail("assertion failed: " + message->text, "AssertionError");
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
void fw_panic(const char *message, const char *type) {
    try {
        fail(message, type);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
}
extern "C" {
int64_t fw_contains_int(Object *p, int64_t item) {
    try {
        for (auto &s : p->slots)
            if (s.integer == item)
                return 1;
        return 0;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_contains_real(Object *p, double item) {
    try {
        for (auto &s : p->slots)
            if (s.real == item)
                return 1;
        return 0;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_contains_object(Object *p, Object *item) {
    try {
        for (auto &s : p->slots)
            if (fw_equal(s.object, item))
                return 1;
        return 0;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
}
namespace {
class JsonParser {
    const std::string &source;
    size_t pos = 0;
    [[noreturn]] void error() {
        fail("invalid JSON at byte " + std::to_string(pos), "JsonError");
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
            fail("JSON nesting exceeds 256", "JsonError");
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
                    fail("duplicate JSON object key '" + key + "'", "JsonError");
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
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\b')
            out += "\\b";
        else if (c == '\f')
            out += "\\f";
        else if (c < 32) {
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
    fail("invalid JSON value", "JsonError");
}
Object *jsonObject(Frame *frame, std::shared_ptr<Json> json) {
    auto p = new Object(Object::JsonValue);
    p->json = std::move(json);
    return own(frame, p);
}
Json &requireJson(Object *p) {
    if (!p || p->kind != Object::JsonValue || !p->json)
        fail("expected JSON value", "JsonError");
    return *p->json;
}
} // namespace
extern "C" {
Object *fw_json_parse(Frame *frame, Object *text) {
    try {
        return jsonObject(frame, JsonParser(text->text).parse());
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_json_stringify(Frame *frame, Object *value) {
    try {
        return string(frame, jsonRender(requireJson(value)));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_json_kind(Frame *frame, Object *value) {
    try {
        static const char *names[] = {"null", "bool", "number", "string", "array", "object"};
        return string(frame, names[requireJson(value).kind]);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_json_has(Object *value, Object *key) {
    try {
        auto &j = requireJson(value);
        if (j.kind != Json::Dictionary)
            fail("expected JSON object", "JsonError");
        return j.fields.count(key->text) != 0;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_json_get(Frame *frame, Object *value, Object *key) {
    try {
        auto &j = requireJson(value);
        if (j.kind != Json::Dictionary)
            fail("expected JSON object", "JsonError");
        auto it = j.fields.find(key->text);
        if (it == j.fields.end())
            fail("missing JSON field '" + key->text + "'", "JsonError");
        return jsonObject(frame, it->second);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_json_string(Frame *frame, Object *value) {
    try {
        auto &j = requireJson(value);
        if (j.kind != Json::Text)
            fail("expected JSON string", "JsonError");
        return string(frame, j.text);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_json_int(Object *value) {
    try {
        auto &j = requireJson(value);
        if (j.kind != Json::Number)
            fail("expected JSON integer", "JsonError");
        int64_t n = 0;
        auto [end, ec] = std::from_chars(j.text.data(), j.text.data() + j.text.size(), n);
        if (ec != std::errc{} || end != j.text.data() + j.text.size())
            fail("JSON integer out of range or non-integral", "JsonError");
        return n;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
double fw_json_real(Object *value) {
    try {
        auto &j = requireJson(value);
        if (j.kind != Json::Number)
            fail("expected JSON number", "JsonError");
        double n;
        try {
            n = std::stod(j.text);
        } catch (...) {
            fail("JSON number out of range", "JsonError");
        }
        if (!std::isfinite(n))
            fail("JSON number out of range", "JsonError");
        return n;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_json_bool(Object *value) {
    try {
        auto &j = requireJson(value);
        if (j.kind != Json::Boolean)
            fail("expected JSON boolean", "JsonError");
        return j.text == "true";
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
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
#include <sys/select.h>
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
namespace {
std::string httpPath(const std::string &path) {
    const char *hex = "0123456789ABCDEF";
    std::string out;
    auto digit = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < path.size(); ++i) {
        auto c = static_cast<unsigned char>(path[i]);
        if (c == '%' && i + 2 < path.size() && digit(path[i + 1]) >= 0 && digit(path[i + 2]) >= 0) {
            out += '%';
            out += hex[digit(path[i + 1])];
            out += hex[digit(path[i + 2])];
            i += 2;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   std::string("/-._~!$&'()*+,;=:@").find(static_cast<char>(c)) !=
                       std::string::npos)
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
} // namespace
extern "C" {
int64_t fw_parse_int(Object *s) {
    try {
        int64_t n;
        auto [end, ec] = std::from_chars(s->text.data(), s->text.data() + s->text.size(), n);
        if (ec != std::errc{} || end != s->text.data() + s->text.size())
            fail("invalid or out-of-range integer string", "NumericError");
        return n;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
uint64_t fw_parse_uint(Object *s) {
    try {
        uint64_t n;
        auto [end, ec] = std::from_chars(s->text.data(), s->text.data() + s->text.size(), n);
        if (ec != std::errc{} || end != s->text.data() + s->text.size())
            fail("invalid or out-of-range unsigned integer string", "NumericError");
        return n;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
double fw_parse_real(Object *s) {
    try {
        double n;
        auto [end, ec] = std::from_chars(s->text.data(), s->text.data() + s->text.size(), n);
        if (ec != std::errc{} || end != s->text.data() + s->text.size() || !std::isfinite(n))
            fail("invalid or out-of-range real string", "NumericError");
        return n;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
}
namespace {
bool textKey(const Slot &key) {
    return key.object && key.object->kind == Object::String;
}
void indexKey(Object *map, size_t position) {
    auto &key = map->keys[position];
    if (!map->objectKeys)
        map->index->integers[key.integer] = position;
    else if (textKey(key))
        map->index->strings[key.object->text] = position;
}
// The index is rebuilt whenever its count no longer matches the key vector.
MapIndex &mapIndex(Object *map) {
    if (!map->index || map->index->count != map->keys.size()) {
        map->index = std::make_unique<MapIndex>();
        for (size_t i = 0; i < map->keys.size(); ++i)
            indexKey(map, i);
        map->index->count = map->keys.size();
    }
    return *map->index;
}
int64_t findKey(Object *map, int64_t integer, Object *object) {
    auto &index = mapIndex(map);
    if (!map->objectKeys) {
        auto it = index.integers.find(integer);
        return it == index.integers.end() ? -1 : static_cast<int64_t>(it->second);
    }
    if (object && object->kind == Object::String) {
        auto it = index.strings.find(object->text);
        return it == index.strings.end() ? -1 : static_cast<int64_t>(it->second);
    }
    for (size_t i = 0; i < map->keys.size(); ++i)
        if (fw_equal(map->keys[i].object, object))
            return static_cast<int64_t>(i);
    return -1;
}
size_t appendKey(Object *map, Slot key, Slot value) {
    mapIndex(map);
    map->keys.push_back(std::move(key));
    map->slots.push_back(std::move(value));
    auto position = map->keys.size() - 1;
    indexKey(map, position);
    map->index->count = map->keys.size();
    return position;
}
} // namespace
extern "C" {
Object *fw_map_new(Frame *frame, int64_t objects) {
    try {
        auto p = new Object(Object::Map);
        p->keyKind = static_cast<Slot::Kind>(objects);
        p->objectKeys = objects == 4;
        return own(frame, p);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_map_index(Object *p, int64_t integer, Object *object, int64_t insert) {
    try {
        auto index = findKey(p, integer, object);
        if (index >= 0)
            return index;
        if (!insert)
            fail("map key not found", "KeyError");
        Slot key;
        key.kind = p->keyKind;
        if (p->objectKeys) {
            retain(object);
            key.object = object;
            key.kind = Slot::Reference;
        } else
            key.integer = integer;
        return static_cast<int64_t>(appendKey(p, std::move(key), Slot{}));
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_map_has(Object *p, int64_t integer, Object *object) {
    try {
        return findKey(p, integer, object) >= 0;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_map_remove(Object *p, int64_t integer, Object *object) {
    try {
        auto i = findKey(p, integer, object);
        if (i < 0)
            return 0;
        auto &index = mapIndex(p);
        if (p->objectKeys) {
            if (textKey(p->keys[static_cast<size_t>(i)]))
                index.strings.erase(p->keys[static_cast<size_t>(i)].object->text);
        } else
            index.integers.erase(p->keys[static_cast<size_t>(i)].integer);
        p->keys.erase(p->keys.begin() + i);
        p->slots.erase(p->slots.begin() + i);
        for (auto j = static_cast<size_t>(i); j < p->keys.size(); ++j)
            indexKey(p, j);
        index.count = p->keys.size();
        return 1;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_map_keys(Frame *frame, Object *p) {
    try {
        auto keys = new Object(Object::Array);
        keys->slots = p->keys;
        return own(frame, keys);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
int64_t fw_array_append(Object *p) {
    try {
        p->slots.emplace_back();
        return static_cast<int64_t>(p->slots.size() - 1);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
void fw_array_pop(Object *p) {
    try {
        if (p->slots.empty())
            fail("pop from empty array", "IndexError");
        p->slots.pop_back();
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
void fw_collection_clear(Object *p) {
    try {
        p->slots.clear();
        p->keys.clear();
        p->index.reset();
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return;
    }
}
}

extern "C" {
int64_t fw_error_pending() {
    return pendingError.value != nullptr;
}
int64_t fw_error_matches(const char *type) {
    return pendingError.value && (std::string(type) == "Error" || pendingError.type == type);
}
void fw_throw(const char *type, Object *value) {
    if (std::string(type) == "Error" && value && value->kind == Object::ErrorValue &&
        !value->errorType.empty()) {
        pendingError.set(value->errorType, value->slots.empty() ? value : value->slots[0].object);
    } else
        pendingError.set(type, value);
}
void fw_error_report() {
    if (pendingError.value) {
        std::cerr << "Flywheel runtime error: " << pendingError.type << ": "
                  << (pendingError.value->text.empty() ? "exception" : pendingError.value->text)
                  << '\n';
        pendingError.clear();
    }
}
Object *fw_error_value(Frame *frame) {
    auto value = pendingError.value;
    retain(value);
    pendingError.clear();
    return own(frame, value);
}
Object *fw_error_take(Frame *frame) {
    auto p = new Object(Object::ErrorValue);
    p->errorType = pendingError.type;
    p->text = pendingError.value && !pendingError.value->text.empty()
                  ? pendingError.value->text
                  : "exception of type " + pendingError.type;
    p->slots.emplace_back();
    p->slots[0].object = pendingError.value;
    p->slots[0].kind = Slot::Reference;
    retain(pendingError.value);
    pendingError.clear();
    return own(frame, p);
}
void fw_error_restore(Object *p) {
    pendingError.set(p->errorType, p->slots[0].object);
}
Object *fw_error_message(Frame *frame, Object *p) {
    try {
        return string(frame, p->text);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
Object *fw_error_new(Frame *frame, Object *message, const char *type) {
    try {
        auto p = new Object(Object::ErrorValue);
        p->text = message->text;
        p->errorType = type;
        return own(frame, p);
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return {};
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return {};
    }
}
}

extern "C" int64_t fw_live_objects() {
    return liveObjects.load();
}

extern "C" Object *fw_range(Frame *frame, int64_t start, int64_t end) {
    try {
        if (end < start)
            end = start;
        auto count = static_cast<uint64_t>(end) - static_cast<uint64_t>(start);
        if (count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            fail("range size overflow", "NumericError");
        auto p = new Object(Object::Array);
        own(frame, p);
        p->slots.resize(static_cast<size_t>(count));
        for (size_t i = 0; i < p->slots.size(); ++i)
            p->slots[i].integer = start + static_cast<int64_t>(i);
        return p;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return nullptr;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return nullptr;
    }
}

extern "C" Object *fw_try_read(Frame *frame, Object *path) {
    try {
        auto value = fw_read(frame, path);
        bool ok = !pendingError.value;
        if (!ok) {
            if (pendingError.type != "FileError")
                return nullptr;
            value = fw_error_value(frame);
        }
        auto result = new Object(Object::Result);
        own(frame, result);
        result->slots.resize(2);
        result->slots[0].integer = ok ? 1 : 0;
        result->slots[0].kind = Slot::Boolean;
        result->slots[1].object = value;
        result->slots[1].kind = Slot::Reference;
        retain(value);
        return result;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return nullptr;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return nullptr;
    }
}

#include "collections.inc"

#include "dom.inc"

#include "process.inc"

#include "desktop.inc"

#include "web.inc"
