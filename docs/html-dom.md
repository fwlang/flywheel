# HTML tree queries and mutation

The HTML API operates on a supplied native markup tree. There is no implicit global document and no browser DOM or runtime HTML parser.

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

array<element> paragraphs = page.query_all(".card p")
for paragraph in paragraphs do
    paragraph.add_class("selected")
end
print page
```

`html.query(page, selector)`, `html.query_all(page, selector)`, and `html.get_by_id(page, id)` are equivalent to the corresponding tree methods. `import html` (including an alias) is sufficient for qualified library calls; markup tag literals still need `import html.*` in their defining module.

## Query behavior

| Method | Result |
|---|---|
| `tree.query(selector)` | `element?`: first matching descendant, or null |
| `tree.query_all(selector)` | `array<element>`: matching descendants in preorder |
| `tree.get_by_id(id)` | `element?`: first descendant with that exact ID, without CSS parsing |
| `node.matches(selector)` | bool for compound selectors and comma-separated alternatives |

Queries search descendants, **excluding the supplied root**, as with an element's `querySelector`. Use `matches` to test the root itself. Fragments are transparent containers and are not selector candidates; named children inside them are searchable. Results retain existing nodes, so mutations through a result affect its original tree. Query arrays are snapshots and keep nodes alive after they are removed from a parent. A shared node is returned once per query, by identity.

Nullable query results can be compared with null. Dereferencing a null result raises `CastError`; `element(result)` explicitly checks/unpacks a nullable element when a non-null function argument is needed.

## Supported selectors

- Tags (`h1`), universal `*`, classes (`.card`), IDs (`#main-title`).
- Compound forms such as `section.card.active#main`.
- Descendants (`.card p`) and direct named children (`.card > p`).
- Comma-separated groups (`h1, .card p`), with duplicate identities removed.
- Attribute presence and comparisons: `[data-state]`, `[name="value"]`, `~=`, `^=`, `$=`, `*=`, and `|=`.

Tags and attribute names compare case-insensitively in ASCII; IDs, class tokens, and attribute values compare exactly. Quoted attribute values support spaces. Pseudo-classes, pseudo-elements, sibling combinators, full CSS identifier escapes, and selector flags are not implemented. Malformed/unsupported selectors raise `SelectorError` rather than pretending nothing matched. `matches` does not accept ancestor/child combinators because elements do not store parent links; use a tree-scoped query for those selectors.

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
| `.clone()` / `.clone(false)` | Deep clone by default, or copy only the root and attributes |
| `.has_class(token)` | bool |
| `.add_class(token)` | bool indicating whether the token was added |
| `.remove_class(token)` | bool indicating whether the token existed |
| `.toggle_class(token)` | bool indicating whether the token is now present |

Attribute/class helpers require a named element; fragments have no attributes. Attribute names currently accept ASCII letters/digits and `_`, `-`, `:`, `.` and are normalized to lowercase. A class token must be nonempty and contain no whitespace. Void HTML elements reject child insertion/text. Appending a node that contains the destination raises `RuntimeError` before mutation, preventing ARC cycles.

Elements retain shared identity. Appending does not automatically remove a node from another parent, and fragments retain their value identity rather than being consumed. Use `clone()` for independent nodes. Parent/closest APIs, live NodeLists, browser layout/events, arbitrary raw inner-HTML assignment, and automatic reparenting are not implemented. `set_text` and compiled markup insertion preserve escaping without introducing a runtime XML/HTML parser.

Selector, child/text traversal, and cloning limit depth to 256; queries also limit traversal to one million visited element occurrences. Serialization uses the existing renderer. Native sockets/web rendering and these tree operations remain on the existing single-threaded language execution path.
