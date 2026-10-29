// Tree-scoped HTML querying and mutation; no runtime XML/HTML parser is involved.
namespace {
Object *element(Object *value) {
    if (!value || value->kind != Object::Element)
        fail("DOM operation requires a non-null element or fragment");
    return value;
}
Object *attributeElement(Object *value) {
    element(value);
    if (value->text.empty())
        fail("fragments do not have attributes");
    return value;
}
std::string lowerAscii(std::string value) {
    for (char &c : value)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return value;
}
bool domSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}
std::vector<std::string> words(const std::string &text) {
    std::vector<std::string> result;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && domSpace(text[i]))
            ++i;
        size_t begin = i;
        while (i < text.size() && !domSpace(text[i]))
            ++i;
        if (i > begin)
            result.push_back(text.substr(begin, i - begin));
    }
    return result;
}
const std::string *attribute(Object *node, const std::string &name) {
    for (auto &[key, value] : node->attributes)
        if (key == name)
            return &value;
    return nullptr;
}
std::string attributeName(const std::string &text) {
    if (text.empty())
        fail("attribute name cannot be empty");
    for (unsigned char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == ':' || c == '.'))
            fail("invalid attribute name");
    return lowerAscii(text);
}
void setAttribute(Object *node, const std::string &name, const std::string &value) {
    for (auto &pair : node->attributes)
        if (pair.first == name) {
            pair.second = value;
            return;
        }
    node->attributes.emplace_back(name, value);
}
struct SelectorAttribute {
    std::string name, op, value;
};
struct SelectorStep;
using SelectorGroup = std::vector<SelectorStep>;
// :first-child, :nth-child(2n+1), :not(.a, p), ... `a`/`b` hold an+b for the nth forms.
struct SelectorPseudo {
    std::string name;
    int64_t a = 0, b = 0;
    std::vector<SelectorGroup> negated;
};
struct SelectorCompound {
    std::string tag;
    std::vector<std::string> ids, classes;
    std::vector<SelectorAttribute> attributes;
    std::vector<SelectorPseudo> pseudos;
};
struct SelectorStep {
    SelectorCompound compound;
    char relation = ' '; // ' ' descendant, '>' child, '+' next sibling, '~' later sibling
};
class SelectorParser {
    const std::string &text;
    size_t position = 0;
    [[noreturn]] void error(const std::string &message) {
        fail("invalid selector at byte " + std::to_string(position) + ": " + message,
             "SelectorError");
    }
    bool space() {
        size_t before = position;
        while (position < text.size() && domSpace(text[position]))
            ++position;
        return before != position;
    }
    bool identChar(unsigned char c) {
        return c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    std::string identifier(bool attr = false) {
        size_t begin = position;
        while (position < text.size() &&
               (identChar(text[position]) ||
                (attr && (text[position] == ':' || text[position] == '.'))))
            ++position;
        if (begin == position)
            error("expected a name");
        return text.substr(begin, position - begin);
    }
    SelectorCompound compound() {
        SelectorCompound out;
        bool found = false;
        if (position < text.size() && text[position] == '*') {
            out.tag = "*";
            ++position;
            found = true;
        } else if (position < text.size() && identChar(text[position])) {
            out.tag = lowerAscii(identifier());
            found = true;
        }
        while (position < text.size()) {
            char c = text[position];
            if (c == '.' || c == '#') {
                ++position;
                (c == '.' ? out.classes : out.ids).push_back(identifier());
                found = true;
            } else if (c == '[') {
                ++position;
                space();
                SelectorAttribute attr;
                attr.name = lowerAscii(identifier(true));
                space();
                if (position < text.size() && text[position] != ']') {
                    if (text[position] == '=')
                        attr.op = text[position++];
                    else if (position + 1 < text.size() &&
                             std::string("~^$*|").find(text[position]) != std::string::npos &&
                             text[position + 1] == '=') {
                        attr.op = text.substr(position, 2);
                        position += 2;
                    } else
                        error("unsupported attribute operator");
                    space();
                    if (position < text.size() &&
                        (text[position] == '\'' || text[position] == '"')) {
                        char quote = text[position++];
                        bool closed = false;
                        while (position < text.size()) {
                            char v = text[position++];
                            if (v == quote) {
                                closed = true;
                                break;
                            }
                            if (v == '\\') {
                                if (position == text.size())
                                    error("unfinished escape");
                                v = text[position++];
                            }
                            attr.value += v;
                        }
                        if (!closed)
                            error("unclosed attribute string");
                    } else
                        attr.value = identifier();
                    space();
                }
                if (position == text.size() || text[position++] != ']')
                    error("expected closing ]");
                out.attributes.push_back(std::move(attr));
                found = true;
            } else if (c == ':') {
                ++position;
                out.pseudos.push_back(pseudo());
                found = true;
            } else
                break;
        }
        if (!found)
            error("expected tag, class, ID, or attribute selector");
        return out;
    }

    // an+b, odd, even, or an integer
    void nth(SelectorPseudo &out) {
        space();
        size_t begin = position;
        while (position < text.size() && text[position] != ')')
            ++position;
        std::string expression;
        for (char c : text.substr(begin, position - begin))
            if (!domSpace(c))
                expression += c;
        expression = lowerAscii(expression);
        if (expression == "odd") {
            out.a = 2;
            out.b = 1;
            return;
        }
        if (expression == "even") {
            out.a = 2;
            out.b = 0;
            return;
        }
        auto n = expression.find('n');
        auto number = [&](const std::string &digits, int64_t fallback) -> int64_t {
            if (digits.empty() || digits == "+")
                return fallback;
            if (digits == "-")
                return -fallback;
            int64_t value = 0;
            auto start = digits.data() + (digits[0] == '+' ? 1 : 0);
            auto [end, ec] = std::from_chars(start, digits.data() + digits.size(), value);
            if (ec != std::errc{} || end != digits.data() + digits.size())
                error("invalid an+b expression");
            return value;
        };
        if (n == std::string::npos) {
            out.b = number(expression, 0);
            if (expression.empty())
                error("expected an+b expression");
            return;
        }
        out.a = number(expression.substr(0, n), 1);
        out.b = number(expression.substr(n + 1), 0);
    }
    SelectorPseudo pseudo() {
        SelectorPseudo out;
        out.name = lowerAscii(identifier());
        static const std::set<std::string> plain = {
            "first-child", "last-child", "only-child", "first-of-type", "last-of-type",
            "only-of-type", "empty",     "checked",    "disabled",      "enabled"};
        static const std::set<std::string> functional = {"nth-child", "nth-last-child",
                                                         "nth-of-type", "not"};
        if (plain.count(out.name))
            return out;
        if (!functional.count(out.name))
            error("unsupported pseudo-class ':" + out.name + "'");
        if (position == text.size() || text[position++] != '(')
            error("expected '(' after :" + out.name);
        if (out.name == "not") {
            // The argument is a selector list; nested parsing stops at the matching ')'.
            int depth = 1;
            size_t begin = position;
            while (position < text.size() && depth) {
                if (text[position] == '(')
                    ++depth;
                else if (text[position] == ')')
                    --depth;
                ++position;
            }
            if (depth)
                error("expected ')'");
            auto inner = text.substr(begin, position - begin - 1);
            out.negated = SelectorParser(inner).parse();
            return out;
        }
        nth(out);
        if (position == text.size() || text[position++] != ')')
            error("expected ')'");
        return out;
    }

  public:
    explicit SelectorParser(const std::string &text) : text(text) {}
    std::vector<SelectorGroup> parse() {
        std::vector<SelectorGroup> groups;
        space();
        if (position == text.size())
            error("selector cannot be empty");
        for (;;) {
            SelectorGroup group;
            char relation = ' ';
            for (;;) {
                group.push_back({compound(), relation});
                bool separated = space();
                if (position == text.size() || text[position] == ',')
                    break;
                if (text[position] == '>' || text[position] == '+' || text[position] == '~') {
                    relation = text[position++];
                    space();
                } else if (separated && text[position] != '\\')
                    relation = ' ';
                else
                    error("unsupported selector syntax");
            }
            groups.push_back(std::move(group));
            if (position == text.size())
                break;
            ++position;
            space();
            if (position == text.size())
                error("empty selector group");
        }
        return groups;
    }
};
std::vector<Object *> elementChildren(Object *parent);
bool nthMatch(int64_t a, int64_t b, int64_t position) {
    if (a == 0)
        return position == b;
    auto difference = position - b;
    return difference % a == 0 && difference / a >= 0;
}
bool groupMatch(Object *node, const std::vector<Object *> &chain, size_t depth, Object *root,
                const SelectorGroup &group, size_t step);
// `parent` is the node's parent element or fragment, used by structural pseudo-classes.
bool simpleMatch(Object *node, const SelectorCompound &selector, Object *parent) {
    if (!node || node->kind != Object::Element || node->text.empty())
        return false;
    if (!selector.tag.empty() && selector.tag != "*" && lowerAscii(node->text) != selector.tag)
        return false;
    auto id = attribute(node, "id");
    for (auto &required : selector.ids)
        if (!id || *id != required)
            return false;
    auto classes = attribute(node, "class");
    auto tokens = classes ? words(*classes) : std::vector<std::string>{};
    for (auto &required : selector.classes)
        if (std::find(tokens.begin(), tokens.end(), required) == tokens.end())
            return false;
    for (auto &attr : selector.attributes) {
        auto value = attribute(node, attr.name);
        if (!value)
            return false;
        if (attr.op.empty())
            continue;
        if (attr.op == "=" && *value != attr.value)
            return false;
        if (attr.op == "~=") {
            auto values = words(*value);
            if (attr.value.empty() ||
                std::find(values.begin(), values.end(), attr.value) == values.end())
                return false;
        }
        if (attr.op == "^=" && (attr.value.empty() || value->rfind(attr.value, 0) != 0))
            return false;
        if (attr.op == "$=" &&
            (attr.value.empty() || value->size() < attr.value.size() ||
             value->compare(value->size() - attr.value.size(), attr.value.size(), attr.value) != 0))
            return false;
        if (attr.op == "*=" && (attr.value.empty() || value->find(attr.value) == std::string::npos))
            return false;
        if (attr.op == "|=" && *value != attr.value && value->rfind(attr.value + "-", 0) != 0)
            return false;
    }
    for (auto &pseudo : selector.pseudos) {
        auto &name = pseudo.name;
        if (name == "empty") {
            for (auto &child : node->slots)
                if (child.object &&
                    (child.object->kind != Object::String || !child.object->text.empty()))
                    return false;
            continue;
        }
        if (name == "checked" || name == "disabled") {
            if (!attribute(node, name))
                return false;
            continue;
        }
        if (name == "enabled") {
            if (attribute(node, "disabled"))
                return false;
            continue;
        }
        if (name == "not") {
            for (auto &group : pseudo.negated)
                if (groupMatch(node, {}, 0, parent, group, group.size() - 1))
                    return false;
            continue;
        }
        auto siblings = elementChildren(parent);
        if (name.find("of-type") != std::string::npos)
            siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
                                          [&](Object *s) { return s->text != node->text; }),
                           siblings.end());
        auto at = std::find(siblings.begin(), siblings.end(), node);
        if (at == siblings.end())
            return false;
        auto index = static_cast<int64_t>(at - siblings.begin()) + 1;
        auto count = static_cast<int64_t>(siblings.size());
        bool ok = name == "first-child" || name == "first-of-type"  ? index == 1
                  : name == "last-child" || name == "last-of-type"  ? index == count
                  : name == "only-child" || name == "only-of-type"  ? count == 1
                  : name == "nth-child" || name == "nth-of-type"    ? nthMatch(pseudo.a, pseudo.b, index)
                                                                    : nthMatch(pseudo.a, pseudo.b,
                                                                               count - index + 1);
        if (!ok)
            return false;
    }
    return true;
}
// `chain` holds the named ancestors of `node` (outermost first) up to `depth`; the parent is
// chain[depth - 1], or `root` (possibly a fragment) at the top.
bool groupMatch(Object *node, const std::vector<Object *> &chain, size_t depth, Object *root,
                const SelectorGroup &group, size_t step) {
    Object *parent = depth ? chain[depth - 1] : root;
    if (!simpleMatch(node, group[step].compound, parent))
        return false;
    if (step == 0)
        return true;
    auto relation = group[step].relation;
    if (relation == '>')
        return depth > 0 && groupMatch(chain[depth - 1], chain, depth - 1, root, group, step - 1);
    if (relation == ' ') {
        for (size_t d = depth; d > 0; --d)
            if (groupMatch(chain[d - 1], chain, d - 1, root, group, step - 1))
                return true;
        return false;
    }
    auto siblings = elementChildren(parent);
    auto at = std::find(siblings.begin(), siblings.end(), node);
    for (auto it = siblings.begin(); it < at; ++it)
        if ((relation == '~' || it + 1 == at) &&
            groupMatch(*it, chain, depth, root, group, step - 1))
            return true;
    return false;
}
template <class Visitor> void walkDom(Object *root, Visitor &&visitor) {
    std::set<Object *> active;
    std::vector<Object *> ancestors;
    size_t visits = 0;
    std::function<void(Object *, size_t)> walk = [&](Object *node, size_t depth) {
        if (!node || node->kind != Object::Element)
            return;
        if (depth > 256 || ++visits > 1000000)
            fail("DOM traversal limit exceeded");
        if (!active.insert(node).second)
            fail("cyclic markup tree");
        if (node != root && !node->text.empty())
            visitor(node, ancestors);
        bool named = !node->text.empty();
        if (named)
            ancestors.push_back(node);
        for (auto &child : node->slots)
            walk(child.object, depth + 1);
        if (named)
            ancestors.pop_back();
        active.erase(node);
    };
    walk(element(root), 0);
}
void childrenOf(Object *node, std::vector<Slot> &output, size_t depth = 0) {
    if (depth > 256)
        fail("DOM child nesting exceeds 256");
    for (auto &child : node->slots)
        if (child.object && child.object->kind == Object::Element) {
            if (child.object->text.empty())
                childrenOf(child.object, output, depth + 1);
            else
                output.push_back(reference(child.object));
        }
}
std::vector<Object *> elementChildren(Object *parent) {
    std::vector<Slot> slots;
    if (parent && parent->kind == Object::Element)
        childrenOf(parent, slots);
    std::vector<Object *> out;
    for (auto &slot : slots)
        out.push_back(slot.object);
    return out;
}
void textOf(Object *node, std::string &output, size_t depth = 0) {
    if (depth > 256)
        fail("DOM text nesting exceeds 256");
    if (!node)
        return;
    if (node->kind == Object::String) {
        output += node->text;
        return;
    }
    if (node->kind != Object::Element)
        fail("invalid markup child");
    for (auto &child : node->slots)
        textOf(child.object, output, depth + 1);
}
void canHaveChildren(Object *node) {
    element(node);
    if (!node->text.empty() &&
        std::string(" area base br col embed hr img input link meta param source track wbr ")
                .find(" " + node->text + " ") != std::string::npos)
        fail("void HTML elements cannot have children");
}
bool containsNode(Object *node, Object *target, std::set<Object *> &visited, size_t depth = 0) {
    if (node == target)
        return true;
    if (!node || node->kind != Object::Element || !visited.insert(node).second)
        return false;
    if (depth > 256)
        fail("DOM nesting exceeds 256");
    for (auto &child : node->slots)
        if (containsNode(child.object, target, visited, depth + 1))
            return true;
    return false;
}
bool removeChild(Object *root, Object *child, size_t depth = 0) {
    if (depth > 256)
        fail("DOM nesting exceeds 256");
    auto &children = root->slots;
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (it->object == child) {
            if (child && child->parentElement == root)
                child->parentElement = nullptr;
            children.erase(it);
            return true;
        }
        if (it->object && it->object->kind == Object::Element && it->object->text.empty() &&
            removeChild(it->object, child, depth + 1))
            return true;
    }
    return false;
}
// Children point back to their most recent parent. The pointer does not own the parent; a
// parent clears it when destroyed (~Object) and remove_child clears it, so it never dangles.
std::mutex parentMutex;
void linkParent(Object *parent, Object *child) {
    if (!child || child->kind != Object::Element)
        return;
    SharedLock lock(parentMutex);
    child->parentElement = parent;
}
Object *parentOf(Object *node) {
    SharedLock lock(parentMutex);
    auto parent = node ? node->parentElement : nullptr;
    if (!parent)
        return nullptr;
    // A stale link (the child was removed or moved elsewhere) is ignored.
    for (auto &slot : parent->slots)
        if (slot.object == node)
            return parent;
    return nullptr;
}
// Named ancestors of `node`, outermost first, following parent links; fragments are skipped
// except that the outermost fragment is kept as the root context.
std::vector<Object *> ancestorChain(Object *node) {
    std::vector<Object *> chain;
    std::set<Object *> seen{node};
    for (auto p = parentOf(node); p && seen.insert(p).second; p = parentOf(p))
        if (!p->text.empty() || !parentOf(p))
            chain.insert(chain.begin(), p);
    return chain;
}
Object *cloneNode(Frame *frame, Object *node, bool deep, size_t depth = 0) {
    if (depth > 256)
        fail("DOM clone nesting exceeds 256");
    element(node);
    auto out = managed(frame, Object::Element);
    out->text = node->text;
    out->attributes = node->attributes;
    if (node->extra)
        out->more().events = node->extra->events;
    if (deep)
        for (auto &child : node->slots) {
            if (child.object && child.object->kind == Object::Element)
                out->slots.push_back(reference(cloneNode(frame, child.object, true, depth + 1)));
            else
                out->slots.push_back(child);
        }
    return out;
}
std::string classToken(const std::string &text) {
    if (text.empty() || std::any_of(text.begin(), text.end(), domSpace))
        fail("class token must be non-empty and contain no whitespace");
    return text;
}
} // namespace
extern "C" {
Object *fw_dom_query(Frame *frame, Object *root, Object *selector, int64_t all) {
    return nativeBoundary([&]() -> Object * {
        auto groups = SelectorParser(selector->text).parse();
        std::vector<Object *> matches;
        std::set<Object *> seen;
        walkDom(root, [&](Object *node, const std::vector<Object *> &ancestors) {
            for (auto &group : groups)
                if (groupMatch(node, ancestors, ancestors.size(), root, group, group.size() - 1)) {
                    if (seen.insert(node).second)
                        matches.push_back(node);
                    break;
                }
        });
        if (all) {
            auto out = managed(frame, Object::Array);
            for (auto node : matches)
                out->slots.push_back(reference(node));
            return out;
        }
        auto result = matches.empty() ? nullptr : matches[0];
        retain(result);
        return own(frame, result);
    });
}
Object *fw_dom_get_by_id(Frame *frame, Object *root, Object *id) {
    return nativeBoundary([&]() -> Object * {
        Object *found = nullptr;
        walkDom(root, [&](Object *node, const std::vector<Object *> &) {
            if (!found)
                if (auto value = attribute(node, "id"); value && *value == id->text)
                    found = node;
        });
        retain(found);
        return own(frame, found);
    });
}
int64_t fw_dom_matches(Object *root, Object *selector) {
    return nativeBoundary([&]() -> int64_t {
        element(root);
        auto groups = SelectorParser(selector->text).parse();
        auto chain = ancestorChain(root);
        for (auto &group : groups)
            if (groupMatch(root, chain, chain.size(), chain.empty() ? nullptr : chain.front(), group,
                           group.size() - 1))
                return 1;
        return 0;
    });
}
Object *fw_dom_children(Frame *frame, Object *root) {
    return nativeBoundary([&] {
        auto out = managed(frame, Object::Array);
        childrenOf(element(root), out->slots);
        return out;
    });
}
Object *fw_dom_text(Frame *frame, Object *root, int64_t mode) {
    return nativeBoundary([&] {
        element(root);
        std::string out;
        if (mode == 0)
            textOf(root, out);
        else if (mode == 1)
            for (auto &child : root->slots)
                render(child.object, out);
        else if (mode == 2)
            render(root, out);
        else {
            attributeElement(root);
            auto value = attribute(root, mode == 3 ? "id" : "class");
            if (value)
                out = *value;
        }
        return string(frame, std::move(out));
    });
}
Object *fw_dom_get_attribute(Frame *frame, Object *root, Object *name) {
    return nativeBoundary([&]() -> Object * {
        auto value = attribute(attributeElement(root), attributeName(name->text));
        if (!value)
            return own(frame, nullptr);
        return string(frame, *value);
    });
}
int64_t fw_dom_has_attribute(Object *root, Object *name) {
    return nativeBoundary([&]() -> int64_t {
        return attribute(attributeElement(root), attributeName(name->text)) != nullptr;
    });
}
void fw_dom_set_attribute(Object *root, Object *name, Object *value) {
    nativeBoundary(
        [&] { setAttribute(attributeElement(root), attributeName(name->text), value->text); });
}
int64_t fw_dom_remove_attribute(Object *root, Object *name) {
    return nativeBoundary([&]() -> int64_t {
        auto key = attributeName(name->text);
        auto &attributes = attributeElement(root)->attributes;
        auto found = std::find_if(attributes.begin(), attributes.end(),
                                  [&](auto &pair) { return pair.first == key; });
        if (found == attributes.end())
            return 0;
        attributes.erase(found);
        return 1;
    });
}
void fw_dom_set_text(Object *root, Object *text) {
    nativeBoundary([&] {
        canHaveChildren(root);
        std::vector<Slot> replacement;
        replacement.push_back(reference(text));
        root->slots.swap(replacement);
    });
}
void fw_dom_append(Object *root, Object *child, int64_t prepend) {
    nativeBoundary([&] {
        canHaveChildren(root);
        if (!child || (child->kind != Object::Element && child->kind != Object::String))
            fail("append requires markup or text");
        std::set<Object *> visited;
        if (containsNode(child, root, visited))
            fail("markup insertion would create a cycle");
        auto value = reference(child);
        if (prepend)
            root->slots.insert(root->slots.begin(), std::move(value));
        else
            root->slots.push_back(std::move(value));
        linkParent(root, child);
    });
}
int64_t fw_dom_remove_child(Object *root, Object *child) {
    return nativeBoundary([&]() -> int64_t { return removeChild(element(root), child); });
}
Object *fw_dom_attributes(Frame *frame, Object *root) {
    return nativeBoundary([&] {
        attributeElement(root);
        auto out = managed(frame, Object::Map);
        out->objectKeys = true;
        out->keyKind = Slot::Reference;
        for (auto &[key, value] : root->attributes) {
            appendKey(out, reference(string(frame, key)), reference(string(frame, value)));
        }
        return out;
    });
}
void fw_dom_clear(Object *root) {
    nativeBoundary([&] { element(root)->slots.clear(); });
}
// 0 parent, 1 next element sibling, 2 previous element sibling (null when there is none).
Object *fw_dom_relative(Frame *frame, Object *node, int64_t which) {
    return nativeBoundary([&]() -> Object * {
        element(node);
        auto parent = parentOf(node);
        Object *found = nullptr;
        if (which == 0)
            found = parent;
        else if (parent) {
            // Siblings are element children of the nearest named ancestor (fragments flatten).
            auto context = parent;
            while (context->text.empty() && parentOf(context))
                context = parentOf(context);
            auto siblings = elementChildren(context);
            auto at = std::find(siblings.begin(), siblings.end(), node);
            if (at != siblings.end()) {
                if (which == 1 && at + 1 != siblings.end())
                    found = *(at + 1);
                if (which == 2 && at != siblings.begin())
                    found = *(at - 1);
            }
        }
        retain(found);
        return own(frame, found);
    });
}
// The node itself or its nearest ancestor matching the selector.
Object *fw_dom_closest(Frame *frame, Object *node, Object *selector) {
    return nativeBoundary([&]() -> Object * {
        element(node);
        auto groups = SelectorParser(selector->text).parse();
        std::set<Object *> seen;
        for (auto current = node; current && seen.insert(current).second;
             current = parentOf(current)) {
            if (current->text.empty())
                continue;
            auto chain = ancestorChain(current);
            for (auto &group : groups)
                if (groupMatch(current, chain, chain.size(), chain.empty() ? nullptr : chain.front(),
                               group, group.size() - 1)) {
                    retain(current);
                    return own(frame, current);
                }
        }
        return nullptr;
    });
}
// Attaches a function-valued event attribute (onclick={...}); arity 1 receives the target value.
void fw_element_event(Object *root, Object *name, Object *closure, int64_t arity) {
    nativeBoundary([&] {
        attributeElement(root);
        for (auto &event : root->more().events)
            if (event.name == name->text) {
                event.handler = reference(closure);
                event.arity = static_cast<int>(arity);
                return;
            }
        root->more().events.push_back({name->text, static_cast<int>(arity), reference(closure)});
    });
}
Object *fw_dom_clone(Frame *frame, Object *root, int64_t deep) {
    return nativeBoundary([&] { return cloneNode(frame, root, deep != 0); });
}
int64_t fw_dom_class(Object *root, Object *token, int64_t operation) {
    return nativeBoundary([&]() -> int64_t {
        attributeElement(root);
        auto name = classToken(token->text);
        auto value = attribute(root, "class");
        auto classes = value ? words(*value) : std::vector<std::string>{};
        auto found = std::find(classes.begin(), classes.end(), name);
        bool present = found != classes.end();
        if (operation == 0)
            return present;
        if (operation == 2 || (operation == 3 && present)) {
            if (present)
                classes.erase(std::remove(classes.begin(), classes.end(), name), classes.end());
        } else if (!present)
            classes.push_back(name);
        std::string out;
        for (auto &item : classes) {
            if (!out.empty())
                out += ' ';
            out += item;
        }
        setAttribute(root, "class", out);
        if (operation == 2)
            return present;
        return operation == 3 ? !present : !present;
    });
}
}
