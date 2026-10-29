"""Black-box tests: build and execute native artifacts, with independent expected results."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

COMPILER = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else "flywheel"

POSITIVE = {
    "hello": ('print "Hello, world!"', "Hello, world!\n"),
    "arithmetic": ("int a = 3\nvar b = 4\nprint a + b", "7\n"),
    "calls": (
        """function add(int a, int b) returns int
return a + b
end
var x = add 2, 3
print add(x, 4)""",
        "9\n",
    ),
    "flow": (
        """int x = 0
while (x < 3) do
x += 1
end
if x == 3 then
print "ok"
else if x == 2 then
print "wrong"
end""",
        "ok\n",
    ),
    "classes": (
        """class Person
string name
int age = 0
function birthday() returns void
inst age += 1
end
function greet() returns string
return "Hi {inst name}!"
end
end
var p = Person "danny", 15
p.birthday()
print p.age
print p.greet()""",
        "16\nHi danny!\n",
    ),
    "markup": (
        """import html.*
component Title = <h1 class="title">{attr inner}</h1>
var name = "Flywheel"
var tree = <>
    <Title>Hello, {name}!</Title>
    <p>Fast &amp; readable</p>
</>
print html.render(tree)""",
        '<h1 class="title">Hello, Flywheel!</h1><p>Fast &amp; readable</p>\n',
    ),
    "escaping": (
        """import html.*
var text = "<script>&\\\"'"
var tree = <p title={text}>{text}</p>
print html.render(tree)""",
        '<p title="&lt;script&gt;&amp;&quot;&#39;">&lt;script&gt;&amp;&quot;&#39;</p>\n',
    ),
    "components": (
        """import html.*
class User
string name
end
component Card = <div style={attr style}><p>{attr user.name}</p>{attr inner}</div>
var u = User("Danny")
print html.render(<Card user={u}>ok</Card>)""",
        '<div style=""><p>Danny</p>ok</div>\n',
    ),
    "arrays": (
        """int[] values = [1, 2, 3]
values[1] = 5
int sum = 0
for value in values do
sum += value
end
print sum
print 5 in values
print len(values)
string[] empty = []
print len(empty)""",
        "9\ntrue\n3\n0\n",
    ),
    "loop_scopes": (
        """var item = 99
for item in [1, 2, 3, 4] do
if item == 2 then
continue
end
if item == 4 then
break
end
print item
end
print item""",
        "1\n3\n99\n",
    ),
    "recursion": (
        """function fib(int n) returns int
if n < 2 then
return n
else
return fib(n - 1) + fib(n - 2)
end
end
print fib(10)""",
        "55\n",
    ),
    "inferred_defaults": (
        """function twice(int n, int increment = 2)
return n * increment
end
function main() returns void
print twice(5)
end
main""",
        "10\n",
    ),
    "function_reference": (
        """function answer() returns int
return 42
end
var f = answer
print f()""",
        "42\n",
    ),
    "short_circuit": (
        """function side() returns bool
print "BAD"
return true
end
print false and side()
print true or side()
print not false and true""",
        "false\ntrue\ntrue\n",
    ),
    "numeric_rules": (
        """i16 small = 2
float f = small
double d = f
print d
print 7 / 2
print -7 // 2
print -7 % 2
print double(9223372036854775807)
print u8(255)
print i8(-128)""",
        "2\n3.5\n-3\n-1\n9.22337203685478e+18\n255\n-128\n",
    ),
    "strings": (
        'print "{{x}} \\u{1F600} {1 + 2} #"\nprint "a\\nb"',
        "{x} 😀 3 #\na\nb\n",
    ),
    "nullable": (
        'string? s = null\nprint s == null\ns = "ok"\nprint s != null',
        "true\ntrue\n",
    ),
    "ownership": (
        """class User
string name
end
function make() returns User
return User("retained")
end
function identity(User p) returns User
return p
end
var user = identity(make())
print user.name
string[] xs = ["a", "b"]
xs[0] = "c"
print xs[0]""",
        "retained\nc\n",
    ),
    "libraries": (
        """import math
import time
import testing
import fs
fs.write("test.txt", "abc")
print fs.exists("test.txt")
print fs.read("test.txt")
fs.remove("test.txt")
print fs.exists("test.txt")
print math.sqrt(9.0)
testing.assert(time.monotonic() > 0.0, "clock")""",
        "true\nabc\nfalse\n3\n",
    ),
    "semicolon": ("int a = 1; a += 2; print a", "3\n"),
    "boolean_attributes": (
        """import html.*
