// Native value formatting and explicit collection conversions. Included by runtime.cpp.
namespace {
template <class F> auto nativeBoundary(F &&function) -> decltype(function()) {
    using R = decltype(function());
    try {
        return function();
    } catch (const NativeFailure &error) {
        recordFailure(error.type, error.message);
    } catch (const std::exception &error) {
        recordFailure("RuntimeError", error.what());
    }
    if constexpr (!std::is_void_v<R>)
        return R{};
}
Slot reference(Object *object) {
    Slot result;
    result.kind = Slot::Reference;
    result.object = object;
    retain(object);
    return result;
}
Object *managed(Frame *frame, Object::Kind kind) {
    return own(frame, new Object(kind));
}
std::string readableType(std::string text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "$m") == 0) {
            size_t p = i + 2;
            while (p < text.size() && text[p] >= '0' && text[p] <= '9')
                ++p;
            if (p > i + 2 && p < text.size() && text[p] == '$') {
                i = p + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}
std::string formatSlot(const Slot &, bool, std::set<Object *> &, size_t);
std::string formatObject(Object *object, bool nested, std::set<Object *> &active, size_t depth) {
    if (!object)
        return "null";
    if (depth > 256)
        fail("formatting nesting exceeds 256");
    if (object->kind == Object::String)
        return nested ? jsonQuote(object->text) : object->text;
    if (object->kind == Object::Element) {
        std::string out;
        render(object, out);
        return out;
    }
    if (object->kind == Object::JsonValue)
        return jsonRender(*object->json);
    if (object->kind == Object::DesktopWindow)
        return "desktop.window(" + jsonQuote(object->desktop->title) + ")";
    if (object->kind == Object::ErrorValue)
        return nested ? readableType(object->errorType) + "(" + jsonQuote(object->text) + ")"
                      : object->text;
    if (!active.insert(object).second)
        return "<cycle>";
    struct Remove {
        std::set<Object *> &values;
        Object *value;
        ~Remove() {
            values.erase(value);
        }
    } remove{active, object};
    auto item = [&](const Slot &slot) { return formatSlot(slot, true, active, depth + 1); };
    std::string out;
    if (object->kind == Object::Map) {
        out = "{";
        for (size_t i = 0; i < object->slots.size(); ++i) {
            if (i)
                out += ", ";
            out += item(object->keys[i]) + ": " + item(object->slots[i]);
        }
        return out + "}";
    }
    if (object->kind == Object::Result) {
        return std::string(object->slots[0].integer ? "Result.ok(" : "Result.error(") +
               item(object->slots[1]) + ")";
    }
    if (object->kind == Object::Instance) {
        out = (object->text.empty() ? "instance" : object->text) + "(";
        for (size_t i = 0; i < object->slots.size(); ++i) {
            if (i)
                out += ", ";
            if (i < object->fieldNames.size())
                out += object->fieldNames[i] + "=";
            out += item(object->slots[i]);
        }
        return out + ")";
    }
    bool tuple = object->kind == Object::Tuple;
    out = tuple ? "(" : "[";
    for (size_t i = 0; i < object->slots.size(); ++i) {
        if (i)
            out += ", ";
        out += item(object->slots[i]);
    }
    if (tuple && object->slots.size() == 1)
        out += ',';
    return out + (tuple ? ")" : "]");
}
std::string formatSlot(const Slot &value, bool nested, std::set<Object *> &active, size_t depth) {
    switch (value.kind) {
    case Slot::Integer:
        return std::to_string(value.integer);
    case Slot::Unsigned:
        return std::to_string(static_cast<uint64_t>(value.integer));
    case Slot::Boolean:
        return value.integer ? "true" : "false";
    case Slot::Real: {
        std::ostringstream out;
        out << std::setprecision(15) << value.real;
        return out.str();
    }
    case Slot::Reference:
        return formatObject(value.object, nested, active, depth);
    }
    fail("invalid value kind");
}
std::string formatValue(const Slot &value) {
    std::set<Object *> active;
    return formatSlot(value, false, active, 0);
}
struct CastType {
    enum Kind { Scalar, Array, Tuple, FixedTuple, Map, Result, Nullable } kind = Scalar;
    std::string text;
    std::vector<CastType> parts;
};
std::vector<std::string> splitTypes(const std::string &text) {
    int depth = 0;
    size_t start = 0;
    std::vector<std::string> result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '[' || text[i] == '<')
            ++depth;
        else if (text[i] == ']' || text[i] == '>')
            --depth;
        else if (text[i] == ',' && depth == 0) {
            result.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size())
        result.push_back(text.substr(start));
    return result;
}
CastType castType(const std::string &text) {
    CastType out;
    out.text = text;
    if (!text.empty() && text.back() == '?') {
        out.kind = CastType::Nullable;
        out.parts.push_back(castType(text.substr(0, text.size() - 1)));
        return out;
    }
    if (text.size() > 2 && text.substr(text.size() - 2) == "[]") {
        out.kind = CastType::Array;
        out.parts.push_back(castType(text.substr(0, text.size() - 2)));
        return out;
    }
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        out.kind = CastType::FixedTuple;
        for (auto &p : splitTypes(text.substr(1, text.size() - 2)))
            out.parts.push_back(castType(p));
        return out;
    }
    for (auto [prefix, kind] : {std::pair{"tuple<", CastType::Tuple},
                                {"map<", CastType::Map},
                                {"Result<", CastType::Result}}) {
        if (text.rfind(prefix, 0) == 0 && text.back() == '>') {
            out.kind = kind;
            size_t n = std::strlen(prefix);
            for (auto &p : splitTypes(text.substr(n, text.size() - n - 1)))
                out.parts.push_back(castType(p));
            break;
        }
    }
    return out;
}
bool sequenceType(const CastType &type) {
    return type.kind == CastType::Array || type.kind == CastType::Tuple ||
           type.kind == CastType::FixedTuple;
}
CastType itemType(const CastType &type, size_t index) {
    return type.parts[type.kind == CastType::FixedTuple ? index : 0];
}
bool sequenceObject(Object *value) {
    return value && (value->kind == Object::Array || value->kind == Object::Tuple);
}
Slot castValue(Frame *, const Slot &, const CastType &, const CastType &, bool, size_t);
Slot castNumber(const Slot &value, const std::string &type) {
    bool real = type == "float" || type == "double";
    bool unsign = !real && type[0] == 'u';
    int bits = real ? (type == "float" ? 32 : 64) : std::stoi(type.substr(1));
    Slot source = value, result;
    if (source.kind == Slot::Reference) {
        if (!source.object || source.object->kind != Object::String)
            fail("numeric cast requires a number or numeric string", "CastError");
        auto &text = source.object->text;
        if (real) {
            double n;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
            if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(n))
                fail("invalid or out-of-range real string", "NumericError");
            source.kind = Slot::Real;
            source.real = n;
        } else if (unsign) {
            uint64_t n;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
            if (error != std::errc{} || end != text.data() + text.size())
                fail("invalid or out-of-range unsigned integer string", "NumericError");
            source.kind = Slot::Unsigned;
            source.integer = static_cast<int64_t>(n);
        } else {
            int64_t n;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
            if (error != std::errc{} || end != text.data() + text.size())
                fail("invalid or out-of-range integer string", "NumericError");
            source.kind = Slot::Integer;
            source.integer = n;
        }
    }
    if (real) {
        double n = source.kind == Slot::Real ? source.real
                   : source.kind == Slot::Unsigned
                       ? static_cast<double>(static_cast<uint64_t>(source.integer))
                   : source.kind == Slot::Boolean ? (source.integer ? 1.0 : 0.0)
                                                  : static_cast<double>(source.integer);
        if (type == "float") {
            if (!std::isfinite(n) || n > std::numeric_limits<float>::max() ||
                n < -std::numeric_limits<float>::max())
                fail("floating cast out of range", "NumericError");
            n = static_cast<float>(n);
        }
        result.kind = Slot::Real;
        result.real = n;
        return result;
    }
    if (source.kind == Slot::Boolean) {
        source.kind = Slot::Integer;
        source.integer = source.integer ? 1 : 0;
    }
    if (source.kind == Slot::Real) {
        double low = unsign ? 0 : -std::ldexp(1.0, bits - 1),
               high = std::ldexp(1.0, bits - (unsign ? 0 : 1));
        if (!std::isfinite(source.real) || source.real < low || source.real >= high)
            fail("floating to integer cast out of range", "NumericError");
        result.kind = unsign ? Slot::Unsigned : Slot::Integer;
        result.integer = unsign ? static_cast<int64_t>(static_cast<uint64_t>(source.real))
                                : static_cast<int64_t>(source.real);
        return result;
    }
    if (unsign) {
        if (source.kind != Slot::Unsigned && source.integer < 0)
            fail("integer cast out of range", "NumericError");
        auto n = static_cast<uint64_t>(source.integer);
        if (bits < 64 && n > ((uint64_t(1) << bits) - 1))
            fail("integer cast out of range", "NumericError");
        result.kind = Slot::Unsigned;
        result.integer = static_cast<int64_t>(n);
    } else {
        if (source.kind == Slot::Unsigned &&
            static_cast<uint64_t>(source.integer) >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            fail("integer cast out of range", "NumericError");
        auto n = source.integer;
        if (bits < 64 && (n < -(int64_t(1) << (bits - 1)) || n > ((int64_t(1) << (bits - 1)) - 1)))
            fail("integer cast out of range", "NumericError");
        result.kind = Slot::Integer;
        result.integer = n;
    }
    return result;
}
Slot castValue(Frame *frame, const Slot &value, const CastType &source, const CastType &target,
               bool root, size_t depth) {
    if (depth > 128)
        fail("cast nesting exceeds 128", "CastError");
    if (source.text == target.text && !root)
        return value;
    if (target.text == "string")
        return reference(string(frame, formatValue(value)));
    bool isNull = value.kind == Slot::Reference && !value.object;
    if (isNull && target.text == "bool") {
        Slot out;
        out.kind = Slot::Boolean;
        out.integer = 0;
        return out;
    }
    if (target.kind == CastType::Nullable) {
        if (isNull)
            return value;
        auto actual = source.kind == CastType::Nullable ? source.parts[0] : source;
        return castValue(frame, value, actual, target.parts[0], false, depth + 1);
    }
    bool wrapNullable =
        source.kind == CastType::Nullable && sequenceType(target) &&
        ((target.kind == CastType::FixedTuple && target.parts.size() == 1 &&
          target.parts[0].text == source.text) ||
         (target.kind != CastType::FixedTuple && target.parts[0].text == source.text));
    if (source.kind == CastType::Nullable && !wrapNullable) {
        if (isNull)
            fail("cannot cast null to a non-null type", "CastError");
        return castValue(frame, value, source.parts[0], target, root, depth + 1);
    }
    if (target.kind == CastType::Map) {
        auto output = managed(frame, Object::Map);
        output->objectKeys = target.parts[0].text == "string";
        output->keyKind = output->objectKeys               ? Slot::Reference
                          : target.parts[0].text == "bool" ? Slot::Boolean
                          : target.parts[0].text[0] == 'u' ? Slot::Unsigned
                                                           : Slot::Integer;
        auto insert = [&](const Slot &key, const CastType &keyType, const Slot &item,
                          const CastType &valueType) {
            auto convertedKey = castValue(frame, key, keyType, target.parts[0], false, depth + 1);
            auto convertedValue =
                castValue(frame, item, valueType, target.parts[1], false, depth + 1);
            auto index = findKey(output, convertedKey.integer, convertedKey.object);
            if (index < 0) {
                output->keys.push_back(convertedKey);
                output->slots.push_back(convertedValue);
            } else
                output->slots[static_cast<size_t>(index)] = convertedValue;
        };
        if (source.kind == CastType::Map) {
            for (size_t i = 0; i < value.object->slots.size(); ++i)
                insert(value.object->keys[i], source.parts[0], value.object->slots[i],
                       source.parts[1]);
        } else if (sequenceType(source) && value.kind == Slot::Reference &&
                   sequenceObject(value.object)) {
            for (size_t i = 0; i < value.object->slots.size(); ++i) {
                auto &pair = value.object->slots[i];
                auto pairType = itemType(source, i);
                if (pair.kind != Slot::Reference || !sequenceObject(pair.object) ||
                    pair.object->slots.size() != 2 || !sequenceType(pairType))
                    fail("map cast requires two-item pairs", "CastError");
                insert(pair.object->slots[0], itemType(pairType, 0), pair.object->slots[1],
                       itemType(pairType, 1));
            }
        } else
            fail("map cast requires a map or sequence of pairs", "CastError");
        return reference(output);
    }
    if (sequenceType(target)) {
        std::vector<Slot> items;
        std::vector<CastType> types;
        if (source.kind == CastType::Map) {
            for (size_t i = 0; i < value.object->slots.size(); ++i) {
                auto pair = managed(frame, Object::Tuple);
                pair->slots = {value.object->keys[i], value.object->slots[i]};
                items.push_back(reference(pair));
                types.push_back(
                    castType("[" + source.parts[0].text + "," + source.parts[1].text + "]"));
            }
        } else if (sequenceType(source) && value.kind == Slot::Reference &&
                   sequenceObject(value.object)) {
            items = value.object->slots;
            for (size_t i = 0; i < items.size(); ++i)
                types.push_back(itemType(source, i));
        } else {
            items.push_back(value);
            types.push_back(source);
        }
        if (target.kind == CastType::FixedTuple && items.size() != target.parts.size())
            fail("tuple cast length mismatch", "CastError");
        auto output =
            managed(frame, target.kind == CastType::Array ? Object::Array : Object::Tuple);
        output->slots.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i)
            output->slots.push_back(
                castValue(frame, items[i], types[i], itemType(target, i), false, depth + 1));
        return reference(output);
    }
    if (target.text == "string")
        return reference(string(frame, formatValue(value)));
    if (target.text == "bool") {
        Slot out;
        out.kind = Slot::Boolean;
        if (value.kind == Slot::Reference) {
            if (!value.object)
                out.integer = 0;
            else if (value.object->kind == Object::String) {
                auto &text = value.object->text;
                if (text == "true" || text == "1")
                    out.integer = 1;
                else if (text == "false" || text == "0")
                    out.integer = 0;
                else
                    fail("boolean string must be true, false, 1, or 0", "CastError");
            } else if (sequenceObject(value.object) || value.object->kind == Object::Map)
                out.integer = !value.object->slots.empty();
            else
                out.integer = 1;
        } else
            out.integer = value.kind == Slot::Real ? value.real != 0 : value.integer != 0;
        return out;
    }
    if ((target.text.size() > 1 && (target.text[0] == 'i' || target.text[0] == 'u') &&
         target.text[1] >= '0' && target.text[1] <= '9') ||
        target.text == "float" || target.text == "double") {
        if (sequenceType(source) && value.kind == Slot::Reference && sequenceObject(value.object)) {
            if (value.object->slots.size() != 1)
                fail("scalar cast requires a single-item sequence", "CastError");
            return castValue(frame, value.object->slots[0], itemType(source, 0), target, false,
                             depth + 1);
        }
        return castNumber(value, target.text);
    }
    if (target.kind == CastType::Result && source.kind == CastType::Result) {
        auto result = managed(frame, Object::Result);
        bool ok = value.object->slots[0].integer != 0;
        result->slots.push_back(value.object->slots[0]);
        result->slots.push_back(castValue(frame, value.object->slots[1], source.parts[ok ? 0 : 1],
                                          target.parts[ok ? 0 : 1], false, depth + 1));
        return reference(result);
    }
    if (value.kind == Slot::Reference && value.object) {
        if (target.text == "element" && value.object->kind == Object::Element)
            return value;
        if (target.text == "fragment" && value.object->kind == Object::Element &&
            value.object->text.empty())
            return value;
        if (target.text == value.object->typeName)
            return value;
    }
    if (source.text == target.text)
        return value;
    fail("cannot cast " + readableType(source.text) + " to " + readableType(target.text),
         "CastError");
}
} // namespace
extern "C" {
Object *fw_instance(Frame *frame, int64_t count, const char *type, const char *name,
                    const char *fields) {
    return nativeBoundary([&] {
        auto result = managed(frame, Object::Instance);
        result->typeName = type;
        result->text = name;
        result->slots.resize(static_cast<size_t>(count));
        std::string names = fields;
        size_t begin = 0;
        while (begin < names.size()) {
            auto end = names.find(',', begin);
            result->fieldNames.push_back(
                names.substr(begin, end == std::string::npos ? end : end - begin));
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return result;
    });
}
Object *fw_format(Frame *frame, Object *value) {
    return nativeBoundary([&] {
        std::set<Object *> active;
        return string(frame, formatObject(value, false, active, 0));
    });
}
Object *fw_collection_cast(Frame *frame, Object *box, const char *source, const char *target) {
    return nativeBoundary([&] {
        auto result = castValue(frame, box->slots[0], castType(source), castType(target), true, 0);
        auto out = managed(frame, Object::Instance);
        out->slots.push_back(result);
        return out;
    });
}
Object *fw_repeat(Frame *frame, Object *source, int64_t count) {
    return nativeBoundary([&] {
        if (count < 0)
            fail("repetition count must be non-negative", "NumericError");
        if (source->kind == Object::String) {
            if (count &&
                source->text.size() > std::string().max_size() / static_cast<uint64_t>(count))
                fail("string repetition size overflow", "NumericError");
            std::string out;
            if (source->text.empty())
                return string(frame, "");
            out.reserve(source->text.size() * static_cast<size_t>(count));
            for (int64_t i = 0; i < count; ++i)
                out += source->text;
            return string(frame, std::move(out));
        }
        if (!sequenceObject(source))
            fail("only arrays, tuples, and strings can be repeated", "CastError");
        auto out = managed(frame, source->kind);
        if (source->slots.empty())
            return out;
        if (count && source->slots.size() > out->slots.max_size() / static_cast<uint64_t>(count))
            fail("collection repetition size overflow", "NumericError");
        out->slots.reserve(source->slots.size() * static_cast<size_t>(count));
        for (int64_t i = 0; i < count; ++i)
            out->slots.insert(out->slots.end(), source->slots.begin(), source->slots.end());
        return out;
    });
}
Object *fw_tuple_range(Frame *frame, int64_t begin, int64_t end, int64_t inclusive) {
    return nativeBoundary([&] {
        auto out = managed(frame, Object::Tuple);
        if (end < begin || (!inclusive && end == begin))
            return out;
        uint64_t count = static_cast<uint64_t>(end) - static_cast<uint64_t>(begin);
        if (inclusive) {
            if (count == std::numeric_limits<uint64_t>::max())
                fail("range size overflow", "NumericError");
            ++count;
        }
        if (count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            count > out->slots.max_size())
            fail("range size overflow", "NumericError");
        out->slots.resize(static_cast<size_t>(count));
        for (size_t i = 0; i < out->slots.size(); ++i)
            out->slots[i].integer = begin + static_cast<int64_t>(i);
        return out;
    });
}
}
