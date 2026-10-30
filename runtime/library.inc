// Formatting specs, encodings, hashing, compression, UUIDs, secure random numbers, and other
// library functions. Included by runtime.cpp.
namespace {
// Bytes in the UTF-8 sequence that starts with `lead`.
size_t utf8Width(unsigned char lead) {
    return lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
}
// A format spec, as in `{price:.2f}`: [[fill]align][sign][#][0][width][,|_][.precision][type].
struct FormatSpec {
    std::string fill = " ";
    char align = 0, sign = '-', type = 0, grouping = 0;
    bool alternate = false, zero = false;
    int64_t width = -1, precision = -1;
};
FormatSpec parseSpec(const std::string &text) {
    FormatSpec spec;
    size_t i = 0;
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^' || c == '='; };
    // The fill is any one character (possibly multi-byte) followed by an alignment.
    size_t first = text.empty() ? 0 : utf8Width(static_cast<unsigned char>(text[0]));
    if (first && first < text.size() && isAlign(text[first])) {
        spec.fill = text.substr(0, first);
        spec.align = text[first];
        i = first + 1;
    } else if (!text.empty() && isAlign(text[0])) {
        spec.align = text[0];
        i = 1;
    }
    if (i < text.size() && (text[i] == '+' || text[i] == '-' || text[i] == ' '))
        spec.sign = text[i++];
    if (i < text.size() && text[i] == '#') {
        spec.alternate = true;
        ++i;
    }
    if (i < text.size() && text[i] == '0') {
        spec.zero = true;
        ++i;
    }
    auto number = [&]() -> int64_t {
        int64_t n = -1;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            n = (n < 0 ? 0 : n) * 10 + (text[i++] - '0');
            if (n > 1000000)
                fail("format width or precision is too large", "ValueError");
        }
        return n;
    };
    spec.width = number();
    if (i < text.size() && (text[i] == ',' || text[i] == '_'))
        spec.grouping = text[i++];
    if (i < text.size() && text[i] == '.') {
        ++i;
        spec.precision = number();
        if (spec.precision < 0)
            fail("format spec '" + text + "' needs digits after '.'", "ValueError");
    }
    if (i < text.size())
        spec.type = text[i++];
    if (i != text.size())
        fail("invalid format spec '" + text + "'", "ValueError");
    return spec;
}
// Inserts grouping separators every three digits of the integer part of `digits`.
std::string group(const std::string &digits, char separator) {
    if (!separator)
        return digits;
    auto end = digits.find_first_not_of("0123456789");
    if (end == std::string::npos)
        end = digits.size();
    std::string out;
    for (size_t i = 0; i < end; ++i) {
        if (i && (end - i) % 3 == 0)
            out += separator;
        out += digits[i];
    }
    return out + digits.substr(end);
}
// Pads `body` (after `prefix`, which is a sign or base prefix) to the spec's width.
std::string pad(const FormatSpec &spec, const std::string &prefix, const std::string &body,
                char defaultAlign) {
    auto length = static_cast<int64_t>(utf8Length(prefix) + utf8Length(body));
    char align = spec.align ? spec.align : spec.zero ? '=' : defaultAlign;
    std::string fill = spec.align ? spec.fill : spec.zero ? "0" : " ";
    if (spec.width <= length)
        return prefix + body;
    auto missing = spec.width - length;
    auto repeat = [&](int64_t n) {
        std::string out;
        for (int64_t k = 0; k < n; ++k)
            out += fill;
        return out;
    };
    switch (align) {
    case '<':
        return prefix + body + repeat(missing);
    case '^':
        return repeat(missing / 2) + prefix + body + repeat(missing - missing / 2);
    case '=':
        return prefix + repeat(missing) + body;
    default:
        return repeat(missing) + prefix + body;
    }
}
std::string signPrefix(const FormatSpec &spec, bool negative) {
    return negative ? "-" : spec.sign == '+' ? "+" : spec.sign == ' ' ? " " : "";
}
std::string formatInteger(int64_t value, bool isUnsigned, const std::string &text) {
    auto spec = parseSpec(text);
    auto type = spec.type ? spec.type : 'd';
    if (type == 'f' || type == 'F' || type == 'e' || type == 'E' || type == 'g' || type == 'G' ||
        type == '%')
        return std::string(); // handled as a real by the caller
    if (spec.precision >= 0)
        fail("precision is not allowed for integer format '" + text + "'", "ValueError");
    bool negative = !isUnsigned && value < 0;
    uint64_t magnitude = isUnsigned ? static_cast<uint64_t>(value)
                         : negative ? 0 - static_cast<uint64_t>(value)
                                    : static_cast<uint64_t>(value);
    if (type == 'c') {
        std::string out;
        if (negative || magnitude > 0x10ffff)
            fail("character code out of range", "ValueError");
        appendUtf8(out, static_cast<uint32_t>(magnitude));
        return pad(spec, "", out, '<');
    }
    int base = type == 'x' || type == 'X' ? 16 : type == 'o' ? 8 : type == 'b' ? 2 : 10;
    if (base == 10 && type != 'd' && type != 'n')
        fail("unknown integer format type '" + std::string(1, type) + "'", "ValueError");
    std::string digits;
    do {
        auto d = static_cast<int>(magnitude % base);
        digits += static_cast<char>(d < 10 ? '0' + d : (type == 'X' ? 'A' : 'a') + d - 10);
        magnitude /= base;
    } while (magnitude);
    std::reverse(digits.begin(), digits.end());
    if (spec.grouping == ',' && base != 10)
        fail("',' grouping applies to decimal numbers", "ValueError");
    if (spec.grouping == '_' && base != 10) {
        std::string grouped;
        for (size_t i = 0; i < digits.size(); ++i) {
            if (i && (digits.size() - i) % 4 == 0)
                grouped += '_';
            grouped += digits[i];
        }
        digits = grouped;
    } else
        digits = group(digits, spec.grouping);
    std::string prefix = signPrefix(spec, negative);
    if (spec.alternate && base != 10)
        prefix += base == 16 ? (type == 'X' ? "0X" : "0x") : base == 8 ? "0o" : "0b";
    return pad(spec, prefix, digits, '>');
}
std::string formatReal(double value, const std::string &text) {
    auto spec = parseSpec(text);
    auto type = spec.type;
    if (type && !std::strchr("fFeEgG%n", type))
        fail("unknown number format type '" + std::string(1, type) + "'", "ValueError");
    bool negative = std::signbit(value) && !std::isnan(value);
    double magnitude = std::fabs(value);
    std::string body;
    if (std::isnan(value) || std::isinf(value))
        body = std::isnan(value) ? "nan" : "inf";
    else if (!type) {
        // Without a type: the shortest round-trip text, or fixed precision when given.
        if (spec.precision >= 0) {
            std::ostringstream out;
            out << std::setprecision(static_cast<int>(std::max<int64_t>(spec.precision, 1)))
                << magnitude;
            body = out.str();
        } else {
            char buffer[64];
            auto result = std::to_chars(buffer, buffer + sizeof buffer, magnitude);
            body.assign(buffer, result.ptr);
            if (body.find_first_of(".en") == std::string::npos)
                body += ".0";
        }
    } else {
        auto precision = static_cast<int>(spec.precision < 0 ? 6 : spec.precision);
        std::ostringstream out;
        out.imbue(std::locale::classic());
        if (type == '%') {
            magnitude *= 100;
            out << std::fixed << std::setprecision(precision) << magnitude << '%';
        } else if (type == 'f' || type == 'F')
            out << std::fixed << std::setprecision(precision) << magnitude;
        else if (type == 'e' || type == 'E') {
            out << std::scientific << std::setprecision(precision) << magnitude;
        } else {
            out << std::setprecision(std::max(precision, 1)) << magnitude;
        }
        body = out.str();
        if (type == 'E' || type == 'G')
            for (auto &c : body)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    body = group(body, spec.grouping);
    return pad(spec, signPrefix(spec, negative), body, '>');
}
std::string formatText(const std::string &value, const std::string &text) {
    auto spec = parseSpec(text);
    if (spec.type && spec.type != 's')
        fail("format type '" + std::string(1, spec.type) + "' needs a number", "ValueError");
    if (spec.sign != '-' || spec.grouping || spec.alternate)
        fail("sign, grouping, and '#' apply to numbers", "ValueError");
    auto body = value;
    if (spec.precision >= 0) {
        // Precision truncates text to that many characters.
        size_t at = 0;
        for (int64_t n = 0; at < body.size() && n < spec.precision; ++n)
            at += utf8Width(static_cast<unsigned char>(body[at]));
        body.resize(std::min(at, body.size()));
    }
    return pad(spec, "", body, '<');
}
} // namespace

extern "C" {
Object *fw_format_int(Frame *frame, int64_t value, int64_t isUnsigned, Object *spec) {
    return nativeBoundary([&]() -> Object * {
        auto &text = requireString(spec)->text;
        auto out = formatInteger(value, isUnsigned != 0, text);
        if (out.empty() && !text.empty())
            out = formatReal(isUnsigned ? static_cast<double>(static_cast<uint64_t>(value))
                                        : static_cast<double>(value),
                             text);
        return string(frame, out);
    });
}
Object *fw_format_real(Frame *frame, double value, Object *spec) {
    return nativeBoundary([&]() -> Object * {
        auto &text = requireString(spec)->text;
        auto parsed = parseSpec(text);
        if (parsed.type && std::strchr("dxXobc", parsed.type)) {
            if (!std::isfinite(value) || value != std::trunc(value))
                fail("format type '" + std::string(1, parsed.type) + "' needs a whole number",
                     "ValueError");
            return string(frame, formatInteger(static_cast<int64_t>(value), false, text));
        }
        return string(frame, formatReal(value, text));
    });
}
Object *fw_format_text(Frame *frame, Object *value, Object *spec) {
    return nativeBoundary([&]() -> Object * {
        return string(frame, formatText(requireString(value)->text, requireString(spec)->text));
    });
}
// Destructuring `var (a, b) = items` requires exactly `count` elements.
void fw_check_length(Object *items, int64_t count) {
    nativeBoundary([&] {
        if (!items)
            fail("cannot destructure null", "ValueError");
        ObjectGuard guard(items);
        auto size = static_cast<int64_t>(items->slots.size());
        if (size != count)
            fail("cannot destructure " + std::to_string(size) + " element(s) into " +
                     std::to_string(count) + " name(s)",
                 "ValueError");
    });
}
}
namespace {
// Equality of enum-with-data members: the same member with equal fields, compared deeply.
bool deepEqual(Object *a, Object *b) {
    if (a == b)
        return true;
    if (!a || !b || a->kind != b->kind)
        return false;
    if (a->kind == Object::String)
        return a->text == b->text;
    if (a->kind == Object::Instance && a->classId != b->classId)
        return false;
    if (a->kind != Object::Instance && a->kind != Object::Tuple && a->kind != Object::Array &&
        a->kind != Object::Box)
        return false;
    // Copy each side's slots under its own lock, never holding two locks at once.
    std::vector<Slot> left, right;
    {
        ObjectGuard guard(a);
        left = a->slots;
    }
    {
        ObjectGuard guard(b);
        right = b->slots;
    }
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        auto &x = left[i], &y = right[i];
        if (x.kind == Slot::Reference || y.kind == Slot::Reference) {
            if (x.kind != y.kind || !deepEqual(x.object, y.object))
                return false;
        } else if (!structuralEqual(x, y))
            return false;
    }
    return true;
}
} // namespace
extern "C" int64_t fw_variant_equal(Object *a, Object *b) {
    return nativeBoundary([&]() -> int64_t { return deepEqual(a, b); });
}
extern "C" int64_t fw_string_order(Object *a, Object *b) {
    return nativeBoundary([&]() -> int64_t {
        auto c = requireString(a)->text.compare(requireString(b)->text);
        return c < 0 ? -1 : c > 0;
    });
}
// freeze (1) marks a collection immutable; 0 only reports whether it is.
extern "C" int64_t fw_freeze(Object *p, int64_t freeze) {
    return nativeBoundary([&]() -> int64_t {
        if (!p)
            fail("null collection");
        if (freeze)
            p->frozenFlag.store(true, std::memory_order_release);
        return p->frozenFlag.load(std::memory_order_acquire);
    });
}