print html.render(<button disabled={false}>a</button>)
print html.render(<input disabled={true} />)""",
        '<button>a</button>\n<input disabled="">\n',
    ),
}
POSITIVE.update(
    {
        "json": (
            'import json\nvar value = json.parse("{{\\"x\\": 42, \\"yes\\": true, \\"label\\": \\"ok\\"}}")\nprint json.as_int(json.get(value, "x"))\nprint json.as_bool(json.get(value, "yes"))\nprint json.as_string(json.get(value, "label"))\nprint json.has(value, "missing")\nprint json.kind(value)\nprint json.stringify(value)',
            '42\ntrue\nok\nfalse\nobject\n{"label":"ok","x":42,"yes":true}\n',
        ),
        "class_json": (
            'class Person\nstring name\nint age = 15\nend\nvar person = Person.from_json("{{\\"name\\": \\"Danny\\"}}")\nprint person.name\nprint person.age\nvar other = Person.from_json("{{\\"name\\": \\"D\\", \\"age\\": 9223372036854775807}}")\nprint other.age',
            "Danny\n15\n9223372036854775807\n",
        ),
        "attribute_entities": (
            'import html.*\nprint html.render(<p title="&quot;&amp;">&#x1F600; # {{ok}}</p>)',
            '<p title="&quot;&amp;">😀 # {ok}</p>\n',
        ),
    }
)

POSITIVE.update(
    {
        "operator_strings": (
            'print "/"\nprint "not"\nprint ")"\nprint "end"\nprint "="',
            "/\nnot\n)\nend\n=\n",
        ),
        "default_scope": (
            "int seed = 3\nclass C\nint n = seed\nend\nfunction make() returns C\nint seed = 9\nreturn C()\nend\nprint make().n",
            "3\n",
        ),
        "iterable_once": (
            'function values() returns int[]\nprint "once"\nreturn [1, 2]\nend\nfor item in values() do\nprint item\nend',
            "once\n1\n2\n",
        ),
        "array_alias": ("int[] a = [1]\nvar b = a\nb[0] = 4\nprint a[0]", "4\n"),
    }
)

POSITIVE.update(
    {
        "reentrant_string": (
            'string name = "before"\nfunction replace() returns string\nname = "after"\nreturn "!"\nend\nprint name + replace()\nprint name',
            "before!\nafter\n",
        ),
        "reentrant_array": (
            "int[] a = [1]\nvar old = a\nfunction replace() returns int\na = [2]\nreturn 3\nend\na[0] = replace()\nprint a[0]\nprint old[0]",
            "2\n3\n",
        ),
        "reentrant_field": (
            'class User\nstring name\nend\nvar user = User("before")\nfunction replace() returns string\nuser.name = "after"\nreturn "!"\nend\nprint user.name + replace()\nprint user.name',
            "before!\nafter\n",
        ),
    }
)

POSITIVE.update(
    {
        "not_precedence": (
            "print not 1 == 2\nprint not 1 < 2\nprint not false and true",
            "true\nfalse\ntrue\n",
        ),
        "float_division": (
            "float a = float(7.0)\nfloat b = float(2.0)\nprint typeof(a / b)\nprint a / b",
            "float\n3.5\n",
        ),
    }
)

POSITIVE.update(
    {
        "numeric_strings": (
            'print int("42")\nprint u64("18446744073709551615")\nprint double("3.5")',
            "42\n18446744073709551615\n3.5\n",
        )
    }
)

POSITIVE.update(
    {
        "literal_postfix": (
            'import html.*\nprint "abc".length\nprint(<p />.tag)\nprint([3, 4][0])',
            "3\np\n3\n",
        ),
        "json_default_scope": (
            'int seed = 3\nclass C\nint n = seed\nend\nfunction make() returns C\nint seed = 9\nreturn C.from_json("{{}}")\nend\nprint make().n',
            "3\n",
        ),
    }
)

POSITIVE.update(
    {
        "print_markup_argument": (
            "import html.*\nprint <h1>Hello, World!</h1>",
            "<h1>Hello, World!</h1>\n",
        ),
        "print_markup_parenthesized": (
            "import html.*\nprint (<h1>Hello, World!</h1>)\nprint(<h1>Hello, World!</h1>)",
            "<h1>Hello, World!</h1>\n<h1>Hello, World!</h1>\n",
        ),
        "print_nested_markup": (
            "import html.*\nprint <div><h1>Hello</h1><p>World!</p></div>\nprint(<div><span>Nested</span></div>)",
            "<div><h1>Hello</h1><p>World!</p></div>\n<div><span>Nested</span></div>\n",
        ),
        "print_fragments": (
            "import html.*\nprint <><h1>Hello</h1><p>World!</p></>\nprint(<><b>one</b><i>two</i></>)\nprint <>plain &amp; text</>\nprint <> </>\nprint <></>",
            "<h1>Hello</h1><p>World!</p>\n<b>one</b><i>two</i>\nplain &amp; text\n \n\n",
        ),
        "markup_argument_calls": (
            "import html.*\nfunction echo(element value) returns element\nreturn value\nend\nfunction join(element left, fragment right) returns fragment\nreturn <>{left}{right}</>\nend\nvar emit = echo\nprint emit(<h1>alias</h1>)\nemit <h1>discarded</h1>\nvar tree = echo <p>declaration</p>\nprint tree\ntree = echo <p>assignment</p>\nprint tree\nvar nodes = join <b>left</b>, <><i>right</i></>\nprint nodes\nprint(join(<b>explicit</b>, <><i>calls</i></>))",
            "<h1>alias</h1>\n<p>declaration</p>\n<p>assignment</p>\n<b>left</b><i>right</i>\n<b>explicit</b><i>calls</i>\n",
        ),
        "markup_member_argument": (
            "import html.*\nclass Output\nfunction show(element value) returns void\nprint value\nend\nend\nvar out = Output()\nout.show <p>member</p>\nout.show(<p>explicit member</p>)",
            "<p>member</p>\n<p>explicit member</p>\n",
        ),
        "markup_formatting": (
            'import html.*\nvar tree = <h1>Hello &amp; World!</h1>\nvar fragment = <><b>A</b><i>B</i></>\nprint string(tree)\nprint string(fragment)\nprint "Tree: {tree}"\nprint "Fragment: {fragment}"\nprint html.render(tree)\nprint html.render(fragment)\nprint tree',
            "<h1>Hello &amp; World!</h1>\n<b>A</b><i>B</i>\nTree: <h1>Hello &amp; World!</h1>\nFragment: <b>A</b><i>B</i>\n<h1>Hello &amp; World!</h1>\n<b>A</b><i>B</i>\n<h1>Hello &amp; World!</h1>\n",
        ),
        "print_markup_escaping": (
            'import html.*\nvar text = "<>&\\"\'"\nprint <p title={text}>{text} &amp; &#x1F600;</p>\nprint(<><span title="&quot;&amp;">&lt;safe&gt;</span>{text}</>)\nprint <p>{string(<b>A &amp; B</b>)}</p>',
            '<p title="&lt;&gt;&amp;&quot;&#39;">&lt;&gt;&amp;&quot;&#39; &amp; 😀</p>\n<span title="&quot;&amp;">&lt;safe&gt;</span>&lt;&gt;&amp;&quot;&#39;\n<p>&lt;b&gt;A &amp;amp; B&lt;/b&gt;</p>\n',
        ),
        "markup_comparison_boundary": (
            "import html.*\nint a = 1\nint b = 2\nprint a<b\nprint a <b and b>a\nif a<b then\nprint <p>comparison</p>\nend\nprint <p>{a <b}</p>",
            "true\ntrue\n<p>comparison</p>\n<p>true</p>\n",
        ),
        "markup_temporary_arc": (
            'import html.*\nfunction make(int n) returns element\nreturn <p>{n}</p>\nend\nvar retained = make(7)\nint i = 0\nint total = 0\nwhile i < 2000 do\nvar text = string(make(i))\ntotal += len(text)\ni += 1\nend\nprint total\nprint retained\nprint retained\nprint "{make(9)}"',
            "20890\n<p>7</p>\n<p>7</p>\n<p>9</p>\n",
        ),
    }
)

POSITIVE.update(
    {
        "markup_self_closing_component": (
            "import html.*\ncomponent Label = <p>{attr inner}</p>\nprint <br />\nprint <input disabled={true} />\nprint <Label>component</Label>",
            '<br>\n<input disabled="">\n<p>component</p>\n',
        )
    }
)

POSITIVE.update(
    {
        "fragment_element_render": (
            "import html.*\nelement page = <><h1>Hello</h1><p>world</p></>\nprint page.render()\nfragment nodes = <><b>A</b><i>B</i></>\nprint nodes.render()",
            "<h1>Hello</h1><p>world</p>\n<b>A</b><i>B</i>\n",
        ),
        "dynamic_arrays": (
            'int[] a = []\na.push(1)\na.push(2)\nvar alias = a\nprint a.pop()\nprint alias.length\na.clear()\nprint len(alias)\nstring[] words = []\nwords.push("retained")\nprint words.pop()',
            "2\n1\n0\nretained\n",
        ),
        "maps": (
            'map<string,int> scores = {"a": 1}\nscores["b"] = 2\nscores["a"] += 3\nprint scores["a"]\nprint "b" in scores\nfor key in scores do\nprint "{key}: {scores[key]}"\nend\nprint scores.remove("a")\nprint scores.remove("missing")\nprint len(scores.keys())\nscores.clear()\nprint len(scores)',
            "4\ntrue\na: 4\nb: 2\ntrue\nfalse\n1\n0\n",
        ),
        "map_values_arc": (
            'map<int,string[]> values = {}\nvalues[1] = ["hello"]\nvalues[1].push("world")\nvar saved = values[1]\nvalues.remove(1)\nprint saved.pop()\nprint saved.pop()',
            "world\nhello\n",
        ),
        "tuples": (
            '[int,string] pair = [42,"answer"]\nprint pair[0]\nprint pair[1]\npair[1] = "changed"\nprint pair[1]\nvar inferred = [true, "yes", [1,2]]\nprint inferred[0]\nprint inferred[1]\nprint inferred[2][1]\nprint len(pair)',
            "42\nanswer\nchanged\ntrue\nyes\n2\n2\n",
        ),
        "typed_errors": (
            'import fs\ntry\nvar text = fs.read("missing-file.txt")\nprint text\ncatch FileError error\nprint error.message\nfinally\nprint "done"\nend',
            "cannot read 'missing-file.txt'\ndone\n",
        ),
        "error_propagation": (
            'function bad() returns int\nint[] a = []\nreturn a[0]\nend\ntry\nprint bad()\ncatch IndexError error\nprint "caught: {error}"\nfinally\nprint "finally"\nend\nprint "after"',
            "caught: index out of bounds\nfinally\nafter\n",
        ),
        "custom_errors": (
            'class Problem\nstring message\nend\nfunction fail() returns void\nthrow Problem("custom")\nend\ntry\nfail()\ncatch Problem problem\nprint problem.message\nend\ntry\nthrow "message"\ncatch string message\nprint message\nend',
            "custom\nmessage\n",
        ),
        "finally_return": (
            'function value() returns string\ntry\nreturn "retained"\nfinally\nprint "cleanup"\nend\nend\nprint value()',
            "cleanup\nretained\n",
        ),
        "finally_nested": (
            'try\ntry\nthrow RuntimeError("bad")\nfinally\nprint "inner"\nend\ncatch RuntimeError error\nprint error\nfinally\nprint "outer"\nend',
            "inner\nbad\nouter\n",
        ),
        "finally_loop_exit": (
            'int i=0\nwhile i<3 do\ntry\ni+=1\nif i==1 then\ncontinue\nend\nbreak\nfinally\nprint "cleanup {i}"\nend\nend',
            "cleanup 1\ncleanup 2\n",
        ),
        "finally_override": (
            'function value() returns string\ntry\nreturn "old"\nfinally\nreturn "new"\nend\nend\nprint value()\ntry\ntry\nthrow "old"\nfinally\nthrow "new"\nend\ncatch string message\nprint message\nend',
            "new\nnew\n",
        ),
        "results": (
            'function good() returns Result<int,string>\nreturn Result.ok(42)\nend\nResult<int,string> bad = Result.error("no")\nvar answer = good()\nprint answer.is_ok\nprint answer.unwrap()\nprint bad.is_error()\nprint bad.error()\ntry\nprint bad.unwrap()\ncatch ResultError error\nprint "caught"\nend',
            "true\n42\ntrue\nno\ncaught\n",
        ),
        "arc_scope_cleanup": (
            'import testing\nint before = testing.live_objects()\nif true then\nvar words = ["a", "b"]\nend\nprint testing.live_objects() == before\ntry\nvar words = ["c"]\nthrow RuntimeError("bad")\ncatch RuntimeError error\nprint error\nend\nprint testing.live_objects() == before',
            "true\nbad\ntrue\n",
        ),
        "error_numeric": (
            'try\nint n=9223372036854775807\nprint n+1\ncatch NumericError error\nprint "overflow caught"\nend\ntry\nint x=int("bad")\ncatch NumericError error\nprint "cast caught"\nend',
            "overflow caught\ncast caught\n",
        ),
    }
)

POSITIVE.update(
    {
        "collection_range": (
            "import collections\nvar a=collections.range(2,5)\nfor n in a do\nprint n\nend\nprint len(collections.range(4,2))",
            "2\n3\n4\n0\n",
        ),
        "nested_tuple_array": (
            '[int,string][] pairs=[]\npairs.push([1,"one"])\nprint pairs[0][1]',
            "one\n",
        ),
        "primitive_throw": (
            'try\nthrow 42\ncatch int n\nprint n\nend\ntry\nthrow [1,"pair"]\ncatch [int,string] pair\nprint pair[1]\nend',
            "42\npair\n",
        ),
        "catch_order": (
            'try\nthrow FileError("missing")\ncatch NumericError wrong\nprint "bad"\ncatch FileError error\nprint error\nfinally\nprint "cleanup"\nend\ntry\nthrow RuntimeError("all")\ncatch error\nprint error.message\nend',
            "missing\ncleanup\nall\n",
        ),
        "finally_inner_catch": (
            'function value() returns string\ntry\nreturn "retained"\nfinally\ntry\nthrow "ignored"\ncatch string message\nprint message\nend\nend\nend\nprint value()',
            "ignored\nretained\n",
        ),
        "finally_error_scope": (
            'import testing\nint before=testing.live_objects()\ntry\ntry\nthrow RuntimeError("old")\nfinally\nthrow RuntimeError("new")\nend\ncatch RuntimeError e\nprint e\nend\nprint testing.live_objects()==before',
            "new\ntrue\n",
        ),
        "reentrant_custom_render": (
            'string text="before"\nclass Callback\nfunction render() returns string\ntext="after"\nreturn "!"\nend\nend\nvar obj=Callback()\nprint text+obj.render()\nprint text',
            "before!\nafter\n",
        ),
        "error_array_pop": (
            'int[] a=[]\ntry\nprint a.pop()\ncatch IndexError error\nprint "empty"\nend\nmap<string,int> m={}\ntry\nprint m["missing"]\ncatch KeyError error\nprint "key"\nend',
            "empty\nkey\n",
        ),
    }
)

POSITIVE.update(
    {
        "fs_result": (
            'import fs\nvar failed=fs.try_read("not-here.txt")\nprint failed.is_error()\nprint failed.error().message\nfs.write("found.txt","ok")\nvar good=fs.try_read("found.txt")\nprint good.unwrap()',
            "true\ncannot read 'not-here.txt'\nok\n",
        )
    }
)

POSITIVE.update(
    {
        "rethrow": (
            'try\ntry\nthrow FileError("again")\ncatch error\nthrow error\nend\ncatch FileError error\nprint error\nend',
            "again\n",
        ),
        "map_assignment_exception": (
            'map<string,int> m={}\nfunction fail() returns int\nthrow RuntimeError("failed")\nend\ntry\nm["x"]=fail()\ncatch RuntimeError error\nprint "caught"\nend\nprint "x" in m\nm["x"]=3\nfunction remove() returns int\nm.clear()\nreturn 4\nend\nm["x"]+=remove()\nprint m["x"]',
            "caught\nfalse\n7\n",
        ),
        "finally_break_cleanup": (
            'import testing\nint before=testing.live_objects()\nwhile true do\ntry\nthrow RuntimeError("old")\nfinally\nbreak\nend\nend\nprint testing.live_objects()==before',
            "true\n",
        ),
    }
)

POSITIVE.update(
    {
        "fragment_file_original": (
            'import fs\nimport html.*\nelement page=<><h1>Hello, World!</h1><p>Welcome to Wednesware!</p><p>Enjoy your stay!</p></>\nfs.write "output.txt", page.render()\nprint fs.read("output.txt")',
            "<h1>Hello, World!</h1><p>Welcome to Wednesware!</p><p>Enjoy your stay!</p>\n",
        ),
        "tuple_constant_expression": (
            '[int,string] pair=[42,"answer"]\nprint pair[1-1]\nprint pair[2//2]',
            "42\nanswer\n",
        ),
    }
)

POSITIVE.update(
    {
        "module_initialization_error": (
            'function read() returns string\nreturn name\nend\ntry\nprint read()\ncatch RuntimeError error\nprint "initialization caught"\nend\nstring name="ready"\nprint read()',
            "initialization caught\nready\n",
        )
    }
)

POSITIVE.update(
    {
        "typed_error_messages": (
            'import testing\ntry\ntesting.assert(false,"JSON read overflow")\ncatch AssertionError error\nprint error.message\nend',
            "assertion failed: JSON read overflow\n",
        )
    }
)

POSITIVE.update(
    {
        "iterable_error_cleanup": (
            'import testing\nint before=testing.live_objects()\ntry\nfor text in ["temporary"] do\nthrow RuntimeError("loop")\nend\ncatch RuntimeError error\nprint error\nend\nprint testing.live_objects()==before',
            "loop\ntrue\n",
        )
    }
)

POSITIVE.update(
    {
        "zero_parameter_declaration": (
            'function main returns void\nprint "Hello, World!"\nend\nmain',
            "Hello, World!\n",
        ),
        "zero_parameter_inference": (
            "function answer\nreturn 42\nend\nfunction main\nprint answer()\nend\nmain()",
            "42\n",
        ),
        "zero_parameter_methods": (
            "class Counter\nint count=0\nfunction increment returns void\ninst count+=1\nend\nfunction value\nreturn inst count\nend\nend\nvar counter=Counter()\ncounter.increment\nprint counter.value()",
            "1\n",
        ),
    }
)

POSITIVE.update(
    {
        "web_wait_empty": (
            'import web\nweb.wait()\nprint "no listeners"',
            "no listeners\n",
        )
    }
)

POSITIVE.update(
    {
        "format_nested_collections": (
            'map<string,int[]> values={"a":[1,2],"empty":[]}\nprint string(values)\nprint values\nvar tuple_value=[true,"x",values]\nprint string(tuple_value)\nprint "Nested: {values}"\n',
            '{"a": [1, 2], "empty": []}\n{"a": [1, 2], "empty": []}\n(true, "x", {"a": [1, 2], "empty": []})\nNested: {"a": [1, 2], "empty": []}\n',
        ),
        "format_class_result_null": (
            'class User\nstring name\nint age\nend\nvar user=User("Danny",15)\nprint string(user)\nResult<int,string> result=Result.error("no")\nprint result\nstring? missing=null\nprint [missing]\nmap<bool,u64> large={true:u64("18446744073709551615")}\nprint large\n',
            'User(name="Danny", age=15)\nResult.error("no")\n[null]\n{true: 18446744073709551615}\n',
        ),
        "collection_casts_atomic_strings": (
            'print array("hello")\nprint tuple("hello")\nprint array(42)\nprint tuple([1,2,3])\nprint array(tuple([1,2,3]))\nprint tuple()\narray<string> empty=array()\nmap<string,int> none=map()\nprint empty\nprint none\n',
            '["hello"]\n("hello",)\n[42]\n(1, 2, 3)\n[1, 2, 3]\n()\n[]\n{}\n',
        ),
        "nested_collection_casts": (
            'array<int> numbers=array<int>(["1","2"])\nprint numbers\narray<array<int>> rows=array<array<int>>([["1","2"],["3"]])\nprint rows\narray<string> words=array<string>([[1,2],[3]])\nprint words\nprint int(array(array("42")))\nprint bool("false")\nprint bool(2)\nprint bool(array<int>())\nstring? missing=null\nprint bool(missing)\narray<string> nullable=array<string>([missing])\nprint nullable\n',
            '[1, 2]\n[[1, 2], [3]]\n["[1, 2]", "[3]"]\n42\nfalse\ntrue\nfalse\nfalse\n["null"]\n',
        ),
        "map_collection_casts": (
            'map<string,int> scores=map<string,int>([["a","1"],["b","2"]])\nprint scores\nprint array(scores)\nprint tuple(scores)\nprint map(array(scores))\nvar pairs=[ ["a",1], ["b",2] ]\nprint map(pairs)\n',
            '{"a": 1, "b": 2}\n[("a", 1), ("b", 2)]\n(("a", 1), ("b", 2))\n{"a": 1, "b": 2}\n{"a": 1, "b": 2}\n',
        ),
        "collection_shallow_copy": (
            "var original=[[1]]\nvar copy=array(original)\ncopy.push([2])\ncopy[0].push(3)\nprint original\nprint copy\n",
            "[[1, 3]]\n[[1, 3], [2]]\n",
        ),
        "repetition": (
            'var array_value=[1,2]\nprint array_value*3\nprint 2*array_value\nprint "ab"*3\nprint 2*"x"\nprint [1,"x"]*2\nprint [1,2]*0\nint[] empty=[]\nprint len(empty*9223372036854775807)\nprint len(""*9223372036854775807)\n',
            '[1, 2, 1, 2, 1, 2]\n[1, 2, 1, 2]\nababab\nxx\n(1, "x", 1, "x")\n[]\n0\n0\n',
        ),
        "ranges": (
            "print 1..3\nprint 1..<3\nprint -2..1\nprint 2..2\nprint 2..<2\nprint 4..1\nint start=2\nint finish=4\nvar runtime_range=start..finish\nprint runtime_range[1]\nprint typeof(runtime_range)\nfor value in runtime_range do\nprint value\nend\nprint 3 in runtime_range\nprint((1..<3)*2)\nprint 1..1+2\n",
            "(1, 2, 3)\n(1, 2)\n(-2, -1, 0, 1)\n(2,)\n()\n()\n3\ntuple<i64>\n2\n3\n4\ntrue\n(1, 2, 1, 2)\n(1, 2, 3)\n",
        ),
        "cast_errors": (
            'try\nprint int([1,2])\ncatch CastError error\nprint "size"\nend\ntry\nprint bool("maybe")\ncatch CastError error\nprint "bool"\nend\ntry\nprint array<i8>(["256"])\ncatch NumericError error\nprint "narrow"\nend\ntry\nprint [1]*-1\ncatch NumericError error\nprint "negative"\nend\ntry\nprint -9223372036854775808..9223372036854775807\ncatch NumericError error\nprint "range"\nend\n',
            "size\nbool\nnarrow\nnegative\nrange\n",
        ),
        "dom_query": (
            'import html.*\nvar tree=<div id="root"><h1 id="main-title">Title</h1><section class="card"><p class="lead">First</p><p>Second</p></section><p>Other</p></div>\nprint tree.query("#main-title").text_content\nprint tree.get_by_id("main-title").tag\narray<element> cards=tree.query_all(".card p")\nfor item in cards do\nprint item.text_content\nend\nprint len(html.query_all(tree,".card > p"))\nprint html.query(tree,".missing")==null\nprint tree.query("#root")==null\nprint tree.matches("div#root")\n',
            "Title\nh1\nFirst\nSecond\n2\ntrue\ntrue\ntrue\n",
        ),
        "dom_selectors": (
            'import html.*\nvar root=<><p id="one" class="card primary" data-state="ready now">A</p><section><p class="card" data-state="ready">B</p></section></>\nprint len(root.query_all("p.card.primary, #one"))\nprint len(root.query_all("[data-state]"))\nprint len(root.query_all("p[data-state~=ready]"))\nprint root.query("section > p.card").text_content\nprint root.query("#one").matches("P.card#one")\ntry\nroot.query("p:hover")\ncatch SelectorError error\nprint "unsupported"\nend\n',
            "1\n2\n2\nB\ntrue\nunsupported\n",
        ),
        "dom_attributes_and_classes": (
            'import html.*\nvar node=<div />\nnode.id="main-title"\nnode.class_name="card"\nprint node.id\nprint node.has_class("card")\nprint node.add_class("active")\nprint node.add_class("active")\nprint node.toggle_class("card")\nprint node.class_name\nnode.set_attribute("data-label","<safe>")\nprint node.get_attribute("data-label")\nprint node.has_attribute("data-label")\nprint node.remove_attribute("data-label")\nprint node.get_attribute("data-label")==null\nprint node\n',
            'main-title\ntrue\ntrue\nfalse\nfalse\nactive\n<safe>\ntrue\ntrue\ntrue\n<div id="main-title" class="active"></div>\n',
        ),
        "dom_mutation_and_aliases": (
            'import html.*\nvar root=<div><p id="text">old</p></div>\nvar node=root.query("#text")\nnode.text_content="<new> & safe"\nprint root\nroot.append(<span>last</span>)\nroot.prepend("first")\nprint root.text_content\nprint len(root.children)\nprint root.remove_child(node)\nprint node.text_content\nprint root.inner_html\nroot.clear()\nprint root\n',
            '<div><p id="text">&lt;new&gt; &amp; safe</p></div>\nfirst<new> & safelast\n2\ntrue\n<new> & safe\nfirst<span>last</span>\n<div></div>\n',
        ),
        "dom_clone_and_fragment_children": (
            'import html.*\nvar root=<div><><p>one</p><p>two</p></></div>\nprint len(root.children)\nvar copy=root.clone()\ncopy.query("p").set_text("changed")\nprint root\nprint copy\nprint root.clone(false)\ntry\nroot.append(root)\ncatch RuntimeError error\nprint "cycle"\nend\n',
            "2\n<div><p>one</p><p>two</p></div>\n<div><p>changed</p><p>two</p></div>\n<div></div>\ncycle\n",
        ),
        "dom_nullable": (
            'import html.*\nvar root=<div />\nelement? missing=root.query("p")\nprint string(missing)\nprint bool(missing)\ntry\nmissing.set_text("bad")\ncatch CastError error\nprint "null"\nend\n',
            "null\nfalse\nnull\n",
        ),
        "reentrant_component": (
            'import html.*\nstring text="before"\nfunction replace returns string\ntext="after"\nreturn "!"\nend\ncomponent SideEffect=<p>{replace()}</p>\nprint text+html.render(<SideEffect />)\nprint text\n',
            "before<p>!</p>\nafter\n",
        ),
    }
)

POSITIVE.update(
    {
        "dom_result_ownership": (
            'import html.*\nfunction detached returns element?\nvar root=<div><p id="owned">retained</p></div>\nreturn root.query("#owned")\nend\nvar node=detached()\nprint node.text_content\nvar root=<div><p>one</p><p>two</p></div>\nvar snapshot=root.query_all("p")\nroot.clear()\nfor item in snapshot do\nprint item.text_content\nend\n',
            "retained\none\ntwo\n",
        ),
        "dom_attribute_selector_escaping": (
            'import html.*\nvar root=<div><p data-label="a &amp; b" class="card small">&lt;text&gt;</p></div>\nprint root.query("[data-label=\\"a & b\\"]").text_content\nprint root.query(".car")==null\nprint root.query(".card.small").tag\nhtml.set_text(root.query("p"),"<script>&")\nprint html.inner_html(root)\n',
            '<text>\ntrue\np\n<p data-label="a &amp; b" class="card small">&lt;script&gt;&amp;</p>\n',
        ),
        "dom_safety_errors": (
            'import html.*\nvar root=<div><section /></div>\ntry\nroot.query("section").append(root)\ncatch RuntimeError error\nprint "cycle"\nend\ntry\nroot.query("[id=")\ncatch SelectorError error\nprint "selector"\nend\ntry\nroot.set_attribute("bad name","x")\ncatch RuntimeError error\nprint "attribute"\nend\nvar br=<br />\ntry\nbr.set_text("bad")\ncatch RuntimeError error\nprint "void"\nend\nprint root\n',
            "cycle\nselector\nattribute\nvoid\n<div><section></section></div>\n",
        ),
        "format_cycle_checked": (
            "class Node\nNode? next=null\nend\nvar node=Node()\nnode.next=node\nprint string(node)\nnode.next=null\n",
            "Node(next=<cycle>)\n",
        ),
        "empty_tuple_repetition": (
            "int count=3\nprint tuple()*9223372036854775807\n[int,int] pair=[1,2]\nprint pair*count\n",
            "()\n(1, 2, 1, 2, 1, 2)\n",
        ),
        "checked_nested_cast_failure": (
            'try\nprint map<string,int>([["a","1"],["bad","oops"]])\ncatch NumericError error\nprint "value"\nend\ntry\nprint map<string,int>([["one"]])\ncatch CastError error\nprint "pair"\nend\narray<int> original=[1,2]\nprint original\n',
            "value\npair\n[1, 2]\n",
        ),
        "format_nested_escaping": (
            'print string(["a\\n\\"b",[true,false]])\n',
            '("a\\n\\"b", [true, false])\n',
        ),
    }
)

POSITIVE.update(
    {
        "boolean_nested_casts": (
            'array<bool> values=array<bool>(["true","false"])\nprint values\nprint true in values\nmap<bool,int> m=map<bool,int>([["true",1]])\nprint m[true]\nprint m.keys()',
            "[true, false]\ntrue\n1\n[true]\n",
        ),
        "html_tag_value_namespace": (
            'import html.*\nvar div=<div>content</div>\nprint div\nfunction p returns string\nreturn "function"\nend\nprint <p>{p()}</p>',
            "<div>content</div>\n<p>function</p>\n",
        ),
    }
)

POSITIVE.update(
    {
        "dom_fragment_child_removal": (
            "import html.*\nvar root=<div><><p>one</p><p>two</p></></div>\nprint root.remove_child(root.children[0])\nprint root",
            "true\n<div><p>two</p></div>\n",
        ),
        "dom_attributes_snapshot": (
            'import html.*\nvar node=<p id="title" class="card">text</p>\nvar attrs=node.attributes\nprint attrs\nattrs["id"]="snapshot"\nprint node.id\nprint html.attributes(node)["class"]',
            '{"id": "title", "class": "card"}\ntitle\ncard\n',
        ),
        "optional_singleton_casts": (
            'import html.*\nvar root=<div><p>one</p></div>\nprint array(root.query("p"))\nprint array(root.query("missing"))\nprint tuple(root.query("missing"))',
            "[<p>one</p>]\n[null]\n(null,)\n",
        ),
        "typed_tuple_cast": (
            '[int,string] pair=tuple(["42",2])\nprint pair\ntuple<int> values=tuple<int>(["1","2"])\nprint values[1]',
            '(42, "2")\n2\n',
        ),
    }
)

POSITIVE.update(
    {
        "fragment_attribute_error": (
            'import html.*\nvar root=<></>\ntry\nroot.set_attribute("id","hidden")\ncatch RuntimeError error\nprint "fragment"\nend',
            "fragment\n",
        )
    }
)

POSITIVE.update(
    {
        "json_value_collection_cast": (
            'import json as j\nj.value value=j.parse("{{\\"x\\":1}}")\nprint string(value)\nprint array(value)',
            '{"x":1}\n[{"x":1}]\n',
        )
    }
)


def fly_string(value):
    return json.dumps(value, ensure_ascii=False).replace("{", "{{").replace("}", "}}")


POSITIVE.update(
    {
        "process_exit_status": (
            'import os as system\nprint system.shell("exit 7")\nprint system.shell("exit 0")',
            "7\n0\n",
        ),
        "process_shell_operators": (
            'import os\nprint os.shell("echo left&&echo right")',
            "left\nright\n0\n",
        ),
        "process_arguments": (
            'import os\nprint "before"\nprint os.command(['
            + fly_string(sys.executable)
            + ', "-X", "utf8", "-c", '
            + fly_string("import sys; print(repr(sys.argv[1:])); sys.exit(9)")
            + ', "a b", "", "a\\"b", "tail\\\\", "é", "$HOME; echo nope"])',
            "before\n"
            + repr(["a b", "", 'a"b', "tail\\", "é", "$HOME; echo nope"])
            + "\n9\n",
        ),
        "process_failures": (
            (
                "import os\nimport testing\nvar base=testing.live_objects()\n"
                "for i in 0..<5 do\n"
                'try\nos.command([])\ncatch ProcessError error\nprint "empty"\nend\n'
                'try\nos.command(["flywheel-no-such-executable-314159"])\ncatch ProcessError error\nprint "missing"\nend\n'
                'try\nos.command(["", "arg"])\ncatch ProcessError error\nprint "name"\nend\n'
                'try\nos.shell("exit 0\\u{0}ignored")\ncatch ProcessError error\nprint "nul"\nend\n'
                'try\nos.command(["echo", "arg\\u{0}hidden"])\ncatch ProcessError error\nprint "arg-nul"\nend\n'
                "end\nprint testing.live_objects()==base"
            ),
            "empty\nmissing\nname\nnul\narg-nul\n" * 5 + "true\n",
        ),
    }
)

POSITIVE.update(
    {
        "desktop_values": (
            """import desktop as gui
