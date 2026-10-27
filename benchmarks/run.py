#!/usr/bin/env python3
"""Reproducible native/CPython comparisons. Never include compilation in execution samples."""

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True)


def distribution(values):
    ordered = sorted(values)
    return {
        "samples_ms": values,
        "min_ms": min(values),
        "median_ms": statistics.median(values),
        "p90_ms": ordered[min(len(ordered) - 1, int(len(ordered) * 0.9))],
        "max_ms": max(values),
    }


def workloads(render_count):
    yield (
        "startup",
        'print "hello"',
        '#include <iostream>\nint main(){std::cout<<"hello\\n";}',
        'print("hello")',
    )
    yield (
        "numeric",
        """int total = 0
int i = 0
while i < 300000 do
    total += (i % 97) * 3
    i += 1
end
print total""",
        """#include <iostream>
#include <cstdint>
int main(){int64_t total=0;for(int64_t i=0;i<300000;++i)total+=(i%97)*3;std::cout<<total<<'\\n';}""",
        """total=0
for i in range(300000): total+=(i%97)*3
print(total)""",
    )
    initial = ",".join(str(i) for i in range(1000))
    yield (
        "array",
        """int[] values = ["""
        + initial
        + """]
int total = 0
int pass = 0
while pass < 200 do
    int i = 0
    while i < len(values) do
        values[i] += 1
        total += values[i]
        i += 1
    end
    pass += 1
end
print total""",
        """#include <iostream>
#include <vector>
#include <cstdint>
int main(){std::vector<int64_t> values;for(int i=0;i<1000;++i)values.push_back(i);int64_t total=0;for(int pass=0;pass<200;++pass)for(auto& v:values){++v;total+=v;}std::cout<<total<<'\\n';}""",
        """values=list(range(1000))
total=0
for _ in range(200):
    for i in range(len(values)):
        values[i]+=1
        total+=values[i]
print(total)""",
    )
    yield (
        "strings",
        """string text = ""
int i = 0
while i < 5000 do
    text = "{text}x{i}"
    i += 1
end
print len(text)""",
        """#include <iostream>
#include <string>
int main(){std::string text;for(int i=0;i<5000;++i)text=text+"x"+std::to_string(i);std::cout<<text.size()<<'\\n';}""",
        """text=""
for i in range(5000): text=f"{text}x{i}"
print(len(text))""",
    )
    yield (
        "markup",
        """import html.*
component Card = <p>{attr index}</p>
int total = 0
int i = 0
while i < """
        + str(render_count)
        + """ do
    total += len(html.render(<Card index={i} />))
    i += 1
end
print total""",
        """#include <iostream>
#include <string>
#include <memory>
struct Element {std::string tag,text;std::string render(){return "<"+tag+">"+text+"</"+tag+">";}};
int main(){int64_t total=0;for(int i=0;i<"""
        + str(render_count)
        + """;++i){auto node=std::make_shared<Element>(Element{"p",std::to_string(i)});total+=node->render().size();}std::cout<<total<<'\\n';}""",
        """class Element:
    def __init__(self,tag,text): self.tag,self.text=tag,text
    def render(self): return f"<{self.tag}>{self.text}</{self.tag}>"
total=0
for i in range("""
        + str(render_count)
        + """):
    node=Element("p",str(i))
    total+=len(node.render())
print(total)""",
    )
    yield (
        "allocations",
        """class Item
    string name
    int value
end
int i = 0
int total = 0
while i < 20000 do
    var item = Item("item{i}", i)
    total += item.value + len(item.name)
    i += 1
end
print total""",
        """#include <iostream>
#include <string>
#include <memory>
struct Item{std::string name;int64_t value;};
int main(){int64_t total=0;for(int64_t i=0;i<20000;++i){auto item=std::make_shared<Item>(Item{"item"+std::to_string(i),i});total+=item->value+item->name.size();}std::cout<<total<<'\\n';}""",
        """class Item:
    def __init__(self,name,value): self.name,self.value=name,value
total=0
for i in range(20000):
    item=Item(f"item{i}",i)
    total+=item.value+len(item.name)
print(total)""",
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="build/flywheel")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--render-count", type=int, default=100000)
    parser.add_argument("--output", default="benchmarks/results.json")
    args = parser.parse_args()
    if args.iterations < 2 or args.render_count < 1:
        parser.error("use at least two iterations and a positive render count")
    compiler = str(Path(args.compiler).resolve())
    cxx = shutil.which(args.cxx)
    if not cxx:
        parser.error("C++ compiler not found")
    cpu = platform.processor()
    if not cpu and Path("/proc/cpuinfo").exists():
        cpu = next(
            (
                line.split(":", 1)[1].strip()
                for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")
            ),
            "unknown",
        )
    cache = Path(compiler).parent / "CMakeCache.txt"
    runtime_build_type = (
        next(
            (
                line.split("=", 1)[1]
                for line in cache.read_text().splitlines()
                if line.startswith("CMAKE_BUILD_TYPE:STRING=")
            ),
            "unknown",
        )
        if cache.exists()
        else "unknown"
    )
    report = {
        "platform": platform.platform(),
        "cpu": cpu,
        "logical_cpus": os.cpu_count(),
        "runtime_build_type": runtime_build_type,
        "python": sys.version,
        "flywheel": subprocess.check_output([compiler, "--version"], text=True).strip(),
        "cxx": subprocess.check_output([cxx, "--version"], text=True).splitlines()[0],
        "clock": "perf_counter_ns; elapsed wall time including process launch",
        "iterations": args.iterations,
        "warmups": 2,
        "render_count": args.render_count,
        "runtime": "archive from the selected Flywheel compiler build; record its CMake build type separately",
        "results": {},
    }
    with tempfile.TemporaryDirectory(prefix="flywheel-bench-") as directory:
        root = Path(directory)
        for name, fly, cpp, py in workloads(args.render_count):
            limits = {
                "numeric": 300000,
                "array": 200,
                "strings": 5000,
                "markup": args.render_count,
                "allocations": 20000,
            }
            if name in limits:
                limit = limits[name]
                os.environ["FW_BENCH_LIMIT"] = str(limit)
                fly = (
                    'import os\nint limit = int(os.env("FW_BENCH_LIMIT"))\n'
                    + fly.replace(
                        f"while i < {limit} do", "while i < limit do"
                    ).replace("while pass < 200 do", "while pass < limit do")
                )
                cpp = "#include <cstdlib>\n#include <string>\n" + cpp.replace(
                    "int main(){",
                    'int main(){const auto limit=std::stoll(std::getenv("FW_BENCH_LIMIT"));',
                ).replace(f"i<{limit};", "i<limit;").replace("pass<200;", "pass<limit;")
                py = (
                    'import os\nlimit=int(os.environ["FW_BENCH_LIMIT"])\n'
                    + py.replace(f"range({limit})", "range(limit)")
                )
            (root / "main.fly").write_text(fly, encoding="utf-8")
            (root / "main.cpp").write_text(cpp, encoding="utf-8")
            (root / "main.py").write_text(py, encoding="utf-8")
            commands = {}
            compilations = {}
            for opt in (0, 3):
                for language in ("flywheel", "cpp"):
                    exe = root / (
                        f"{language}-{opt}" + (".exe" if os.name == "nt" else "")
                    )
                    command = (
                        [
                            compiler,
                            "build",
                            str(root / "main.fly"),
                            f"-O{opt}",
                            "-o",
                            str(exe),
                        ]
                        if language == "flywheel"
                        else [
                            cxx,
                            "-std=c++20",
                            f"-O{opt}",
                            str(root / "main.cpp"),
                            "-o",
                            str(exe),
                        ]
                    )
                    start = time.perf_counter_ns()
                    run(command, root)
                    key = f"{language}-O{opt}"
                    compilations[key] = {
                        "ms": (time.perf_counter_ns() - start) / 1e6,
                        "command": command,
                    }
                    commands[key] = [str(exe)]
            commands["cpython"] = [sys.executable, str(root / "main.py")]
            outputs = {
                key: run(command, root).stdout for key, command in commands.items()
            }
            if len(set(outputs.values())) != 1:
                raise RuntimeError(f"{name}: non-equivalent outputs: {outputs}")
            results = {}
            for key, command in commands.items():
                for _ in range(2):
                    run(command, root)
                samples = []
                for _ in range(args.iterations):
                    start = time.perf_counter_ns()
                    run(command, root)
                    samples.append((time.perf_counter_ns() - start) / 1e6)
                results[key] = distribution(samples)
                # RSS is measured separately so GNU time does not affect execution samples.
                if sys.platform.startswith("linux") and Path("/usr/bin/time").exists():
                    rssfile = root / "rss.txt"
                    run(
                        ["/usr/bin/time", "-f", "%M", "-o", str(rssfile), *command],
                        root,
                    )
                    results[key]["peak_rss_kib"] = int(rssfile.read_text().strip())
            report["results"][name] = {
                "compilation": compilations,
                "execution": results,
                "stdout": next(iter(outputs.values())),
                "runtime_limit": limits.get(name),
            }
            print(
                name,
                {key: round(value["median_ms"], 3) for key, value in results.items()},
                flush=True,
            )
    Path(args.output).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
