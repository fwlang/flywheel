# Runtime and ownership

LLVM values hold primitive numbers, booleans, chars, and enum values directly. Immutable UTF-8 strings, dynamic arrays, tuples, maps, sets, class instances, Results, boxed optional primitives, JSON documents, errors, closures, markup nodes, and native resources (files, sockets, threads) are heap objects with atomic reference counts. Object slots, map keys, and element children retain references. Assigning a collection or instance aliases it; mutations are visible through both bindings.

Generated functions establish an ownership frame. Reference locals and parameters have initialized owning roots. Stores retain the new reference before releasing the previous value. Lexical scope exit clears local roots, including exits through exceptions, break, and continue. Temporary references remain alive through their statement and are drained at safe boundaries. A conservative analysis skips temporary retains for reads in expressions containing only known non-mutating operations.

`for` evaluates the iterable once. Array iteration captures the initial length; map and set iteration uses a key snapshot. Return values and pending errors have protected roots while `finally` runs.

Native C++ failures are caught at runtime C ABI boundaries and recorded in a thread-local typed error channel. Generated code checks the channel after fallible native and generated calls. C++ exceptions do not unwind through generated LLVM functions. Uncaught errors report to stderr and return exit status 1.

## Cycle collection

Reference counting frees most objects immediately. Unreachable reference cycles are found by a synchronous cycle collector (trial deletion, as in Bacon and Rajan). Containers whose count is decremented without reaching zero become *possible roots*; the collector subtracts internal references among them and frees groups that nothing outside the group references. References held by variables, frames, and native owners (tasks, channels, web routes, windows) count as outside references, so the collector is conservative.

Collection runs automatically at statement boundaries once enough possible roots accumulate, after the program body finishes (so cycles held by globals are reclaimed at exit), and on demand with `gc.collect()`, which returns the number of objects freed. `gc.pending()` reports the current number of possible roots. Collection only runs while no spawned threads or web workers are active; it resumes when they finish. `weak<T>` remains available for back links that should not keep their target alive.

`weak<T>` holds a non-owning reference to a class instance, element, or other managed reference type. Assigning a `T` (or calling `weak(x)`) creates one; `get()` returns `T?` and `alive()` reports whether the target still exists. Each referenced object owns a shared cell that is cleared when the object is destroyed, under a lock, and `get()` only takes a reference while the target's count is above zero, so weak reads are safe across threads.

## Closures and captures

Closures are ARC-managed objects holding a code pointer and their captured values in ordinary slots. A named function used as a value is wrapped in a capture-free closure; a bound method (`obj.method`) captures its receiver. Locals that are both captured by a lambda and assigned live in a shared one-slot cell, so assignments are visible to the declaring scope and every closure; other captures are copied.

## Threads

`thread.spawn` runs a closure on a native thread with its own ownership frames and error channel; see [concurrency](concurrency.md). Reference counts and the runtime's internal tables are thread-safe. While spawned threads run, each array, map, set, field, and global operation takes a lock chosen by the object's address (skipped when only one thread runs); sequences of operations still need `thread.mutex` or `thread.channel`. See [shared state](concurrency.md#shared-state) for the exact guarantees.

## Markup, processes, and windows

Markup literals are constructed by compiled native calls; `html.parse` builds the same trees from HTML text at run time. Fragments preserve their sequence of children and serialize without a wrapper. Children keep a weak link to their most recent parent for `.parent`, siblings, and `closest`. Function-valued event attributes are stored on elements as retained closures.

Managed slots carry a compact value-kind tag for recursive formatting, JSON encoding, and checked collection conversion. Classes include compiler-generated field-name/type identity metadata; each class also has a unique identity used by dynamic dispatch, `is`, and `match`.

Child processes run synchronously. `os.run` uses `fork`/`execvp` with pipes and `poll` on POSIX and `CreateProcessW` with pipes and reader threads on Windows. Desktop windows keep their loaded tree and re-render it after callbacks; native GUI dependencies are separate from the core runtime and loaded only by `run`.

`testing.live_objects()` reports the current number of live managed objects, enabling lifetime regression checks. AddressSanitizer/UBSan tests exercise standalone programs, imported projects, threads, and cycle collection.