import html.*
import testing
var base=testing.live_objects()
function make returns gui.window
    gui.Window view=gui.Window("Hello",480,320)
    view.load(<><h1>Hello &amp; World</h1><p>{"<text>"}</p></>)
    return view
end
function check returns void
    var window=make()
    print typeof(window)
    print string(window)
    print testing.live_objects()>base
    window.load(<p>Replacement</p>)
    print testing.live_objects()>base
    gui.window? optional=window
    optional.load(<p>Nullable alias</p>)
    var defaults=gui.window()
    print defaults
end
check()
print testing.live_objects()==base""",
            'desktop.window\ndesktop.window("Hello")\ntrue\ntrue\ndesktop.window("Flywheel")\ntrue\n',
        ),
        "desktop_invalid_values": (
            """import desktop
import html.*
import testing
var base=testing.live_objects()
for n in [0,-1,2147483648] do
    try
        var bad=desktop.window("bad",n,200)
    catch DesktopError error
        print "size"
    end
end
try
    var bad=desktop.window("nul\\u{0}")
catch DesktopError error
    print "title"
end
function invalid_html returns void
    var window=desktop.window()
    try
        window.load(<p>{"nul\\u{0}"}</p>)
    catch DesktopError error
        print "html"
    end
end
invalid_html()
print testing.live_objects()==base""",
            "size\nsize\nsize\ntitle\nhtml\ntrue\n",
        ),
    }
)

POSITIVE.update(
    {
        "bare_desktop_constructors": (
            """import desktop as gui
import html.*
import testing
var base=testing.live_objects()
function check returns void
    var Window=gui.window
    print Window
    print typeof(Window)
    var window=gui.window()
    print typeof(window)
    window.load(<h1>Hello, World!</h1>)
    var named=gui.window "my-window"
    print named
    var called=gui.window("my-window", 480, 320)
    print called
    print testing.live_objects()>=base+3
    var explicit=Window("Custom",480,320)
    print explicit
    var alias=Window
    var other=alias()
    other.load(<><p>Fragment</p></>)
    var outer=alias "Outer"
    print outer
    gui.window typed=gui.Window()
    print typed
    var grouped=((gui.Window))
    print grouped
    var direct=(gui.window)()
    print direct
    print bool(gui.window)
end
check()
print testing.live_objects()==base""",
            '<class desktop.window>\nclass:desktop.window\ndesktop.window\ndesktop.window("my-window")\ndesktop.window("my-window")\ntrue\ndesktop.window("Custom")\ndesktop.window("Outer")\ndesktop.window("Flywheel")\n<class desktop.window>\ndesktop.window("Flywheel")\ntrue\ntrue\n',
        ),
        "bare_class_constructor_references": (
            """import testing
class Person
    string name="Default"
end
function answer returns int
    return 42
end
var Class=Person
print Class
var person=Person()
print person.name
var explicit=Class("Custom")
print explicit.name
var outer=Class "Outer"
print outer.name
var defaults=Class()
print defaults.name
var grouped=(Person)
print grouped
var reference=answer
print reference()
print (answer)
print((1+2)*3)""",
            "<class Person>\nDefault\nCustom\nOuter\nDefault\n<class Person>\n42\n<function answer>\n9\n",
        ),
    }
)

POSITIVE.update(
    {
        "grouped_zero_argument_statements": (
            """import desktop
import testing
var base=testing.live_objects()
class Counter
    int value=0
    function increment returns void
        inst value+=1
    end
end
function hello returns void
    print "called"
end
function check returns void
    var counter=Counter()
    counter.increment
    (counter.increment)
    ((counter.increment))
    print counter.value
    hello
    (hello)
    var callback=hello
    (callback)
end
check()
print testing.live_objects()==base""",
            "3\ncalled\ncalled\ncalled\ntrue\n",
        ),
    }
)

POSITIVE.update(
    {
        "closures_higher_order": (
            """function double(int x) returns int
    return x * 2
end
function apply(function(int) returns int f, int x) returns int
    return f(x)
end
print apply(double, 21)
function run() returns void
    int base = 10
    var add = function(int x) returns int => x + base
    print add(5)
    print apply(add, 1)
    var nums = [1, 2, 3, 4, 5]
    print nums.map(function(int x) => x * x)
    print nums.filter(function(int x) => x % 2 == 1)
    print nums.reduce(0, function(int total, int x) => total + x)
    print nums.any(function(int x) => x > 4)
    print nums.all(function(int x) => x > 4)
    nums.each(function(int x)
        if x == 3 then
            print "three"
        end
    end)
    print nums.map(double)
    print nums.map(function(int n) returns string
        var label = "n"
        return label + string(n)
    end)
    array<function(int) returns int> ops = [double, add]
    for op in ops do
        print op(3)
    end
    var three = (function(int x) => x + 1)(2)
    print three
    print add.call(1)
    print typeof(add)
    print add
end
run()
function counter() returns function() returns int
    int[] count = [0]
    return function() returns int
        count[0] += 1
        return count[0]
    end
end
var next = counter()
next()
next
print next()""",
            '42\n15\n11\n[1, 4, 9, 16, 25]\n[1, 3, 5]\n15\ntrue\nfalse\nthree\n[2, 4, 6, 8, 10]\n["n1", "n2", "n3", "n4", "n5"]\n6\n13\n3\n11\nfunction(i64) returns i64\n<function>\n3\n',
        ),
        "closure_fields_and_nesting": (
            """import testing
class Button
    string label
    function() returns void on_click = function() => print("default")
    function greeter() returns function(string) returns string
        return function(string who) returns string => "{inst label} greets {who}"
    end
end
function outer(int a) returns int
    var mid = function(int b) returns int
        var inner = function(int c) returns int => a + b + c
        return inner(100)
    end
    return mid(10)
end
function scenario() returns void
    var button = Button("OK")
    button.on_click()
    weak<Button> self = button
    button.on_click = function()
        var target = self.get()
        if target != null then
            print "clicked {target.label}"
        end
    end
    button.on_click()
    print button.greeter()("Ann")
    print outer(1)
    var fail = function(int x) returns int
        try
            if x > 1 then
                throw RuntimeError("too big")
            end
            return x
        finally
            print "finally {x}"
        end
    end
    print fail(1)
    try
        fail(2)
    catch RuntimeError error
        print error.message
    end
end
var before = testing.live_objects()
scenario()
print testing.live_objects() == before""",
            "default\nclicked OK\nOK greets Ann\n111\nfinally 1\n1\nfinally 2\ntoo big\ntrue\n",
        ),
        "weak_references": (
            """import testing
import html.*
class Node
    string name
    weak<Node> parent = null
    Node[] children = []
end
function tree() returns void
    var root = Node("root")
    var child = Node("child", root)
    root.children.push(child)
    var parent = child.parent.get()
    if parent != null then
        print parent.name
    end
    print child.parent.alive()
    print child.parent
end
var before = testing.live_objects()
tree()
print testing.live_objects() == before
weak<Node> temporary = weak(Node("temp"))
print temporary.alive()
print temporary.get() == null
print temporary
try
    print temporary.get().name
