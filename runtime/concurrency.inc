// Cycle collection and the thread module. Included by runtime.cpp.
#include <condition_variable>
#include <deque>
#include <unordered_set>

namespace {
// Strong references an object holds: its slots and map/set keys.
template <class F> void eachChild(Object *p, F &&visit) {
    for (auto &slot : p->slots)
        if (slot.kind == Slot::Reference && slot.object)
            visit(slot.object);
    for (auto &slot : p->keys)
        if (slot.kind == Slot::Reference && slot.object)
            visit(slot.object);
    for (auto &event : p->eventList())
        if (event.handler.object)
            visit(event.handler.object);
}
enum : uint8_t { gcBlack = 0, gcGray = 1, gcWhite = 2 };
// Synchronous trial deletion over the possible roots (Bacon and Rajan). References from
// outside the object graph (variables, frames, native owners) keep trial counts above zero,
// so only unreachable cycles are freed. Must run while no other thread executes Flywheel code.
int64_t collectCycles() {
    auto &c = candidates();
    std::vector<Object *> roots;
    {
        std::lock_guard<std::mutex> lock(c.lock);
        for (auto p = c.head; p; p = p->nextCandidate)
            roots.push_back(p);
        for (auto p : roots) {
            p->buffered.store(false);
            p->previousCandidate = p->nextCandidate = nullptr;
        }
        c.head = nullptr;
        c.count = 0;
    }
    std::vector<Object *> stack, touched;
    auto gray = [&](Object *p) {
        if (p->gcColor == gcGray)
            return;
        p->gcColor = gcGray;
        p->gcCount = static_cast<int64_t>(p->refs.load());
        touched.push_back(p);
        stack.push_back(p);
        while (!stack.empty()) {
            auto s = stack.back();
            stack.pop_back();
            eachChild(s, [&](Object *child) {
                if (child->gcColor != gcGray) {
                    child->gcColor = gcGray;
                    child->gcCount = static_cast<int64_t>(child->refs.load());
                    touched.push_back(child);
                    stack.push_back(child);
                }
                --child->gcCount;
            });
        }
    };
    auto black = [&](Object *p) {
        p->gcColor = gcBlack;
        stack.push_back(p);
        while (!stack.empty()) {
            auto s = stack.back();
            stack.pop_back();
            eachChild(s, [&](Object *child) {
                ++child->gcCount;
                if (child->gcColor != gcBlack) {
                    child->gcColor = gcBlack;
                    stack.push_back(child);
                }
            });
        }
    };
    for (auto p : roots)
        gray(p);
    for (auto root : roots) {
        if (root->gcColor != gcGray)
            continue;
        std::vector<Object *> pending{root};
        while (!pending.empty()) {
            auto s = pending.back();
            pending.pop_back();
            if (s->gcColor != gcGray)
                continue;
            if (s->gcCount > 0)
                black(s);
            else {
                s->gcColor = gcWhite;
                eachChild(s, [&](Object *child) { pending.push_back(child); });
            }
        }
    }
    std::vector<Object *> garbage;
    for (auto root : roots) {
        if (root->gcColor != gcWhite)
            continue;
        std::vector<Object *> pending{root};
        root->gcColor = gcBlack;
        while (!pending.empty()) {
            auto s = pending.back();
            pending.pop_back();
            garbage.push_back(s);
            eachChild(s, [&](Object *child) {
                if (child->gcColor == gcWhite) {
                    child->gcColor = gcBlack;
                    pending.push_back(child);
                }
            });
        }
    }
    for (auto p : touched)
        p->gcColor = gcBlack;
    // Break edges inside the garbage without releasing, then free it. Releasing the remaining
    // edges to live objects follows the ordinary path.
    std::unordered_set<Object *> dead(garbage.begin(), garbage.end());
    for (auto p : garbage) {
        for (auto *slots : {&p->slots, &p->keys})
            for (auto &slot : *slots)
                if (slot.kind == Slot::Reference && dead.count(slot.object))
                    slot.object = nullptr;
        if (p->extra)
            for (auto &event : p->extra->events)
                if (dead.count(event.handler.object))
                event.handler.object = nullptr;
    }
    for (auto p : garbage) {
        if (p->buffered.exchange(false)) {
            std::lock_guard<std::mutex> lock(c.lock);
            unlinkCandidate(c, p);
        }
        delete p;
    }
    c.threshold = std::max<size_t>(20000, c.count.load() * 2);
    return static_cast<int64_t>(garbage.size());
}

// thread module -------------------------------------------------------------------------------
using TaskRunner = void (*)(Object *closure, Object *result);
struct TaskResource : NativeResource {
    std::thread worker;
    Object *closure = nullptr, *result = nullptr, *error = nullptr;
    std::string errorType;
    std::atomic<bool> done{false};
    bool joined = false;
    ~TaskResource() override {
        if (worker.joinable())
            worker.join();
        release(closure);
        release(result);
        release(error);
    }
};
struct MutexResource : NativeResource {
    std::mutex mutex;
};
struct ChannelResource : NativeResource {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Slot> items;
    size_t capacity = 0; // 0 is unbounded
    bool closed = false;
};
template <class T> T &resourceOf(Object *p, const char *type) {
    auto resource = p && p->kind == Object::Resource ? dynamic_cast<T *>(p->nativeResource())
                                                     : nullptr;
    if (!resource)
        fail(std::string("expected ") + type, "RuntimeError");
    return *resource;
}
Object *resourceObject(Frame *frame, const char *type, std::unique_ptr<NativeResource> resource) {
    auto object = managed(frame, Object::Resource);
    object->text = type;
    object->more().resource = std::move(resource);
    return object;
}
} // namespace