// hashing, encodings, secure random numbers, UUIDs ---------------------------------------------
#if defined(__linux__)
#include <sys/random.h>
#endif
namespace {
uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}
uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
std::string sha256(const std::string &message) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string data = message;
    uint64_t bits = static_cast<uint64_t>(message.size()) * 8;
    data += static_cast<char>(0x80);
    while (data.size() % 64 != 56)
        data += '\0';
    for (int i = 7; i >= 0; --i)
        data += static_cast<char>(bits >> (i * 8));
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(data[chunk + i * 4])) << 24) |
                   (uint32_t(uint8_t(data[chunk + i * 4 + 1])) << 16) |
                   (uint32_t(uint8_t(data[chunk + i * 4 + 2])) << 8) |
                   uint32_t(uint8_t(data[chunk + i * 4 + 3]));
        for (int i = 16; i < 64; ++i) {
            auto s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            auto s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            auto s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            auto ch = (e & f) ^ (~e & g);
            auto t1 = hh + s1 + ch + k[i] + w[i];
            auto s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            auto maj = (a & b) ^ (a & c) ^ (b & c);
            auto t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    std::string out;
    for (auto v : h)
        for (int i = 3; i >= 0; --i)
            out += static_cast<char>(v >> (i * 8));
    return out;
}
std::string sha1(const std::string &message) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string data = message;
    uint64_t bits = static_cast<uint64_t>(message.size()) * 8;
    data += static_cast<char>(0x80);
    while (data.size() % 64 != 56)
        data += '\0';
    for (int i = 7; i >= 0; --i)
        data += static_cast<char>(bits >> (i * 8));
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(data[chunk + i * 4])) << 24) |
                   (uint32_t(uint8_t(data[chunk + i * 4 + 1])) << 16) |
                   (uint32_t(uint8_t(data[chunk + i * 4 + 2])) << 8) |
                   uint32_t(uint8_t(data[chunk + i * 4 + 3]));
        for (int i = 16; i < 80; ++i)
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)
                f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40)
                f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60)
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else
                f = b ^ c ^ d, k = 0xCA62C1D6;
            auto t = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out;
    for (auto v : h)
        for (int i = 3; i >= 0; --i)
            out += static_cast<char>(v >> (i * 8));
    return out;
}
std::string md5(const std::string &message) {
    static const uint32_t s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t k[64];
    for (int i = 0; i < 64; ++i)
        k[i] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0));
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::string data = message;
    uint64_t bits = static_cast<uint64_t>(message.size()) * 8;
    data += static_cast<char>(0x80);
    while (data.size() % 64 != 56)
        data += '\0';
    for (int i = 0; i < 8; ++i)
        data += static_cast<char>(bits >> (i * 8));
    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = uint32_t(uint8_t(data[chunk + i * 4])) |
                   (uint32_t(uint8_t(data[chunk + i * 4 + 1])) << 8) |
                   (uint32_t(uint8_t(data[chunk + i * 4 + 2])) << 16) |
                   (uint32_t(uint8_t(data[chunk + i * 4 + 3])) << 24);
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16)
                f = (b & c) | (~b & d), g = i;
            else if (i < 32)
                f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48)
                f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else
                f = c ^ (b | ~d), g = (7 * i) % 16;
            f = f + a + k[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + rotl32(f, static_cast<int>(s[i]));
        }
        a0 += a, b0 += b, c0 += c, d0 += d;
    }
    std::string out;
    for (auto v : {a0, b0, c0, d0})
        for (int i = 0; i < 4; ++i)
            out += static_cast<char>(v >> (i * 8));
    return out;
}
std::string hmac(const std::string &key, const std::string &message,
                 std::string (*hash)(const std::string &)) {
    auto k = key.size() > 64 ? hash(key) : key;
    k.resize(64, '\0');
    std::string inner(64, '\0'), outer(64, '\0');
    for (int i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(k[i] ^ 0x36);
        outer[i] = static_cast<char>(k[i] ^ 0x5c);
    }
    return hash(outer + hash(inner + message));
}
std::string hexEncode(const std::string &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
std::string hexDecode(const std::string &text) {
    if (text.size() % 2)
        fail("hex text must have an even number of digits", "ValueError");
    std::string out;
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi = hexDigit(text[i]), lo = hexDigit(text[i + 1]);
        if (hi < 0 || lo < 0)
            fail("invalid hex digit in '" + text.substr(i, 2) + "'", "ValueError");
        out += static_cast<char>(hi * 16 + lo);
    }
    return out;
}
std::string base64Encode(const std::string &bytes, bool url) {
    const char *alphabet = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                               : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
        for (int s = 18; s >= 0; s -= 6)
            out += alphabet[(v >> s) & 63];
    }
    if (i < bytes.size()) {
        uint32_t v = uint8_t(bytes[i]) << 16;
        if (i + 1 < bytes.size())
            v |= uint8_t(bytes[i + 1]) << 8;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        if (i + 1 < bytes.size())
            out += alphabet[(v >> 6) & 63];
        else if (!url)
            out += '=';
        if (!url)
            out += '=';
    }
    return out;
}
std::string base64Decode(const std::string &text) {
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    size_t padding = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+' || c == '-')
            v = 62;
        else if (c == '/' || c == '_')
            v = 63;
        else if (c == '=') {
            ++padding;
            continue;
        } else if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        else
            fail(std::string("invalid base64 character '") + c + "'", "ValueError");
        if (padding)
            fail("base64 data continues after '='", "ValueError");
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xff);
        }
    }
    if (bits >= 6)
        fail("truncated base64 data", "ValueError");
    return out;
}
std::string percentEncode(const std::string &text, bool form) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else if (form && c == ' ')
            out += '+';
        else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}
std::string strictPercentDecode(const std::string &text, bool form) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hexDigit(text[i + 1]) >= 0 &&
            hexDigit(text[i + 2]) >= 0) {
            out += static_cast<char>(hexDigit(text[i + 1]) * 16 + hexDigit(text[i + 2]));
            i += 2;
        } else if (text[i] == '%')
            fail("invalid percent escape in '" + text + "'", "ValueError");
        else
            out += form && text[i] == '+' ? ' ' : text[i];
    }
    return out;
}
// Cryptographically secure random bytes from the operating system.
std::string secureBytes(size_t count) {
    std::string out(count, '\0');
#ifdef _WIN32
    // RtlGenRandom (exported as SystemFunction036), the source rand_s uses.
    using GenRandom = unsigned char(__stdcall *)(void *, unsigned long);
    static auto generate = reinterpret_cast<GenRandom>(reinterpret_cast<void *>(
        GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036")));
    if (!generate || (count && !generate(out.data(), static_cast<unsigned long>(count))))
        fail("secure random source failed");
#elif defined(__linux__)
    for (size_t done = 0; done < count;) {
        auto n = getrandom(out.data() + done, count - done, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail("secure random source failed");
        }
        done += static_cast<size_t>(n);
    }
#else
    std::ifstream source("/dev/urandom", std::ios::binary);
    if (!source.read(out.data(), static_cast<std::streamsize>(count)))
        fail("secure random source failed");
#endif
    return out;
}
uint64_t secureBelow(uint64_t bound) {
    // Rejection sampling avoids modulo bias.
    uint64_t limit = UINT64_MAX - UINT64_MAX % bound;
    while (true) {
        uint64_t v;
        auto bytes = secureBytes(8);
        std::memcpy(&v, bytes.data(), 8);
        if (v < limit)
            return v % bound;
    }
}
std::string uuidText(std::string bytes) {
    auto hex = hexEncode(bytes);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20);
}
} // namespace
extern "C" {
// 0 sha256, 1 sha1, 2 md5; hex digest unless raw; hmac when key is not null.
Object *fw_hash(Frame *frame, Object *text, int64_t algorithm, int64_t raw, Object *key) {
    return nativeBoundary([&] {
        auto &data = requireString(text)->text;
        std::string (*hash)(const std::string &) = algorithm == 0 ? sha256 : algorithm == 1 ? sha1 : md5;
        auto digest = key ? hmac(requireString(key)->text, data, hash) : hash(data);
        return string(frame, raw ? digest : hexEncode(digest));
    });
}
// 0 base64, 1 base64url (unpadded), 2 hex, 3 percent (URL component), 4 form (+ for space)
Object *fw_encode(Frame *frame, Object *text, int64_t format, int64_t decode) {
    return nativeBoundary([&] {
        auto &data = requireString(text)->text;
        std::string out;
        switch (format) {
        case 0:
        case 1:
            out = decode ? base64Decode(data) : base64Encode(data, format == 1);
            break;
        case 2:
            out = decode ? hexDecode(data) : hexEncode(data);
            break;
        default:
            out = decode ? strictPercentDecode(data, format == 4) : percentEncode(data, format == 4);
        }
        return string(frame, out);
    });
}
// Constant-time comparison for secrets such as tokens and MACs.
int64_t fw_secure_equal(Object *a, Object *b) {
    return nativeBoundary([&]() -> int64_t {
        auto &x = requireString(a)->text, &y = requireString(b)->text;
        unsigned char difference = x.size() != y.size();
        for (size_t i = 0; i < x.size() && i < y.size(); ++i)
            difference |= static_cast<unsigned char>(x[i] ^ y[i]);
        return difference == 0;
    });
}
Object *fw_random_bytes(Frame *frame, int64_t count, int64_t hex) {
    return nativeBoundary([&] {
        if (count < 0 || count > (1 << 26))
            fail("byte count must be in 0..67108864", "ValueError");
        auto bytes = secureBytes(static_cast<size_t>(count));
        return string(frame, hex ? hexEncode(bytes) : bytes);
    });
}
int64_t fw_secure_int(int64_t low, int64_t high) {
    return nativeBoundary([&]() -> int64_t {
        if (low > high)
            fail("random_int requires low <= high", "ValueError");
        auto span = static_cast<uint64_t>(high) - static_cast<uint64_t>(low);
        if (span == UINT64_MAX) {
            int64_t v;
            auto bytes = secureBytes(8);
            std::memcpy(&v, bytes.data(), 8);
            return v;
        }
        return static_cast<int64_t>(static_cast<uint64_t>(low) + secureBelow(span + 1));
    });
}
// version 4 (random) or 7 (Unix time in milliseconds, then random)
Object *fw_uuid(Frame *frame, int64_t version) {
    return nativeBoundary([&] {
        auto bytes = secureBytes(16);
        if (version == 7) {
            auto ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
            for (int i = 0; i < 6; ++i)
                bytes[i] = static_cast<char>(ms >> (8 * (5 - i)));
        }
        bytes[6] = static_cast<char>((bytes[6] & 0x0f) | (version << 4));
        bytes[8] = static_cast<char>((bytes[8] & 0x3f) | 0x80);
        return string(frame, uuidText(bytes));
    });
}
int64_t fw_uuid_valid(Object *text) {
    return nativeBoundary([&]() -> int64_t {
        auto &s = requireString(text)->text;
        if (s.size() != 36)
            return 0;
        for (size_t i = 0; i < 36; ++i) {
            bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? s[i] != '-' : hexDigit(s[i]) < 0)
                return 0;
        }
        return 1;
    });
}
}

// compression: DEFLATE (RFC 1951), gzip (RFC 1952), zlib (RFC 1950), zip archives -----------
namespace {
uint32_t crc32(const std::string &data, uint32_t crc = 0) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    });
    crc = ~crc;
    for (unsigned char b : data)
        crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}