catch CastError error
    print error.message
end
var kept = Node("kept")
weak<Node> alias = kept
print alias.get().name
weak<element> page = weak(<p>hi</p>)
print page.alive()
var div = <div>x</div>
weak<element> view = div
print view.get()""",
            'root\ntrue\nweak(Node(name="root", parent=weak(null), children=[Node(name="child", parent=weak(<cycle>), children=[])]))\ntrue\nfalse\ntrue\nweak(null)\ncannot cast null to Node\nkept\nfalse\n<div>x</div>\n',
        ),
        "hashed_maps": (
            """map<string, int> words = {}
int i = 0
while i < 3000 do
    words["k" + string(i)] = i
    i += 1
end
int total = 0
for key in words do
    total += words[key]
end
print total
print words.remove("k1500")
print "k1500" in words
print words["k2999"]
words["k1500"] = -1
print words.keys()[2999]
print len(words)
map<int, string> small = {3: "c", 1: "a", 2: "b"}
print small.remove(1)
small[1] = "z"
small[3] = "C"
print small
small.clear()
small[7] = "x"
print small
map<bool, int> flags = {true: 1}
flags[false] = 0
print flags[false]
print map<string,int>([["a", "1"], ["b", "2"], ["a", "3"]])""",
            '4498500\ntrue\nfalse\n2999\nk1500\n3000\ntrue\n{3: "C", 2: "b", 1: "z"}\n{7: "x"}\n0\n{"a": 3, "b": 2}\n',
        ),
    }
)

DESKTOP_FAILURE = """import desktop
import html.*
import testing
var base=testing.live_objects()
function attempt returns void
    var window=desktop.window()
    window.load(<><h1>Test</h1></>)
    for i in 0..<2 do
        try
            (window.run)
        catch DesktopError error
            print "desktop error"
        finally
            print "cleanup"
        end
    end
end
attempt()
print testing.live_objects()==base"""

INPUT_CASES = {
    "lines": (
        (
            'import core\nprint input("Name: ")\nprint core.input()\nprint input()\n'
            "try\ninput()\ncatch InputError error\nprint error.message\nend"
        ),
        "é & text\r\n\nlast line",
        "Name: é & text\n\nlast line\nend of input\n",
    ),
    "empty": (
        (
            "import testing\nvar base=testing.live_objects()\n"
            "for i in 0..<5 do\ntry\nvar line=input()\n"
            'catch InputError error\nprint "eof"\nend\nend\nprint testing.live_objects()==base'
        ),
        "",
        "eof\n" * 5 + "true\n",
    ),
}

NEGATIVE = {
    "markup_argument_mismatch": ("import html.*\nprint <p>bad</div>", "mismatched"),
    "markup_argument_unknown": ("print <h1>bad</h1>", "unknown tag"),
    "unknown_tag": ("var t = <p>test</p>", "unknown tag"),
    "missing_attr": (
        "import html.*\ncomponent C = <p>{attr user}</p>\nvar c = <C />",
        "missing required",
    ),
    "mismatch": ("import html.*\nvar t = <p>test</div>", "mismatched"),
    "fragment_close": ("import html.*\nvar t = <p>test</>", "mismatched"),
    "null": ("string s = null", "cannot convert"),
    "wrong_type": ('int i = "text"', "cannot convert"),
    "undeclared": ("score = 5", "undeclared"),
    "then": ("if true\nprint 1\nend", "expected 'then'"),
    "do": ("while true\nbreak\nend", "expected 'do'"),
    "nested_call": (
        "function add(int a, int b) returns int\nreturn a+b\nend\nprint add 2, 3",
        "nested calls",
    ),
    "for_binding": ("for (item in [1]) do\nend", "parentheses"),
    "fallthrough": (
        "function f() returns int\nif true then\nreturn 1\nend\nend",
        "return on all paths",
    ),
    "narrowing": ("int n = 100\ni8 x = n", "explicit checked cast"),
    "int_double": ("int n = 1\ndouble d = n", "explicit checked cast"),
    "const": ("const int n = 1\nn = 2", "const"),
    "truthiness": ("if 1 then\nend", "bool"),
    "break": ("break", "requires a loop"),
    "attr": ("print attr name", "component"),
    "attribute_type": ("import html.*\nvar t = <p style={true}>x</p>", "requires a string or number"),
    "attribute_unknown": (
        'import html.*\nvar t = <p bogus="x" />',
        "unknown attribute",
    ),
    "import_collision": ("import fs\nimport math as fs", "duplicate import"),
    "unicode": ('print "\\u{D800}"', "Unicode"),
    "unsupported_module": ("import gizmo", "not found or not implemented"),
    "init_unassigned": (
        "class C\nint x\nfunction init()\nend\nend",
        "init must assign required field 'x'",
    ),
}
TRAPS = {
    "overflow": ("int n = 9223372036854775807\nprint n + 1", "overflow"),
    "underflow": ("int n = -9223372036854775808\nprint n - 1", "overflow"),
    "division": ("int n = 0\nprint 5 // n", "division by zero"),
    "division_overflow": ("int n = -9223372036854775808\nprint n // -1", "overflow"),
    "narrow_cast": ("int n = 256\nprint u8(n)", "cast out of range"),
    "signed_cast": ("int n = -1\nprint u64(n)", "cast out of range"),
    "bounds": ("var a = [1]\nprint a[1]", "bounds"),
    "assertion": ('import testing\ntesting.assert(false, "no")', "assertion failed"),
}

NEGATIVE.update(
    {
        "component_type": (
            'import html.*\ncomponent C = <p>{attr v}</p>\nvar a = <C v={1} />\nvar b = <C v="two" />',
            "inconsistent component",
        ),
        "exceptions": ("try\nend", "try requires catch or finally"),
    }
)
TRAPS.update(
    {
        "float_cast": ("double d = 1e100\nprint float(d)", "cast out of range"),
        "json_missing": (
            'import json\nprint json.as_int(json.get(json.parse("{{}}"), "x"))',
            "missing JSON field",
        ),
        "json_invalid": ('import json\nvar j = json.parse("[1,]")', "invalid JSON"),
    }
)


TRAPS.update(
    {
        "invalid_int_string": ('print int("12x")', "integer string"),
        "string_overflow": ('print int("9223372036854775808")', "integer string"),
    }
)


NEGATIVE.update(
    {
        "tuple_dynamic_index": (
            '[int,string] pair=[1,"x"]\nint i=0\nprint pair[i]',
            "tuple indices must be compile-time",
        ),
        "tuple_length": ("[int,string] pair=[1]", "tuple length"),
        "map_bad_key": ("map<double,int> m={}", "map keys"),
        "array_wrong_push": ('int[] a=[]\na.push("x")', "cannot convert"),
        "result_context": ("var r=Result.ok(1)", "expected Result"),
        "catch_after_all": (
            "try\nthrow 1\ncatch Error e\ncatch int i\nend",
            "catch-all must be the last",
        ),
    }
)

NEGATIVE.update(
    {
        "parameter_parentheses_required": (
            "function add int a, int b returns int\nreturn a+b\nend",
            "function declarations with parameters require parentheses",
        ),
        "default_parameter_parentheses_required": (
            "function value int n = 1 returns int\nreturn n\nend",
            "function declarations with parameters require parentheses",
        ),
    }
)

NEGATIVE.update(
    {
        "map_repetition": (
            "map<string,int> m={}\nprint m*2",
            "maps cannot be repeated",
        ),
        "range_float": ("print 1.0..2.0", "range endpoints must be integers"),
        "hetero_dynamic_repeat": (
            'var pair=[1,"a"]\nint count=2\nprint pair*count',
            "constant count",
        ),
        "dom_no_root": (
            'import html\nprint html.query("p")',
            "requires an element or fragment",
        ),
    }
)

NEGATIVE.update(
    {
        "input_type": ("input(42)", "cannot convert"),
        "input_arity": ('input("a", "b")', "expects zero or one"),
        "command_type": ("import os\nos.command([1, 2])", "cannot convert"),
        "command_scalar": (
            'import os\nos.command("echo hi")',
            "expects an array of strings",
        ),
        "shell_type": ('import os\nos.shell(["echo"])', "cannot convert"),
    }
)

NEGATIVE.update(
    {
        "desktop_constructor_arity": (
            'import desktop\ndesktop.window("Title",1,2,3)',
            "expects optional title",
        ),
        "desktop_constructor_title": (
            "import desktop\ndesktop.window(42)",
            "cannot convert",
        ),
        "desktop_constructor_size": (
            'import desktop\ndesktop.window("Title",1.5)',
            "explicit checked cast",
        ),
        "desktop_load_type": (
            'import desktop\nvar window=desktop.window()\nwindow.load("html")',
            "expects an element or fragment",
        ),
        "desktop_load_arity": (
            "import desktop\nvar window=desktop.window()\nwindow.load()",
            "expects one element or fragment",
        ),
        "desktop_run_arity": (
            "import desktop\nvar window=desktop.window()\nwindow.run(1)",
            "expects no arguments",
        ),
        "desktop_unknown_method": (
            "import desktop\nvar window=desktop.window()\nwindow.unknown()",
            "unknown desktop.window method",
        ),
    }
)

NEGATIVE.update(
    {
        "grouped_constructor_instance": (
            "import desktop\ndesktop.window value=(desktop.window)",
            "is a class reference, not an instance",
        ),
        "constructor_reference_method": (
            "import desktop\nvar Window=(desktop.window)\nWindow.run()",
            "has no instance methods",
        ),

        "constructor_reference_cast": (
            "import desktop\nprint int((desktop.window))",
            "unsupported cast source",
        ),
        "bare_constructor_missing_required": (
            "class Person\nstring name\nend\nvar person=Person()",
            "missing required constructor field",
        ),
        "class_reference_statement": (
            "import desktop\ndesktop.window",
            "is a class reference; call it with ()",
        ),
        "class_reference_field": (
            "class Person\nstring name=\"x\"\nend\nvar person=Person\nprint person.name",
            "is a class reference, not an instance",
        ),
        "desktop_member_diagnostic": (
            "import desktop\nvar value=desktop.windows",
            "module 'desktop' has no member 'windows'",
        ),
    }
)

NEGATIVE.update(
    {
        "generic_inference": (
            "function first<T>(T[] items) returns T\nreturn items[0]\nend\nint[] none = []\nprint first()",
            "cannot infer type parameter T",
        ),
        "generic_arity": ("class Box<T>\nT value\nend\nBox<int, int> b = Box(1)", "expects 1 type argument"),
        "constructor_signature": (
            "class P\nstring name\nend\nfunction(int) returns P make = P",
            "constructor P does not match type",
        ),
        "closure_arity": (
            "var f = function(int x) => x\nprint f(1, 2)",
            "expects 1 argument",
        ),
        "closure_argument_type": (
            'var f = function(int x) => x\nprint f("a")',
            "cannot convert string to i64",
        ),
        "function_value_mismatch": (
            "function g(int a, int b) returns int\nreturn a\nend\nfunction(int) returns int f = g",
            "does not match type function(i64) returns i64",
        ),
        "not_callable": ("var n = 5\nprint n(1)", "is not callable"),
        "weak_primitive": ("weak<int> w = 1", "weak<T> requires a non-null reference type"),
        "filter_predicate": (
            "print [1, 2].filter(function(int x) => x)",
            "cannot convert i64 to bool",
        ),
        "lambda_default": ("var f = function(int x = 1) => x", "cannot have defaults"),
        "lambda_all_paths": (
            "var f = function(int x)\nif x > 0 then\nreturn 1\nend\nend",
            "must return on all paths",
        ),
        "weak_methods": (
            "class A\nint x = 0\nend\nweak<A> w = A()\nw.reset()",
            "weak references support get() and alive()",
        ),
        "web_handler_signature": (
            'import web\nweb.serve("/", function(int x) => "a", 0)',
            "HTTP handler must take no arguments or one web.request",
        ),
    }
)

MODULE_CASES = [
    (
        "module_lambdas",
        {
            "main.fw": "import tools\nprint tools.scaled([1, 2, 3])\nvar f = tools.adder(5)\nprint f(1)",
            "tools.fw": "private int factor = 3\npublic function scaled(int[] items) returns int[]\nreturn items.map(function(int x) => x * factor)\nend\npublic function adder(int n) returns function(int) returns int\nreturn function(int x) => x + n\nend",
        },
        "[3, 6, 9]\n6\n",
    ),
    (
        "local_alias",
        {
            "main.fw": "import helpers as h\nprint h.add(2,3)\nprint h.label",
            "helpers.fw": 'public const string label="module"\nprivate function secret() returns int\nreturn 3\nend\npublic function add(int a,int b) returns int\nreturn a+b+secret()\nend',
        },
        "8\nmodule\n",
    ),
    (
        "wildcard",
        {
            "main.fw": 'import utils.*\nprint twice(4)\nvar person=Person("Danny")\nprint person.name',
            "utils.fly": "class Person\nstring name\nend\nfunction twice(int n) returns int\nreturn n*2\nend",
        },
        "8\nDanny\n",
    ),
    (
        "diamond",
        {
            "main.fw": "import left\nimport right\nprint left.value()+right.value()",
            "left.fw": "import shared\nfunction value() returns int\nreturn shared.number\nend",
            "right.fw": "import shared\nfunction value() returns int\nreturn shared.number\nend",
            "shared.fw": 'print "once"\nint number=4',
        },
        "once\n8\n",
    ),
    (
        "components",
        {
            "main.fw": "import widgets as ui\nimport html.*\nprint <ui.Title>Hi</ui.Title>",
            "widgets.fw": "import html.*\ncomponent Title=<h1>{attr inner}</h1>",
        },
        "<h1>Hi</h1>\n",
    ),
    (
        "local_first",
        {
            "main.fw": "import math\nprint math.answer()",
            "math.fw": "function answer() returns int\nreturn 42\nend",
        },
        "42\n",
    ),
    (
        "dotted",
        {
            "main.fw": "import pkg.util as u\nprint u.answer()",
            "pkg/util.fw": "import sibling\nfunction answer() returns int\nreturn sibling.value\nend",
            "pkg/sibling.fly": "int value=9",
        },
        "9\n",
    ),
    (
        "module_exception",
        {
            "main.fw": "import service\ntry\nservice.fail()\ncatch service.Problem e\nprint e.message\nend",
            "service.fw": 'class Problem\nstring message\nend\nfunction fail() returns void\nthrow Problem("module error")\nend',
        },
        "module error\n",
    ),
]

MODULE_CASES.extend(
    [
        (
            "import_order",
            {
                "main.fw": 'import z\nimport a\nprint "main"',
                "z.fw": 'print "z"',
                "a.fw": 'print "a"',
            },
            "z\na\nmain\n",
        ),
        (
            "module_state",
            {
                "main.fw": 'import state as a\nimport state as b\na.count+=1\nprint b.count\nprint a.Counter("x").name',
                "state.fw": "int count=0\nclass Counter\nstring name\nend",
            },
            "1\nx\n",
        ),
    ]
)

MODULE_CASES.extend(
    [
        (
            "zero_parameter_import",
            {
                "main.fw": "import helper\nprint helper.answer()",
                "helper.fw": "private function value returns int\nreturn 42\nend\npublic function answer returns int\nreturn value()\nend",
            },
            "42\n",
        )
    ]
)

MODULE_CASES.extend(
    [
        (
            "generic_collection_class",
            {
                "main.fw": 'import users\narray<users.User> list=array<users.User>([users.User("A")])\nprint string(list)\nprint list[0].name',
                "users.fw": "class User\nstring name\nend",
            },
            '[User(name="A")]\nA\n',
        )
    ]
)

MODULE_ERRORS = [
    (
        "private",
        {
            "main.fw": "import secrets\nprint secrets.value",
            "secrets.fw": "private int value=1",
        },
        "is private",
    ),
    (
        "cycle",
        {"main.fw": "import a", "a.fw": "import b", "b.fw": "import a"},
        "import cycle:",
    ),
    (
        "collision",
        {
            "main.fw": "import a.*\nimport b.*",
            "a.fw": "int value=1",
            "b.fw": "int value=2",
        },
        "wildcard import collision",
    ),
    (
        "unknown_export",
        {"main.fw": "import a\nprint a.nope", "a.fw": "int value=1"},
        "no exported name",
    ),
    (
        "html_scope",
        {"main.fw": "import html.*\nimport a", "a.fw": "var x=<p>no import</p>"},
        "unknown tag",
    ),
    (
        "module_diagnostic",
        {
            "main.fw": "import broken",
            "broken.fw": 'function value() returns int\nreturn "bad"\nend',
        },
        "broken.fw:2:",
    ),
]


MODULE_CASES.append(
    (
        "constructor_references",
        {
            "factories.fw": """import desktop as gui
