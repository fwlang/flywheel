#include "flywheel/frontend.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#ifdef FW_JIT
#include <cerrno>
#include <cstdio>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/MemoryBuffer.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace fs = std::filesystem;
namespace {
int execute(const std::string &executable, const std::vector<std::string> &args) {
    std::vector<llvm::StringRef> refs;
    refs.emplace_back(executable);
    for (auto &a : args)
        refs.emplace_back(a);
    std::string error;
    int result = llvm::sys::ExecuteAndWait(executable, refs, std::nullopt, {}, 0, 0, &error);
    if (!error.empty())
        std::cerr << error << '\n';
    return result < 0 ? 1 : result;
}
struct Temp {
    fs::path path;
    Temp() {
        for (int i = 0; i < 100; ++i) {
            auto name =
                "flywheel-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(i);
            std::error_code ec;
            auto p = fs::temp_directory_path() / name;
            if (fs::create_directory(p, ec)) {
                path = p;
                return;
            }
        }
        throw std::runtime_error("cannot create temporary directory");
    }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
fs::path runtimePath(const char *executable) {
    // Installed binaries locate the archive relative to the executable; build-tree binaries use
    // CMake's path.
    auto found = llvm::sys::findProgramByName(executable);
    auto binary = fs::absolute(found ? fs::path(*found) : fs::path(executable));
#ifdef _WIN32
    auto installed = binary.parent_path() / ".." / "lib" / "flywheel_runtime.lib";
#else
    auto installed = binary.parent_path() / ".." / "lib" / "libflywheel_runtime.a";
#endif
    if (fs::exists(installed))
        return installed;
    if (fs::exists(FW_RUNTIME))
        return FW_RUNTIME;
    throw std::runtime_error("cannot locate Flywheel runtime archive");
}
std::string desktopPath(const fs::path &runtime) {
#ifdef _WIN32
    auto name = "flywheel_desktop.dll";
#elif defined(__APPLE__)
    auto name = "libflywheel_desktop.dylib";
#else
    auto name = "libflywheel_desktop.so";
#endif
    auto installed = runtime.parent_path() / name;
    if (fs::exists(installed))
        return fs::absolute(installed).string();
    return FW_DESKTOP_BACKEND;
}
#ifdef FW_JIT
// Runs a compiled object without linking it: the runtime is part of this executable and its fw_*
// symbols are exported for the JIT. The program runs in a child process, so traps and crashes
// behave as they would in a built executable. Returns nullopt when the JIT is unavailable.
std::optional<int> runInProcess(const std::string &object) {
    auto jit = llvm::orc::LLJITBuilder().create();
    std::optional<llvm::orc::ExecutorAddr> entry;
    if (jit) {
        auto buffer = llvm::MemoryBuffer::getMemBufferCopy(object, "program.o");
        if (auto error = (*jit)->addObjectFile(std::move(buffer)))
            llvm::consumeError(std::move(error));
        else if (auto symbol = (*jit)->lookup("main"))
            entry = *symbol;
        else
            llvm::consumeError(symbol.takeError());
    } else
        llvm::consumeError(jit.takeError());
    if (!entry)
        return std::nullopt;
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    [[maybe_unused]] pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0)
        return std::nullopt;
    if (child == 0) {
#ifdef __linux__
        // Stop the program (and free e.g. its listening sockets) if the driver is killed.
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(1);
#endif
        std::exit(entry->toPtr<int (*)()>()());
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "Flywheel 0.1.0 (LLVM " LLVM_VERSION_STRING ")\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "test") {
        auto python = llvm::sys::findProgramByName("python3");
        if (!python)
            python = llvm::sys::findProgramByName("python");
        if (!python) {
            std::cerr << "Python 3 is required for the development test harness\n";
            return 1;
        }
        return execute(*python, {std::string(FW_TEST_DIR) + "/test_compiler.py",
                                 fs::absolute(argv[0]).string()});
    }
    if (argc < 3) {
        std::cerr << "Usage: flywheel {build|run|check} source.fly [-O0..3] [-o output] [--emit-ir "
                     "file.ll]\n       flywheel test | --version\n";
        return 2;
    }
    std::string command = argv[1], filename = argv[2], output, ir;
    int optimization = 0;
    if (command != "build" && command != "run" && command != "check") {
        std::cerr << "unknown command: " << command << '\n';
        return 2;
    }
    try {
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.size() == 3 && arg.substr(0, 2) == "-O" && arg[2] >= '0' && arg[2] <= '3')
                optimization = arg[2] - '0';
            else if ((arg == "-o" || arg == "--emit-ir") && i + 1 < argc) {
                if (arg == "-o")
                    output = argv[++i];
                else
                    ir = argv[++i];
            } else
                throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        auto program = fw::loadProgram(filename);
        if (command == "check") {
            fw::compile(program, filename, {optimization, ir, "", ""});
            return 0;
        }
        auto runtime = runtimePath(argv[0]);
#ifdef FW_JIT
        if (command == "run") {
            std::string object;
            fw::compile(program, filename,
                        {optimization, ir, "", desktopPath(runtime), &object});
            if (auto status = runInProcess(object))
                return *status;
            program = fw::loadProgram(filename);
        }
#endif
        Temp temp;
#ifdef _WIN32
        auto object = temp.path / "program.obj";
#else
        auto object = temp.path / "program.o";
#endif
        fw::compile(program, filename, {optimization, ir, object.string(), desktopPath(runtime)});
#ifdef _WIN32
        std::string suffix = ".exe";
#else
        std::string suffix;
#endif
        auto binary =
            command == "run"
                ? temp.path / ("program" + suffix)
                : fs::absolute(output.empty() ? fs::path(filename).stem().string() + suffix
                                              : output);
        std::vector<std::string> link;
#ifdef _MSC_VER
        link = {"/nologo", "/MD",       object.string(), runtime.string(), "/Fe:" + binary.string(),
                "/link",   "Ws2_32.lib"};
#else
        link = {object.string(), runtime.string(), "-o", binary.string(), "-pthread"};
        if (*FW_LINKER_FLAG)
            link.push_back(FW_LINKER_FLAG);
#endif
#if defined(_WIN32) && !defined(_MSC_VER)
        link.push_back("-lws2_32");
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
        link.push_back("-ldl");
#endif
        int linked = execute(FW_CXX, link);
        if (linked)
            return linked;
        if (command == "run")
            return execute(binary.string(), {});
        return 0;
    } catch (const fw::Error &e) {
        std::cerr << (e.loc.file.empty() ? filename : e.loc.file) << ':' << e.loc.line << ':'
                  << e.loc.column << ": error: " << e.what() << '\n';
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "flywheel: error: " << e.what() << '\n';
        return 1;
    }
}