uint32_t adler32(const std::string &data) {
    uint32_t a = 1, b = 0;
    for (unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}
const uint16_t lengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t distanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                   33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// Decoder for one canonical Huffman code: symbol counts per length and symbols in order.
struct Huffman {
    uint16_t counts[16] = {};
    std::vector<uint16_t> symbols;
    void build(const uint8_t *lengths, size_t n) {
        std::fill(std::begin(counts), std::end(counts), 0);
        for (size_t i = 0; i < n; ++i)
            ++counts[lengths[i]];
        counts[0] = 0;
        uint16_t offsets[16] = {};
        for (int i = 1; i < 16; ++i)
            offsets[i] = offsets[i - 1] + counts[i - 1];
        symbols.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (lengths[i])
                symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
    }
};
struct Inflater {
    const std::string &in;
    size_t position = 0;
    uint32_t bitBuffer = 0;
    int bitCount = 0;
    std::string out;
    size_t limit;
    Inflater(const std::string &data, size_t start, size_t limit) : in(data), position(start), limit(limit) {}
    int bit() {
        if (!bitCount) {
            if (position >= in.size())
                fail("compressed data is truncated", "ValueError");
            bitBuffer = static_cast<uint8_t>(in[position++]);
            bitCount = 8;
        }
        int b = bitBuffer & 1;
        bitBuffer >>= 1;
        --bitCount;
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v |= static_cast<uint32_t>(bit()) << i;
        return v;
    }
    int decode(const Huffman &h) {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length < 16; ++length) {
            code |= bit();
            int count = h.counts[length];
            if (code - count < first)
                return h.symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        fail("invalid Huffman code in compressed data", "ValueError");
    }
    void codes(const Huffman &literal, const Huffman &distance) {
        while (true) {
            int symbol = decode(literal);
            if (symbol < 256) {
                out += static_cast<char>(symbol);
                if (out.size() > limit)
                    fail("decompressed data exceeds the size limit", "ValueError");
            } else if (symbol == 256)
                return;
            else {
                symbol -= 257;
                if (symbol >= 29)
                    fail("invalid length code in compressed data", "ValueError");
                size_t length = lengthBase[symbol] + bits(lengthExtra[symbol]);
                int d = decode(distance);
                if (d >= 30)
                    fail("invalid distance code in compressed data", "ValueError");
                size_t back = distanceBase[d] + bits(distanceExtra[d]);
                if (back > out.size())
                    fail("invalid distance in compressed data", "ValueError");
                if (out.size() + length > limit)
                    fail("decompressed data exceeds the size limit", "ValueError");
                size_t from = out.size() - back;
                for (size_t i = 0; i < length; ++i)
                    out += out[from + i];
            }
        }
    }
    void run() {
        bool last;
        do {
            last = bit();
            int type = static_cast<int>(bits(2));
            if (type == 0) {
                bitCount = 0;
                if (position + 4 > in.size())
                    fail("compressed data is truncated", "ValueError");
                size_t length = uint8_t(in[position]) | (uint8_t(in[position + 1]) << 8);
                position += 4;
                if (position + length > in.size())
                    fail("compressed data is truncated", "ValueError");
                out.append(in, position, length);
                position += length;
            } else if (type == 1) {
                static Huffman literal, distance;
                static std::once_flag once;
                std::call_once(once, [] {
                    uint8_t lengths[288];
                    for (int i = 0; i < 288; ++i)
                        lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                    literal.build(lengths, 288);
                    uint8_t d[30];
                    std::fill(std::begin(d), std::end(d), 5);
                    distance.build(d, 30);
                });
                codes(literal, distance);
            } else if (type == 2) {
                size_t nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                  11, 4,  12, 3, 13, 2, 14, 1, 15};
                uint8_t lengths[320] = {};
                for (size_t i = 0; i < ncode; ++i)
                    lengths[order[i]] = static_cast<uint8_t>(bits(3));
                Huffman lencode;
                lencode.build(lengths, 19);
                std::fill(std::begin(lengths), std::end(lengths), 0);
                for (size_t i = 0; i < nlen + ndist;) {
                    int symbol = decode(lencode);
                    if (symbol < 16)
                        lengths[i++] = static_cast<uint8_t>(symbol);
                    else {
                        uint8_t value = 0;
                        size_t repeat;
                        if (symbol == 16) {
                            if (!i)
                                fail("invalid repeat in compressed data", "ValueError");
                            value = lengths[i - 1];
                            repeat = 3 + bits(2);
                        } else
                            repeat = symbol == 17 ? 3 + bits(3) : 11 + bits(7);
                        if (i + repeat > nlen + ndist)
                            fail("invalid code lengths in compressed data", "ValueError");
                        while (repeat--)
                            lengths[i++] = value;
                    }
                }
                Huffman literal, distance;
                literal.build(lengths, nlen);
                distance.build(lengths + nlen, ndist);
                codes(literal, distance);
            } else
                fail("invalid block type in compressed data", "ValueError");
        } while (!last);
    }
};
struct BitWriter {
    std::string out;
    uint32_t buffer = 0;
    int count = 0;
    void put(uint32_t value, int n) {
        buffer |= value << count;
        count += n;
        while (count >= 8) {
            out += static_cast<char>(buffer & 0xff);
            buffer >>= 8;
            count -= 8;
        }
    }
    // Huffman codes are written most significant bit first.
    void code(uint32_t value, int n) {
        uint32_t reversed = 0;
        for (int i = 0; i < n; ++i)
            reversed |= ((value >> i) & 1) << (n - 1 - i);
        put(reversed, n);
    }
    void flush() {
        if (count)
            out += static_cast<char>(buffer & 0xff);
        buffer = 0;
        count = 0;
    }
};
void fixedLiteral(BitWriter &w, int symbol) {
    if (symbol < 144)
        w.code(0x30 + symbol, 8);
    else if (symbol < 256)
        w.code(0x190 + symbol - 144, 9);
    else if (symbol < 280)
        w.code(symbol - 256, 7);
    else
        w.code(0xc0 + symbol - 280, 8);
}
// DEFLATE with LZ77 (hash chains over a 32 KiB window) and the fixed Huffman code.
std::string deflate(const std::string &data, int level) {
    BitWriter w;
    if (level == 0 || data.empty()) {
        size_t i = 0;
        do {
            size_t n = std::min<size_t>(65535, data.size() - i);
            bool last = i + n >= data.size();
            w.put(last, 1);
            w.put(0, 2);
            w.flush();
            w.out += static_cast<char>(n & 0xff);
            w.out += static_cast<char>(n >> 8);
            w.out += static_cast<char>(~n & 0xff);
            w.out += static_cast<char>((~n >> 8) & 0xff);
            w.out.append(data, i, n);
            i += n;
        } while (i < data.size());
        return w.out;
    }
    w.put(1, 1);
    w.put(1, 2);
    const size_t window = 32768, hashSize = 1 << 15;
    int chainLimit = level >= 9 ? 1024 : level >= 6 ? 128 : 16;
    std::vector<int64_t> head(hashSize, -1), previous(data.size(), -1);
    auto hashAt = [&](size_t i) {
        return ((uint8_t(data[i]) << 10) ^ (uint8_t(data[i + 1]) << 5) ^ uint8_t(data[i + 2])) &
               (hashSize - 1);
    };
    auto insert = [&](size_t i) {
        if (i + 2 < data.size()) {
            auto h = hashAt(i);
            previous[i] = head[h];
            head[h] = static_cast<int64_t>(i);
        }
    };
    for (size_t i = 0; i < data.size();) {
        size_t bestLength = 0, bestDistance = 0;
        if (i + 2 < data.size()) {
            int chain = chainLimit;
            for (auto candidate = head[hashAt(i)];
                 candidate >= 0 && i - static_cast<size_t>(candidate) <= window && chain--;
                 candidate = previous[candidate]) {
                size_t length = 0, c = static_cast<size_t>(candidate);
                while (length < 258 && i + length < data.size() && data[c + length] == data[i + length])
                    ++length;
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = i - c;
                    if (length == 258)
                        break;
                }
            }
        }
        if (bestLength >= 3) {
            int l = 28;
            while (lengthBase[l] > bestLength)
                --l;
            fixedLiteral(w, 257 + l);
            w.put(static_cast<uint32_t>(bestLength - lengthBase[l]), lengthExtra[l]);
            int d = 29;
            while (distanceBase[d] > bestDistance)
                --d;
            w.code(static_cast<uint32_t>(d), 5);
            w.put(static_cast<uint32_t>(bestDistance - distanceBase[d]), distanceExtra[d]);
            for (size_t k = 0; k < bestLength; ++k)
                insert(i + k);
            i += bestLength;
        } else {
            fixedLiteral(w, uint8_t(data[i]));
            insert(i);
            ++i;
        }
    }
    fixedLiteral(w, 256);
    w.flush();
    return w.out;
}
constexpr size_t inflateLimit = size_t(1) << 31;
std::string inflate(const std::string &data, size_t start = 0, size_t *end = nullptr) {
    Inflater inflater(data, start, inflateLimit);
    inflater.run();
    if (end)
        *end = inflater.position;
    return std::move(inflater.out);
}
void putLittle(std::string &out, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i)
        out += static_cast<char>(value >> (8 * i));
}
uint64_t getLittle(const std::string &in, size_t at, int bytes) {
    if (at + bytes > in.size())
        fail("archive data is truncated", "ValueError");
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v |= static_cast<uint64_t>(uint8_t(in[at + i])) << (8 * i);
    return v;
}
std::string gzip(const std::string &data, int level) {
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    out += deflate(data, level);
    putLittle(out, crc32(data), 4);
    putLittle(out, data.size() & 0xffffffffu, 4);
    return out;
}
std::string gunzip(const std::string &data) {
    std::string out;
    size_t at = 0;
    // Concatenated gzip members decompress to the concatenation of their data.
    while (at < data.size()) {
        if (data.size() - at < 18 || uint8_t(data[at]) != 0x1f || uint8_t(data[at + 1]) != 0x8b ||
            data[at + 2] != 8)
            fail("not gzip data", "ValueError");
        int flags = uint8_t(data[at + 3]);
        size_t p = at + 10;
        if (flags & 4)
            p += 2 + getLittle(data, p, 2);
        for (int f : {8, 16})
            if (flags & f) {
                while (p < data.size() && data[p])
                    ++p;
                ++p;
            }
        if (flags & 2)
            p += 2;
        size_t end;
        auto member = inflate(data, p, &end);
        if (getLittle(data, end, 4) != crc32(member))
            fail("gzip checksum mismatch", "ValueError");
        out += member;
        at = end + 8;
    }
    return out;
}
std::string zlibCompress(const std::string &data, int level) {
    std::string out("\x78\x9c", 2);
    out += deflate(data, level);
    auto a = adler32(data);
    for (int i = 3; i >= 0; --i)
        out += static_cast<char>(a >> (8 * i));
    return out;
}
std::string zlibDecompress(const std::string &data) {
    if (data.size() < 6 || (uint8_t(data[0]) & 0x0f) != 8 ||
        ((uint8_t(data[0]) << 8) | uint8_t(data[1])) % 31 != 0)
        fail("not zlib data", "ValueError");
    size_t end;
    auto out = inflate(data, 2, &end);
    if (end + 4 > data.size())
        fail("zlib data is truncated", "ValueError");
    uint32_t stored = (uint8_t(data[end]) << 24) | (uint8_t(data[end + 1]) << 16) |
                      (uint8_t(data[end + 2]) << 8) | uint8_t(data[end + 3]);
    if (stored != adler32(out))
        fail("zlib checksum mismatch", "ValueError");
    return out;
}
// A zip archive of (name, data) entries, each deflated (or stored when that is smaller).
std::string zipArchive(const std::vector<std::pair<std::string, std::string>> &entries) {
    std::string out, directory;
    for (auto &[name, data] : entries) {
        auto compressed = deflate(data, 6);
        bool store = compressed.size() >= data.size();
        auto &body = store ? data : compressed;
        auto crc = crc32(data);
        size_t offset = out.size();
        auto header = [&](std::string &to, bool central) {
            putLittle(to, central ? 0x02014b50 : 0x04034b50, 4);
            if (central)
                putLittle(to, 20, 2); // made by
            putLittle(to, 20, 2);     // version needed
            putLittle(to, 0x0800, 2); // UTF-8 names
            putLittle(to, store ? 0 : 8, 2);
            putLittle(to, 0, 4); // time and date
            putLittle(to, crc, 4);
            putLittle(to, body.size(), 4);
            putLittle(to, data.size(), 4);
            putLittle(to, name.size(), 2);
            putLittle(to, 0, 2);
            if (central) {
                putLittle(to, 0, 2); // comment
                putLittle(to, 0, 2); // disk
                putLittle(to, 0, 2); // internal attributes
                putLittle(to, 0, 4); // external attributes
                putLittle(to, offset, 4);
            }
            to += name;
        };
        if (data.size() > 0xffffffffu || out.size() > 0xffffffffu)
            fail("zip archives over 4 GiB are not supported", "ValueError");
        header(out, false);
        out += body;
        header(directory, true);
    }
    size_t start = out.size();
    out += directory;
    putLittle(out, 0x06054b50, 4);
    putLittle(out, 0, 4);
    putLittle(out, entries.size(), 2);
    putLittle(out, entries.size(), 2);
    putLittle(out, directory.size(), 4);
    putLittle(out, start, 4);
    putLittle(out, 0, 2);
    return out;
}
std::vector<std::pair<std::string, std::string>> unzipArchive(const std::string &data) {
    // The end-of-central-directory record is in the last 64 KiB + 22 bytes.
    if (data.size() < 22)
        fail("not a zip archive", "ValueError");
    size_t end = std::string::npos;
    for (size_t i = data.size() - 22 + 1; i-- > 0 && data.size() - i <= 65557;)
        if (getLittle(data, i, 4) == 0x06054b50) {
            end = i;
            break;
        }
    if (end == std::string::npos)
        fail("not a zip archive", "ValueError");
    auto count = getLittle(data, end + 10, 2);
    size_t p = getLittle(data, end + 16, 4);
    std::vector<std::pair<std::string, std::string>> entries;
    for (uint64_t n = 0; n < count; ++n) {
        if (getLittle(data, p, 4) != 0x02014b50)
            fail("corrupt zip central directory", "ValueError");
        auto method = getLittle(data, p + 10, 2);
        auto crc = static_cast<uint32_t>(getLittle(data, p + 16, 4));
        auto compressedSize = getLittle(data, p + 20, 4);
        auto nameLength = getLittle(data, p + 28, 2), extra = getLittle(data, p + 30, 2),
             comment = getLittle(data, p + 32, 2);
        auto local = getLittle(data, p + 42, 4);
        if (p + 46 + nameLength > data.size())
            fail("corrupt zip central directory", "ValueError");
        std::string name = data.substr(p + 46, nameLength);
        p += 46 + nameLength + extra + comment;
        if (getLittle(data, local, 4) != 0x04034b50)
            fail("corrupt zip entry '" + name + "'", "ValueError");
        size_t body = local + 30 + getLittle(data, local + 26, 2) + getLittle(data, local + 28, 2);
        if (body + compressedSize > data.size())
            fail("zip entry '" + name + "' is truncated", "ValueError");
        std::string content;
        if (method == 0)
            content = data.substr(body, compressedSize);
        else if (method == 8)
            content = inflate(data.substr(body, compressedSize));
        else
            fail("zip entry '" + name + "' uses unsupported compression method " +
                     std::to_string(method),
                 "ValueError");
        if (crc32(content) != crc)
            fail("zip entry '" + name + "' fails its checksum", "ValueError");
        if (!name.empty() && name.back() == '/')
            continue; // directory entry
        entries.emplace_back(std::move(name), std::move(content));
    }
    return entries;
}
} // namespace
extern "C" {
// format: 0 gzip, 1 zlib, 2 raw deflate
Object *fw_compress(Frame *frame, Object *text, int64_t format, int64_t level) {
    return nativeBoundary([&] {
        if (level < 0 || level > 9)
            fail("compression level must be in 0..9", "ValueError");
        auto &data = requireString(text)->text;
        int l = static_cast<int>(level);
        return string(frame, format == 0   ? gzip(data, l)
                             : format == 1 ? zlibCompress(data, l)
                                           : deflate(data, l));
    });
}
Object *fw_decompress(Frame *frame, Object *text, int64_t format) {
    return nativeBoundary([&] {
        auto &data = requireString(text)->text;
        return string(frame, format == 0   ? gunzip(data)
                             : format == 1 ? zlibDecompress(data)
                                           : inflate(data));
    });
}
int64_t fw_crc32(Object *text) {
    return nativeBoundary([&]() -> int64_t { return crc32(requireString(text)->text); });
}
Object *fw_zip(Frame *frame, Object *files) {
    return nativeBoundary([&] {
        std::vector<std::pair<std::string, std::string>> entries;
        {
            ObjectGuard guard(files);
            for (size_t i = 0; i < files->keys.size(); ++i)
                entries.emplace_back(requireString(files->keys[i].object)->text,
                                     requireString(files->slots[i].object)->text);
        }
        return string(frame, zipArchive(entries));
    });
}
Object *fw_unzip(Frame *frame, Object *data) {
    return nativeBoundary([&] {
        auto entries = unzipArchive(requireString(data)->text);
        auto map = stringMap(frame);
        for (auto &[name, content] : entries)
            put(frame, map, name, content);
        return map;
    });
}
}