public const Window=(gui.window)
class Person
    string name="module default"
end
public const PersonClass=(Person)
""",
            "main.fw": """import factories as factory
var window=factory.Window()
print window
var constructor=factory.Window
print constructor("Module window")
var person=factory.Person()
print person.name
var person_constructor=factory.PersonClass
print person_constructor("Named").name
""",
        },
        'desktop.window("Flywheel")\ndesktop.window("Module window")\nmodule default\nNamed\n',
    )
)


POSITIVE.update(
    {
        "integer_division_any_numeric": (
            "print 7 // 2\nprint -7 // 2\nprint 7.5 // 2.0\nprint -7.9 // 2\nprint typeof(7.5 // 2)\nvar x = 10\nx //= 3\nprint x",
            "3\n-3\n3\n-3\ni64\n3\n",
        ),
        "unicode_string_length": (
            'print "héllo".length\nprint len("日本語")\nvar größe = 3\nprint größe',
            "5\n3\n3\n",
        ),
        "unsigned_literals": (
            "u64 big = 18446744073709551615\nprint big\nprint typeof(9223372036854775808)",
            "18446744073709551615\nu64\n",
        ),
        "line_continuation_and_chains": (
            "var total = 1 +\n    2 +\n    3\nvar other = 4 \\\n    + 5\nprint total, other\nprint 1 < 2 < 3, 3 > 2 > 2, 1 <= 1 < 2\nfunction side(int n) returns int\n    print \"eval\"\n    return n\nend\nprint 5 < side(1) < 9",
            "6 9\ntrue false true\neval\nfalse\n",
        ),
        "chars_and_string_methods": (
            """var s = "  Héllo, Wörld  "
print s.trim().upper()
print s.trim().lower()
print "a,b,,c".split(",")
print "one  two\\tthree".split()
print ", ".join(["x", "y"]), ["p", "q"].join("+")
print "banana".replace("an", "AN")
print "banana".find("na"), "banana".find_last("na"), "banana".count("a"), "héllo".find("l")
print "hello".starts_with("he"), "hello".ends_with("lo"), "hello".contains("ell")
print "héllo"[1], "héllo"[1..3], "héllo"[1..<3], "héllo".slice(-3)
print "abc".reverse(), "7".pad_start(3, "0"), "a\\nb\\r\\nc".lines()
char c = 'é'
print c, int(c), char(65), 'a' < 'b', 'x' in "xyz", "ab" + 'c'
print "héllo".chars()
for ch in "añ" do
    print ch
end""",
            'HÉLLO, WÖRLD\nhéllo, wörld\n["a", "b", "", "c"]\n["one", "two", "three"]\nx, y p+q\nbANANa\n2 4 3 2\ntrue true true\né éll él llo\ncba 007 ["a", "b", "c"]\né 233 A true true abc\n[\'h\', \'é\', \'l\', \'l\', \'o\']\na\nñ\n',
        ),
        "array_tools": (
            """int[] xs = [5, 3, 9, 1]
xs.sort()
print xs
xs.sort(function(int a, int b) => a > b)
print xs
xs.reverse()
print xs, xs.slice(1, 3), xs[0..1], xs.index_of(9), xs.contains(4)
xs.insert(0, 42)
print xs, xs.remove_at(0), xs.remove(9), xs
xs.extend([7, 8])
print xs.join("-"), xs.copy()
string[] names = ["bob", "Al", "carl"]
names.sort()
print names""",
            '[1, 3, 5, 9]\n[9, 5, 3, 1]\n[1, 3, 5, 9] [3, 5] [1, 3] 3 false\n[1, 3, 5] 42 true [1, 3, 5]\n1-3-5-7-8 [1, 3, 5, 7, 8]\n["Al", "bob", "carl"]\n',
        ),
        "math_tools": (
            """import math
print math.abs(-5), math.abs(-2.5), math.min(3, 1, 2), math.max(1.5, 2.5), math.clamp(15, 0, 10)
print math.floor(2.7), math.ceil(2.1), math.round(2.5), math.log(math.E), math.log(8, 2)
print math.is_nan(math.NAN), math.is_finite(math.INF), math.tan(0.0), math.atan2(0.0, 1.0)
math.seed(42)
var r = math.random_int(1, 6)
print 1 <= r <= 6, math.random() < 1.0""",
            "5 2.5 1 2.5 10\n2 3 3 1 3\ntrue false 0 0\ntrue true\n",
        ),
        "nullable_primitives": (
            """int? a = null
int? b = 5
print a, b, a ?? 10, b ?? 10
print a == null, b == 5, b != 5, a == b
if b != null then
    print b + 1
end
if a != null and a > 0 then
    print "no"
else
    print "a missing"
end
function first_even(int[] xs) returns int?
    for x in xs do
        if x % 2 == 0 then
            return x
        end
    end
    return null
end
print first_even([1, 3, 4]), first_even([1])
function describe(int? n) returns string
    if n == null then
        return "nothing"
    end
    return "got " + string(n * 2)
end
print describe(null), describe(4)
bool? flag = true
double? d = 2.5
print flag ?? false, d
map<string, int?> m = {"x": 1, "y": null}
print m, int(b), string(a)
class Opt
    int? value = null
end
var o = Opt()
print o.value ?? -1
o.value = 3
print o""",
            'null 5 10 5\ntrue true false false\n6\na missing\n4 null\nnothing got 8\ntrue 2.5\n{"x": 1, "y": null} 5 null\n-1\nOpt(value=3)\n',
        ),
        "classes_interfaces_enums_match": (
            """enum Color
    Red, Green
    Blue
end
interface Shape
    function area() returns double
    function label() returns string
end
class Base implements Shape
    string tag = "base"
    function area() returns double
        return 0.0
    end
    function label() returns string
        return "{inst tag}:{inst area()}"
    end
end
class Circle extends Base
    double r
    function init(double r)
        inst r = r
        inst tag = "circle"
    end
    function area() returns double
        return 3.0 * inst r * inst r
    end
end
class Square extends Base
    double side
    function label() returns string
        return "square " + super.label()
    end
    function area() returns double
        return inst side * inst side
    end
end
var c = Color.Green
print c, c.value, c.name, Color.values(), Color.from_name("Blue"), Color.from_name("Pink")
match c
    case Color.Red then
        print "red"
    case Green, Blue then
        print "cool"
end
Shape[] shapes = [Circle(1.0), Square("sq", 2.0), Base()]
for s in shapes do
    print s.label()
end
Base b = Circle(2.0)
print b.area(), b is Circle, b is Square, (b as Circle).r
Circle? none = b as Circle?
print none != null
if b is Circle then
    print "radius", b.r
end
function describe(Shape shape) returns string
    match shape
        case Circle k if k.r > 1.5 then
            return "big circle"
        case Circle k then
            return "circle"
        case Square sq then
            return "square {sq.side}"
        case Base then
            return "base"
    end
end
for s in shapes do
    print describe(s)
end
print describe(b)
for n in [0, 3, 7, 9] do
    match n
        case 0 then
            print "zero"
        case 1..5 then
            print "small"
        case 6, 7 then
            print "six or seven"
        else
            print "big"
    end
end
Result<int, string> res = Result.ok(3)
match res
    case ok v then
        print "ok", v
    case error e then
        print "error", e
end
map<Color, int> counts = {Color.Red: 1}
counts[Color.Blue] = 2
print counts
int? maybe = 4
match maybe
    case null then
        print "none"
    case int v then
        print "value", v
end""",
            "Green 1 Green [Red, Green, Blue] Blue null\ncool\ncircle:3\nsquare sq:4\nbase:0\n12 true false 2\ntrue\nradius 2\ncircle\nsquare 2\nbase\nbig circle\nzero\nsmall\nsix or seven\nbig\nok 3\n{Red: 1, Blue: 2}\nvalue 4\n",
        ),
    }
)

NEGATIVE.update(
    {
        "missing_interface_method": (
            "interface S\n    function f() returns int\nend\nclass A implements S\n    int x = 0\nend",
            "class A must implement S.f",
        ),
        "override_signature": (
            'class A\n    function f() returns int\n        return 1\n    end\nend\nclass B extends A\n    function f() returns string\n        return "x"\n    end\nend',
            "overrides A.f with a different signature",
        ),
        "match_exhaustive": (
            "enum E\n    X, Y\nend\nvar e = E.X\nmatch e\n    case E.X then\n        print 1\nend",
            "match is not exhaustive (missing Y)",
        ),
        "init_reads_inst": (
            "class A\n    int x\n    function init()\n        print inst x\n        inst x = 1\n    end\nend",
            "init uses 'inst' before assigning required field 'x'",
        ),
        "init_early_return": (
            "class A\n    int x\n    function init(bool b)\n        if b then\n            return\n        end\n        inst x = 1\n    end\nend",
            "init can return before assigning required field 'x'",
        ),
        "interface_construct": (
            "interface S\n    function f() returns int\nend\nvar s = S()",
            "interface S cannot be constructed",
        ),
        "inheritance_cycle": ("class A extends A\n    int x = 1\nend", "inheritance cycle"),
        "unrelated_cast": (
            "class A\n    int x = 1\nend\nclass B\n    int y = 1\nend\nvar a = A()\nvar b = a as B",
            "cannot cast A to unrelated type B",
        ),
        "nullable_arithmetic": ("int? a = 1\nprint a + 1", "requires non-null operands"),
        "nullable_implicit": ("int? a = 1\nint b = a", "cannot implicitly convert i64? to i64"),
        "char_literal_length": ("print 'ab'", "exactly one character"),
        "string_immutable": ('var s = "abc"\ns[0] = \'x\'', "strings are immutable"),
    }
)
TRAPS.update(
    {
        "nullable_cast": ("int? a = null\nprint int(a)", "cannot cast null"),
        "real_division_zero": ("double z = 0.0\nprint 1.0 // z", "division by zero"),
        "narrowed_global_changed": (
            "class A\n    int x = 1\nend\nclass B extends A\n    int y = 2\nend\nA a = B()\nfunction reset() returns void\n    a = A()\nend\nif a is B then\n    reset()\n    print a.y\nend",
            "no longer B",
        ),
    }
)


POSITIVE.update(
    {
        "first_class_methods_and_constructors": (
            """import desktop
class Counter
    int value = 0
    function add(int n) returns int
        inst value += n
        return inst value
    end
    function adder() returns function(int) returns int
        return inst add
    end
end
class Person
    string name
    int age = 0
end
var c = Counter()
var add = c.add
print add(2), add(3), c.value
print c.adder()(10), [1, 2, 3].map(c.add)
function(string) returns Person make = Person
function(string, int) returns Person full = Person
print make("Ann").name, full("Bo", 4)
var makers = [Person, Person]
print makers[0]("Cy", 1)
function(string) returns desktop.window win = desktop.window
var windows = [desktop.window]
print win("from function"), windows[0]("x", 10, 10)""",
            '2 5 5\n15 [16, 18, 21]\nAnn Person(name="Bo", age=4)\nPerson(name="Cy", age=1)\ndesktop.window("from function") desktop.window("x")\n',
        ),
        "mutable_captures": (
            """import testing
var base = testing.live_objects()
function counter() returns function() returns int
    int count = 0
    return function() returns int
        count += 1
        return count
    end
end
function local_effects() returns void
    var next = counter()
    print next(), next(), next()
    int total = 0
    string log = ""
    [1, 2, 3].each(function(int x)
        total += x
        log = log + string(x)
    end)
    print total, log
    var later = function() => total * 10
    total = 7
    print later()
    var fns = [function() => 0]
    fns.clear()
    for i in [1, 2] do
        fns.push(function() => i * 100)
    end
    print fns[0](), fns[1]()
    int depth = 0
    var outer = function()
        var inner = function()
            depth += 5
        end
        inner()
        inner()
    end
    outer()
    print depth
end
local_effects()
print testing.live_objects() == base""",
            "1 2 3\n6 123\n70\n100 200\n10\ntrue\n",
        ),
        "generics": (
            """class Box<T>
    T value
    function get() returns T
        return inst value
    end
    function with(T other) returns Box<T>
        return Box<T>(other)
    end
end
class Pair<A, B>
    A first
    B second
    function swap() returns Pair<B, A>
        return Pair(inst second, inst first)
    end
end
class Stack<T>
    T[] items
    function init()
        inst items = []
    end
    function push(T item)
        inst items.push(item)
    end
    function pop() returns T?
        if inst items.length == 0 then
            return null
        end
        return inst items.pop()
    end
end
function first<T>(T[] items) returns T
    return items[0]
end
function map_all<T, U>(T[] items, function(T) returns U f) returns U[]
    U[] out = []
    for item in items do
        out.push(f(item))
    end
    return out
