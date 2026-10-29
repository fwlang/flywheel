// Native string, array, and math helpers. Included by runtime.cpp after collections.inc.
// Strings are UTF-8; every index and length here counts Unicode scalar values.
#include <random>
namespace {
std::vector<uint32_t> decodeUtf8(const std::string &s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        uint32_t v = n == 1 ? c : c & (0x3f >> (n - 1));
        for (size_t k = 1; k < n && i + k < s.size(); ++k)
            v = (v << 6) | (static_cast<unsigned char>(s[i + k]) & 63);
        out.push_back(v);
        i += n;
    }
    return out;
}
void appendUtf8(std::string &out, uint32_t v) {
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
}
std::string encodeUtf8(const std::vector<uint32_t> &chars, size_t begin, size_t end) {
    std::string out;
    for (size_t i = begin; i < end; ++i)
        appendUtf8(out, chars[i]);
    return out;
}
bool validScalar(int64_t v) {
    return v >= 0 && v <= 0x10ffff && !(v >= 0xd800 && v <= 0xdfff);
}
// Simple case mapping for ASCII, Latin-1, Latin Extended-A, Greek, and Cyrillic. Characters
// outside these ranges, and multi-character mappings such as German sharp s, are unchanged.
// In Latin Extended-A, 0x139-0x148 and 0x179-0x17e pair odd capitals with even small letters;
// the rest of the block pairs even capitals with odd small letters.
bool latinExtendedCapital(uint32_t c) {
    bool oddCapitals = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e);
    return c % 2 == (oddCapitals ? 1u : 0u);
}
bool latinExtendedPair(uint32_t c) {
    return c >= 0x100 && c <= 0x17e && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 &&
           c != 0x178;
}
uint32_t caseMap(uint32_t c, bool upper) {
    if (upper) {
        if (c == 0xff)
            return 0x178;
        if (c == 0x3c2)
            return 0x3a3;
        if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) ||
            (c >= 0x3b1 && c <= 0x3c9) || (c >= 0x430 && c <= 0x44f))
            return c - 0x20;
        if (c >= 0x450 && c <= 0x45f)
            return c - 0x50;
        if (latinExtendedPair(c) && !latinExtendedCapital(c))
            return c - 1;
        return c;
    }
    if (c == 0x178)
        return 0xff;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) ||
        (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) || (c >= 0x410 && c <= 0x42f))
        return c + 0x20;
    if (c >= 0x400 && c <= 0x40f)
        return c + 0x50;
    if (latinExtendedPair(c) && latinExtendedCapital(c))
        return c + 1;
    return c;
}
bool spaceScalar(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
           c == 0x205f || c == 0x3000;
}
// Slice bounds: negative values count from the end and results are clamped. mode 0 has no end,
// 1 has an exclusive end (a..<b), and 2 an inclusive end (a..b).
std::pair<size_t, size_t> sliceBounds(int64_t size, int64_t begin, int64_t end, int64_t mode) {
    if (mode == 0)
        end = size;
    if (begin < 0)
        begin += size;
    if (end < 0)
        end += size;
    if (mode == 2)
        end = end == std::numeric_limits<int64_t>::max() ? end : end + 1;
    begin = std::clamp<int64_t>(begin, 0, size);
    end = std::clamp<int64_t>(end, begin, size);
    return {static_cast<size_t>(begin), static_cast<size_t>(end)};
}
Object *stringArray(Frame *frame, const std::vector<std::string> &items) {
    auto array = managed(frame, Object::Array);
    for (auto &item : items) {
        auto text = new Object(Object::String);
        text->text = item;
        Slot slot;
        slot.kind = Slot::Reference;
        slot.object = text;
        array->slots.push_back(std::move(slot));
    }
    return array;
}
Object *requireString(Object *p) {
    if (!p || p->kind != Object::String)
        fail("expected string");
    return p;
}
int compareSlots(const Slot &a, const Slot &b) {
    switch (a.kind) {
    case Slot::Unsigned: {
        auto x = static_cast<uint64_t>(a.integer), y = static_cast<uint64_t>(b.integer);
        return x < y ? -1 : x > y;
    }
    case Slot::Real:
        if (std::isnan(a.real) || std::isnan(b.real))
            fail("cannot sort NaN values", "NumericError");
        return a.real < b.real ? -1 : a.real > b.real;
    case Slot::Reference:
        if (!a.object || !b.object || a.object->kind != Object::String ||
            b.object->kind != Object::String)
            fail("sort() without a comparator requires numbers, chars, bools, or strings");
        {
            auto order = a.object->text.compare(b.object->text);
            return order < 0 ? -1 : order > 0;
        }
    default:
        return a.integer < b.integer ? -1 : a.integer > b.integer;
    }
}
bool slotEqual(const Slot &a, const Slot &b) {
    if (a.kind == Slot::Real || b.kind == Slot::Real)
        return a.real == b.real;
    if (a.kind == Slot::Reference || b.kind == Slot::Reference)
        return fw_equal(a.object, b.object) != 0;
    return a.integer == b.integer;
}
std::mt19937_64 &randomEngine() {
    thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}