// HTTP client (web.get, web.post, web.fetch) ---------------------------------------------------
namespace {
struct Url {
    std::string scheme, host, path; // path includes the query
    int64_t port = 0;
};
Url parseUrl(const std::string &text) {
    Url url;
    auto colon = text.find("://");
    if (colon == std::string::npos)
        fail("URL needs a scheme such as https:// ('" + text + "')", "HttpError");
    url.scheme = lower(text.substr(0, colon));
    if (url.scheme != "http" && url.scheme != "https")
        fail("unsupported URL scheme '" + url.scheme + "'", "HttpError");
    auto rest = text.substr(colon + 3);
    auto slash = rest.find_first_of("/?#");
    auto authority = rest.substr(0, slash);
    url.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (auto hash = url.path.find('#'); hash != std::string::npos)
        url.path.resize(hash);
    if (url.path.empty() || url.path[0] == '?')
        url.path = "/" + url.path;
    if (auto at = authority.rfind('@'); at != std::string::npos)
        authority = authority.substr(at + 1); // credentials in URLs are not sent
    url.port = url.scheme == "https" ? 443 : 80;
    std::string portText;
    if (!authority.empty() && authority[0] == '[') {
        auto close = authority.find(']');
        if (close == std::string::npos)
            fail("invalid IPv6 host in URL '" + text + "'", "HttpError");
        url.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':')
            portText = authority.substr(close + 2);
    } else {
        auto p = authority.rfind(':');
        url.host = authority.substr(0, p);
        if (p != std::string::npos)
            portText = authority.substr(p + 1);
    }
    if (!portText.empty()) {
        try {
            size_t used;
            url.port = std::stoll(portText, &used);
            if (used != portText.size() || url.port < 1 || url.port > 65535)
                throw std::out_of_range("port");
        } catch (const std::exception &) {
            fail("invalid port in URL '" + text + "'", "HttpError");
        }
    }
    if (url.host.empty())
        fail("URL has no host ('" + text + "')", "HttpError");
    return url;
}
// Client-side OpenSSL functions, from the library tls() loads.
struct TlsClient {
    void *(*clientMethod)() = nullptr;
    int (*defaultPaths)(void *) = nullptr;
    void (*setVerify)(void *, int, void *) = nullptr;
    long (*control)(void *, int, long, void *) = nullptr;
    int (*setHost)(void *, const char *) = nullptr;
    int (*connect)(void *) = nullptr;
    long (*verifyResult)(const void *) = nullptr;
    void *context = nullptr;
};
TlsClient &tlsClient() {
    static TlsClient api;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (api.context)
        return api;
    auto &base = tls();
    auto bind = [&](auto &target, const char *name) {
#ifdef _WIN32
        auto found = reinterpret_cast<void *>(
            GetProcAddress(static_cast<HMODULE>(base.library), name));
#else
        auto found = dlsym(base.library, name);
#endif
        if (!found)
            fail(std::string("OpenSSL is missing ") + name, "HttpError");
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(found);
    };
    bind(api.clientMethod, "TLS_client_method");
    bind(api.defaultPaths, "SSL_CTX_set_default_verify_paths");
    bind(api.setVerify, "SSL_CTX_set_verify");
    bind(api.control, "SSL_ctrl");
    bind(api.setHost, "SSL_set1_host");
    bind(api.connect, "SSL_connect");
    bind(api.verifyResult, "SSL_get_verify_result");
    auto context = base.newContext(api.clientMethod());
    if (!context)
        fail("cannot create a TLS client context", "HttpError");
    api.defaultPaths(context);
    api.setVerify(context, 1 /* SSL_VERIFY_PEER */, nullptr);
    api.context = context;
    return api;
}
struct ClientConnection {
    Socket socket = invalidSocket;
    void *ssl = nullptr;
    ~ClientConnection() {
        if (ssl) {
            tls().shutdown(ssl);
            tls().freeSession(ssl);
        }
        if (socket != invalidSocket)
            closeSocket(socket);
    }
    void send(const std::string &data) {
        size_t done = 0;
        while (done < data.size()) {
            auto chunk = static_cast<int>(std::min<size_t>(data.size() - done, 1 << 20));
            int n = ssl ? tls().write(ssl, data.data() + done, chunk)
                        : static_cast<int>(::send(socket, data.data() + done, chunk, 0));
            if (n <= 0)
                fail("connection failed while sending the request", "HttpError");
            done += static_cast<size_t>(n);
        }
    }
    // Reads more bytes into `buffer`; false at the end of the stream.
    bool receive(std::string &buffer) {
        char chunk[16384];
        int n = ssl ? tls().read(ssl, chunk, sizeof chunk)
                    : static_cast<int>(::recv(socket, chunk, sizeof chunk, 0));
        if (n > 0) {
            buffer.append(chunk, static_cast<size_t>(n));
            return true;
        }
        if (n == 0 || (ssl && tls().error(ssl, n) == tlsZeroReturn))
            return false;
        // An HTTP/1.0-style server may close without a TLS close_notify; treat as the end.
        if (ssl)
            return false;
        fail("connection failed or timed out while reading the response", "HttpError");
    }
};
struct HttpResult {
    int64_t status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string header(const std::string &name) const {
        for (auto &[key, value] : headers)
            if (key == name)
                return value;
        return "";
    }
};
HttpResult httpOnce(const std::string &method, const Url &url, const std::string &body,
                    const std::vector<std::pair<std::string, std::string>> &headers,
                    double timeout) {
    ClientConnection connection;
    AddressList list;
    resolveAddresses(list, url.host, url.port, false);
    int code = 0;
    for (auto a = list.head; a; a = a->ai_next) {
        Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == invalidSocket) {
            code = socketError();
            continue;
        }
        if (timeout > 0)
            setTimeout(s, timeout);
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
            connection.socket = s;
            break;
        }
        code = socketError();
        closeSocket(s);
    }
    if (connection.socket == invalidSocket)
        fail("cannot connect to " + url.host + ":" + std::to_string(url.port) + ": " +
                 socketMessage(code),
             "HttpError");
    if (url.scheme == "https") {
        auto &client = tlsClient();
        connection.ssl = tls().newSession(client.context);
        if (!connection.ssl)
            fail("cannot create a TLS session", "HttpError");
        tls().setDescriptor(connection.ssl, static_cast<int>(connection.socket));
        client.control(connection.ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0,
                       const_cast<char *>(url.host.c_str()));
        client.setHost(connection.ssl, url.host.c_str());
        if (client.connect(connection.ssl) != 1) {
            auto verify = client.verifyResult(connection.ssl);
            fail("TLS handshake with " + url.host + " failed" +
                     (verify ? " (certificate verification error " + std::to_string(verify) + ")"
                             : ""),
                 "HttpError");
        }
    }
    bool ipv6 = url.host.find(':') != std::string::npos;
    std::string host = ipv6 ? "[" + url.host + "]" : url.host;
    if ((url.scheme == "http" && url.port != 80) || (url.scheme == "https" && url.port != 443))
        host += ":" + std::to_string(url.port);
    std::string request = method + " " + url.path + " HTTP/1.1\r\nHost: " + host + "\r\n";
    std::set<std::string> given;
    for (auto &[name, value] : headers) {
        if (name.find_first_of("\r\n:") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos)
            fail("invalid header '" + name + "'", "HttpError");
        given.insert(lower(name));
        request += name + ": " + value + "\r\n";
    }
    if (!given.count("user-agent"))
        request += "User-Agent: Flywheel\r\n";
    if (!given.count("accept-encoding"))
        request += "Accept-Encoding: gzip, deflate\r\n";
    if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH")
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    connection.send(request + body);
    std::string buffer;
    size_t headerEnd;
    while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (buffer.size() > maxHeaderBytes)
            fail("response headers are too large", "HttpError");
        if (!connection.receive(buffer))
            fail("connection closed before the response headers ended", "HttpError");
    }
    HttpResult result;
    std::istringstream lines(buffer.substr(0, headerEnd));
    std::string line;
    std::getline(lines, line);
    if (line.rfind("HTTP/", 0) != 0 || line.size() < 12)
        fail("invalid HTTP status line", "HttpError");
    result.status = std::atoi(line.c_str() + 9);
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        result.headers.emplace_back(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
    std::string rest = buffer.substr(headerEnd + 4);
    bool noBody = method == "HEAD" || result.status == 204 || result.status == 304 ||
                  (result.status >= 100 && result.status < 200);
    auto length = result.header("content-length");
    if (noBody) {
    } else if (lower(result.header("transfer-encoding")).find("chunked") != std::string::npos) {
        size_t at = 0;
        while (true) {
            size_t lineEnd;
            while ((lineEnd = rest.find("\r\n", at)) == std::string::npos)
                if (!connection.receive(rest))
                    fail("connection closed inside a chunked body", "HttpError");
            auto size = std::strtoull(rest.c_str() + at, nullptr, 16);
            at = lineEnd + 2;
            if (size == 0)
                break;
            while (rest.size() < at + size + 2)
                if (!connection.receive(rest))
                    fail("connection closed inside a chunked body", "HttpError");
            result.body.append(rest, at, size);
            at += size + 2;
            if (result.body.size() > (size_t(1) << 31))
                fail("response body is too large", "HttpError");
        }
    } else if (!length.empty()) {
        auto size = std::strtoull(length.c_str(), nullptr, 10);
        while (rest.size() < size)
            if (!connection.receive(rest))
                fail("connection closed before the response body ended", "HttpError");
        rest.resize(size);
        result.body = std::move(rest);
    } else {
        while (connection.receive(rest))
            if (rest.size() > (size_t(1) << 31))
                fail("response body is too large", "HttpError");
        result.body = std::move(rest);
    }
    auto encoding = lower(result.header("content-encoding"));
    if (encoding == "gzip" || encoding == "x-gzip")
        result.body = gunzip(result.body);
    else if (encoding == "deflate") {
        // Servers send either zlib-wrapped or raw deflate data.
        try {
            result.body = zlibDecompress(result.body);
        } catch (const NativeFailure &) {
            result.body = inflate(result.body);
        }
    }
    return result;
}
// Follows up to 10 redirects; 303 (and 301/302 after POST) continue as GET without a body.
HttpResult httpFetch(std::string method, std::string address, std::string body,
                     std::vector<std::pair<std::string, std::string>> headers, double timeout) {
    for (int redirects = 0;; ++redirects) {
        auto url = parseUrl(address);
        auto result = httpOnce(method, url, body, headers, timeout);
        auto location = result.header("location");
        bool redirect = result.status == 301 || result.status == 302 || result.status == 303 ||
                        result.status == 307 || result.status == 308;
        if (!redirect || location.empty())
            return result;
        if (redirects == 10)
            fail("too many redirects", "HttpError");
        if (location.find("://") == std::string::npos) {
            auto origin = url.scheme + "://" +
                          (url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host) +
                          ":" + std::to_string(url.port);
            if (location.rfind("//", 0) == 0)
                location = url.scheme + ":" + location;
            else if (!location.empty() && location[0] == '/')
                location = origin + location;
            else {
                auto path = url.path.substr(0, url.path.find('?'));
                location = origin + path.substr(0, path.rfind('/') + 1) + location;
            }
        }
        if (result.status == 303 || ((result.status == 301 || result.status == 302) && method == "POST")) {
            method = "GET";
            body.clear();
            std::erase_if(headers, [](auto &h) { return lower(h.first) == "content-type"; });
        }
        // Credentials are not forwarded to another host.
        if (parseUrl(location).host != url.host)
            std::erase_if(headers, [](auto &h) {
                auto name = lower(h.first);
                return name == "authorization" || name == "cookie";
            });
        address = location;
    }
}
} // namespace
extern "C" void fw_http_fetch(Frame *frame, Object *method, Object *url, Object *body,
                              Object *headers, double timeout, Object *response) {
    nativeBoundary([&] {
        std::vector<std::pair<std::string, std::string>> requestHeaders;
        {
            ObjectGuard guard(headers);
            for (size_t i = 0; i < headers->keys.size(); ++i)
                requestHeaders.emplace_back(requireString(headers->keys[i].object)->text,
                                            requireString(headers->slots[i].object)->text);
        }
        auto verb = requireString(method)->text;
        for (auto &c : verb)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto result = httpFetch(verb, requireString(url)->text, requireString(body)->text,
                                requestHeaders, timeout);
        auto map = stringMap(frame);
        for (auto &[name, value] : result.headers)
            put(frame, map, name, value, true);
        ObjectGuard guard(response);
        response->slots.at(0) = integerSlot(result.status);
        response->slots.at(1) = text(frame, result.body);
        response->slots.at(2) = reference(map);
    });
}