end
var b = Box(5)
print b.get(), typeof(b), Box<string>("hi").with("there").get()
print first([3, 4]), first(["a"]), first<double>([1.5])
print map_all([1, 2, 3], function(int x) => "n{x}")
var p = Pair("one", 1)
print p.swap(), typeof(p.swap())
Stack<int> st = Stack()
st.push(1)
st.push(2)
print st.pop(), st.pop(), st.pop()
Box<int>[] boxes = [Box(1), Box(2)]
print boxes.map(function(Box<int> x) => x.get() * 10)
var getter = first<int>
print getter([9])""",
            '5 Box<i64> there\n3 a 1.5\n["n1", "n2", "n3"]\nPair<i64,string>(first=1, second="one") Pair<i64,string>\n2 1 null\n[10, 20]\n9\n',
        ),
    }
)

POSITIVE.update({"sets_hashing_iterators": ('import testing\nvar base = testing.live_objects()\nclass Point\n    int x\n    int y\n    function equals(Point other) returns bool\n        return inst x == other.x and inst y == other.y\n    end\n    function hash() returns int\n        return inst x * 31 + inst y\n    end\nend\nclass Countdown\n    int n\n    function next() returns int?\n        if inst n == 0 then\n            return null\n        end\n        inst n -= 1\n        return inst n + 1\n    end\nend\nclass Range\n    int stop\n    function iterator() returns Countdown\n        return Countdown(inst stop)\n    end\nend\nfunction check() returns void\n    set<int> s = {3, 1, 3, 2}\n    print s, s.length, 2 in s, 5 in s\n    s.add(5)\n    print s.remove(1), s.remove(9), s\n    var words = set(["a", "b", "a"])\n    print words, words.contains("b")\n    set<string> empty = {}\n    print empty.length\n    var other = set([2, 9])\n    print s.union(other), s.intersection(other), s.difference(other), set([2]).is_subset(s)\n    for x in s do\n        print "item", x\n    end\n    print Point(1, 2) == Point(1, 2), Point(1, 2) != Point(2, 1)\n    map<Point, string> names = {Point(1, 2): "a"}\n    names[Point(1, 2)] = "b"\n    names[Point(3, 4)] = "c"\n    print names.length, names[Point(1, 2)], Point(3, 4) in names, Point(9, 9) in names\n    print names.remove(Point(3, 4)), names.length\n    set<Point> points = {Point(0, 0), Point(0, 0)}\n    print points.length\n    map<[int, string], int> pairs = {}\n    pairs[[1, "a"]] = 10\n    pairs[[1, "a"]] += 1\n    print pairs, [1, "a"] in pairs, [1, "b"] in pairs\n    set<[int, int]> grid = {[0, 0], [0, 1], [0, 0]}\n    print grid.length\n    for n in Countdown(3) do\n        print "count", n\n    end\n    for n in Range(2) do\n        print "range", n\n    end\n    map<string, int> ages = {"ann": 3, "bo": 4}\n    for name, age in ages do\n        print name, age\n    end\n    for i, w in ["x", "y"] do\n        print i, w\n    end\n    print ages.values()\nend\ncheck()\nprint testing.live_objects() == base\n', '{3, 1, 2} 3 true false\ntrue false {3, 2, 5}\n{"a", "b"} true\n0\n{3, 2, 5, 9} {2} {3, 5} true\nitem 3\nitem 2\nitem 5\ntrue true\n2 b true false\ntrue 1\n1\n{(1, "a"): 11} true false\n2\ncount 3\ncount 2\ncount 1\nrange 2\nrange 1\nann 3\nbo 4\n0 x\n1 y\n[3, 4]\ntrue\n')})
NEGATIVE.update(
    {
        "map_key_without_hash": (
            "class P\n    int x = 0\nend\nmap<P, int> m = {}\nm[P()] = 1",
            "must define hash() returns int and equals",
        ),
        "iterator_not_nullable": (
            "class C\n    function next() returns int\n        return 1\n    end\nend\nfor x in C() do\nend",
            "next() must return a nullable value",
        ),
        "set_pairs": ("set<int> s = {1}\nfor i, x in s do\nend", "sets have no index"),
    }
)

POSITIVE.update(
    {
        "stdlib_fs_time_regex_json_io": ('import os\nimport fs\nimport time\nimport regex\nimport io\nimport json\nenum Role\n    Admin, User\nend\nclass Address\n    string city\n    int? zip = null\nend\nclass Person\n    string name\n    int age = 0\n    Address[] homes = []\n    Role role = Role.User\n    map<string, int> scores = {}\n    string? nickname = null\nend\nfs.mkdir("fsdemo/sub")\nfs.write("fsdemo/a.txt", "one\\n")\nfs.append("fsdemo/a.txt", "two\\n")\nprint fs.read("fsdemo/a.txt").lines(), fs.list("fsdemo"), fs.walk("fsdemo")\nprint fs.is_dir("fsdemo"), fs.is_file("fsdemo/a.txt"), fs.size("fsdemo/a.txt")\nprint fs.join("fsdemo", "sub", "x.json"), fs.basename("a/b.txt"), fs.extension("a/b.txt"), fs.stem("a/b.txt"), fs.dirname("a/b.txt")\nfs.copy("fsdemo/a.txt", "fsdemo/b.txt")\nfs.move("fsdemo/b.txt", "fsdemo/sub/c.txt")\nprint fs.walk("fsdemo")\nprint fs.remove_all("fsdemo"), fs.is_dir("fsdemo")\nprint time.format(0.0), time.format(86400.0 * 365.0, "%Y-%m-%d")\nvar d = time.date(1700000000.0)\nprint d.year, d.month, d.day, d.weekday\nprint time.from_date(2023, 11, 14, 22, 13, 20.0), time.parse("2001-02-03T04:05:06Z")\nprint time.now() > 1700000000.0\nprint regex.matches("[a-z]+", "abc"), regex.contains("\\\\d", "a1"), regex.find("\\\\d+", "ab12c345")\nprint regex.find_all("\\\\d+", "a1b22c333"), regex.groups("(\\\\w+)@(\\\\w+)", "me@host"), regex.groups("x", "y")\nprint regex.replace("(\\\\d+)", "a1b22", "<$1>"), regex.split(",\\\\s*", "a, b,c")\nvar j = json.parse("{{\\"items\\": [1, 2, {{\\"k\\": true}}], \\"name\\": \\"x\\"}}")\nprint j["items"][1], json.length(j["items"]), json.keys(j), j["items"][2]["k"]\nfor item in json.items(j["items"]) do\n    print json.kind(item)\nend\nvar p = Person("Ann", 30, [Address("Oslo", 1234)], Role.Admin, {"math": 9}, null)\nvar text = json.encode(p)\nprint text\nPerson back = json.decode(text)\nprint back\nprint Person.from_json("{{\\"name\\": \\"Bo\\", \\"homes\\": [{{\\"city\\": \\"Rome\\"}}]}}")\nprint json.decode<int[]>("[1, 2, 3]"), json.decode<map<string, double?>>("{{\\"a\\": 1.5, \\"b\\": null}}")\nprint json.encode([1, 2], 2)\nprint json.encode({"k": [true, false]})\nprint json.decode<set<int>>("[3, 3, 1]")\nprint os.args()\nio.write("no newline")\nio.write("\\n")\nvar f = io.open("io-demo.txt", "w")\nf.write("first\\nsecond\\n")\nf.close()\nvar g = io.open("io-demo.txt")\nprint g.read_line(), g.read_line(), g.read_line()\ng.close()\nprint io.open("io-demo.txt").lines()\nfs.remove("io-demo.txt")\n', '["one", "two"] ["a.txt", "sub"] ["a.txt", "sub"]\ntrue true 8\nfsdemo/sub/x.json b.txt .txt b a\n["a.txt", "sub", "sub/c.txt"]\n4 false\n1970-01-01T00:00:00Z 1971-01-01\n2023 11 14 2\n1700000000 981173106\ntrue\ntrue true 12\n["1", "22", "333"] ["me", "host"] null\na<1>b<22> ["a", "b", "c"]\n2 3 ["items", "name"] true\nnumber\nnumber\nobject\n{"name":"Ann","age":30,"homes":[{"city":"Oslo","zip":1234}],"role":"Admin","scores":{"math":9},"nickname":null}\nPerson(name="Ann", age=30, homes=[Address(city="Oslo", zip=1234)], role=Admin, scores={"math": 9}, nickname=null)\nPerson(name="Bo", age=0, homes=[Address(city="Rome", zip=null)], role=User, scores={}, nickname=null)\n[1, 2, 3] {"a": 1.5, "b": null}\n[\n  1,\n  2\n]\n{"k":[true,false]}\n{3, 1}\n[]\nno newline\nfirst second null\n["first", "second"]\n'),
        "stdlib_net": ('import net\nvar server = net.listen("127.0.0.1", 0)\nvar port = server.port()\nvar client = net.connect("localhost", port, 2.0)\nvar peer = server.accept()\nclient.send("hello\\nworld\\n")\nprint peer.receive_line(), peer.receive_line()\npeer.send("pong")\npeer.close()\nprint client.receive(), client.receive() == ""\nprint client.receive_line()\nclient.close()\nserver.close()\nprint net.resolve("localhost").length > 0\nvar v6 = net.listen("::1", 0)\nvar c6 = net.connect("::1", v6.port())\nvar p6 = v6.accept()\nc6.send("six")\nprint p6.receive(3), c6.address().starts_with("[::1]")\ntry\n    c6.close()\n    c6.send("x")\ncatch NetworkError e\n    print e.message\nend\n', 'hello world\npong true\nnull\ntrue\nsix true\nsocket is closed\n'),
    }
)
if os.name != "nt":
    POSITIVE["stdlib_processes"] = ('import os\nvar r = os.run(["sh", "-c", "echo out; echo err >&2; exit 3"])\nprint r.status, r.stdout.trim(), r.stderr.trim(), r.timed_out\nvar pr = os.process(["cat"])\npr.input = "fed through stdin"\nprint pr.run().stdout\nvar slow = os.process(["sleep", "5"])\nslow.timeout = 0.2\nvar sr = slow.run()\nprint sr.timed_out\nvar cw = os.process(["pwd"])\ncw.cwd = "/tmp"\ncw.env["FLY_X"] = "42"\nprint cw.run().stdout.trim()\nvar envp = os.process(["sh", "-c", "echo $FLY_X"])\nenvp.env["FLY_X"] = "42"\nprint envp.run().stdout.trim()\nprint os.pipeline([["printf", "b\\\\na\\\\nc\\\\n"], ["sort"], ["head", "-n", "2"]]).stdout.lines()\n', '3 out err false\nfed through stdin\ntrue\n/tmp\n42\n["a", "b"]\n')
NEGATIVE.update(
    {
        "json_decode_target": ('import json\nprint json.decode("[1]")', "needs a target type"),
        "json_decode_interface": (
            'import json\ninterface S\n    function f() returns int\nend\nS s = json.decode("{{}}")',
            "cannot decode S from JSON",
        ),
    }
)
TRAPS.update(
    {
        "regex_invalid": ('import regex\nprint regex.matches("(", "x")', "RegexError"),
        "json_missing_field": (
            'import json\nclass P\n    string name\nend\nprint P.from_json("{{}}")',
            "missing JSON field 'name'",
        ),
        "net_refused": ('import net\nvar l = net.listen("127.0.0.1", 0)\nvar p = l.port()\nl.close()\nnet.connect("127.0.0.1", p)', "NetworkError"),
    }
)

POSITIVE.update(
    {
        "threads_channels_mutex": ('import thread\nimport gc\nimport testing\nclass Node\n    string name\n    Node? next = null\nend\nfunction work(int n) returns int\n    int total = 0\n    for i in 0..<n do\n        total += i\n    end\n    return total\nend\nfunction check() returns void\n    var base = testing.live_objects()\n    thread.task<int>[] tasks = []\n    for i in 1..4 do\n        tasks.push(thread.spawn(function() => work(i * 1000)))\n    end\n    print tasks.map(function(thread.task<int> t) => t.join())\n    var failing = thread.spawn(function() returns int\n        throw RuntimeError("boom")\n    end)\n    try\n        failing.join()\n    catch RuntimeError e\n        print "caught", e.message\n    end\n    thread.channel<string> ch = thread.channel(2)\n    var producer = thread.spawn(function()\n        for w in ["a", "b", "c", "d"] do\n            ch.send(w)\n        end\n        ch.close()\n    end)\n    string got = ""\n    var item = ch.receive()\n    while item != null do\n        got = got + item\n        item = ch.receive()\n    end\n    producer.join()\n    print got, ch.try_receive()\n    var lock = thread.mutex()\n    int[] counter = [0]\n    thread.task<void>[] adders = []\n    for i in 0..<4 do\n        adders.push(thread.spawn(function()\n            for k in 0..<1000 do\n                lock.lock()\n                counter[0] += 1\n                lock.unlock()\n            end\n        end))\n    end\n    for a in adders do\n        a.join()\n    end\n    print counter[0], thread.cores() >= 1\n    tasks.clear()\n    adders.clear()\n    var a = Node("a")\n    var b = Node("b", a)\n    a.next = b\n    print testing.live_objects() > base\nend\ncheck()\nprint gc.collect() >= 2\nvar x = Node("x")\nx.next = x\n', "[499500, 1999000, 4498500, 7998000]\ncaught boom\nabcd null\n4000 true\ntrue\ntrue\n"),
        "cycle_collection": ('import gc\nimport testing\nimport html.*\nclass Node\n    string name\n    Node? next = null\n    function() returns string callback = function() => "none"\nend\nvar base = testing.live_objects()\nfunction churn() returns void\n    for i in 0..<1000 do\n        var a = Node("a")\n        var b = Node("b", a)\n        a.next = b\n        map<string, Node> registry = {}\n        registry["self"] = a\n        a.callback = function() returns string\n            return registry["self"].name\n        end\n    end\n    var page = <div><p>text</p></div>\n    var keep = Node("keep")\n    print keep.name\nend\nchurn()\nprint testing.live_objects() > base\nvar freed = gc.collect()\nprint freed > 0, testing.live_objects() == base\n', "keep\ntrue\ntrue true\n"),
    }
)
NEGATIVE.update(
    {
        "spawn_arguments": ("import thread\nthread.spawn(function(int x) => x)", "function taking no arguments"),
        "channel_type": ("import thread\nvar c = thread.channel()", "needs an element type"),
    }
)

POSITIVE.update({"html_selectors_families_svg": ('import html.*\nimport svg.*\nelements widgets\n    my-card: title, size\n    my-spacer void: height\n    my-any: *\nend\nvar page = <div id="root">\n    <ul class="list">\n        <li class="item">one</li>\n        <li class="item special">two</li>\n        <li class="item">three</li>\n        <li class="item">four</li>\n    </ul>\n    <p>after</p>\n    <span>x</span>\n    <input type="number" min={0} max={10} step={0.5} disabled={false} />\n    <progress value={3} max={10}></progress>\n    <svg viewBox="0 0 10 10" width={10}><circle cx="5" cy="5" r={4} fill="red" /></svg>\n    <my-card title="Hi" size={3}>content</my-card>\n    <my-spacer height="4" />\n    <my-any whatever="ok" />\n    <button onclick="alert(1)">js</button>\n    <details open={true}><summary>s</summary></details>\n</div>\nprint page.query_all("li:first-child").map(function(element e) => e.text_content)\nprint page.query_all("li:last-child, li:nth-child(2)").map(function(element e) => e.text_content)\nprint page.query_all("li:nth-child(odd)").length, page.query_all("li:nth-child(2n+3)").length, page.query_all("li:nth-last-child(1)")[0].text_content\nprint page.query_all("li:not(.special)").length, page.query_all("ul + p").length, page.query_all("ul ~ span").length, page.query_all("li.special + li")[0].text_content\nprint page.query_all("p:empty").length, page.query_all("li:only-child").length, page.query_all("div > ul > li:first-of-type")[0].text_content\nvar two = page.query(".special")\nif two != null then\n    var parent = two.parent\n    if parent != null then\n        print parent.tag\n    end\n    var next = two.next_sibling\n    if next != null then\n        print next.text_content, two.previous_sibling != null\n    end\n    var root = two.closest("div")\n    if root != null then\n        print root.id\n    end\n    print two.closest("section") == null, two.matches("ul > .special"), two.matches("li + li")\nend\nprint page.query("svg")\nprint page.query("my-card")\nprint page.query("input")\n', '["one"]\n["two", "four"]\n2 1 four\n3 1 1 three\n0 0 one\nul\nthree true\nroot\ntrue true true\n<svg viewBox="0 0 10 10" width="10"><circle cx="5" cy="5" r="4" fill="red"></circle></svg>\n<my-card title="Hi" size="3">content</my-card>\n<input type="number" min="0" max="10" step="0.5">\n')})
NEGATIVE.update(
    {
        "unused_component_tag": (
            "import html.*\ncomponent Broken = <dvi>{attr inner}</dvi>",
            "unknown tag 'dvi'",
        ),
        "unused_component_attribute": (
            'import html.*\ncomponent Broken = <p hrefx="x">{attr inner}</p>',
            "unknown attribute 'hrefx' on <p>",
        ),
        "family_attribute": (
            "elements w\n    my-card: title\nend\nvar x = <my-card size=\"1\" />",
            "unknown attribute 'size' on <my-card>",
        ),
        "event_handler_type": (
            "import html.*\nvar x = <button onclick={function(int n) => print(n)}>b</button>",
            "event attribute 'onclick' expects",
        ),
    }
)
TRAPS["selector_pseudo"] = ('import html.*\nprint <p />.matches("p:hover")', "unsupported pseudo-class")

POSITIVE.update(
    {
        "markup_sequences": (
            'import html.*\nstring[] names = ["a", "<b>"]\nelement? none = null\nprint <ul>{names.map(function(string n) => <li>{n}</li>)}{none}</ul>\nprint <p>{[1, 2]}</p>',
            "<ul><li>a</li><li>&lt;b&gt;</li></ul>\n<p>12</p>\n",
        ),
        "example_types": ('# Classes, interfaces, enums, generics, optional values, and pattern matching.\ninterface Shape\n    function area() returns double\n    function name() returns string\nend\n\nclass Circle implements Shape\n    double radius\n    function area() returns double\n        return 3.14159 * inst radius * inst radius\n    end\n    function name() returns string\n        return "circle"\n    end\nend\n\nclass Rectangle implements Shape\n    double width\n    double height\n    function area() returns double\n        return inst width * inst height\n    end\n    function name() returns string\n        return "rectangle"\n    end\nend\n\nclass Square extends Rectangle\n    function init(double side)\n        super.init(side, side)\n    end\n    function name() returns string\n        return "square"\n    end\nend\n\nenum Verdict\n    Small, Medium, Large\nend\n\nfunction judge(Shape shape) returns Verdict\n    match shape\n        case Square s if s.width > 3.0 then\n            return Verdict.Large\n        case Circle c if c.radius < 1.0 then\n            return Verdict.Small\n        else\n            return Verdict.Medium\n    end\nend\n\nclass Stack<T>\n    T[] items\n    function init()\n        inst items = []\n    end\n    function push(T item)\n        inst items.push(item)\n    end\n    function pop() returns T?\n        if inst items.length == 0 then\n            return null\n        end\n        return inst items.pop()\n    end\nend\n\nShape[] shapes = [Circle(0.5), Rectangle(2.0, 3.0), Square(4.0)]\nfor shape in shapes do\n    print shape.name(), shape.area(), judge(shape)\nend\n\nStack<string> names = Stack()\nnames.push("first")\nnames.push("second")\nvar top = names.pop()\nprint top ?? "empty", names.pop() ?? "empty", names.pop() ?? "empty"\n', "circle 0.7853975 Small\nrectangle 6 Medium\nsquare 16 Large\nsecond first empty\n"),
    }
)

class CompilerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="flywheel-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "case.fly"

    def command(self, *args):
        return subprocess.run(
            [COMPILER, *map(str, args)],
            cwd=self.root,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=45,
        )

    def test_native_programs(self):
        for name, (source, expected) in POSITIVE.items():
            for opt in ("-O0", "-O3"):
                with self.subTest(name=name, optimization=opt):
                    self.source.write_text(source, encoding="utf-8")
                    exe = self.root / ("program.exe" if os.name == "nt" else "program")
                    result = self.command("build", self.source, opt, "-o", exe)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    # Executing independently confirms there is no source interpreter dependency.
                    result = subprocess.run(
                        [str(exe)],
                        cwd=self.root,
                        check=False,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        timeout=15,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected)

    def test_input(self):
        for name, (source, stdin, expected) in INPUT_CASES.items():
            for opt in ("-O0", "-O3"):
                with self.subTest(name=name, optimization=opt):
                    self.source.write_text(source, encoding="utf-8")
                    exe = self.root / ("input.exe" if os.name == "nt" else "input")
                    result = self.command("build", self.source, opt, "-o", exe)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run(
                        [str(exe)],
                        check=False,
                        input=stdin,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        timeout=15,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected)

    def test_input_prompt_is_flushed(self):
        import queue
        import threading

        self.source.write_text('print input("Name: ")', encoding="utf-8")
        exe = self.root / ("prompt.exe" if os.name == "nt" else "prompt")
        result = self.command("build", self.source, "-O3", "-o", exe)
        self.assertEqual(result.returncode, 0, result.stderr)
        process = subprocess.Popen(
            [str(exe)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        prompts = queue.Queue()
        reader = threading.Thread(
            target=lambda: prompts.put(process.stdout.read(6)), daemon=True
        )
        reader.start()
        try:
            self.assertEqual(prompts.get(timeout=5), "Name: ")
            reader.join(timeout=5)
            stdout, stderr = process.communicate("Ada\n", timeout=5)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(stdout, "Ada\n")
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)
            reader.join(timeout=5)

    def test_process_streams(self):
        script = "import os, sys; print(sys.stdin.readline().strip()); print(os.environ['FW_PROCESS_TEST']); print('child error', file=sys.stderr)"
        source = (
            'import os\nprint "before"\nprint os.command(['
            + ", ".join(map(fly_string, [sys.executable, "-X", "utf8", "-c", script]))
            + "])"
        )
        self.source.write_text(source, encoding="utf-8")
        for opt in ("-O0", "-O3"):
            with self.subTest(optimization=opt):
                exe = self.root / ("process.exe" if os.name == "nt" else "process")
                result = self.command("build", self.source, opt, "-o", exe)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run(
                    [str(exe)],
                    check=False,
                    input="inherited input\n",
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    timeout=15,
                    env={**os.environ, "FW_PROCESS_TEST": "inherited env"},
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout, "before\ninherited input\ninherited env\n0\n"
                )
                self.assertEqual(result.stderr, "child error\n")

    @unittest.skipIf(os.name == "nt", "POSIX signal return codes")
    def test_process_signal(self):
        self.source.write_text(
            'import os\nprint os.command(["/bin/sh", "-c", "kill -TERM $$"])',
            encoding="utf-8",
        )
        result = self.command("run", self.source, "-O3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "-15\n")

    def test_desktop_failure_cleanup(self):
        self.source.write_text(DESKTOP_FAILURE, encoding="utf-8")
        exe = self.root / ("desktop.exe" if os.name == "nt" else "desktop")
        result = self.command("build", self.source, "-O3", "-o", exe)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(
            [str(exe)],
            cwd=self.root,
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
            env={
                **os.environ,
                "FLYWHEEL_DESKTOP_BACKEND": str(self.root / "missing-backend"),
            },
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "desktop error\ncleanup\n" * 2 + "true\n")

    def test_compile_errors(self):
        for name, (source, message) in NEGATIVE.items():
            with self.subTest(name=name):
                self.source.write_text(source, encoding="utf-8")
                result = self.command("check", self.source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertIn(": error:", result.stderr)

    def test_modules(self):
        cases = MODULE_CASES
        for name, files, expected in cases:
            for opt in ("-O0", "-O3"):
                with self.subTest(name=name, optimization=opt):
                    directory = self.root / (name + opt)
                    directory.mkdir()
                    for path, content in files.items():
                        target = directory / path
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_text(content, encoding="utf-8")
                    result = self.command("run", directory / "main.fw", opt)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected)

    def test_module_errors(self):
        for name, files, expected in MODULE_ERRORS:
            with self.subTest(name=name):
                directory = self.root / name
                directory.mkdir()
                for path, content in files.items():
                    target = directory / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(content, encoding="utf-8")
                result = self.command("check", directory / "main.fw")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)

    def test_arc_borrowing_ir(self):
        ir = self.root / "borrowing.ll"
        self.source.write_text(
            'class User\nstring name\nend\nvar user=User("Danny")\nprint user.name',
            encoding="utf-8",
        )
        result = self.command("check", self.source, "--emit-ir", ir)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("@fw_keep(", ir.read_text())
        self.source.write_text(
            'string text="before"\nfunction replace() returns string\ntext="after"\nreturn "!"\nend\nprint text+replace()',
            encoding="utf-8",
        )
        result = self.command("check", self.source, "--emit-ir", ir)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("@fw_keep(", ir.read_text())

    def test_statement_diagnostics(self):
        for source, unexpected in [
            ("var x = 1 2", "2"),
            ("print 1 2", "2"),
            ("var x = 1, 2", ","),
        ]:
            with self.subTest(source=source):
                self.source.write_text(source, encoding="utf-8")
                result = self.command("check", self.source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    f"expected end of statement, got '{unexpected}'", result.stderr
                )
                self.assertNotIn("nested calls", result.stderr)
        self.source.write_text(
            "ii function page() returns element\nend", encoding="utf-8"
        )
        result = self.command("check", self.source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpected keyword 'function'", result.stderr)
        self.assertNotIn("nested calls", result.stderr)

    def test_runtime_errors(self):
        for name, (source, message) in TRAPS.items():
            for opt in ("-O0", "-O3"):
                with self.subTest(name=name, optimization=opt):
                    self.source.write_text(source, encoding="utf-8")
                    result = self.command("run", self.source, opt)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(message, result.stderr)

    def test_http(self):
        import http.client
        import socket
        import time

        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        self.source.write_text(
            'import web\nimport html.*\nfunction page()\nreturn <><p>Hello &amp; HTTP</p></>\nend\nweb.serve "/", page, '
            + str(port),
            encoding="utf-8",
        )
        exe = self.root / ("server.exe" if os.name == "nt" else "server")
        result = self.command("build", self.source, "-O3", "-o", exe)
        self.assertEqual(result.returncode, 0, result.stderr)
        process = subprocess.Popen(
            [str(exe)], cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    connection = http.client.HTTPConnection(
                        "127.0.0.1", port, timeout=1
                    )
                    connection.request("GET", "/")
                    response = connection.getresponse()
                    break
                except OSError:
                    if process.poll() is not None:
                        self.fail(process.stderr.read().decode())
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.02)
            self.assertEqual(response.status, 200)
            self.assertEqual(
                response.getheader("Content-Type"), "text/html; charset=utf-8"
            )
            self.assertEqual(response.read(), b"<p>Hello &amp; HTTP</p>")
            connection.close()
            for method, path, status in [
                ("GET", "/missing", 404),
                ("POST", "/", 405),
                ("GET", "/?x=1", 200),
            ]:
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                connection.request(method, path)
                response = connection.getresponse()
                self.assertEqual(response.status, status)
                response.read()
                connection.close()
        finally:
            process.terminate()
            process.communicate(timeout=10)

    def start_url_server(self, source, opt):
        import queue
        import threading

        self.source.write_text(source, encoding="utf-8")
        exe = self.root / ("url-server.exe" if os.name == "nt" else "url-server")
        result = self.command("build", self.source, opt, "-o", exe)
        self.assertEqual(result.returncode, 0, result.stderr)
        process = subprocess.Popen(
            [str(exe)],
            cwd=self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )

        def stop():
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=10)
            reader.join(timeout=2)
            process.stdout.close()
            process.stderr.close()

        self.addCleanup(stop)
        lines = queue.Queue()

        def read_lines():
            for line in process.stdout:
                lines.put(line.rstrip("\n"))
            lines.put(None)

        reader = threading.Thread(target=read_lines, daemon=True)
        reader.start()
        return process, lines

    def test_web_keepalive_chunked_ipv6_tls(self):
        import http.client
        import shutil
        import socket
        import ssl

        tls = shutil.which("openssl") is not None
        if tls:
            made = subprocess.run(
                ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
                 "key.pem", "-out", "cert.pem", "-days", "2", "-subj", "/CN=localhost"],
                cwd=self.root, capture_output=True,
            )
            tls = made.returncode == 0
        source = """import web