// Structural hashing and equality for tuple map keys (strings, primitives, nested tuples).
uint64_t mix(uint64_t h, uint64_t v) {
    return (h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
}
uint64_t structuralHash(const Slot &slot);
uint64_t structuralHash(Object *object) {
    if (!object)
        return 0;
    if (object->kind == Object::String)
        return std::hash<std::string>{}(object->text);
    if (object->kind == Object::Tuple || object->kind == Object::Array ||
        object->kind == Object::Box) {
        uint64_t h = 1469598103934665603ULL + static_cast<uint64_t>(object->kind);
        for (auto &item : object->slots)
            h = mix(h, structuralHash(item));
        return h;
    }
    return std::hash<const void *>{}(object);
}
uint64_t structuralHash(const Slot &slot) {
    if (slot.kind == Slot::Reference)
        return structuralHash(slot.object);
    if (slot.kind == Slot::Real)
        return std::hash<double>{}(slot.real == 0 ? 0.0 : slot.real);
    return std::hash<int64_t>{}(slot.integer);
}
bool structuralEqual(const Slot &a, const Slot &b);
bool structuralEqual(Object *a, Object *b) {
    if (a == b)
        return true;
    if (!a || !b || a->kind != b->kind)
        return false;
    if (a->kind == Object::String)
        return a->text == b->text;
    if (a->kind == Object::Tuple || a->kind == Object::Array || a->kind == Object::Box) {
        if (a->slots.size() != b->slots.size())
            return false;
        for (size_t i = 0; i < a->slots.size(); ++i)
            if (!structuralEqual(a->slots[i], b->slots[i]))
                return false;
        return true;
    }
    return false;
}
bool structuralEqual(const Slot &a, const Slot &b) {
    if (a.kind == Slot::Reference || b.kind == Slot::Reference)
        return a.kind == b.kind && structuralEqual(a.object, b.object);
    if (a.kind == Slot::Real)
        return a.real == b.real;
    return a.integer == b.integer;
}
// A comparator callback already recorded a pending error; stop without replacing it.
struct CallbackFailure {};
} // namespace
extern "C" {
// Equality of nullable primitives: both null, or both present with equal values.
int64_t fw_box_equal(Object *a, Object *b) {
    if (!a || !b)
        return a == b;
    return slotEqual(a->slots.at(0), b->slots.at(0));
}
uint64_t fw_structural_hash(Object *key) {
    return structuralHash(key);
}
// Finds a tuple or class key by hash. `equal` is the generated key.equals(other) callback for
// class keys, or null for structural equality. mode 0 raises KeyError when the key is missing,
// 1 inserts it, and 2 returns -1.
int64_t fw_map_find_hashed(Object *map, uint64_t hash, Object *key,
                           int64_t (*equal)(Object *, Object *), int64_t mode) {
    try {
        auto &index = mapIndex(map);
        auto [begin, end] = index.hashed.equal_range(hash);
        for (auto it = begin; it != end; ++it) {
            auto candidate = map->keys[it->second].object;
            bool same = equal ? equal(key, candidate) != 0 : structuralEqual(key, candidate);
            if (pendingError.value)
                throw CallbackFailure{};
            if (same)
                return static_cast<int64_t>(it->second);
        }
        if (mode == 0)
            fail("map key not found", "KeyError");
        if (mode == 2)
            return -1;
        map->more().keyHashes.push_back(hash);
        return static_cast<int64_t>(appendKey(map, reference(key), Slot{}));
    } catch (const CallbackFailure &) {
        return -1;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
        return -1;
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
        return -1;
    }
}
void fw_map_remove_at(Object *map, int64_t index) {
    nativeBoundary([&] {
        auto i = static_cast<size_t>(index);
        map->keys.erase(map->keys.begin() + index);
        map->slots.erase(map->slots.begin() + index);
        if (map->extra && i < map->extra->keyHashes.size())
            map->extra->keyHashes.erase(map->extra->keyHashes.begin() + index);
        map->index.reset();
    });
}
Object *fw_map_values(Frame *frame, Object *map) {
    return nativeBoundary([&] {
        auto values = managed(frame, Object::Array);
        values->slots = map->slots;
        return values;
    });
}
// Inserts every item of a sequence into a set built for the item kind (see fw_map_new).
void fw_set_add_all(Object *set, Object *items) {
    nativeBoundary([&] {
        for (auto &item : items->slots) {
            if (set->hashMode() == 1) {
                auto hash = structuralHash(item.object);
                if (fw_map_find_hashed(set, hash, item.object, nullptr, 2) < 0)
                    fw_map_find_hashed(set, hash, item.object, nullptr, 1);
            } else if (findKey(set, item.integer, item.object) < 0) {
                Slot key = item;
                if (!set->objectKeys)
                    key.kind = set->keyKind;
                appendKey(set, key, Slot{});
            }
        }
    });
}
Object *fw_char_string(Frame *frame, int64_t scalar) {
    return nativeBoundary([&] {
        if (!validScalar(scalar))
            fail("invalid Unicode scalar", "CastError");
        std::string out;
        appendUtf8(out, static_cast<uint32_t>(scalar));
        return string(frame, out);
    });
}
int64_t fw_string_to_char(Object *s) {
    return nativeBoundary([&]() -> int64_t {
        auto chars = decodeUtf8(requireString(s)->text);
        if (chars.size() != 1)
            fail("char() requires a string holding exactly one character", "CastError");
        return chars[0];
    });
}
int64_t fw_int_to_char(int64_t v) {
    return nativeBoundary([&]() -> int64_t {
        if (!validScalar(v))
            fail("integer is not a Unicode scalar value", "CastError");
        return v;
    });
}
int64_t fw_string_char_at(Object *s, int64_t index) {
    return nativeBoundary([&]() -> int64_t {
        auto &text = requireString(s)->text;
        if (index >= 0) {
            size_t count = 0;
            for (size_t i = 0; i < text.size();) {
                size_t length;
                auto c = static_cast<unsigned char>(text[i]);
                length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
                if (count++ == static_cast<size_t>(index))
                    return decodeUtf8(text.substr(i, length))[0];
                i += length;
            }
        }
        fail("string index out of bounds", "IndexError");
    });
}
Object *fw_string_chars(Frame *frame, Object *s) {
    return nativeBoundary([&] {
        auto array = managed(frame, Object::Array);
        for (auto c : decodeUtf8(requireString(s)->text)) {
            Slot slot;
            slot.kind = Slot::Char;
            slot.integer = c;
            array->slots.push_back(slot);
        }
        return array;
    });
}
// 0 upper, 1 lower, 2 trim, 3 trim_start, 4 trim_end, 5 reverse
Object *fw_string_transform(Frame *frame, Object *s, int64_t operation) {
    return nativeBoundary([&] {
        auto chars = decodeUtf8(requireString(s)->text);
        if (operation <= 1) {
            for (auto &c : chars)
                c = caseMap(c, operation == 0);
            return string(frame, encodeUtf8(chars, 0, chars.size()));
        }
        if (operation == 5) {
            std::reverse(chars.begin(), chars.end());
            return string(frame, encodeUtf8(chars, 0, chars.size()));
        }
        size_t begin = 0, end = chars.size();
        if (operation != 4)
            while (begin < end && spaceScalar(chars[begin]))
                ++begin;
        if (operation != 3)
            while (end > begin && spaceScalar(chars[end - 1]))
                --end;
        return string(frame, encodeUtf8(chars, begin, end));
    });
}
// separator == null splits on runs of whitespace and drops empty pieces.
Object *fw_string_split(Frame *frame, Object *s, Object *separator, int64_t lines) {
    return nativeBoundary([&] {
        auto &text = requireString(s)->text;
        std::vector<std::string> pieces;
        if (lines) {
            std::string current;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n' || text[i] == '\r') {
                    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                        ++i;
                    pieces.push_back(current);
                    current.clear();
                } else
                    current += text[i];
            }
            if (!current.empty())
                pieces.push_back(current);
        } else if (!separator) {
            auto chars = decodeUtf8(text);
            size_t i = 0;
            while (i < chars.size()) {
                while (i < chars.size() && spaceScalar(chars[i]))
                    ++i;
                size_t begin = i;
                while (i < chars.size() && !spaceScalar(chars[i]))
                    ++i;
                if (i > begin)
                    pieces.push_back(encodeUtf8(chars, begin, i));
            }
        } else {
            auto &sep = requireString(separator)->text;
            if (sep.empty())
                fail("split separator cannot be empty; use chars() instead");
            size_t begin = 0;
            for (auto at = text.find(sep); at != std::string::npos; at = text.find(sep, begin)) {
                pieces.push_back(text.substr(begin, at - begin));
                begin = at + sep.size();
            }
            pieces.push_back(text.substr(begin));
        }
        return stringArray(frame, pieces);
    });
}
Object *fw_string_replace(Frame *frame, Object *s, Object *from, Object *to) {
    return nativeBoundary([&] {
        auto &text = requireString(s)->text;
        auto &old = requireString(from)->text;
        auto &replacement = requireString(to)->text;
        if (old.empty())
            fail("replace() requires a non-empty search string");
        std::string out;
        size_t begin = 0;
        for (auto at = text.find(old); at != std::string::npos; at = text.find(old, begin)) {
            out += text.substr(begin, at - begin);
            out += replacement;
            begin = at + old.size();
        }
        out += text.substr(begin);
        return string(frame, out);
    });
}
// 0 find, 1 contains, 2 starts_with, 3 ends_with, 4 count, 5 find_last. find results are
// character indices, or -1 when absent.
int64_t fw_string_search(Object *s, Object *needle, int64_t operation) {
    return nativeBoundary([&]() -> int64_t {
        auto &text = requireString(s)->text;
        auto &sub = requireString(needle)->text;
        if (operation == 2)
            return text.compare(0, sub.size(), sub) == 0;
        if (operation == 3)
            return text.size() >= sub.size() &&
                   text.compare(text.size() - sub.size(), sub.size(), sub) == 0;
        if (operation == 4) {
            if (sub.empty())
                fail("count() requires a non-empty search string");
            int64_t n = 0;
            for (auto at = text.find(sub); at != std::string::npos;
                 at = text.find(sub, at + sub.size()))
                ++n;
            return n;
        }
        auto at = operation == 5 ? text.rfind(sub) : text.find(sub);
        if (operation == 1)
            return at != std::string::npos;
        return at == std::string::npos ? -1
                                       : static_cast<int64_t>(utf8Length(text.substr(0, at)));
    });
}
Object *fw_string_slice(Frame *frame, Object *s, int64_t begin, int64_t end, int64_t mode) {
    return nativeBoundary([&] {
        auto chars = decodeUtf8(requireString(s)->text);
        auto [from, to] = sliceBounds(static_cast<int64_t>(chars.size()), begin, end, mode);
        return string(frame, encodeUtf8(chars, from, to));
    });
}
Object *fw_string_pad(Frame *frame, Object *s, int64_t width, Object *fill, int64_t atEnd) {
    return nativeBoundary([&] {
        auto &text = requireString(s)->text;
        auto padding = decodeUtf8(requireString(fill)->text);
        if (padding.size() != 1)
            fail("padding must be exactly one character");
        auto length = static_cast<int64_t>(utf8Length(text));
        std::string pad;
        for (int64_t i = length; i < width; ++i)
            appendUtf8(pad, padding[0]);
        return string(frame, atEnd ? text + pad : pad + text);
    });
}
// Joins any array; non-string items use the same formatting as string(value).
Object *fw_array_join(Frame *frame, Object *array, Object *separator) {
    return nativeBoundary([&] {
        auto &sep = requireString(separator)->text;
        std::string out;
        for (size_t i = 0; i < array->slots.size(); ++i) {
            if (i)
                out += sep;
            auto &item = array->slots[i];
            out += item.kind == Slot::Reference && item.object &&
                           item.object->kind == Object::String
                       ? item.object->text
                       : formatValue(item);
        }
        return string(frame, out);
    });
}
void fw_array_sort(Object *array) {
    nativeBoundary([&] {
        std::stable_sort(array->slots.begin(), array->slots.end(),
                         [](const Slot &a, const Slot &b) { return compareSlots(a, b) < 0; });
    });
}
// Sorts with a generated callback: less(closure, array, i, j) compares the original items i, j.
void fw_array_sort_by(Object *array, Object *closure,
                      int64_t (*less)(Object *, Object *, int64_t, int64_t)) {
    try {
        std::vector<int64_t> order(array->slots.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<int64_t>(i);
        std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
            auto result = less(closure, array, a, b);
            if (pendingError.value)
                throw CallbackFailure{};
            return result != 0;
        });
        std::vector<Slot> sorted;
        sorted.reserve(order.size());
        for (auto i : order)
            sorted.push_back(array->slots[static_cast<size_t>(i)]);
        array->slots = std::move(sorted);
    } catch (const CallbackFailure &) {
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
    }
}
void fw_array_reverse(Object *array) {
    nativeBoundary([&] { std::reverse(array->slots.begin(), array->slots.end()); });
}
Object *fw_array_slice(Frame *frame, Object *array, int64_t begin, int64_t end, int64_t mode) {
    return nativeBoundary([&] {
        auto [from, to] = sliceBounds(static_cast<int64_t>(array->slots.size()), begin, end, mode);
        auto out = managed(frame, array->kind);
        out->slots.assign(array->slots.begin() + static_cast<ptrdiff_t>(from),
                          array->slots.begin() + static_cast<ptrdiff_t>(to));
        return out;
    });
}
// Searches with the item boxed in slot 0 of `box`; returns the first index or -1.
int64_t fw_array_index_of(Object *array, Object *box) {
    return nativeBoundary([&]() -> int64_t {
        auto &item = box->slots.at(0);
        for (size_t i = 0; i < array->slots.size(); ++i)
            if (slotEqual(array->slots[i], item))
                return static_cast<int64_t>(i);
        return -1;
    });
}
// Opens slot `index` (0..length) for an inserted value and returns it.
int64_t fw_array_insert(Object *array, int64_t index) {
    return nativeBoundary([&]() -> int64_t {
        if (index < 0 || static_cast<size_t>(index) > array->slots.size())
            fail("insert index out of bounds", "IndexError");
        array->slots.insert(array->slots.begin() + index, Slot{});
        return index;
    });
}
void fw_array_remove_at(Object *array, int64_t index) {
    nativeBoundary([&] {
        if (index < 0 || static_cast<size_t>(index) >= array->slots.size())
            fail("remove_at index out of bounds", "IndexError");
        array->slots.erase(array->slots.begin() + index);
    });
}
Object *fw_array_copy(Frame *frame, Object *array) {
    return nativeBoundary([&] {
        auto out = managed(frame, array->kind);
        out->slots = array->slots;
        return out;
    });
}
void fw_array_extend(Object *array, Object *other) {
    nativeBoundary([&] {
        auto items = other->slots;
        array->slots.insert(array->slots.end(), items.begin(), items.end());
    });
}
// 0 floor, 1 ceil, 2 round (half away from zero), 3 trunc, 4 log, 5 log10, 6 log2, 7 exp,
// 8 tan, 9 asin, 10 acos, 11 atan, 12 abs
double fw_math_unary(double x, int64_t operation) {
    switch (operation) {
    case 0:
        return std::floor(x);
    case 1:
        return std::ceil(x);
    case 2:
        return std::round(x);
    case 3:
        return std::trunc(x);
    case 4:
        return std::log(x);
    case 5:
        return std::log10(x);
    case 6:
        return std::log2(x);
    case 7:
        return std::exp(x);
    case 8:
        return std::tan(x);
    case 9:
        return std::asin(x);
    case 10:
        return std::acos(x);
    case 11:
        return std::atan(x);
    default:
        return std::fabs(x);
    }
}
double fw_math_binary(double a, double b, int64_t operation) {
    return operation == 0 ? std::atan2(a, b) : std::hypot(a, b);
}
double fw_random() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(randomEngine());
}
int64_t fw_random_int(int64_t low, int64_t high) {
    return nativeBoundary([&]() -> int64_t {
        if (low > high)
            fail("random_int requires low <= high", "NumericError");
        return std::uniform_int_distribution<int64_t>(low, high)(randomEngine());
    });
}
void fw_random_seed(int64_t seed) {
    randomEngine().seed(static_cast<uint64_t>(seed));
}
}