// sqlite: SQLite 3 databases through the system library (libsqlite3), loaded on first use ---
namespace {
struct Sqlite {
    void *library = nullptr;
    int (*open)(const char *, void **, int, const char *) = nullptr;
    int (*close)(void *) = nullptr;
    const char *(*message)(void *) = nullptr;
    int (*prepare)(void *, const char *, int, void **, const char **) = nullptr;
    int (*finalize)(void *) = nullptr;
    int (*step)(void *) = nullptr;
    int (*bindInt)(void *, int, int64_t) = nullptr;
    int (*bindDouble)(void *, int, double) = nullptr;
    int (*bindText)(void *, int, const char *, int, intptr_t) = nullptr;
    int (*bindBlob)(void *, int, const void *, int, intptr_t) = nullptr;
    int (*bindNull)(void *, int) = nullptr;
    int (*parameterCount)(void *) = nullptr;
    int (*columnCount)(void *) = nullptr;
    const char *(*columnName)(void *, int) = nullptr;
    int (*columnType)(void *, int) = nullptr;
    int64_t (*columnInt)(void *, int) = nullptr;
    double (*columnDouble)(void *, int) = nullptr;
    const unsigned char *(*columnText)(void *, int) = nullptr;
    int (*columnBytes)(void *, int) = nullptr;
    int (*changes)(void *) = nullptr;
    int64_t (*lastInsert)(void *) = nullptr;
    int (*busyTimeout)(void *, int) = nullptr;
};
Sqlite &sqlite() {
    static Sqlite api;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (api.library)
        return api;
#ifdef _WIN32
    for (auto name : {"sqlite3.dll", "winsqlite3.dll"})
        if ((api.library = reinterpret_cast<void *>(LoadLibraryA(name))))
            break;
    auto symbol = [&](const char *name) {
        return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(api.library), name));
    };
#else
    for (auto name : {"libsqlite3.so.0", "libsqlite3.dylib", "libsqlite3.so"})
        if ((api.library = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
            break;
    auto symbol = [&](const char *name) { return dlsym(api.library, name); };
#endif
    if (!api.library)
        fail("sqlite requires the SQLite 3 library (libsqlite3), which could not be loaded",
             "DatabaseError");
    auto bind = [&](auto &target, const char *name) {
        auto found = symbol(name);
        if (!found)
            fail(std::string("SQLite is missing ") + name, "DatabaseError");
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(found);
    };
    bind(api.open, "sqlite3_open_v2");
    bind(api.close, "sqlite3_close_v2");
    bind(api.message, "sqlite3_errmsg");
    bind(api.prepare, "sqlite3_prepare_v2");
    bind(api.finalize, "sqlite3_finalize");
    bind(api.step, "sqlite3_step");
    bind(api.bindInt, "sqlite3_bind_int64");
    bind(api.bindDouble, "sqlite3_bind_double");
    bind(api.bindText, "sqlite3_bind_text");
    bind(api.bindBlob, "sqlite3_bind_blob");
    bind(api.bindNull, "sqlite3_bind_null");
    bind(api.parameterCount, "sqlite3_bind_parameter_count");
    bind(api.columnCount, "sqlite3_column_count");
    bind(api.columnName, "sqlite3_column_name");
    bind(api.columnType, "sqlite3_column_type");
    bind(api.columnInt, "sqlite3_column_int64");
    bind(api.columnDouble, "sqlite3_column_double");
    bind(api.columnText, "sqlite3_column_text");
    bind(api.columnBytes, "sqlite3_column_bytes");
    bind(api.changes, "sqlite3_changes");
    bind(api.lastInsert, "sqlite3_last_insert_rowid");
    bind(api.busyTimeout, "sqlite3_busy_timeout");
    return api;
}
struct DatabaseResource : NativeResource {
    void *db = nullptr;
    std::mutex mutex; // SQLite connections are used by one thread at a time
    ~DatabaseResource() override {
        if (db)
            sqlite().close(db);
    }
};
DatabaseResource &databaseOf(Object *p) {
    auto resource = p && p->kind == Object::Resource
                        ? dynamic_cast<DatabaseResource *>(p->nativeResource())
                        : nullptr;
    if (!resource)
        fail("expected sqlite.database", "DatabaseError");
    if (!resource->db)
        fail("database is closed", "DatabaseError");
    return *resource;
}
struct Statement {
    void *handle = nullptr;
    ~Statement() {
        if (handle)
            sqlite().finalize(handle);
    }
};
std::string numberText(double value) {
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}
// Prepares `sql`, binds `params` (an array of mixed slots), and calls `row` for each result row.
template <class F>
void runStatement(DatabaseResource &database, const std::string &sql, Object *params, F &&row) {
    auto &api = sqlite();
    std::vector<Slot> values;
    if (params) {
        ObjectGuard guard(params);
        values = params->slots;
    }
    const char *tail = sql.c_str();
    const char *end = sql.c_str() + sql.size();
    size_t used = 0;
    bool any = false;
    // Several statements may be given; parameters are consumed in order across them.
    while (tail < end) {
        while (tail < end && std::isspace(static_cast<unsigned char>(*tail)))
            ++tail;
        if (tail >= end)
            break;
        Statement statement;
        const char *next = nullptr;
        if (api.prepare(database.db, tail, static_cast<int>(end - tail), &statement.handle, &next) != 0)
            fail(api.message(database.db), "DatabaseError");
        tail = next;
        if (!statement.handle)
            continue;
        any = true;
        int count = api.parameterCount(statement.handle);
        for (int i = 1; i <= count; ++i, ++used) {
            if (used >= values.size())
                fail("the SQL has more ? parameters than values were given", "DatabaseError");
            Slot slot = values[used];
            if (slot.kind == Slot::Reference && slot.object && slot.object->kind == Object::Box) {
                Slot inner = slot.object->slots.at(0);
                slot = inner;
            }
            int status;
            switch (slot.kind) {
            case Slot::Real:
                status = api.bindDouble(statement.handle, i, slot.real);
                break;
            case Slot::Reference:
                if (!slot.object)
                    status = api.bindNull(statement.handle, i);
                else if (slot.object->kind == Object::String)
                    status = api.bindText(statement.handle, i, slot.object->text.c_str(),
                                          static_cast<int>(slot.object->text.size()), -1);
                else
                    fail("SQL parameters must be numbers, bools, strings, or null",
                         "DatabaseError");
                break;
            default:
                status = api.bindInt(statement.handle, i, slot.integer);
            }
            if (status != 0)
                fail(api.message(database.db), "DatabaseError");
        }
        while (true) {
            int status = api.step(statement.handle);
            if (status == 101) // SQLITE_DONE
                break;
            if (status != 100) // SQLITE_ROW
                fail(api.message(database.db), "DatabaseError");
            row(statement.handle);
        }
    }
    if (!any)
        fail("empty SQL statement", "DatabaseError");
    if (used != values.size())
        fail("more values were given than the SQL has ? parameters", "DatabaseError");
}
} // namespace
extern "C" {
Object *fw_sqlite_open(Frame *frame, Object *path) {
    return nativeBoundary([&] {
        auto &api = sqlite();
        auto resource = std::make_unique<DatabaseResource>();
        // SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX
        int status = api.open(requireString(path)->text.c_str(), &resource->db,
                              0x2 | 0x4 | 0x40 | 0x10000, nullptr);
        if (status != 0) {
            std::string message = resource->db ? api.message(resource->db) : "out of memory";
            fail("cannot open database '" + path->text + "': " + message, "DatabaseError");
        }
        api.busyTimeout(resource->db, 5000);
        auto object = managed(frame, Object::Resource);
        object->text = "sqlite.database";
        object->more().resource = std::move(resource);
        return object;
    });
}
// Runs statements that return no rows; returns the number of changed rows.
int64_t fw_sqlite_execute(Object *database, Object *sql, Object *params) {
    return nativeBoundary([&]() -> int64_t {
        auto &db = databaseOf(database);
        std::lock_guard<std::mutex> lock(db.mutex);
        runStatement(db, requireString(sql)->text, params, [](void *) {});
        return sqlite().changes(db.db);
    });
}
// Runs a query; the rows are a JSON array of objects keyed by column name.
Object *fw_sqlite_query(Frame *frame, Object *database, Object *sql, Object *params) {
    return nativeBoundary([&] {
        auto &db = databaseOf(database);
        auto &api = sqlite();
        auto rows = std::make_shared<Json>();
        rows->kind = Json::List;
        std::lock_guard<std::mutex> lock(db.mutex);
        runStatement(db, requireString(sql)->text, params, [&](void *statement) {
            auto row = std::make_shared<Json>();
            row->kind = Json::Dictionary;
            for (int i = 0, n = api.columnCount(statement); i < n; ++i) {
                auto cell = std::make_shared<Json>();
                switch (api.columnType(statement, i)) {
                case 1:
                    cell->kind = Json::Number;
                    cell->text = std::to_string(api.columnInt(statement, i));
                    break;
                case 2:
                    cell->kind = Json::Number;
                    cell->text = numberText(api.columnDouble(statement, i));
                    break;
                case 5:
                    cell->kind = Json::Null;
                    break;
                default: {
                    auto text = api.columnText(statement, i);
                    cell->kind = Json::Text;
                    cell->text.assign(reinterpret_cast<const char *>(text),
                                      static_cast<size_t>(api.columnBytes(statement, i)));
                }
                }
                row->fields[api.columnName(statement, i)] = cell;
            }
            rows->items.push_back(row);
        });
        return jsonObject(frame, rows);
    });
}
int64_t fw_sqlite_last_insert(Object *database) {
    return nativeBoundary([&]() -> int64_t {
        auto &db = databaseOf(database);
        std::lock_guard<std::mutex> lock(db.mutex);
        return sqlite().lastInsert(db.db);
    });
}
void fw_sqlite_close(Object *database) {
    nativeBoundary([&] {
        auto resource = database && database->kind == Object::Resource
                            ? dynamic_cast<DatabaseResource *>(database->nativeResource())
                            : nullptr;
        if (!resource)
            fail("expected sqlite.database", "DatabaseError");
        std::lock_guard<std::mutex> lock(resource->mutex);
        if (resource->db)
            sqlite().close(resource->db);
        resource->db = nullptr;
    });
}
}

