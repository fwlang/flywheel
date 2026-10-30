// Stack overflow is reported rather than left as a bare segfault. Each Flywheel thread records its
// stack bounds and gets an alternate signal stack; the fault handler checks whether the faulting
// address lies in the guard region just below the stack. Other faults keep their default action.
#ifdef _WIN32
namespace {
LONG CALLBACK stackOverflowHandler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    static const char message[] =
        "Flywheel runtime error: stack overflow (recursion too deep or unbounded)\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof message - 1, &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 1);
    return EXCEPTION_CONTINUE_SEARCH;
}
void guardStack(bool = false) {
    // Leave room on the overflowed stack for the handler to run.
    thread_local bool guarded = false;
    if (guarded)
        return;
    guarded = true;
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
    static std::once_flag installed;
    std::call_once(installed, [] { AddVectoredExceptionHandler(1, stackOverflowHandler); });
}
} // namespace
#else
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
namespace {
thread_local uintptr_t stackLow = 0, stackHigh = 0;
// Faults this far below the stack's end count as overflow (a large frame can skip the guard page).
constexpr uintptr_t guardReach = 1024 * 1024;
void stackFault(int signal, siginfo_t *info, void *) {
    auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    if (stackLow && address < stackHigh && address + guardReach >= stackLow) {
        static const char message[] =
            "Flywheel runtime error: stack overflow (recursion too deep or unbounded)\n";
        std::fflush(stdout);
        [[maybe_unused]] auto ignored = write(2, message, sizeof message - 1);
        _exit(1);
    }
    // Not an overflow: restore the default action; the faulting instruction runs again.
    struct sigaction fallback {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
}
struct AltStack {
    // Left uninitialized, so its pages are not touched until a fault handler runs on them.
    static constexpr size_t size = 64 * 1024;
    std::unique_ptr<char[]> memory{new char[size]};
    AltStack() {
        stack_t s{};
        s.ss_sp = memory.get();
        s.ss_size = size;
        sigaltstack(&s, nullptr);
    }
    ~AltStack() {
        stack_t s{};
        s.ss_flags = SS_DISABLE;
        sigaltstack(&s, nullptr);
    }
};
// `main` is true on the main thread, whose bounds come from the stack rlimit: asking pthread for
// them reads /proc/self/maps, which costs more than the rest of program startup.
void guardStack(bool main = false) {
    thread_local std::unique_ptr<AltStack> alternate;
    if (alternate)
        return;
    rlimit limit{};
    if (main && getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
        // The stack ends a few pages above this frame (arguments and environment).
        char here;
        stackHigh = reinterpret_cast<uintptr_t>(&here) + 64 * 1024;
        stackLow = reinterpret_cast<uintptr_t>(&here) - static_cast<uintptr_t>(limit.rlim_cur);
    } else {
#ifdef __APPLE__
        stackHigh = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
        stackLow = stackHigh - pthread_get_stacksize_np(pthread_self());
#else
        pthread_attr_t attributes;
        if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
            void *address = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attributes, &address, &size) == 0) {
                stackLow = reinterpret_cast<uintptr_t>(address);
                stackHigh = stackLow + size;
            }
            pthread_attr_destroy(&attributes);
        }
#endif
    }
    alternate = std::make_unique<AltStack>();
    static std::once_flag installed;
    std::call_once(installed, [] {
        struct sigaction action {};
        action.sa_sigaction = stackFault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGBUS, &action, nullptr);
    });
}
} // namespace
#endif
