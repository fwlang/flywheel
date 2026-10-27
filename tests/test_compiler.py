"""Black-box tests: build and execute native artifacts, with independent expected results."""

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

NEGATIVE = {
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
        "exceptions": ("try\nend", "not implemented"),
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

    def test_compile_errors(self):
        for name, (source, message) in NEGATIVE.items():
            with self.subTest(name=name):
                self.source.write_text(source, encoding="utf-8")
                result = self.command("check", self.source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertIn(": error:", result.stderr)

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
            'import web\nimport html.*\nfunction page() returns element\nreturn <p>Hello &amp; HTTP</p>\nend\nweb.serve "/", page, '
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