// html.parse: a lenient HTML parser that builds the same element trees as markup literals ---
namespace {
const std::map<std::string, std::string> &htmlEntities() {
    static const std::map<std::string, std::string> entities = {
        {"amp", "&"},        {"lt", "<"},        {"gt", ">"},        {"quot", "\""},
        {"apos", "'"},       {"nbsp", " "}, {"copy", "©"}, {"reg", "®"},
        {"trade", "™"}, {"hellip", "…"}, {"mdash", "—"}, {"ndash", "–"},
        {"lsquo", "‘"}, {"rsquo", "’"}, {"ldquo", "“"}, {"rdquo", "”"},
        {"laquo", "«"}, {"raquo", "»"}, {"bull", "•"}, {"middot", "·"},
        {"deg", "°"},   {"times", "×"}, {"divide", "÷"}, {"euro", "€"},
        {"pound", "£"}, {"yen", "¥"},  {"cent", "¢"},  {"sect", "§"},
        {"para", "¶"},  {"plusmn", "±"}, {"frac12", "½"}, {"larr", "←"},
        {"rarr", "→"},  {"uarr", "↑"}, {"darr", "↓"},  {"hearts", "♥"},
        {"shy", "­"},   {"zwj", "‍"},  {"zwnj", "‌"},  {"ensp", " "},
        {"emsp", " "},  {"thinsp", " "}};
    return entities;
}
std::string decodeEntities(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        auto end = text.find(';', i);
        if (end == std::string::npos || end - i > 32) {
            out += '&';
            continue;
        }
        auto name = text.substr(i + 1, end - i - 1);
        if (!name.empty() && name[0] == '#') {
            bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            auto digits = name.substr(hex ? 2 : 1);
            uint32_t value = 0;
            bool ok = !digits.empty();
            for (char c : digits) {
                int d = hex ? hexDigit(c) : (c >= '0' && c <= '9' ? c - '0' : -1);
                if (d < 0 || value > 0x10ffff) {
                    ok = false;
                    break;
                }
                value = value * (hex ? 16 : 10) + static_cast<uint32_t>(d);
            }
            if (ok && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff) && value) {
                appendUtf8(out, value);
                i = end;
                continue;
            }
        } else if (auto it = htmlEntities().find(name); it != htmlEntities().end()) {
            out += it->second;
            i = end;
            continue;
        }
        out += '&';
    }
    return out;
}
bool voidTag(const std::string &tag) {
    static const std::set<std::string> tags = {"area", "base", "br",   "col",   "embed",
                                               "hr",   "img",  "input", "link", "meta",
                                               "param", "source", "track", "wbr"};
    return tags.count(tag) > 0;
}
// Tags whose start closes an open <p>.
bool closesParagraph(const std::string &tag) {
    static const std::set<std::string> tags = {
        "address", "article", "aside", "blockquote", "details", "div", "dl", "fieldset",
        "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
        "hr", "main", "nav", "ol", "p", "pre", "section", "table", "ul"};
    return tags.count(tag) > 0;
}
class HtmlParser {
    const std::string &s;
    size_t i = 0;
    Frame *frame;
    std::vector<Object *> stack; // open elements; stack[0] is the fragment
    void append(Object *child) {
        fw_child(stack.back(), child);
    }
    void text(const std::string &raw) {
        if (raw.empty())
            return;
        auto &parent = stack.back()->text;
        static const std::set<std::string> structural = {"", "html", "head", "table", "thead",
                                                         "tbody", "tfoot", "tr", "ul", "ol",
                                                         "select", "dl"};
        if (structural.count(parent) && raw.find_first_not_of(" \t\r\n\f") == std::string::npos)
            return;
        // Merge with a preceding text node.
        auto &children = stack.back()->slots;
        if (!children.empty() && children.back().object &&
            children.back().object->kind == Object::String) {
            children.back().object->text += decodeEntities(raw);
            return;
        }
        append(string(frame, decodeEntities(raw)));
    }
    bool open(const std::string &tag) {
        for (size_t k = stack.size(); k-- > 1;)
            if (stack[k]->text == tag)
                return true;
        return false;
    }
    void close(const std::string &tag) {
        for (size_t k = stack.size(); k-- > 1;)
            if (stack[k]->text == tag) {
                stack.resize(k);
                return;
            }
    }
    // Implied end tags: a new <li> ends the previous one, and so on.
    void implied(const std::string &tag) {
        auto top = [&] { return stack.size() > 1 ? stack.back()->text : std::string(); };
        if (closesParagraph(tag) && open("p"))
            close("p");
        if (tag == "li" && open("li") && top() == "li")
            close("li");
        if ((tag == "dt" || tag == "dd") && (top() == "dt" || top() == "dd"))
            stack.pop_back();
        if (tag == "option" && top() == "option")
            stack.pop_back();
        if (tag == "tr" && open("tr")) {
            while (top() == "td" || top() == "th")
                stack.pop_back();
            if (top() == "tr")
                stack.pop_back();
        }
        if ((tag == "td" || tag == "th") && (top() == "td" || top() == "th"))
            stack.pop_back();
    }

  public:
    HtmlParser(const std::string &source, Frame *f) : s(source), frame(f) {}
    Object *run() {
        auto root = own(frame, new Object(Object::Element)); // empty tag: a fragment
        stack.push_back(root);
        size_t textStart = 0;
        while (i < s.size()) {
            if (s[i] != '<') {
                ++i;
                continue;
            }
            auto flush = [&] { text(s.substr(textStart, i - textStart)); };
            if (s.compare(i, 4, "<!--") == 0) {
                flush();
                auto end = s.find("-->", i + 4);
                i = end == std::string::npos ? s.size() : end + 3;
                textStart = i;
                continue;
            }
            if (i + 1 < s.size() && (s[i + 1] == '!' || s[i + 1] == '?')) {
                flush();
                auto end = s.find('>', i);
                i = end == std::string::npos ? s.size() : end + 1;
                textStart = i;
                continue;
            }
            bool closing = i + 1 < s.size() && s[i + 1] == '/';
            size_t nameStart = i + (closing ? 2 : 1);
            if (nameStart >= s.size() || !std::isalpha(static_cast<unsigned char>(s[nameStart]))) {
                ++i; // a literal '<'
                continue;
            }
            flush();
            size_t p = nameStart;
            while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) && s[p] != '>' &&
                   s[p] != '/')
                ++p;
            auto tag = lowerAscii(s.substr(nameStart, p - nameStart));
            if (closing) {
                auto end = s.find('>', p);
                i = end == std::string::npos ? s.size() : end + 1;
                textStart = i;
                if (tag == "p" && !open("p"))
                    append(fw_element(frame, string(frame, "p"))); // </p> alone is an empty <p>
                else
                    close(tag);
                continue;
            }
            implied(tag);
            auto element = fw_element(frame, string(frame, tag));
            bool selfClosing = false;
            // Attributes: name, name=value, name="value", name='value'.
            while (p < s.size() && s[p] != '>') {
                if (std::isspace(static_cast<unsigned char>(s[p]))) {
                    ++p;
                    continue;
                }
                if (s[p] == '/') {
                    selfClosing = p + 1 < s.size() && s[p + 1] == '>';
                    ++p;
                    continue;
                }
                size_t a = p;
                while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) &&
                       s[p] != '=' && s[p] != '>' && !(s[p] == '/' && p + 1 < s.size() && s[p + 1] == '>'))
                    ++p;
                auto name = lowerAscii(s.substr(a, p - a));
                while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
                    ++p;
                std::string value;
                if (p < s.size() && s[p] == '=') {
                    ++p;
                    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
                        ++p;
                    if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                        char quote = s[p++];
                        auto end = s.find(quote, p);
                        if (end == std::string::npos)
                            end = s.size();
                        value = s.substr(p, end - p);
                        p = std::min(end + 1, s.size());
                    } else {
                        size_t v = p;
                        while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) &&
                               s[p] != '>')
                            ++p;
                        value = s.substr(v, p - v);
                    }
                }
                bool duplicate = false;
                for (auto &[existing, ignored] : element->attributes)
                    duplicate = duplicate || existing == name;
                if (!name.empty() && !duplicate)
                    element->attributes.emplace_back(name, decodeEntities(value));
            }
            i = p < s.size() ? p + 1 : s.size();
            textStart = i;
            append(element);
            if (voidTag(tag) || selfClosing)
                continue;
            static const std::set<std::string> raw = {"script", "style", "textarea", "title"};
            if (raw.count(tag)) {
                // Raw text runs to the matching end tag, case-insensitively.
                size_t end = i;
                while (true) {
                    end = s.find("</", end);
                    if (end == std::string::npos ||
                        lowerAscii(s.substr(end + 2, tag.size())) == tag)
                        break;
                    end += 2;
                }
                if (end == std::string::npos)
                    end = s.size();
                auto content = s.substr(i, end - i);
                if (!content.empty())
                    fw_child(element, string(frame, tag == "script" || tag == "style"
                                                        ? content
                                                        : decodeEntities(content)));
                auto close = end == s.size() ? s.size() : s.find('>', end);
                i = close == std::string::npos ? s.size() : close + 1;
                textStart = i;
                continue;
            }
            stack.push_back(element);
        }
        text(s.substr(textStart));
        return root;
    }
};
} // namespace
extern "C" Object *fw_html_parse(Frame *frame, Object *text) {
    return nativeBoundary([&] { return HtmlParser(requireString(text)->text, frame).run(); });
}

