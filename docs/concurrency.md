# Concurrency

The `thread` module runs Flywheel code in parallel on native threads. Each thread has its own ownership frames and error channel; reference counting is atomic and the runtime's own tables are synchronized.

```flywheel
import thread

function work(int n) returns int
    int total = 0
    for i in 0..<n do
        total += i
    end
    return total
end

thread.task<int>[] tasks = []
for i in 1..4 do
    tasks.push(thread.spawn(function() => work(i * 100000)))
end
print tasks.map(function(thread.task<int> t) => t.join())
```

| API | Behavior |
|---|---|
| `thread.spawn(function() returns T)` | Start the function on a new thread; returns `thread.task<T>` (`thread.task<void>` for no result) |
| `task.join()` | Wait for the task; returns its result, or re-raises the error that ended it |
| `task.done()` | Whether the task has finished |
| `thread.channel<T>(capacity = 0)` | A queue between threads; 0 is unbounded, otherwise `send` waits while full |
| `channel.send(x)` | Enqueue a value; raises `RuntimeError` after `close` |
| `channel.receive()` | `T?`: the next value, waiting for one; null once the channel is closed and empty |
| `channel.try_receive()` | `T?`: the next value, or null immediately when none is waiting |
| `channel.close()` | Stop accepting values and wake waiting receivers |
| `thread.mutex()` | A lock with `lock()`, `unlock()`, and `try_lock()` (bool) |
| `thread.cores()` | The number of hardware threads |

```flywheel
import thread
thread.channel<string> lines = thread.channel(16)
var producer = thread.spawn(function()
    for word in ["a", "b", "c"] do
        lines.send(word)
    end
    lines.close()
end)
var line = lines.receive()
while line != null do
    print line
    line = lines.receive()
end
producer.join()
```

A task that is never joined is joined when its last reference is released. A task's error stays with the task until `join` re-raises it. Values sent through a channel are shared references, not copies.

## Shared state

While spawned threads run, each operation on an array, map, set, class instance, global, or captured variable is atomic: `xs.push(x)` from several threads keeps every value, `xs.pop()` never returns the same item twice, and a value read into a variable or passed to a function stays alive even if another thread replaces it. Formatting, `json.encode`, casts, and copies read a snapshot of each collection. When no other thread is running these locks are skipped.

Only single operations are atomic. A sequence such as `counter[0] += 1` (a read then a write), or `if xs.length > 0 then xs.pop()`, can interleave with other threads, so combine steps with a mutex or pass values through channels:

```flywheel
import thread
var lock = thread.mutex()
int[] counter = [0]
thread.task<void>[] workers = []
for i in 0..<4 do
    workers.push(thread.spawn(function()
        for k in 0..<1000 do
            lock.lock()
            counter[0] += 1
            lock.unlock()
        end
    end))
end
for w in workers do
    w.join()
end
print counter[0]   # 4000
```

A value read directly as part of a simple expression, such as the argument in `print xs[0]`, is kept alive by the reading thread while other threads run, so replacing that slot from another thread cannot free it while it is in use. HTML elements are not locked, so build and change a markup tree on one thread.

To find missing synchronization, build or run with `--race`: the program stops with a report when two threads access the same collection, instance, captured variable, or global without a mutex, channel, or join ordering them. See [race detection](tooling.md#race-detection).

`desktop` windows and `web.wait` must run on the main thread. The cycle collector pauses while spawned threads or web workers are running. There is no `async`/`await`; tasks serve as futures.

## Web handlers

`web.workers(n)` runs HTTP handlers on a pool of `n` threads instead of the serving thread. Connections are still multiplexed by the serving thread, and each connection answers its requests in order. Handlers then run concurrently, so they must follow the shared-state rules above. An uncaught handler error stops the server and propagates from the serving thread as usual.