function echo(web.request r) returns string
    return "{r.method} {r.body}"
end
function hello() returns string
    return "hi"
end
print web.serve("/", hello, 0)
print web.route("POST", "/echo", echo, 0, "::1")
"""
        if tls:
            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            free = probe.getsockname()[1]
            probe.close()
            source += f'print web.tls("cert.pem", "key.pem", {free})\nprint web.serve("/", hello, {free})\n'
        process, lines = self.start_url_server(source, "-O0")
        port = lambda url: int(url.split("://", 1)[1].rsplit(":", 1)[1].split("/")[0])
        plain = lines.get(timeout=30)
        v6 = lines.get(timeout=10)
        stalled = socket.create_connection(("127.0.0.1", port(plain)), timeout=5)
        self.addCleanup(stalled.close)
        stalled.send(b"GET / HTTP/1.1\r\n")
        connection = http.client.HTTPConnection("127.0.0.1", port(plain), timeout=5)
        self.addCleanup(connection.close)
        connection.request("GET", "/")
        response = connection.getresponse()
        self.assertEqual(response.read(), b"hi")
        self.assertEqual(response.getheader("Connection"), "keep-alive")
        first = connection.sock
        connection.request("GET", "/")
        self.assertEqual(connection.getresponse().read(), b"hi")
        self.assertIs(connection.sock, first)
        try:
            chunked = http.client.HTTPConnection("::1", port(v6), timeout=5)
        except OSError:
            chunked = None
        if chunked is not None:
            self.addCleanup(chunked.close)
            chunked.putrequest("POST", "/echo")
            chunked.putheader("Transfer-Encoding", "chunked")
            chunked.endheaders()
            for part in [b"hello ", b"chunked"]:
                chunked.send(b"%x\r\n%s\r\n" % (len(part), part))
            chunked.send(b"0\r\n\r\n")
            self.assertEqual(chunked.getresponse().read(), b"POST hello chunked")
        if tls:
            secure = lines.get(timeout=10)
            self.assertTrue(secure.startswith("https://127.0.0.1:"))
            self.assertEqual(lines.get(timeout=10), secure + "/")
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            https = http.client.HTTPSConnection("127.0.0.1", port(secure), timeout=5, context=context)
            self.addCleanup(https.close)
            for _ in range(2):
                https.request("GET", "/")
                self.assertEqual(https.getresponse().read(), b"hi")

    def test_tooling(self):
        tests = self.root / "suite"
        (tests / "nested").mkdir(parents=True)
        (tests / "test_ok.fw").write_text(
            "import testing\nfunction test_add() returns void\n    testing.assert_equal(1 + 1, 2)\nend\n"
            "function helper() returns void\n    testing.fail(\"not a test\")\nend\n",
            encoding="utf-8",
        )
        (tests / "nested" / "math_test.fw").write_text(
            "import testing\nfunction test_bad() returns void\n    testing.assert_equal(2 * 2, 5, \"product\")\nend\n"
            "function test_good() returns void\n    testing.assert(true)\nend\n",
            encoding="utf-8",
        )
        result = self.command("test", tests)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("  ok   test_add", result.stdout)
        self.assertIn("  FAIL test_bad: assertion failed: product: expected 5, got 4", result.stdout)
        self.assertIn("  ok   test_good", result.stdout)
        self.assertIn("2 test file(s), 1 failed", result.stdout)
        self.assertEqual(self.command("test", tests / "test_ok.fw").returncode, 0)

        messy = self.root / "messy.fw"
        messy.write_text(
            "class P\n  int x = 0\n      function get() returns int\nreturn inst x\n end\nend\n"
            "function main\nvar xs = [1,\n2]\nif true then\nprint xs\n  else\nprint 0\nend\n"
            "xs.each(function(int x)\nprint x\n    end)\nvar s = \"a  \n   b\"\nend   \n",
            encoding="utf-8",
        )
        expected = (
            "class P\n    int x = 0\n    function get() returns int\n        return inst x\n    end\nend\n"
            "function main\n    var xs = [1,\n        2]\n    if true then\n        print xs\n    else\n        print 0\n    end\n"
            "    xs.each(function(int x)\n        print x\n    end)\n    var s = \"a  \n   b\"\nend\n"
        )
        self.assertEqual(self.command("fmt", "--check", messy).returncode, 1)
        self.assertEqual(self.command("fmt", messy).returncode, 0)
        self.assertEqual(messy.read_text(encoding="utf-8"), expected)
        self.assertEqual(self.command("fmt", "--check", messy).returncode, 0)

        self.source.write_text("function f() returns int\n    return 1\nend\nprint f()", encoding="utf-8")
        result = self.command("check", "-g", self.source, "--emit-ir", self.root / "debug.ll")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("!DISubprogram(name: \"f\"", (self.root / "debug.ll").read_text())

        server = subprocess.Popen(
            [COMPILER, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=self.root
        )
        self.addCleanup(server.kill)

        def send(message):
            body = json.dumps(message).encode()
            server.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
            server.stdin.flush()

        def receive():
            length = 0
            while True:
                line = server.stdout.readline().strip()
                if not line:
                    break
                if line.startswith(b"Content-Length:"):
                    length = int(line.split(b":")[1])
            return json.loads(server.stdout.read(length))

        uri = self.source.resolve().as_uri()
        send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        self.assertTrue(receive()["result"]["capabilities"]["documentFormattingProvider"])
        send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
              "params": {"textDocument": {"uri": uri, "text": 'int x = "no"\n'}}})
        diagnostics = receive()["params"]["diagnostics"]
        self.assertEqual(diagnostics[0]["range"]["start"], {"line": 0, "character": 8})
        self.assertIn("cannot convert string", diagnostics[0]["message"])
        send({"jsonrpc": "2.0", "method": "textDocument/didChange",
              "params": {"textDocument": {"uri": uri}, "contentChanges": [{"text": "function f\nprint 1\nend\n"}]}})
        self.assertEqual(receive()["params"]["diagnostics"], [])
        send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting",
              "params": {"textDocument": {"uri": uri}, "options": {}}})
        self.assertEqual(receive()["result"][0]["newText"], "function f\n    print 1\nend\n")
        send({"jsonrpc": "2.0", "id": 3, "method": "shutdown"})
        receive()
        send({"jsonrpc": "2.0", "method": "exit"})
        self.assertEqual(server.wait(timeout=10), 0)

    def test_web_workers(self):
        import threading
        import time
        import urllib.request

        source = """import web