// toml.parse: TOML 1.0 documents as JSON values (dates and times become strings) ------------
namespace {
class TomlParser {
    const std::string &s;
    size_t i = 0;
    int line = 1;
    std::shared_ptr<Json> root = std::make_shared<Json>();
    std::set<const Json *> defined, inlineTables; // tables declared by headers; sealed tables
    [[noreturn]] void error(const std::string &message) {
        fail("TOML line " + std::to_string(line) + ": " + message, "ValueError");
    }
    bool at(char c) const {
        return i < s.size() && s[i] == c;
    }
    void spaces() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
    }
    void comment() {
        spaces();
        if (at('#'))
            while (i < s.size() && s[i] != '\n')
                ++i;
    }
    // Spaces, newlines, and comments (inside arrays).
    void blank() {
        while (true) {
            comment();
            if (at('\n')) {
                ++i;
                ++line;
            } else if (at('\r') && i + 1 < s.size() && s[i + 1] == '\n')
                i += 2, ++line;
            else
                return;
        }
    }
    void endOfLine() {
        comment();
        if (i >= s.size())
            return;
        if (at('\r'))
            ++i;
        if (!at('\n'))
            error("expected the end of the line");
        ++i;
        ++line;
    }
    std::string bareKey() {
        size_t start = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '-'))
            ++i;
        if (start == i)
            error("expected a key");
        return s.substr(start, i - start);
    }
    std::string key() {
        if (at('"'))
            return basicString(false);
        if (at('\''))
            return literalString(false);
        return bareKey();
    }
    std::vector<std::string> dottedKey() {
        std::vector<std::string> parts;
        while (true) {
            spaces();
            parts.push_back(key());
            spaces();
            if (!at('.'))
                return parts;
            ++i;
        }
    }
    void escape(std::string &out) {
        if (++i >= s.size())
            error("unterminated escape");
        char c = s[i++];
        switch (c) {
        case 'b': out += '\b'; return;
        case 't': out += '\t'; return;
        case 'n': out += '\n'; return;
        case 'f': out += '\f'; return;
        case 'r': out += '\r'; return;
        case 'e': out += '\x1b'; return;
        case '"': out += '"'; return;
        case '\\': out += '\\'; return;
        case 'u':
        case 'U': {
            size_t n = c == 'u' ? 4 : 8;
            if (i + n > s.size())
                error("incomplete Unicode escape");
            uint32_t v = 0;
            for (size_t k = 0; k < n; ++k) {
                int d = hexDigit(s[i + k]);
                if (d < 0)
                    error("invalid Unicode escape");
                v = v * 16 + static_cast<uint32_t>(d);
            }
            if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                error("invalid Unicode scalar in escape");
            appendUtf8(out, v);
            i += n;
            return;
        }
        default:
            error(std::string("unknown escape \\") + c);
        }
    }
    std::string basicString(bool allowMultiline = true) {
        if (allowMultiline && s.compare(i, 3, "\"\"\"") == 0) {
            i += 3;
            if (at('\n'))
                ++i, ++line;
            else if (s.compare(i, 2, "\r\n") == 0)
                i += 2, ++line;
            std::string out;
            while (true) {
                if (i >= s.size())
                    error("unterminated multi-line string");
                if (s.compare(i, 3, "\"\"\"") == 0) {
                    i += 3;
                    // Up to two extra quotes belong to the content.
                    for (int extra = 0; extra < 2 && at('"'); ++extra)
                        out += s[i++];
                    return out;
                }
                if (s[i] == '\\') {
                    size_t k = i + 1;
                    while (k < s.size() && (s[k] == ' ' || s[k] == '\t'))
                        ++k;
                    if (k < s.size() && (s[k] == '\n' || s[k] == '\r')) {
                        // A line-ending backslash trims the newline and following whitespace.
                        i = k;
                        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
                            if (s[i] == '\n')
                                ++line;
                            ++i;
                        }
                        continue;
                    }
                    escape(out);
                    continue;
                }
                if (s[i] == '\n')
                    ++line;
                out += s[i++];
            }
        }
        ++i;
        std::string out;
        while (true) {
            if (i >= s.size() || s[i] == '\n')
                error("unterminated string");
            if (s[i] == '"') {
                ++i;
                return out;
            }
            if (s[i] == '\\')
                escape(out);
            else
                out += s[i++];
        }
    }
    std::string literalString(bool allowMultiline = true) {
        if (allowMultiline && s.compare(i, 3, "'''") == 0) {
            i += 3;
            if (at('\n'))
                ++i, ++line;
            else if (s.compare(i, 2, "\r\n") == 0)
                i += 2, ++line;
            auto end = s.find("'''", i);
            if (end == std::string::npos)
                error("unterminated multi-line string");
            while (end + 3 < s.size() && s[end + 3] == '\'' && end + 3 - i < s.size())
                ++end;
            auto out = s.substr(i, end - i);
            line += static_cast<int>(std::count(out.begin(), out.end(), '\n'));
            i = end + 3;
            return out;
        }
        ++i;
        auto end = s.find_first_of("'\n", i);
        if (end == std::string::npos || s[end] != '\'')
            error("unterminated string");
        auto out = s.substr(i, end - i);
        i = end + 1;
        return out;
    }
    std::shared_ptr<Json> make(Json::Kind kind, std::string text = "") {
        auto j = std::make_shared<Json>();
        j->kind = kind;
        j->text = std::move(text);
        return j;
    }
    std::shared_ptr<Json> scalar() {
        size_t start = i;
        while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}' && s[i] != '\n' &&
               s[i] != '#' && s[i] != '\r')
            ++i;
        auto token = s.substr(start, i - start);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();
        // A date and time may contain one space: 1979-05-27 07:32:00Z.
        if (token == "true" || token == "false")
            return make(Json::Boolean, token);
        if (token == "inf" || token == "+inf" || token == "-inf" || token == "nan" ||
            token == "+nan" || token == "-nan")
            error("JSON values cannot hold inf or nan ('" + token + "')");
        static const std::regex date(R"(\d{4}-\d{2}-\d{2}([Tt ]\d{2}:\d{2}(:\d{2}(\.\d+)?)?([Zz]|[+-]\d{2}:\d{2})?)?|\d{2}:\d{2}(:\d{2}(\.\d+)?)?)");
        if (std::regex_match(token, date))
            return make(Json::Text, token);
        std::string digits;
        for (size_t k = 0; k < token.size(); ++k) {
            if (token[k] == '_') {
                if (k == 0 || k + 1 == token.size() || !std::isalnum(static_cast<unsigned char>(token[k - 1])) ||
                    !std::isalnum(static_cast<unsigned char>(token[k + 1])))
                    error("misplaced '_' in number '" + token + "'");
                continue;
            }
            digits += token[k];
        }
        if (digits.size() > 2 && digits[0] == '0' &&
            (digits[1] == 'x' || digits[1] == 'o' || digits[1] == 'b')) {
            int base = digits[1] == 'x' ? 16 : digits[1] == 'o' ? 8 : 2;
            uint64_t v = 0;
            auto [end, ec] = std::from_chars(digits.data() + 2, digits.data() + digits.size(), v, base);
            if (ec != std::errc{} || end != digits.data() + digits.size() || v > uint64_t(INT64_MAX))
                error("invalid integer '" + token + "'");
            return make(Json::Number, std::to_string(v));
        }
        static const std::regex integer(R"([+-]?(0|[1-9]\d*))");
        static const std::regex real(R"([+-]?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?)");
        if (std::regex_match(digits, integer)) {
            int64_t v;
            auto text = digits[0] == '+' ? digits.substr(1) : digits;
            auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
            if (ec != std::errc{})
                error("integer out of range '" + token + "'");
            return make(Json::Number, std::to_string(v));
        }
        if (std::regex_match(digits, real))
            return make(Json::Number, digits[0] == '+' ? digits.substr(1) : digits);
        error(token.empty() ? "expected a value" : "invalid value '" + token + "'");
    }
    std::shared_ptr<Json> value() {
        spaces();
        if (at('"'))
            return make(Json::Text, basicString());
        if (at('\''))
            return make(Json::Text, literalString());
        if (at('[')) {
            ++i;
            auto array = make(Json::List);
            while (true) {
                blank();
                if (at(']')) {
                    ++i;
                    return array;
                }
                array->items.push_back(value());
                blank();
                if (at(',')) {
                    ++i;
                    continue;
                }
                if (!at(']'))
                    error("expected ',' or ']' in array");
            }
        }
        if (at('{')) {
            ++i;
            auto table = make(Json::Dictionary);
            spaces();
            if (at('}')) {
                ++i;
                inlineTables.insert(table.get());
                return table;
            }
            while (true) {
                auto path = dottedKey();
                if (!at('='))
                    error("expected '=' in inline table");
                ++i;
                assign(*table, path, value());
                spaces();
                if (at(',')) {
                    ++i;
                    continue;
                }
                if (!at('}'))
                    error("expected ',' or '}' in inline table");
                ++i;
                inlineTables.insert(table.get());
                return table;
            }
        }
        return scalar();
    }
    Json &descend(Json &table, const std::string &name, bool forDottedKey) {
        auto &slot = table.fields[name];
        if (!slot)
            slot = make(Json::Dictionary);
        if (slot->kind == Json::List && !forDottedKey && !slot->items.empty() &&
            slot->items.back()->kind == Json::Dictionary)
            return *slot->items.back(); // [a.b] inside the latest [[a]]
        if (slot->kind != Json::Dictionary || inlineTables.count(slot.get()))
            error("key '" + name + "' is already defined");
        return *slot;
    }
    void assign(Json &table, const std::vector<std::string> &path, std::shared_ptr<Json> v) {
        Json *t = &table;
        for (size_t k = 0; k + 1 < path.size(); ++k)
            t = &descend(*t, path[k], true);
        if (t->fields.count(path.back()))
            error("key '" + path.back() + "' is already defined");
        t->fields[path.back()] = std::move(v);
    }

  public:
    explicit TomlParser(const std::string &source) : s(source) {
        root->kind = Json::Dictionary;
    }
    std::shared_ptr<Json> run() {
        Json *current = root.get();
        while (true) {
            blank();
            if (i >= s.size())
                return root;
            if (at('[')) {
                bool array = s.compare(i, 2, "[[") == 0;
                i += array ? 2 : 1;
                auto path = dottedKey();
                if (array ? s.compare(i, 2, "]]") != 0 : !at(']'))
                    error("expected ']' after table name");
                i += array ? 2 : 1;
                Json *t = root.get();
                for (size_t k = 0; k + 1 < path.size(); ++k)
                    t = &descend(*t, path[k], false);
                auto &slot = t->fields[path.back()];
                if (array) {
                    if (!slot)
                        slot = make(Json::List);
                    if (slot->kind != Json::List)
                        error("'" + path.back() + "' is not an array of tables");
                    slot->items.push_back(make(Json::Dictionary));
                    current = slot->items.back().get();
                } else {
                    if (!slot)
                        slot = make(Json::Dictionary);
                    if (slot->kind != Json::Dictionary || defined.count(slot.get()) ||
                        inlineTables.count(slot.get()))
                        error("table '" + path.back() + "' is already defined");
                    defined.insert(slot.get());
                    current = slot.get();
                }
                endOfLine();
                continue;
            }
            auto path = dottedKey();
            if (!at('='))
                error("expected '=' after key");
            ++i;
            assign(*current, path, value());
            endOfLine();
        }
    }
};
} // namespace
extern "C" Object *fw_toml_parse(Frame *frame, Object *text) {
    return nativeBoundary([&] { return jsonObject(frame, TomlParser(requireString(text)->text).run()); });
}

