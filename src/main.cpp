#include "flywheel/frontend.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>

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
        Temp temp;
#ifdef _WIN32
        auto object = temp.path / "program.obj";
#else
        auto object = temp.path / "program.o";
#endif
        auto runtime = runtimePath(argv[0]);
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