extern "C" {
int64_t fw_gc_collect() {
    return nativeBoundary([&]() -> int64_t {
        if (activeThreads.load() != 0)
            return 0;
        return collectCycles();
    });
}
int64_t fw_gc_pending() {
    return static_cast<int64_t>(candidates().count.load());
}
// Runs `runner(closure, result)` on a new thread. Errors raised there are kept and re-raised by
// join.
Object *fw_thread_spawn(Frame *frame, Object *closure, TaskRunner runner) {
    return nativeBoundary([&] {
        auto task = std::make_unique<TaskResource>();
        auto state = task.get();
        retain(closure);
        state->closure = closure;
        state->result = new Object(Object::Box);
        state->result->slots.resize(1);
        auto object = resourceObject(frame, "thread.task", std::move(task));
        ++activeThreads;
        try {
            state->worker = std::thread([state, runner] {
                runner(state->closure, state->result);
                if (pendingError.value) {
                    state->errorType = pendingError.type;
                    state->error = pendingError.value;
                    retain(state->error);
                    pendingError.clear();
                }
                state->done = true;
                --activeThreads;
            });
        } catch (...) {
            --activeThreads;
            throw;
        }
        return object;
    });
}
// Waits for the task and returns its result box, or re-raises the task's error.
Object *fw_thread_join(Frame *frame, Object *taskObject) {
    return nativeBoundary([&]() -> Object * {
        auto &task = resourceOf<TaskResource>(taskObject, "thread.task");
        if (task.worker.joinable()) {
            if (task.worker.get_id() == std::this_thread::get_id())
                fail("a task cannot join itself", "RuntimeError");
            task.worker.join();
        }
        if (task.error) {
            pendingError.set(task.errorType, task.error);
            return nullptr;
        }
        retain(task.result);
        return own(frame, task.result);
    });
}
int64_t fw_thread_done(Object *taskObject) {
    return nativeBoundary(
        [&]() -> int64_t { return resourceOf<TaskResource>(taskObject, "thread.task").done.load(); });
}
int64_t fw_thread_cores() {
    return std::max<int64_t>(1, static_cast<int64_t>(std::thread::hardware_concurrency()));
}
Object *fw_thread_mutex(Frame *frame) {
    return nativeBoundary(
        [&] { return resourceObject(frame, "thread.mutex", std::make_unique<MutexResource>()); });
}
// 0 lock, 1 unlock, 2 try_lock
int64_t fw_mutex_operation(Object *mutexObject, int64_t operation) {
    return nativeBoundary([&]() -> int64_t {
        auto &m = resourceOf<MutexResource>(mutexObject, "thread.mutex").mutex;
        if (operation == 0)
            m.lock();
        else if (operation == 1)
            m.unlock();
        else
            return m.try_lock();
        return 1;
    });
}
Object *fw_channel_new(Frame *frame, int64_t capacity) {
    return nativeBoundary([&] {
        if (capacity < 0)
            fail("channel capacity must be non-negative", "RuntimeError");
        auto channel = std::make_unique<ChannelResource>();
        channel->capacity = static_cast<size_t>(capacity);
        return resourceObject(frame, "thread.channel", std::move(channel));
    });
}
// Sends the value in slot 0 of `box`, waiting while a bounded channel is full.
void fw_channel_send(Object *channelObject, Object *box) {
    nativeBoundary([&] {
        auto &channel = resourceOf<ChannelResource>(channelObject, "thread.channel");
        std::unique_lock<std::mutex> lock(channel.mutex);
        channel.changed.wait(lock, [&] {
            return channel.closed || !channel.capacity || channel.items.size() < channel.capacity;
        });
        if (channel.closed)
            fail("send on a closed channel", "RuntimeError");
        channel.items.push_back(box->slots.at(0));
        channel.changed.notify_all();
    });
}
// Returns a one-slot box holding the next value; null when the channel is closed and drained,
// or (wait == 0) when nothing is available.
Object *fw_channel_receive(Frame *frame, Object *channelObject, int64_t wait) {
    return nativeBoundary([&]() -> Object * {
        auto &channel = resourceOf<ChannelResource>(channelObject, "thread.channel");
        std::unique_lock<std::mutex> lock(channel.mutex);
        if (wait)
            channel.changed.wait(lock, [&] { return channel.closed || !channel.items.empty(); });
        if (channel.items.empty())
            return nullptr;
        auto box = managed(frame, Object::Box);
        box->slots.push_back(std::move(channel.items.front()));
        channel.items.pop_front();
        channel.changed.notify_all();
        return box;
    });
}
void fw_channel_close(Object *channelObject) {
    nativeBoundary([&] {
        auto &channel = resourceOf<ChannelResource>(channelObject, "thread.channel");
        std::lock_guard<std::mutex> lock(channel.mutex);
        channel.closed = true;
        channel.changed.notify_all();
    });
}
}
