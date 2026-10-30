# HTML tree queries and mutation

The HTML API operates on a supplied native markup tree: one built from markup literals, or one read from HTML text with `html.parse`. There is no implicit global document or browser DOM.

```flywheel
import html.*

var page = <main>
    <h1 id="main-title">Welcome</h1>
    <section class="card"><p>First</p><p>Second</p></section>
</main>

element? title = page.query("#main-title")
if title != null then
    title.text_content = "Hello & welcome!"
end

array<element> paragraphs = page.query_all(".card p:last-child")
for paragraph in paragraphs do
    paragraph.add_class("selected")
end
print page
```

`html.query(page, selector)`, `html.query_all(page, selector)`, and `html.get_by_id(page, id)` are equivalent to the corresponding tree methods. `import html` (including an alias) is sufficient for qualified library calls; markup tag literals still need `import html.*` in their defining module.

## Parsing HTML

`html.parse(text)` reads HTML (from a file, a request, or `web.get`) into a `fragment` of ordinary elements and text, which the query and mutation tools here work on and which renders back to HTML:

```flywheel
import html
var doc = html.parse("<ul><li>one<li class=x>two</ul><p>Fish &amp; chips")
print html.query_all(doc, "li").length                 # 2
var item = html.query(doc, "li.x")
if item != null then
    print item.text_content                            # two
end
print doc   # <ul><li>one</li><li class="x">two</li></ul><p>Fish &amp; chips</p>
```

The parser is lenient, as browsers are: tag and attribute names are lower-cased; attributes may be unquoted or valueless; void elements (`br`, `img`, ...) and `/>` take no children; `<p>`, `<li>`, `<dt>`/`<dd>`, `<tr>`, `<td>`/`<th>`, and `<option>` close implicitly; unmatched end tags are ignored and unclosed elements end at the end of input. `script`, `style`, `textarea`, and `title` contain raw text. Named entities (common ones such as `&nbsp;`, `&copy;`, `&mdash;`) and numeric entities are decoded; comments, the doctype, and processing instructions are dropped, as is whitespace-only text directly inside `html`, `head`, tables, and lists. It does not build an implicit `<html>`/`<head>`/`<body>` structure or perform the HTML5 adoption-agency repairs for misnested formatting tags.

## Query behavior

| Method | Result |
|---|---|
| `tree.query(selector)` | `element?`: first matching descendant, or null |
| `tree.query_all(selector)` | `array<element>`: matching descendants in preorder |
| `tree.get_by_id(id)` | `element?`: first descendant with that exact ID, without CSS parsing |
| `node.matches(selector)` | bool; combinators and structural pseudo-classes use the node's parent links |
| `node.closest(selector)` | `element?`: the node itself or its nearest matching ancestor |

Queries search descendants, **excluding the supplied root**, as with an element's `querySelector`. Use `matches` to test the root itself. Fragments are transparent containers and are not selector candidates; named children inside them are searchable, and siblings are counted across fragment boundaries. Results retain existing nodes, so mutations through a result affect its original tree. Query arrays are snapshots. A shared node is returned once per query, by identity.

Nullable query results can be compared with null, and null checks narrow them. Dereferencing a null result raises `CastError`.

## Supported selectors

- Tags (`h1`), universal `*`, classes (`.card`), IDs (`#main-title`), and compound forms such as `section.card.active#main`.
- Combinators: descendant (`.card p`), child (`.card > p`), next sibling (`h1 + p`), and later sibling (`h1 ~ p`).
- Comma-separated groups (`h1, .card p`), with duplicate identities removed.
- Attribute presence and comparisons: `[data-state]`, `[name="value"]`, `~=`, `^=`, `$=`, `*=`, and `|=`.
- Pseudo-classes: `:first-child`, `:last-child`, `:only-child`, `:nth-child(an+b)`, `:nth-last-child(an+b)`, `:first-of-type`, `:last-of-type`, `:only-of-type`, `:nth-of-type(an+b)`, `:empty`, `:not(selector list)`, `:checked`, `:disabled`, and `:enabled`. `an+b` accepts forms such as `3`, `2n+1`, `-n+3`, `odd`, and `even`.

