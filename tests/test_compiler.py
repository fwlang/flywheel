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
            'import html.*\nvar root=<><p id="one" class="card primary" data-state="ready now">A</p><section><p class="card" data-state="ready">B</p></section></>\nprint len(root.query_all("p.card.primary, #one"))\nprint len(root.query_all("[data-state]"))\nprint len(root.query_all("p[data-state~=ready]"))\nprint root.query("section > p.card").text_content\nprint root.query("#one").matches("P.card#one")\ntry\nroot.query("p:first-child")\ncatch SelectorError error\nprint "unsupported"\nend\n',
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
    print testing.live_objects()==base+1
    window.load(<p>Replacement</p>)
    print testing.live_objects()==base+1
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
    var window=gui.window
    print typeof(window)
    window.load(<h1>Hello, World!</h1>)
    var original=window
    window=gui.Window
    print window==original
    var Window=(gui.window)
    print testing.live_objects()==base+2
    print Window
    var explicit=Window("Custom",480,320)
    print explicit
    var default=Window
    print default
    var alias=(Window)
    var other=alias()
    other.load(<><p>Fragment</p></>)
    gui.window typed=gui.window
    print typed
    var grouped=((gui.Window))
    print grouped
    var called=(gui.window)()
    print called
    print bool((gui.window))
    (gui.window)
end
check()
print testing.live_objects()==base""",
            'desktop.window\nfalse\ntrue\n<class desktop.window>\ndesktop.window("Custom")\ndesktop.window("Flywheel")\ndesktop.window("Flywheel")\n<class desktop.window>\ndesktop.window("Flywheel")\ntrue\ntrue\n',
        ),
        "bare_class_constructor_references": (
            """import testing
class Person
    string name="Default"
end
function answer returns int
    return 42
end
var person=Person
print person.name
var Class=(Person)
print Class
var explicit=Class("Custom")
print explicit.name
var defaults=Class
print defaults.name
var reference=answer
print reference()
print (answer)
print((1+2)*3)""",
            "Default\n<class Person>\nCustom\nDefault\n42\n<function answer>\n9\n",
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
    var counter=Counter
    counter.increment
    (counter.increment)
    ((counter.increment))
    print counter.value
    hello
    (hello)
    var callback=hello
    (callback)
    (desktop.window)
    var Window=(desktop.window)
    (Window)
end
check()
print testing.live_objects()==base""",
            "3\ncalled\ncalled\ncalled\ntrue\n",
        ),
    }
)

DESKTOP_FAILURE = """import desktop
import html.*
import testing
var base=testing.live_objects()
function attempt returns void
    var window=desktop.window
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
    "attribute_type": ("import html.*\nvar t = <p style={3}>x</p>", "requires string"),
    "attribute_unknown": (
        'import html.*\nvar t = <p bogus="x" />',
        "unknown attribute",
    ),
    "import_collision": ("import fs\nimport math as fs", "duplicate import"),
    "unicode": ('print "\\u{D800}"', "Unicode"),
    "unsupported_module": ("import net", "not implemented"),
    "unsupported_constructor": (
        "class C\nfunction init() returns void\nend\nend",
        "not implemented",
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
            "constructor reference",
        ),
        "constructor_reference_method": (
            "import desktop\nvar Window=(desktop.window)\nWindow.run()",
            "has no instance methods",
        ),
        "constructor_reference_array": (
            "import desktop\nvar references=[(desktop.window)]",
            "constructor references cannot be stored",
        ),
        "constructor_reference_cast": (
            "import desktop\nprint int((desktop.window))",
            "unsupported cast source",
        ),
        "bare_constructor_missing_required": (
            "class Person\nstring name\nend\nvar person=Person",
            "missing required constructor field",
        ),
        "desktop_member_diagnostic": (
            "import desktop\nvar value=desktop.windows",
            "module 'desktop' has no member 'windows'",
        ),
    }
)

MODULE_CASES = [
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
var window=factory.Window
print window
var constructor=(factory.Window)
print constructor("Module window")
var person=factory.Person
print person.name
var person_constructor=(factory.PersonClass)
print person_constructor("Named").name
""",
        },
        'desktop.window("Flywheel")\ndesktop.window("Module window")\nmodule default\nNamed\n',
    )
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
        self.assertIn("define i32 @main()", ir.read_text())
        self.assertFalse((self.root / "case").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