// yaml.parse: YAML 1.2 (core schema) documents as JSON values ----------------------------------
// Block mappings and sequences, flow collections, plain/quoted/block scalars, anchors and
// aliases, merge keys (<<), comments, and one document with optional `---`/`...` markers.
namespace {
class YamlParser {
    struct Line {
        int number = 0, indent = 0, blanks = 0; // blank lines directly before this one
        std::string text; // content without indentation and comments
        std::string raw;  // content without indentation (block scalars keep '#')
    };
    std::vector<Line> lines;
    int trailingBlanks = 0;
    size_t at = 0;
    std::map<std::string, std::shared_ptr<Json>> anchors;
    [[noreturn]] static void error(int line, const std::string &message) {
        fail("YAML line " + std::to_string(line) + ": " + message, "ValueError");
    }
    static std::shared_ptr<Json> make(Json::Kind kind, std::string text = "") {
        auto j = std::make_shared<Json>();
        j->kind = kind;
        j->text = std::move(text);
        return j;
    }
    static std::string rtrim(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
            s.pop_back();
        return s;
    }
    static std::string ltrim(std::string s) {
        size_t n = 0;
        while (n < s.size() && (s[n] == ' ' || s[n] == '\t'))
            ++n;
        return s.substr(n);
    }
    // Scans `text` outside quotes and brackets, calling stop(i, depth) at each character.
    template <class F> static size_t scan(const std::string &text, F &&stop) {
        char quote = 0;
        int depth = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (quote) {
                if (c == '\\' && quote == '"')
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            bool tokenStart = i == 0 || std::strchr(" \t[{,", text[i - 1]);
            if ((c == '"' || c == '\'') && tokenStart) {
                quote = c;
                continue;
            }
            if (stop(i, depth))
                return i;
            if (c == '[' || c == '{')
                ++depth;
            else if (c == ']' || c == '}')
                --depth;
        }
        return std::string::npos;
    }
    static std::string stripComment(const std::string &text) {
        auto at = scan(text, [&](size_t i, int) {
            return text[i] == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t');
        });
        return at == std::string::npos ? text : text.substr(0, at);
    }
    // The colon of `key: value` (or `key:` at the end), outside quotes and brackets.
    static size_t mappingColon(const std::string &text) {
        return scan(text, [&](size_t i, int depth) {
            return text[i] == ':' && depth == 0 &&
                   (i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\t');
        });
    }
    static bool balanced(const std::string &text) {
        int final = 0;
        bool open = false;
        char quote = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (quote) {
                if (c == '\\' && quote == '"')
                    ++i;
                else if (c == quote)
                    quote = 0;
            } else if ((c == '"' || c == '\'') && (i == 0 || std::strchr(" \t[{,:", text[i - 1])))
                quote = c;
            else if (c == '[' || c == '{')
                ++final;
            else if (c == ']' || c == '}')
                --final;
        }
        open = quote != 0;
        return final <= 0 && !open;
    }
    static bool sequenceLine(const std::string &text) {
        return text == "-" || text.rfind("- ", 0) == 0;
    }
    std::string doubleQuoted(const std::string &s, size_t &i, int line) {
        std::string out;
        for (++i; i < s.size(); ++i) {
            char c = s[i];
            if (c == '"') {
                ++i;
                return out;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (++i >= s.size())
                error(line, "unterminated escape");
            switch (s[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case 'b': out += '\b'; break;
            case 'e': out += '\x1b'; break;
            case ' ': out += ' '; break;
            case '/': out += '/'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'N': out += "\u0085"; break;
            case '_': out += " "; break;
            case 'x':
            case 'u':
            case 'U': {
                size_t n = s[i] == 'x' ? 2 : s[i] == 'u' ? 4 : 8;
                if (i + n >= s.size())
                    error(line, "incomplete escape");
                uint32_t v = 0;
                for (size_t k = 1; k <= n; ++k) {
                    int d = hexDigit(s[i + k]);
                    if (d < 0)
                        error(line, "invalid escape");
                    v = v * 16 + static_cast<uint32_t>(d);
                }
                if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
                    error(line, "invalid Unicode escape");
                appendUtf8(out, v);
                i += n;
                break;
            }
            default:
                error(line, std::string("unknown escape \\") + s[i]);
            }
        }
        error(line, "unterminated string");
    }
    std::string singleQuoted(const std::string &s, size_t &i, int line) {
        std::string out;
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') {
                    out += '\'';
                    ++i;
                    continue;
                }
                ++i;
                return out;
            }
            out += s[i];
        }
        error(line, "unterminated string");
    }
    // Plain scalars resolve by the core schema: null, bool, int, float, otherwise string.
    std::shared_ptr<Json> plain(const std::string &raw, int line) {
        auto text = rtrim(raw);
        if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL")
            return make(Json::Null);
        if (text == "true" || text == "True" || text == "TRUE")
            return make(Json::Boolean, "true");
        if (text == "false" || text == "False" || text == "FALSE")
            return make(Json::Boolean, "false");
        static const std::regex decimal(R"([-+]?[0-9]+)"), based(R"(0o[0-7]+|0x[0-9a-fA-F]+)"),
            real(R"([-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?)"),
            special(R"([-+]?\.(inf|Inf|INF)|\.(nan|NaN|NAN))");
        if (std::regex_match(text, decimal)) {
            auto digits = text[0] == '+' ? text.substr(1) : text;
            int64_t v;
            auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
            return make(Json::Number, ec == std::errc{} ? std::to_string(v) : digits);
        }
        if (std::regex_match(text, based)) {
            uint64_t v = 0;
            auto [end, ec] = std::from_chars(text.data() + 2, text.data() + text.size(), v,
                                             text[1] == 'o' ? 8 : 16);
            if (ec != std::errc{} || v > uint64_t(INT64_MAX))
                error(line, "integer out of range '" + text + "'");
            return make(Json::Number, std::to_string(v));
        }
        if (std::regex_match(text, special))
            error(line, "JSON values cannot hold inf or nan ('" + text + "')");
        if (std::regex_match(text, real)) {
            auto t = text[0] == '+' ? text.substr(1) : text;
            bool negative = !t.empty() && t[0] == '-';
            auto body = negative ? t.substr(1) : t;
            if (body[0] == '.')
                body = "0" + body;
            auto dot = body.find('.');
            if (dot != std::string::npos &&
                (dot + 1 == body.size() || !std::isdigit(static_cast<unsigned char>(body[dot + 1]))))
                body.insert(dot + 1, "0");
            return make(Json::Number, (negative ? "-" : "") + body);
        }
        return make(Json::Text, text);
    }
    std::shared_ptr<Json> alias(const std::string &name, int line) {
        auto it = anchors.find(name);
        if (it == anchors.end())
            error(line, "unknown alias '*" + name + "'");
        return it->second;
    }
    void spaces(const std::string &s, size_t &i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
    }
    // A flow node: [a, b], {k: v}, an alias, a quoted scalar, or a plain scalar.
    std::shared_ptr<Json> flow(const std::string &s, size_t &i, int line, bool inside) {
        spaces(s, i);
        if (i >= s.size())
            return make(Json::Null);
        if (s[i] == '[' || s[i] == '{') {
            bool list = s[i] == '[';
            char close = list ? ']' : '}';
            auto out = make(list ? Json::List : Json::Dictionary);
            ++i;
            while (true) {
                spaces(s, i);
                if (i >= s.size())
                    error(line, std::string("expected '") + close + "'");
                if (s[i] == close) {
                    ++i;
                    return out;
                }
                if (list)
                    out->items.push_back(flow(s, i, line, true));
                else {
                    std::string name;
                    if (s[i] == '"')
                        name = doubleQuoted(s, i, line);
                    else if (s[i] == '\'')
                        name = singleQuoted(s, i, line);
                    else {
                        size_t start = i;
                        while (i < s.size() && !std::strchr(",}", s[i]) &&
                               !(s[i] == ':' && (i + 1 == s.size() || std::strchr(" ,}", s[i + 1]))))
                            ++i;
                        name = rtrim(s.substr(start, i - start));
                    }
                    spaces(s, i);
                    auto value = make(Json::Null);
                    if (i < s.size() && s[i] == ':') {
                        ++i;
                        value = flow(s, i, line, true);
                    }
                    out->fields[name] = value;
                }
                spaces(s, i);
                if (i < s.size() && s[i] == ',')
                    ++i;
                else if (i >= s.size() || s[i] != close)
                    error(line, std::string("expected ',' or '") + close + "'");
            }
        }
        if (s[i] == '*') {
            size_t start = ++i;
            while (i < s.size() && !std::strchr(" ,]}", s[i]))
                ++i;
            return alias(s.substr(start, i - start), line);
        }
        if (s[i] == '"')
            return make(Json::Text, doubleQuoted(s, i, line));
        if (s[i] == '\'')
            return make(Json::Text, singleQuoted(s, i, line));
        size_t start = i;
        while (i < s.size() && !(inside && std::strchr(",]}", s[i])) &&
               !(inside && s[i] == ':' && (i + 1 == s.size() || std::strchr(" ,]}", s[i + 1]))))
            ++i;
        return plain(s.substr(start, i - start), line);
    }
    // `|` or `>` scalars: the following lines indented past `parent`.
    std::shared_ptr<Json> blockScalar(const std::string &header, int parent) {
        bool literal = header[0] == '|';
        char chomp = 0;
        for (char c : header.substr(1))
            if (c == '-' || c == '+')
                chomp = c;
        std::vector<std::pair<int, std::string>> body; // blank lines before, text
        int indent = -1;
        while (at < lines.size() && lines[at].indent > parent) {
            if (indent < 0)
                indent = lines[at].indent;
            body.emplace_back(lines[at].blanks,
                              std::string(static_cast<size_t>(std::max(0, lines[at].indent - indent)), ' ') +
                                  lines[at].raw);
            ++at;
        }
        int after = at < lines.size() ? lines[at].blanks : trailingBlanks;
        std::string out;
        for (size_t k = 0; k < body.size(); ++k) {
            auto &[blanks, text] = body[k];
            if (k > 0) {
                bool fold = !literal && blanks == 0 && text[0] != ' ' && body[k - 1].second[0] != ' ';
                out += fold ? " " : "\n";
                if (!literal && blanks > 0)
                    out += std::string(static_cast<size_t>(blanks - 1), '\n');
            }
            if (literal && k > 0)
                out += std::string(static_cast<size_t>(blanks), '\n');
            out += text;
        }
        if (!body.empty() && chomp != '-')
            out += "\n";
        if (chomp == '+')
            out += std::string(static_cast<size_t>(after), '\n');
        return make(Json::Text, out);
    }
    // The value after `key:` or `- ` on the same line, or (when empty) the block below.
    std::shared_ptr<Json> value(std::string rest, int indent, int line, bool item) {
        rest = ltrim(rtrim(rest));
        std::string anchor;
        if (!rest.empty() && rest[0] == '&') {
            auto space = rest.find(' ');
            anchor = rest.substr(1, space == std::string::npos ? std::string::npos : space - 1);
            rest = space == std::string::npos ? "" : ltrim(rest.substr(space + 1));
        }
        if (!rest.empty() && rest[0] == '!') { // tags are ignored
            auto space = rest.find(' ');
            rest = space == std::string::npos ? "" : ltrim(rest.substr(space + 1));
        }
        std::shared_ptr<Json> result;
        if (rest.empty()) {
            // A block below; a sequence may sit at its mapping key's indentation.
            if (at < lines.size() && (lines[at].indent > indent ||
                                      (!item && lines[at].indent == indent && sequenceLine(lines[at].text))))
                result = node(lines[at].indent);
            else
                result = make(Json::Null);
        } else if (rest[0] == '|' || rest[0] == '>')
            result = blockScalar(rest, indent);
        else {
            // Flow collections and quoted scalars may continue on more-indented lines; plain
            // scalars fold continuation lines into one.
            bool structured = rest[0] == '[' || rest[0] == '{' || rest[0] == '"' || rest[0] == '\'';
            while (structured && !balanced(rest) && at < lines.size() && lines[at].indent > indent) {
                rest += " " + lines[at].text;
                ++at;
            }
            while (!structured && rest[0] != '*' && at < lines.size() && lines[at].indent > indent &&
                   lines[at].blanks == 0 && mappingColon(lines[at].text) == std::string::npos &&
                   !sequenceLine(lines[at].text)) {
                rest += " " + lines[at].text;
                ++at;
            }
            size_t i = 0;
            result = flow(rest, i, line, false);
            spaces(rest, i);
            if (i < rest.size())
                error(line, "unexpected text after value: '" + rest.substr(i) + "'");
        }
        if (!anchor.empty())
            anchors[anchor] = result;
        return result;
    }
    std::string key(const std::string &text, int line) {
        auto t = rtrim(text);
        size_t i = 0;
        if (!t.empty() && t[0] == '"')
            return doubleQuoted(t, i, line);
        if (!t.empty() && t[0] == '\'')
            return singleQuoted(t, i, line);
        return t;
    }
    std::shared_ptr<Json> node(int indent) {
        auto &first = lines[at];
        if (sequenceLine(first.text)) {
            auto out = make(Json::List);
            while (at < lines.size() && lines[at].indent == indent && sequenceLine(lines[at].text)) {
                auto line = lines[at];
                auto after = line.text.size() > 1 ? line.text.substr(2) : std::string();
                auto lead = after.size() - ltrim(after).size();
                auto rest = ltrim(after);
                bool nested = sequenceLine(rest) ||
                              (mappingColon(rest) != std::string::npos && rest[0] != '[' && rest[0] != '{');
                if (nested) {
                    // "- key: value" starts a nested node on the item's own line.
                    lines[at].indent = indent + 2 + static_cast<int>(lead);
                    lines[at].text = rest;
                    lines[at].raw = ltrim(line.raw.substr(std::min<size_t>(1, line.raw.size())));
                    out->items.push_back(node(lines[at].indent));
                } else {
                    ++at;
                    out->items.push_back(value(rest, indent, line.number, true));
                }
            }
            if (at < lines.size() && lines[at].indent > indent)
                error(lines[at].number, "unexpected indentation");
            return out;
        }
        if (mappingColon(first.text) != std::string::npos) {
            auto out = make(Json::Dictionary);
            std::set<std::string> merged;
            while (at < lines.size() && lines[at].indent == indent && !sequenceLine(lines[at].text)) {
                auto line = lines[at];
                auto colon = mappingColon(line.text);
                if (colon == std::string::npos)
                    error(line.number, "expected 'key: value'");
                auto name = key(line.text.substr(0, colon), line.number);
                ++at;
                auto v = value(line.text.substr(colon + 1), indent, line.number, false);
                if (name == "<<") {
                    // Merge keys copy fields the mapping does not set itself.
                    auto sources = v->kind == Json::List ? v->items : std::vector<std::shared_ptr<Json>>{v};
                    for (auto &source : sources) {
                        if (source->kind != Json::Dictionary)
                            error(line.number, "<< merges mappings only");
                        for (auto &[k, item] : source->fields)
                            if (out->fields.emplace(k, item).second)
                                merged.insert(k);
                    }
                    continue;
                }
                if (out->fields.count(name) && !merged.erase(name))
                    error(line.number, "duplicate key '" + name + "'");
                out->fields[name] = v;
            }
            if (at < lines.size() && lines[at].indent > indent)
                error(lines[at].number, "unexpected indentation");
            return out;
        }
        ++at;
        return value(first.text, indent - 1, first.number, true);
    }

  public:
    explicit YamlParser(const std::string &source) {
        std::istringstream in(source);
        std::string raw;
        int number = 0, blanks = 0;
        bool content = false, marker = false;
        while (std::getline(in, raw)) {
            ++number;
            if (!raw.empty() && raw.back() == '\r')
                raw.pop_back();
            if (!content && raw.rfind("%", 0) == 0)
                continue; // directives
            if (raw.rfind("---", 0) == 0 && (raw.size() == 3 || raw[3] == ' ' || raw[3] == '\t')) {
                if (content || marker)
                    error(number, "only one YAML document is supported");
                marker = true;
                raw = raw.substr(3);
            }
            if (raw == "...")
                break;
            auto first = raw.find_first_not_of(' ');
            if (first != std::string::npos && raw[first] == '\t' && !rtrim(raw).empty())
                error(number, "tabs cannot indent YAML");
            auto contentText = first == std::string::npos ? std::string() : rtrim(raw.substr(first));
            auto text = rtrim(stripComment(contentText));
            if (text.empty()) {
                ++blanks;
                continue;
            }
            lines.push_back({number, static_cast<int>(first), blanks, text, contentText});
            blanks = 0;
            content = true;
        }
        trailingBlanks = blanks;
    }
    std::shared_ptr<Json> run() {
        if (lines.empty())
            return make(Json::Null);
        auto result = node(lines[0].indent);
        if (at < lines.size())
            error(lines[at].number, "unexpected content");
        return result;
    }
};
} // namespace
extern "C" Object *fw_yaml_parse(Frame *frame, Object *text) {
    return nativeBoundary([&] { return jsonObject(frame, YamlParser(requireString(text)->text).run()); });
}

// coverage: line counters registered by programs built for `flywheel test --coverage` -------
namespace {
struct CoverageTable {
    int64_t **counters = nullptr;
    int64_t count = 0;
    std::string table;
};
CoverageTable &coverageTable() {
    static CoverageTable *table = new CoverageTable; // used at exit
    return *table;
}
void writeCoverage() {
    auto &c = coverageTable();
    auto path = std::getenv("FW_COVERAGE_FILE");
    if (!path || !c.counters)
        return;
    std::ofstream out(path, std::ios::app);
    std::istringstream lines(c.table);
    std::string line;
    for (int64_t i = 0; i < c.count && std::getline(lines, line); ++i)
        out << line << '\t' << *c.counters[i] << '\n';
}
} // namespace
extern "C" void fw_coverage_register(int64_t **counters, int64_t count, const char *table) {
    auto &c = coverageTable();
    c.counters = counters;
    c.count = count;
    c.table = table;
    std::atexit(writeCoverage);
}

// For debuggers: `print fw_debug_format(variable)` shows any Flywheel value as text.
extern "C" const char *fw_debug_format(Object *value) {
    static thread_local std::string text;
    try {
        std::set<Object *> seen;
        Slot slot;
        slot.kind = Slot::Reference;
        slot.object = value;
        retain(value);
        text = formatSlot(slot, false, seen, 0);
    } catch (...) {
        text = "<unprintable>";
    }
    return text.c_str();
}

// --race: turned on by programs built with `--race`; globals are checked by address.
extern "C" void fw_race_enable() {
    raceMode.store(true);
    raceThread();
}
extern "C" void fw_race_global(const void *address, const char *name, int64_t write) {
    if (raceMode.load(std::memory_order_relaxed))
        raceCheck(address, [name] { return std::string("global variable '") + name + "'"; }, write != 0);
}
