// Installed packages. Flywheel only resolves packages that a package manager (Pulley) has
// installed; it does not read dependency lists or fetch anything.
//
// A package is a directory named by its import name, or a file `<name>.link` holding the path
// of the package directory (used for path dependencies). Packages are searched in the nearest
// `.flywheel-modules` directory at or above the importing file, then in the installation-wide
// `modules` directory: `<install>/modules`, where <install> is the directory containing
// `bin/flywheel` (or the executable's directory when it is not in a `bin` directory), or
// `$FLYWHEEL_HOME/modules` when that variable is set.
#include "flywheel/frontend.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fw {
namespace {
namespace fs = std::filesystem;
std::string &executablePath() {
    static std::string path;
    return path;
}
std::string trim(const std::string &s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
// The package directory for `name` inside a modules directory, or an empty path.
fs::path packageIn(const fs::path &modules, const std::string &name) {
    std::error_code ec;
    auto directory = modules / name;
    if (fs::is_directory(directory, ec))
        return directory;
    auto link = modules / (name + ".link");
    if (fs::is_regular_file(link, ec)) {
        std::ifstream in(link);
        std::string target;
        std::getline(in, target);
        target = trim(target);
        fs::path resolved = fs::path(target).is_absolute() ? fs::path(target) : modules / target;
        if (fs::is_directory(resolved, ec))
            return fs::weakly_canonical(resolved, ec);
    }
    return {};
}
} // namespace
void setExecutablePath(const std::string &path) {
    std::error_code ec;
    auto absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
    executablePath() = ec ? path : absolute.string();
}
std::string globalModulesDirectory() {
    if (auto home = std::getenv("FLYWHEEL_HOME"); home && *home)
        return (fs::path(home) / "modules").string();
    if (executablePath().empty())
        return "";
    auto directory = fs::path(executablePath()).parent_path();
    if (directory.filename() == "bin")
        directory = directory.parent_path();
    return (directory / "modules").string();
}
std::vector<std::string> moduleDirectories(const std::string &sourceFile) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto directory = fs::absolute(fs::path(sourceFile).parent_path(), ec);;
         directory = directory.parent_path()) {
        if (fs::is_directory(directory / ".flywheel-modules", ec)) {
            out.push_back((directory / ".flywheel-modules").string());
            break;
        }
        if (directory == directory.parent_path())
            break;
    }
    auto global = globalModulesDirectory();
    if (!global.empty() && fs::is_directory(global, ec))
        out.push_back(global);
    return out;
}
std::map<std::string, std::string> packageRoots(const std::string &sourceFile) {
    // Only the packages that exist are returned; lookups happen per import name, so this
    // lists every package visible from the file (the nearest directory wins).
    std::map<std::string, std::string> roots;
    std::error_code ec;
    for (auto &modules : moduleDirectories(sourceFile))
        for (auto &entry : fs::directory_iterator(modules, ec)) {
            auto name = entry.path().filename().string();
            if (entry.is_regular_file(ec) && entry.path().extension() == ".link")
                name = entry.path().stem().string();
            else if (!entry.is_directory(ec) || name.empty() || name[0] == '.')
                continue;
            if (roots.count(name))
                continue;
            auto directory = packageIn(modules, name);
            if (!directory.empty())
                roots[name] = directory.string();
        }
    return roots;
}
std::string packageMain(const std::string &directory, const std::string &name) {
    // `main` in the package's pulley.toml [package] table, else <name>.fw, main.fw, or lib.fw.
    std::ifstream in(fs::path(directory) / "pulley.toml");
    std::string line, table;
    while (std::getline(in, line)) {
        auto text = trim(line.substr(0, line.find('#')));
        if (text.empty())
            continue;
        if (text.front() == '[') {
            table = trim(text.substr(1, text.find(']') - 1));
            continue;
        }
        auto eq = text.find('=');
        if (table != "package" || eq == std::string::npos || trim(text.substr(0, eq)) != "main")
            continue;
        auto value = trim(text.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\''))
            return value.substr(1, value.size() - 2);
    }
    for (auto candidate : {name + ".fw", std::string("main.fw"), std::string("lib.fw")})
        if (fs::is_regular_file(fs::path(directory) / candidate))
            return candidate;
    return name + ".fw";
}
} // namespace fw
