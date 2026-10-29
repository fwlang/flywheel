// Blocking terminal input and native child processes. Included by runtime.cpp.
#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace {
void processArgument(const std::string &text) {
    if (text.find('\0') != std::string::npos)
        fail("process arguments cannot contain NUL bytes", "ProcessError");
}
#ifdef _WIN32
std::wstring processWide(const std::string &text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        fail("process argument is too long", "ProcessError");
    auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
    if (!n)
        fail("process arguments must be valid UTF-8", "ProcessError");
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), n);
    return result;
}
// Quote one argv item according to the Windows C runtime parsing convention.
std::wstring processQuote(const std::wstring &text) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"')
            result += L'\\';
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
struct ProcessHandles {
    PROCESS_INFORMATION info{};
    ~ProcessHandles() {
        if (info.hThread)
            CloseHandle(info.hThread);
        if (info.hProcess)
            CloseHandle(info.hProcess);
    }
};
int64_t runProcess(const std::vector<std::string> &arguments, bool shell = false) {
    std::wstring command;
    for (auto &arg : arguments) {
        if (!command.empty())
            command += L' ';
        command += processQuote(processWide(arg));
    }
    if (shell) {
        // cmd.exe /s /c strips the outer pair of quotes around the shell text.
        command = processQuote(processWide(arguments[0])) + L" /d /s /c \"" +
                  processWide(arguments.back()) + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    ProcessHandles child;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &startup, &child.info))
        fail("cannot start process (Windows error " + std::to_string(GetLastError()) + ")",
             "ProcessError");
    if (WaitForSingleObject(child.info.hProcess, INFINITE) != WAIT_OBJECT_0)
        fail("cannot wait for process", "ProcessError");
    DWORD status = 0;
    if (!GetExitCodeProcess(child.info.hProcess, &status))
        fail("cannot read process exit status", "ProcessError");
    return status;
}
#else
int64_t runProcess(const std::vector<std::string> &arguments, bool = false) {
    std::vector<char *> argv;
    for (auto &arg : arguments)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t child;
    int error = posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error)
        fail("cannot start process '" + arguments[0] + "': " + std::strerror(error),
             "ProcessError");
    int status;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR)
            fail("cannot wait for process: " + std::string(std::strerror(errno)), "ProcessError");
    }
    return WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
}
#endif
void flushProcessOutput() {
    std::cout.flush();
    std::cerr.flush();
}
} // namespace

extern "C" Object *fw_input(Frame *frame, Object *prompt) {
    return nativeBoundary([&] {
        std::cout << prompt->text << std::flush;
        if (!std::cout)
            fail("cannot write input prompt", "InputError");
        std::string line;
        if (!std::getline(std::cin, line))
            fail(std::cin.eof() ? "end of input" : "cannot read input", "InputError");
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return string(frame, std::move(line));
    });
}
extern "C" int64_t fw_shell(Object *command) {
    return nativeBoundary([&] {
        processArgument(command->text);
        flushProcessOutput();
#ifdef _WIN32
        wchar_t directory[MAX_PATH + 1];
        auto n = GetSystemDirectoryW(directory, MAX_PATH + 1);
        if (!n || n > MAX_PATH)
            fail("cannot locate Windows command shell", "ProcessError");
        auto path = std::filesystem::path(directory) / L"cmd.exe";
        auto utf8 = path.u8string();
        return runProcess(
            {std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size()), command->text},
            true);
#else
        return runProcess({"/bin/sh", "-c", command->text});
#endif
    });
}
extern "C" int64_t fw_command(Object *command) {
    return nativeBoundary([&] {
        if (command->slots.empty())
            fail("os.command requires a nonempty argument array", "ProcessError");
        std::vector<std::string> arguments;
        for (auto &slot : command->slots) {
            auto &text = slot.object->text;
            processArgument(text);
            arguments.push_back(text);
        }
        if (arguments[0].empty())
            fail("os.command requires a nonempty executable name", "ProcessError");
        flushProcessOutput();
        return runProcess(arguments);
    });
}
