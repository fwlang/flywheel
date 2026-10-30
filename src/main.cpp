#include "flywheel/frontend.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
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
// Compiles `load()` and runs it (run, desktop, serve), links it (build), or only checks it
// (check). `load` is called again when the in-process JIT is unavailable, because compilation
// consumes a program.
int compileProgram(const std::string &command, const std::string &filename,
                   const std::function<fw::Program()> &load, const fw::Options &base,
                   const std::string &output, const std::vector<std::string> &programArguments,
                   const char *argv0) {
    auto program = load();
    if (command == "check") {
        auto options = base;
        options.entry = "check";
        fw::compile(program, filename, options);
        return 0;
    }
    auto runtime = runtimePath(argv0);
    auto options = base;
    options.desktopBackend = desktopPath(runtime);
    if (command == "desktop" || command == "serve")
        options.entry = command;
    bool runs = command != "build";
#ifdef FW_JIT
    if (runs) {
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
    auto binary =
        runs ? temp.path / ("program" + suffix)
             : fs::absolute(output.empty() ? fs::path(filename).stem().string() + suffix : output);
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
    if (runs)
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
// Prints line coverage per source file (test files excluded) and writes coverage.lcov.
void reportCoverage(const std::string &path) {
    std::map<std::string, std::map<int, int64_t>> files;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto a = line.find('\t'), b = line.rfind('\t');
        if (a == std::string::npos || a == b)
            continue;
        auto file = line.substr(0, a);
        if (testFile(file))
            continue;
        files[file][std::stoi(line.substr(a + 1, b - a - 1))] += std::stoll(line.substr(b + 1));
    }
    if (files.empty()) {
        std::cout << "coverage: no lines outside test files were compiled\n";
        return;
    }
    std::ofstream lcov("coverage.lcov");
    size_t allLines = 0, allHit = 0;
    std::cout << "coverage:\n";
    for (auto &[file, lines] : files) {
        size_t hit = 0;
        lcov << "SF:" << file << "\n";
        for (auto &[number, count] : lines) {
            hit += count > 0;
            lcov << "DA:" << number << "," << count << "\n";
        }
        lcov << "LF:" << lines.size() << "\nLH:" << hit << "\nend_of_record\n";
        allLines += lines.size();
        allHit += hit;
        std::string missed;
        for (auto &[number, count] : lines)
            if (!count)
                missed += (missed.empty() ? "" : ",") + std::to_string(number);
        auto relative = fs::relative(file).string();
        std::cout << "  " << relative << "  " << hit << "/" << lines.size() << " lines ("
                  << (lines.empty() ? 100 : hit * 100 / lines.size()) << "%)";
        if (!missed.empty())
            std::cout << "  missed: " << missed;
        std::cout << "\n";
    }
    std::cout << "  total  " << allHit << "/" << allLines << " lines ("
              << allHit * 100 / allLines << "%); wrote coverage.lcov\n";
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
    // Programs append "file<TAB>line<TAB>count" lines to this file when they exit.
    Temp coverageDirectory;
    auto coverageFile = (coverageDirectory.path / "coverage.tsv").string();
    if (options.coverage) {
#ifdef _WIN32
        _putenv_s("FW_COVERAGE_FILE", coverageFile.c_str());
#else
        setenv("FW_COVERAGE_FILE", coverageFile.c_str(), 1);
#endif
    }
    for (auto &file : files) {
        auto filename = file.string();
        std::cout << filename << '\n' << std::flush;
        auto load = [&]() {
            auto program = fw::loadProgram(filename);
            // Test functions of the file itself are those of the first loaded module ($m0$).
            std::vector<std::pair<std::string, std::string>> tests;
            // Fixtures: setup/teardown run around each test, setup_all/teardown_all once.
            std::set<std::string> fixtures;
            for (auto &f : program.functions) {
                if (f.name.rfind("$m0$test_", 0) == 0 && f.params.empty() && f.owner.empty())
                    tests.emplace_back(f.name, f.name.substr(4));
                for (std::string fixture : {"setup", "teardown", "setup_all", "teardown_all"})
                    if (f.name == "$m0$" + fixture && f.params.empty() && f.owner.empty())
                        fixtures.insert(fixture);
            }
            std::string runner = "int __fw_failed = 0\n";
            if (fixtures.count("setup_all"))
                runner += "__fw_fixture_setup_all()\n";
            for (size_t i = 0; i < tests.size(); ++i) {
                std::string body = "        __fw_test_" + std::to_string(i) + "()\n";
                if (fixtures.count("setup"))
                    body = "        __fw_fixture_setup()\n" + body;
                if (fixtures.count("teardown"))
                    body = "    try\n" + body + "    finally\n        __fw_fixture_teardown()\n    end\n";
                else
                    body = body.substr(4);
                runner += "try\n" + body + "    print \"  ok   " + tests[i].second +
                          "\"\ncatch Error __fw_error\n    print \"  FAIL " + tests[i].second +
                          ": \" + __fw_error.message\n    __fw_failed += 1\nend\n";
            }
            if (fixtures.count("teardown_all"))
                runner += "__fw_fixture_teardown_all()\n";
            runner += "if __fw_failed > 0 then\n    throw AssertionError(string(__fw_failed) + "
                      "\" of " +
                      std::to_string(tests.size()) + " test(s) failed\")\nend\n";
            auto extra = fw::Parser(fw::lex(runner)).parse();
            std::function<void(fw::Expr &)> rename = [&](fw::Expr &e) {
                if (e.kind == "name" && e.text.rfind("__fw_test_", 0) == 0)
                    e.text = tests.at(std::stoul(e.text.substr(10))).first;
                else if (e.kind == "name" && e.text.rfind("__fw_fixture_", 0) == 0)
                    e.text = "$m0$" + e.text.substr(13);
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
                    statements(st->finalizer);
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
    if (options.coverage)
        reportCoverage(coverageFile);
    return failedFiles ? 1 : 0;
}
// flywheel repl: each input is added to a growing program that is re-run from the start;
// only output produced after the previous inputs is shown. Declarations (functions, classes,
// enums, imports) are kept; statements are kept when they run without error.
bool declaration(const fw::Program &p) {
    return p.body.empty() &&
           (!p.functions.empty() || !p.classes.empty() || !p.enums.empty() || !p.imports.empty() ||
            !p.components.empty() || !p.families.empty());
}
int repl(const char *argv0) {
    Temp temp;
    auto source = temp.path / "repl.fw", out = temp.path / "out.txt", err = temp.path / "err.txt";
    std::vector<std::string> declarations, statements;
    const std::string sentinel = "\x1f\x1f fw-repl \x1f\x1f";
    std::cout << "Flywheel " FW_VERSION " REPL. Enter statements, expressions, or declarations; "
                 ":quit exits, :reset clears, :show lists the session.\n";
    auto run = [&](const std::vector<std::string> &decls, const std::vector<std::string> &before,
                   const std::string &current, std::string &output, std::string &errors) {
        std::error_code ignored;
        fs::remove(out, ignored);
        fs::remove(err, ignored);
        std::ofstream file(source, std::ios::binary);
        for (auto &d : decls)
            file << d << "\n";
        for (auto &st : before)
            file << st << "\n";
        file << "print \"" << sentinel << "\"\n" << current << "\n";
        file.close();
        auto outPath = out.string(), errPath = err.string();
        std::optional<llvm::StringRef> redirects[] = {std::nullopt, llvm::StringRef(outPath),
                                                      llvm::StringRef(errPath)};
        std::string self = fs::absolute(argv0).string();
        auto status = llvm::sys::ExecuteAndWait(self, {self, "run", source.string()}, std::nullopt,
                                                redirects);
        auto read = [](const fs::path &p) {
            std::ifstream in(p, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        };
        output = read(out);
        errors = read(err);
        auto at = output.find(sentinel + "\n");
        output = at == std::string::npos ? "" : output.substr(at + sentinel.size() + 1);
        // Report locations relative to the input rather than the generated file.
        auto marker = source.string() + ":";
        for (size_t p; (p = errors.find(marker)) != std::string::npos;)
            errors.replace(p, marker.size(), "input:");
        return status;
    };
    std::string line, chunk;
    while (true) {
        std::cout << (chunk.empty() ? "fw> " : "... ") << std::flush;
        if (!std::getline(std::cin, line))
            break;
        if (chunk.empty()) {
            if (line == ":quit" || line == ":q" || line == ":exit")
                break;
            if (line == ":reset") {
                declarations.clear();
                statements.clear();
                continue;
            }
            if (line == ":show") {
                for (auto &d : declarations)
                    std::cout << d << "\n";
                for (auto &st : statements)
                    std::cout << st << "\n";
                continue;
            }
            if (line.find_first_not_of(" \t") == std::string::npos)
                continue;
        }
        chunk += line + "\n";
        fw::Program parsed;
        try {
            parsed = fw::Parser(fw::lex(chunk)).parse();
        } catch (const fw::Error &e) {
            // An error on the last line usually means the input continues (an open block).
            int lines = static_cast<int>(std::count(chunk.begin(), chunk.end(), '\n'));
            if (e.loc.line >= lines && line.find_first_not_of(" \t") != std::string::npos)
                continue;
            std::cerr << "input:" << e.loc.line << ":" << e.loc.column << ": error: " << e.what()
                      << "\n";
            chunk.clear();
            continue;
        }
        auto input = chunk;
        chunk.clear();
        std::string output, errors;
        if (declaration(parsed)) {
            auto decls = declarations;
            decls.push_back(input);
            run(decls, statements, "", output, errors);
            if (!errors.empty())
                std::cerr << errors;
            else
                declarations.push_back(input);
            continue;
        }
        // A lone expression is printed (unless it has no value, such as a void call).
        int status = -1;
        auto ran = input;
        if (parsed.body.size() == 1 && parsed.body[0]->kind == "expr" &&
            parsed.functions.empty() && parsed.classes.empty()) {
            ran = "print (" + input.substr(0, input.size() - 1) + ")";
            status = run(declarations, statements, ran, output, errors);
            if (status != 0 && errors.find("error:") != std::string::npos && output.empty())
                status = -1;
        }
        if (status == -1) {
            ran = input;
            status = run(declarations, statements, input, output, errors);
        }
        std::cout << output << std::flush;
        std::cerr << errors << std::flush;
        if (status == 0)
            statements.push_back(ran);
    }
    std::cout << "\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    static int anchor;
    fw::setExecutablePath(llvm::sys::fs::getMainExecutable(argv[0], &anchor));
    std::string usage =
        "Usage: flywheel {build|run|check} source.fw [-O0..3] [-g] [--race] [-o output] "
        "[--emit-ir file.ll] [-- program arguments]\n"
        "       flywheel desktop source.fw [options] [-- program arguments]\n"
        "       flywheel serve source.fw [--port N] [--host H] [options] [-- program arguments]\n"
        "       flywheel test [files or directories] [-O0..3] [--coverage] [--race]\n"
        "       flywheel fmt [--check] files...\n"
        "       flywheel doc module.fw [-o output.html]\n"

        "       flywheel repl | lsp | selftest | --version\n";
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "Flywheel " FW_VERSION ", LLVM " LLVM_VERSION_STRING "\n";
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
    if (command == "new" || command == "get" || command == "add" || command == "install") {
        std::cerr << "flywheel: packages are managed by Pulley: use `pulley " << command
                  << "` (or `ppm " << command << "`)\n";
        return 2;
    }
    if (command == "doc")
        return fw::docCommand(std::vector<std::string>(argv + 2, argv + argc));
    if (command == "repl")
        return repl(argv[0]);
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
    if (command != "build" && command != "run" && command != "check" && command != "test" &&
        command != "desktop" && command != "serve") {
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
            else if (arg == "--coverage" && command == "test")
                options.coverage = true;
            else if (arg == "--race")
                options.race = true;
            else if ((arg == "--port" || arg == "--host") && command == "serve" && i + 1 < argc) {
                std::string value = argv[++i];
                if (arg == "--host")
                    options.serveHost = value;
                else if (value.empty() ||
                         value.find_first_not_of("0123456789") != std::string::npos ||
                         value.size() > 5 || std::stoi(value) > 65535)
                    throw std::runtime_error("--port expects a number in 0..65535");
                else
                    options.servePort = std::stoi(value);
            } else if ((arg == "-o" || arg == "--emit-ir") && i + 1 < argc) {
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
