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
| startup | 0.819 | 0.862 | 0.852 | 0.832 | 8.884 |
| numeric | 1.463 | 1.153 | 1.152 | 0.944 | 28.913 |
| array | 3.980 | 3.967 | 1.162 | 0.921 | 29.458 |
| strings | 4.369 | 3.872 | 4.701 | 2.673 | 10.645 |
| markup | 21.253 | 20.583 | 78.719 | 4.841 | 36.934 |
| allocations | 4.699 | 4.906 | 7.445 | 1.068 | 14.111 |

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

This checkpoint includes local modules, typed error propagation, tagged collection values, recursive formatting/casts, HTML tree tools, and managed-object lifetime metrics. Borrowed-read and scope-release optimizations are verified by ownership tests and IR checks. Overall timings include these additional language/runtime checks; they are not an isolated before/after measurement of ARC alone.