Tags and attribute names compare case-insensitively in ASCII; IDs, class tokens, and attribute values compare exactly. Quoted attribute values support spaces. Pseudo-elements, dynamic pseudo-classes such as `:hover`, full CSS identifier escapes, and selector flags are not implemented. Malformed or unsupported selectors raise `SelectorError` rather than pretending nothing matched.

## Parent and sibling links

Every child keeps a weak link to the parent it was most recently added to (by markup construction, `append`, or `prepend`). Links never keep a tree alive and stop resolving once the child is removed from that parent.

| Property | Result |
|---|---|
| `.parent` | `element?`: the parent element or fragment |
| `.next_sibling`, `.previous_sibling` | `element?`: neighbouring named elements under the nearest named ancestor |

Because one node may be shared by several trees, these follow the most recent insertion; use `clone()` for independent copies.

## Other tools

These operations also have qualified `html.operation(tree, ...)` forms.

| Property or method | Behavior |
|---|---|
| `.tag` | Tag string; empty for fragments |
| `.attributes` / `.attributes()` | `map<string,string>` snapshot; edit via attribute methods to change the tree |
| `.children` / `.children()` | `array<element>` snapshot of direct named children; transparent fragments are flattened, text is excluded |
| `.text_content` / `.get_text()` | Concatenated decoded text, without HTML markup |
| `.text_content = text` / `.set_text(text)` | Replace children with escaped text |
| `.inner_html` | Serialize children; read-only |
| `.outer_html` / `.render()` | Serialize the complete value |
| `.id`, `.class_name` | String getters/setters; missing attributes read as empty string |
| `.get_attribute(name)` | `string?`; null when absent |
| `.has_attribute(name)` | bool |
| `.set_attribute(name, value)` | Set a string value; names are validated, values are escaped on rendering |
| `.remove_attribute(name)` | bool indicating removal |
| `.append(value)`, `.prepend(value)` | Retain markup children or insert formatted, escaped text |
| `.remove_child(node)` | bool; remove a direct logical child, including children inside transparent fragments |
| `.clear()` | Remove all children |
| `.clone()` / `.clone(false)` | Deep clone by default, or copy only the root, attributes, and event handlers |
| `.has_class(token)`, `.add_class(token)`, `.remove_class(token)`, `.toggle_class(token)` | Class token helpers returning bool |

Attribute/class helpers require a named element; fragments have no attributes. A class token must be nonempty and contain no whitespace. Void elements reject child insertion/text. Appending a node that contains the destination raises `RuntimeError` before mutation, preventing cycles. Live NodeLists, browser layout, arbitrary raw inner-HTML assignment, and automatic reparenting are not implemented.

Selector, child/text traversal, and cloning limit depth to 256; queries also limit traversal to one million visited element occurrences.

## Tags, attributes, and events

`import html.*` provides the HTML5 element set (from `a` to `wbr`) with their global and element-specific attributes; `data-*` and `aria-*` attributes are accepted on every tag. Boolean attributes such as `disabled`, `checked`, and `open` take a `bool` and are rendered only when true. Other attributes take strings or numbers: `<input min={0} step={0.5} />` renders `min="0" step="0.5"`. Void elements (`br`, `img`, `input`, `meta`, ...) cannot have children.

Event attributes (`onclick`, `oninput`, ...) accept either inline JavaScript text, `onclick="history.back()"`, or a Flywheel function: `function() returns void`, or `function(string value) returns void`, which receives the event target's `value`. Function handlers run in [desktop windows](desktop.md); HTML rendered with `render`, `print`, or `web` responses omits them, since a browser page cannot call back into the program.

`import svg.*` provides SVG elements (`svg`, `g`, `path`, `circle`, `rect`, `line`, `polygon`, `text`, `defs`, `linearGradient`, `use`, ...) with their presentation attributes, and can be combined with `html.*`.

## Element families

A module can declare its own tag set, for example for custom elements or another XML vocabulary:

```flywheel
elements widgets
    my-card: title, size
    my-icon void: name
    my-slot: *
end

var card = <my-card title="Hi" size={2}><my-icon name="star" /></my-card>
```

Each line names a tag, optionally `void` (no children), and after `:` its allowed attributes, or `*` for any attribute. Tags are visible in the declaring module and in modules that import it. Unused components are still checked against these rules.