import time
function slow() returns string
    time.sleep(0.4)
    return "slow"
end
function boom() returns string
    throw RuntimeError("worker failure")
end
web.workers(4)
print web.serve("/slow", slow, 0)
print web.serve("/boom", boom, 0)
"""
        process, lines = self.start_url_server(source, "-O0")
        slow = lines.get(timeout=30)
        boom = lines.get(timeout=10)
        results = []
        threads = [
            threading.Thread(target=lambda: results.append(urllib.request.urlopen(slow, timeout=10).read()))
            for _ in range(3)
        ]
        start = time.monotonic()
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(results, [b"slow"] * 3)
        self.assertLess(time.monotonic() - start, 1.1)
        with self.assertRaises(Exception):
            urllib.request.urlopen(boom, timeout=10).read()
        self.assertEqual(process.wait(timeout=10), 1)
        self.assertIn("worker failure", process.stderr.read())

    def test_web_serve_returns_url(self):
        import http.client
        import queue
        import urllib.parse

        source = 'import web\nimport html.*\nfunction page returns element\n    return <p>{url}</p>\nend\nstring url = web.serve "/hello", page, 0\nprint url\nprint typeof(url)\n'
        for opt, route, expected_path in [
            ("-O0", "/hello", "/hello"),
            ("-O3", "/hello", "/hello"),
            ("-O3", "/café room", "/caf%C3%A9%20room"),
        ]:
            with self.subTest(optimization=opt, route=route):
                process, lines = self.start_url_server(
                    source.replace('"/hello"', '"' + route + '"'), opt
                )
                try:
                    url = lines.get(timeout=10)
                    self.assertIsNotNone(url)
                    self.assertEqual(lines.get(timeout=10), "string")
                except queue.Empty:
                    self.fail(
                        "serve did not return and print its URL before handling requests"
                    )
                address = urllib.parse.urlsplit(url)
                self.assertEqual(address.scheme, "http")
                self.assertEqual(address.hostname, "127.0.0.1")
                self.assertEqual(address.path, expected_path)
                self.assertGreater(address.port, 0)
                for _ in range(3):
                    connection = http.client.HTTPConnection(
                        address.hostname, address.port, timeout=2
                    )
                    connection.request("GET", address.path)
                    response = connection.getresponse()
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read().decode(), "<p>" + url + "</p>")
                    connection.close()
                process.terminate()
                process.wait(timeout=10)

    def test_web_serve_temporary_route_and_wait(self):
        import http.client
        import urllib.parse

        source = 'import web\nimport html.*\nfunction page returns element\n    try\n        web.wait()\n    catch RuntimeError error\n        return <p>recovered</p>\n    end\n    return <p>bad</p>\nend\nfunction start returns string\n    string route = "/temporary"\n    return web.serve(route, page, 0)\nend\nprint start()\nweb.wait()\n'
        process, lines = self.start_url_server(source, "-O3")
        url = lines.get(timeout=10)
        self.assertIsNotNone(url)
        address = urllib.parse.urlsplit(url)
        for _ in range(2):
            connection = http.client.HTTPConnection(
                address.hostname, address.port, timeout=2
            )
            connection.request("GET", address.path)
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b"<p>recovered</p>")
            connection.close()
        process.terminate()
        process.wait(timeout=10)

    def test_web_multiple_returned_urls(self):
        import http.client
        import urllib.parse

        source = 'import web\nimport html.*\nfunction first returns element\n    return <p>first</p>\nend\nfunction second returns element\n    return <p>second</p>\nend\nprint web.serve("/", first, 0)\nprint web.serve("/second", second, 0)\n'
        process, lines = self.start_url_server(source, "-O3")
        for expected in (b"<p>first</p>", b"<p>second</p>"):
            url = lines.get(timeout=10)
            self.assertIsNotNone(url)
            address = urllib.parse.urlsplit(url)
            connection = http.client.HTTPConnection(
                address.hostname, address.port, timeout=2
            )
            connection.request("GET", address.path)
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), expected)
            connection.close()
        process.terminate()
        process.wait(timeout=10)

    def test_web_routing_requests_and_static(self):
        import http.client
        import socket
        import urllib.parse

        public = self.root / "public"
        (public / "css").mkdir(parents=True)
        (public / "css" / "app.css").write_text("body{color:red}", encoding="utf-8")
        (public / "index.html").write_text("<p>index</p>", encoding="utf-8")
        (self.root / "secret.txt").write_text("secret", encoding="utf-8")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        source = f"""import web
import html.*

map<string, string> users = {{"1": "Ann", "2": "Bo"}}

function home returns element
    return <h1>Home</h1>
end
function user(web.request req) returns web.response
    var id = req.params["id"]
    if id in users then
        return web.html(<p>User {{users[id]}}</p>)
    end
    return web.text("no user " + id, 404)
end
function create(web.request req) returns web.response
    users[req.form["id"]] = req.form["name"]
    return web.redirect("/users/" + req.form["id"], 303)
end
web.serve("/", home, {port})
web.serve("/users/:id", user, {port})
web.route("POST", "/users", create, {port})
web.serve("/api/echo", function(web.request req) returns web.response
    var r = web.json("{{{{\\"q\\":\\"" + req.query["q"] + "\\",\\"agent\\":\\"" + req.headers["x-test"] + "\\"}}}}")
    r.headers["x-powered-by"] = "flywheel"
    return r
end, {port})
web.serve("/hello", function(web.request req) => "hi " + req.method, {port})
print web.static("/assets", "public", {port})
"""
        process, lines = self.start_url_server(source, "-O3")
        url = lines.get(timeout=10)
        self.assertEqual(url, f"http://127.0.0.1:{port}/assets/")

        def fetch(method, path, body=None, headers=None):
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            data = response.read()
            connection.close()
            return response, data

        response, data = fetch("GET", "/")
        self.assertEqual((response.status, data), (200, b"<h1>Home</h1>"))
        response, data = fetch("GET", "/users/2")
        self.assertEqual((response.status, data), (200, b"<p>User Bo</p>"))
        self.assertEqual(response.getheader("Content-Type"), "text/html; charset=utf-8")
        response, data = fetch("GET", "/users/9")
        self.assertEqual((response.status, data), (404, b"no user 9"))
        self.assertEqual(response.getheader("Content-Type"), "text/plain; charset=utf-8")

        form = urllib.parse.urlencode({"id": "3", "name": "Cy Dé"})
        response, data = fetch(
            "POST",
            "/users",
            form,
            {"Content-Type": "application/x-www-form-urlencoded"},
        )
        self.assertEqual(response.status, 303)
        self.assertEqual(response.getheader("Location"), "/users/3")
        response, data = fetch("GET", "/users/3")
        self.assertEqual(data.decode(), "<p>User Cy Dé</p>")

        response, data = fetch("GET", "/api/echo?q=a%20b", headers={"X-Test": "yes"})
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Type"), "application/json")
        self.assertEqual(response.getheader("x-powered-by"), "flywheel")
        self.assertEqual(data, b'{"q":"a b","agent":"yes"}')

        response, data = fetch("DELETE", "/users")
        self.assertEqual(response.status, 405)
        self.assertEqual(response.getheader("Allow"), "POST")
        response, data = fetch("GET", "/hello")
        self.assertEqual(data, b"hi GET")
        response, data = fetch("HEAD", "/hello")
        self.assertEqual((response.status, data), (200, b""))
        response, data = fetch("PUT", "/hello")
        self.assertEqual(response.status, 405)

        response, data = fetch("GET", "/assets/css/app.css")
        self.assertEqual((response.status, data), (200, b"body{color:red}"))
        self.assertEqual(response.getheader("Content-Type"), "text/css; charset=utf-8")
        response, data = fetch("GET", "/assets/")
        self.assertEqual(data, b"<p>index</p>")
        for escape in ("/assets/../secret.txt", "/assets/%2e%2e/secret.txt"):
            response, data = fetch("GET", escape)
            self.assertEqual(response.status, 404, escape)
        response, data = fetch("GET", "/missing")
        self.assertEqual(response.status, 404)

    def test_web_startup_failure(self):
        import socket

        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            port = occupied.getsockname()[1]
            self.source.write_text(
                'import web\nimport html.*\nfunction page returns element\nreturn <p>unused</p>\nend\nprint web.serve("/",page,'
                + str(port)
                + ")",
                encoding="utf-8",
            )
            result = self.command("run", self.source)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertRegex(result.stderr, "cannot (bind|listen)")

    def test_canonical_demo(self):
        import http.client
        import json
        import socket
        import time

        original = (
            Path(__file__).resolve().parents[1] / "examples/demo.fly"
        ).read_text(encoding="utf-8")
        for user in (None, {"name": "Alice & Bob", "age": 31}):
            with self.subTest(user=user):
                with socket.socket() as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                self.source.write_text(
                    original.replace(
                        "const int PORT = 8000", f"const int PORT = {port}"
                    ),
                    encoding="utf-8",
                )
                if user:
                    (self.root / "user.json").write_text(
                        json.dumps(user), encoding="utf-8"
                    )
                exe = self.root / ("demo.exe" if os.name == "nt" else "demo")
                result = self.command("build", self.source, "-O3", "-o", exe)
                self.assertEqual(result.returncode, 0, result.stderr)
                process = subprocess.Popen(
                    [str(exe)],
                    cwd=self.root,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                try:
                    deadline = time.monotonic() + 10
                    while True:
                        try:
                            connection = http.client.HTTPConnection(
                                "127.0.0.1", port, timeout=1
                            )
                            connection.request("GET", "/")
                            response = connection.getresponse()
                            break
                        except OSError:
                            if process.poll() is not None:
                                self.fail(process.stderr.read().decode())
                            if time.monotonic() > deadline:
                                raise
                            time.sleep(0.02)
                    body = response.read().decode("utf-8")
                    connection.close()
                    self.assertEqual(response.status, 200)
                    self.assertIn("<title>Flywheel Demo</title>", body)
                    self.assertIn(
                        "<h1>Alice &amp; Bob</h1>" if user else "<h1>danny</h1>", body
                    )
                    self.assertIn("Age: 31" if user else "Age: 15", body)
                    self.assertIn("Welcome!", body)
                    self.assertIn("This is a title card", body)
                finally:
                    process.terminate()
                    process.communicate(timeout=10)

    def test_ir_and_check(self):
        self.source.write_text('print "native"', encoding="utf-8")
        ir = self.root / "program.ll"
        result = self.command("check", self.source, "--emit-ir", ir)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("define i32 @main(i32 %0, ptr %1)", ir.read_text())
        self.assertFalse((self.root / "case").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
