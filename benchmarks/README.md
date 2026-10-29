# Benchmark checkpoint

Reproduce with a **Release** compiler/runtime build:

```sh
python3 benchmarks/run.py --compiler build-llvm21/flywheel --iterations 7 \
  --output benchmarks/linux-llvm21.json
```

The script compiles each workload separately at `-O0` and `-O3`, validates identical stdout across Flywheel, C++, and CPython, performs two warmups, and records seven individual elapsed samples. Compilation/link time is recorded separately and excluded from execution samples. Loop limits come from an environment variable parsed at runtime to prevent compile-time precomputation. Checksums/lengths are printed to keep results observable.

Checked environment: Linux-7.2.11-zen1-1-zen-x86_64-with-glibc2.44; AMD Ryzen 5 7600X 6-Core Processor; 12 logical CPUs; Flywheel 0.1.0 (LLVM 21.1.8); c++ (GCC) 16.2.1 20260810; Python 3.14.8. Runtime archive build type: Release.

## Measured medians (milliseconds)

| Workload | Flywheel O0 | Flywheel O3 | C++ O0 | C++ O3 | CPython |
|---|---:|---:|---:|---:|---:|
| startup | 0.874 | 0.896 | 0.778 | 0.788 | 8.590 |
| numeric | 1.532 | 1.062 | 1.148 | 0.995 | 27.711 |
| array | 4.264 | 3.941 | 1.010 | 0.913 | 28.304 |
| strings | 3.871 | 5.089 | 4.386 | 3.028 | 9.985 |
| markup | 25.241 | 24.906 | 82.300 | 4.758 | 34.179 |
| allocations | 5.632 | 5.248 | 6.901 | 1.302 | 13.275 |

The markup workload renders 100,000 components, the array workload mutates/traverses 1,000 elements for 200 passes, and allocation tests construct 20,000 class instances. The string workload builds 5,000 interpolated segments. Numeric throughput runs 300,000 checked arithmetic iterations.

## Interpretation and limits

- Measurements include process launch, native/Python runtime startup, and output collection. Sub-millisecond and short numeric results are dominated by launch overhead and scheduler noise; use longer workloads for throughput conclusions.
- The C++ baselines use C++20 and the indicated optimization flags, with explicit 64-bit numeric values where relevant. Inputs remain in range, so equivalent C++ operations do not invoke signed-overflow undefined behavior.
- Flywheel optimization flags apply to generated LLVM IR. The runtime archive is compiled once in Release mode for both O0/O3 program variants; no cross-module runtime LTO is enabled.
- C++ and Python implement the same observable workloads and output. Their implementations and object lifetimes differ, and each optimizer may eliminate allocations. Those differences are part of the comparison, not a claim of identical generated machine work.
- GNU time peak RSS is measured in a separate execution on Linux and reported in KiB in the JSON. Allocation counts and retained/peak heap size are not yet instrumented; peak RSS includes allocator and runtime overhead.
- Strings, array access, markup allocation, and ARC remain optimization targets. These data do not establish universal superiority over C++ or CPython.
- The stored JSON includes raw samples, min/median/p90/max, build commands, compile times, runtime limits, output checksums, and memory measurements. These are local measurements, not cross-platform performance claims.

See [linux-llvm21.json](linux-llvm21.json) for complete results.

Compared with the previous checkpoint, allocation-heavy workloads (markup, allocations) are roughly 10–15% slower. Measured separately, the cost is the cycle collector's possible-root bookkeeping (a few nanoseconds per container released while still referenced) and larger object headers (class identity, collector state, parent links). Rarely used per-object state lives in a separately allocated block, and cross-thread locks are taken only while other threads run. Array and numeric workloads are unchanged within noise. Short workloads such as `strings` vary by a millisecond or more between runs.

This checkpoint includes local modules, typed error propagation, tagged collection values, recursive formatting/casts, HTML tree tools, and managed-object lifetime metrics. Borrowed-read and scope-release optimizations are verified by ownership tests and IR checks. Overall timings include these additional language/runtime checks; they are not an isolated before/after measurement of ARC alone.
