#include "flywheel/frontend.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <optional>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#ifdef FW_JIT
#include <cerrno>
#include <cstring>
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
std::optional<int> runInProcess(const std::string &object, std::vector<std::string> arguments) {
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
        std::vector<char *> argv;
        for (auto &argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::exit(entry->toPtr<int (*)(int, char **)>()(static_cast<int>(arguments.size()),
                                                        argv.data()));
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return 1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    // Report a crash the way a shell would, rather than exiting with a bare status.
    int signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    std::cerr << "flywheel: program terminated by signal " << signal;
    if (auto name = signal ? strsignal(signal) : nullptr)
        std::cerr << " (" << name << ")";
    std::cerr << '\n';
    return 128 + signal;
}
#endif
// Compiles `load()` and runs it (run), links it (build), or only checks it (check). `load` is
// called again when the in-process JIT is unavailable, because compilation consumes a program.
int compileProgram(const std::string &command, const std::string &filename,
                   const std::function<fw::Program()> &load, const fw::Options &base,
                   const std::string &output, const std::vector<std::string> &programArguments,
                   const char *argv0) {
    auto program = load();
    if (command == "check") {
        fw::compile(program, filename, base);
        return 0;
    }
    auto runtime = runtimePath(argv0);
    auto options = base;
    options.desktopBackend = desktopPath(runtime);
#ifdef FW_JIT
    if (command == "run") {
        std::string object;
        auto jit = options;
        jit.objectData = &object;
        fw::compile(program, filename, jit);
        std::vector<std::string> arguments{filename};
        arguments.insert(arguments.end(), programArguments.begin(), programArguments.end());
        if (auto status = runInProcess(object, arguments))
            return *status;
        program = load();
    }
#endif
    Temp temp;
#ifdef _WIN32
    auto object = temp.path / "program.obj";
#else
    auto object = temp.path / "program.o";
#endif
    options.object = object.string();
    fw::compile(program, filename, options);
#ifdef _WIN32
    std::string suffix = ".exe";
#else
    std::string suffix;
#endif
    auto binary = command == "run"
                      ? temp.path / ("program" + suffix)
                      : fs::absolute(output.empty() ? fs::path(filename).stem().string() + suffix
                                                    : output);
    std::vector<std::string> link;
#ifdef _MSC_VER
    link = {"/nologo", "/MD", object.string(), runtime.string(), "/Fe:" + binary.string(), "/link",
            "Ws2_32.lib"};
    if (options.debug)
        link.insert(link.begin() + 2, "/Zi");
#else
    link = {object.string(), runtime.string(), "-o", binary.string(), "-pthread"};
    if (*FW_LINKER_FLAG)
        link.push_back(FW_LINKER_FLAG);
    if (options.debug)
        link.push_back("-g");
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
        return execute(binary.string(), programArguments);
    return 0;
}
void reportError(const fw::Error &e, const std::string &filename) {
    std::cerr << (e.loc.file.empty() ? filename : e.loc.file) << ':' << e.loc.line << ':'
              << e.loc.column << ": error: " << e.what() << '\n';
}
bool testFile(const fs::path &path) {
    auto name = path.filename().string(), extension = path.extension().string();
    if (extension != ".fw" && extension != ".fly")
        return false;
    auto stem = path.stem().string();
    return stem.rfind("test_", 0) == 0 ||
           (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "_test") == 0);
}
// flywheel test: runs every zero-argument `test_*` function in test_*.fw / *_test.fw files.
int runTests(std::vector<std::string> paths, const fw::Options &options, const char *argv0) {
    if (paths.empty())
        paths.push_back(".");
    std::vector<fs::path> files;
    for (auto &path : paths) {
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            for (auto it = fs::recursive_directory_iterator(path, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                auto name = it->path().filename().string();
                if (it->is_directory() && (name == "build" || (!name.empty() && name[0] == '.')))
                    it.disable_recursion_pending();
                else if (it->is_regular_file() && testFile(it->path()))
                    files.push_back(it->path());
            }
        } else if (fs::is_regular_file(path, ec))
            files.push_back(path);
        else {
            std::cerr << "flywheel: no such test file or directory: " << path << '\n';
            return 2;
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "flywheel: no test files found (test_*.fw or *_test.fw)\n";
        return 1;
    }
    int failedFiles = 0;
    size_t total = 0;
    for (auto &file : files) {
        auto filename = file.string();
        std::cout << filename << '\n' << std::flush;
        auto load = [&]() {
            auto program = fw::loadProgram(filename);
            // Test functions of the file itself are those of the first loaded module ($m0$).
            std::vector<std::pair<std::string, std::string>> tests;
            for (auto &f : program.functions)
                if (f.name.rfind("$m0$test_", 0) == 0 && f.params.empty() && f.owner.empty())
                    tests.emplace_back(f.name, f.name.substr(4));
            std::string runner = "int __fw_failed = 0\n";
            for (size_t i = 0; i < tests.size(); ++i)
                runner += "try\n    __fw_test_" + std::to_string(i) + "()\n    print \"  ok   " +
                          tests[i].second + "\"\ncatch Error __fw_error\n    print \"  FAIL " +
                          tests[i].second +
                          ": \" + __fw_error.message\n    __fw_failed += 1\nend\n";
            runner += "if __fw_failed > 0 then\n    throw AssertionError(string(__fw_failed) + "
                      "\" of " +
                      std::to_string(tests.size()) + " test(s) failed\")\nend\n";
            auto extra = fw::Parser(fw::lex(runner)).parse();
            std::function<void(fw::Expr &)> rename = [&](fw::Expr &e) {
                if (e.kind == "name" && e.text.rfind("__fw_test_", 0) == 0)
                    e.text = tests.at(std::stoul(e.text.substr(10))).first;
                for (auto &a : e.args)
                    if (a)
                        rename(*a);
            };
            std::function<void(std::vector<fw::SP> &)> statements = [&](std::vector<fw::SP> &body) {
                for (auto &st : body) {
                    if (st->expr)
                        rename(*st->expr);
                    if (st->target)
                        rename(*st->target);
                    statements(st->body);
                    statements(st->other);
                    for (auto &c : st->catches)
                        statements(c);
                }
            };
            statements(extra.body);
            for (auto &st : extra.body)
                program.body.push_back(std::move(st));
            total += tests.size();
            return program;
        };
        try {
            if (compileProgram("run", filename, load, options, "", {}, argv0) != 0)
                ++failedFiles;
        } catch (const fw::Error &e) {
            reportError(e, filename);
            ++failedFiles;
        } catch (const std::exception &e) {
            std::cerr << "flywheel: error: " << e.what() << '\n';
            ++failedFiles;
        }
    }
    std::cout << files.size() << " test file(s), " << failedFiles << " failed\n";
    return failedFiles ? 1 : 0;
}
} // namespace
int main(int argc, char **argv) {
    std::string usage =
        "Usage: flywheel {build|run|check} source.fw [-O0..3] [-g] [-o output] [--emit-ir "
        "file.ll] [-- program arguments]\n"
        "       flywheel test [files or directories] [-O0..3]\n"
        "       flywheel fmt [--check] files...\n"
        "       flywheel lsp | selftest | --version\n";
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "Flywheel " FW_VERSION " (LLVM " LLVM_VERSION_STRING ")\n";
        return 0;
    }
    if (argc < 2) {
        std::cerr << usage;
        return 2;
    }
    std::string command = argv[1];
    if (command == "selftest") {
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
    if (command == "lsp")
        return fw::languageServer();
    if (command == "fmt") {
        bool check = false;
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--check")
                check = true;
            else
                files.push_back(arg);
        }
        if (files.empty()) {
            std::cerr << usage;
            return 2;
        }
        int status = 0;
        for (auto &file : files) {
            std::ifstream input(file, std::ios::binary);
            if (!input) {
                std::cerr << "flywheel: cannot read " << file << '\n';
                status = 1;
                continue;
            }
            std::ostringstream text;
            text << input.rdbuf();
            try {
                auto formatted = fw::format(text.str());
                if (formatted == text.str())
                    continue;
                if (check) {
                    std::cout << file << " needs formatting\n";
                    status = 1;
                } else {
                    std::ofstream out(file, std::ios::binary);
                    out << formatted;
                }
            } catch (const fw::Error &e) {
                reportError(e, file);
                status = 1;
            }
        }
        return status;
    }
    if (command != "build" && command != "run" && command != "check" && command != "test") {
        std::cerr << (command.rfind("-", 0) == 0 ? usage : "unknown command: " + command + "\n");
        return 2;
    }
    std::string filename, output;
    std::vector<std::string> programArguments, testPaths; // after `--`: arguments for run
    fw::Options options;
    try {
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--") {
                programArguments.assign(argv + i + 1, argv + argc);
                break;
            }
            if (arg.size() == 3 && arg.substr(0, 2) == "-O" && arg[2] >= '0' && arg[2] <= '3')
                options.optimization = arg[2] - '0';
            else if (arg == "-g")
                options.debug = true;
            else if ((arg == "-o" || arg == "--emit-ir") && i + 1 < argc) {
                if (arg == "-o")
                    output = argv[++i];
                else
                    options.ir = argv[++i];
            } else if (!arg.empty() && arg[0] != '-') {
                if (command == "test")
                    testPaths.push_back(arg);
                else if (filename.empty())
                    filename = arg;
                else
                    throw std::runtime_error("unexpected argument: " + arg);
            } else
                throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        if (command == "test")
            return runTests(testPaths, options, argv[0]);
        if (filename.empty()) {
            std::cerr << usage;
            return 2;
        }
        return compileProgram(command, filename, [&] { return fw::loadProgram(filename); },
                              options, output, programArguments, argv[0]);
    } catch (const fw::Error &e) {
        reportError(e, filename);
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "flywheel: error: " << e.what() << '\n';
        return 1;
    }
}
