// Sources named by URL: `flywheel run https://...`, `import "https://..."`, and remote native
// imports. Downloads use curl (http, https, ftp, ftps, sftp) or ssh (ssh, scp), which are
// installed on Linux and Windows 10+; file:// URLs read local files.
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <optional>
#include <set>
#include <sstream>

namespace fw {
namespace {
namespace fs = std::filesystem;
std::string scheme(const std::string &text) {
    auto end = text.find("://");
    if (end == std::string::npos || end == 0)
        return "";
    std::string s = text.substr(0, end);
    for (auto &c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.')
            return "";
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
std::string percentDecode(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else
            out += text[i];
    }
    return out;
}
std::string readFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
struct Outcome {
    int status = -1;
    std::string out, err;
};
// Runs a program with stdout and stderr captured in temporary files; stdin stays the terminal so
// ssh can ask for a password.
Outcome capture(const std::string &program, const std::vector<std::string> &args) {
    llvm::SmallString<128> outPath, errPath;
    if (llvm::sys::fs::createTemporaryFile("flywheel-fetch", "out", outPath) ||
        llvm::sys::fs::createTemporaryFile("flywheel-fetch", "err", errPath))
        throw std::runtime_error("cannot create a temporary file for a download");
    std::vector<llvm::StringRef> refs{program};
    for (auto &a : args)
        refs.emplace_back(a);
    std::optional<llvm::StringRef> redirects[] = {std::nullopt, llvm::StringRef(outPath),
                                                  llvm::StringRef(errPath)};
    std::string error;
    Outcome result;
    result.status = llvm::sys::ExecuteAndWait(program, refs, std::nullopt, redirects, 0, 0, &error);
    result.out = readFile(std::string(outPath));
    result.err = error.empty() ? readFile(std::string(errPath)) : error;
    llvm::sys::fs::remove(outPath);
    llvm::sys::fs::remove(errPath);
    while (!result.err.empty() && std::isspace(static_cast<unsigned char>(result.err.back())))
        result.err.pop_back();
    return result;
}
std::string tool(const std::string &name, const std::string &purpose) {
    auto found = llvm::sys::findProgramByName(name);
    if (!found)
        throw std::runtime_error(purpose + " needs `" + name + "` on PATH");
    return *found;
}
// Fetches a URL. Returns nullopt when the resource does not exist (or cannot be reached and
// `probe` is set); throws with the tool's message otherwise.
std::optional<std::string> download(const std::string &url, bool probe) {
    auto kind = scheme(url);
    if (kind == "file") {
        auto path = fileUrlPath(url);
        if (!fs::is_regular_file(path)) {
            if (probe)
                return std::nullopt;
            throw std::runtime_error("cannot read " + url);
        }
        return readFile(path);
    }
    if (kind == "ws" || kind == "wss")
        throw std::runtime_error(kind + ":// URLs are message streams, not files; serve " +
                                 url.substr(kind.size() + 3) + " over https:// instead");
    if (kind == "http" || kind == "https" || kind == "ftp" || kind == "ftps" || kind == "sftp") {
        auto curl = tool("curl", "fetching " + kind + ":// sources");
        auto result =
            capture(curl, {"--fail", "--silent", "--show-error", "--location", "--max-redirs", "10",
                           "--proto", "=" + kind + ",https,http", "--", url});
        if (result.status == 0)
            return result.out;
        if (probe)
            return std::nullopt;
        throw std::runtime_error("cannot fetch " + url +
                                 (result.err.empty() ? "" : ": " + result.err));
    }
    if (kind == "ssh" || kind == "scp") {
        // ssh://[user@]host[:port]/path runs `cat` on the host. A path starting /~/ is relative
        // to the remote home directory.
        auto rest = url.substr(kind.size() + 3);
        auto slash = rest.find('/');
        if (slash == std::string::npos || slash == 0)
            throw std::runtime_error(url +
                                     " needs a host and a path, such as ssh://host/path/file.fw");
        auto authority = rest.substr(0, slash), path = percentDecode(rest.substr(slash));
        std::string port;
        auto at = authority.rfind('@');
        auto colon = authority.rfind(':');
        if (colon != std::string::npos && (at == std::string::npos || colon > at) &&
            authority.find(']') == std::string::npos) {
            port = authority.substr(colon + 1);
            authority.resize(colon);
        }
        std::string remote;
        if (path.rfind("/~/", 0) == 0) {
            remote = "~/";
            path = path.substr(3);
        }
        std::string quoted = "'";
        for (char c : path)
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        remote += quoted + "'";
        auto ssh = tool("ssh", "fetching " + kind + ":// sources");
        std::vector<std::string> args;
        if (!port.empty())
            args = {"-p", port};
        args.insert(args.end(), {"--", authority, "cat " + remote});
        auto result = capture(ssh, args);
        if (result.status == 0)
            return result.out;
        if (probe)
            return std::nullopt;
        throw std::runtime_error("cannot fetch " + url +
                                 (result.err.empty() ? "" : ": " + result.err));
    }
    throw std::runtime_error("unsupported URL scheme '" + kind +
                             "://' (use https, http, ftp, ftps, sftp, ssh, scp, or file)");
}
} // namespace
bool isUrl(const std::string &text) {
    static const std::set<std::string> schemes = {"http", "https", "ftp",  "ftps", "sftp",
                                                  "scp",  "ssh",   "file", "ws",   "wss"};
    return schemes.count(scheme(text)) > 0;
}
std::string fileUrlPath(const std::string &url) {
    if (scheme(url) != "file")
        return "";
    auto rest = url.substr(7);
    rest = rest.substr(0, rest.find_first_of("?#"));
    if (rest.rfind("localhost/", 0) == 0)
        rest = rest.substr(9);
    rest = percentDecode(rest);
#ifdef _WIN32
    if (rest.size() > 2 && rest[0] == '/' && rest[2] == ':')
        rest = rest.substr(1); // file:///C:/dir/x.fw
#endif
    return rest.empty() ? "/" : rest;
}
std::string fetchUrl(const std::string &url) {
    return *download(url, false);
}
bool tryFetchUrl(const std::string &url, std::string &contents) {
    auto result = download(url, true);
    if (!result)
        return false;
    contents = std::move(*result);
    return true;
}
std::string contentHash(const std::string &text) {
    uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (int i = 60; i >= 0; i -= 4)
        out += digits[(hash >> i) & 15];
    return out;
}
std::string cacheDirectory(const std::string &kind) {
    fs::path base;
    if (auto xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        base = xdg;
#ifdef _WIN32
    else if (auto local = std::getenv("LOCALAPPDATA"); local && *local)
        base = local;
#else
    else if (auto home = std::getenv("HOME"); home && *home)
        base = fs::path(home) / ".cache";
#endif
    else
        base = fs::temp_directory_path();
    auto directory = base / "flywheel" / kind;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
        throw std::runtime_error("cannot create cache directory " + directory.string() + ": " +
                                 ec.message());
    return directory.string();
}
std::string cacheDownload(const std::string &url, const std::string &contents) {
    auto path = url.substr(0, url.find_first_of("?#"));
    auto name = path.substr(path.find_last_of('/') + 1);
    std::string safe;
    for (char c : name)
        safe += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'
                    ? c
                    : '_';
    if (safe.empty() || safe[0] == '.')
        safe = "download" + safe;
    auto directory = fs::path(cacheDirectory("downloads")) / contentHash(url);
    fs::create_directories(directory);
    auto file = directory / safe;
    std::ofstream out(file, std::ios::binary);
    out << contents;
    if (!out)
        throw std::runtime_error("cannot write " + file.string());
    return file.string();
}
} // namespace fw
