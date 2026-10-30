// `flywheel doc`: renders the `#` comments directly above public declarations as HTML.
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fw {
namespace {
namespace fs = std::filesystem;
std::string escape(const std::string &s) {
    std::string out;
    for (char c : s)
        out += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : std::string(1, c);
    return out;
}
// Comment text to HTML: blank lines separate paragraphs, `code` spans, and lines indented by
// four spaces (or fenced with ```) form code blocks.
std::string prose(const std::vector<std::string> &lines) {
    std::string out, paragraph;
    bool fenced = false;
    auto inline_ = [](const std::string &text) {
        std::string out;
        bool code = false;
        for (char c : escape(text)) {
            if (c == '`') {
                out += code ? "</code>" : "<code>";
                code = !code;
            } else
                out += c;
        }
        return out + (code ? "</code>" : "");
    };
    auto flush = [&] {
        if (!paragraph.empty())
            out += "<p>" + inline_(paragraph) + "</p>\n";
        paragraph.clear();
    };
    for (auto &line : lines) {
        if (line.rfind("```", 0) == 0) {
            flush();
            out += fenced ? "</code></pre>\n" : "<pre><code>";
            fenced = !fenced;
            continue;
        }
        if (fenced) {
            out += escape(line) + "\n";
            continue;
        }
        if (line.empty())
            flush();
        else
            paragraph += (paragraph.empty() ? "" : " ") + line;
    }
    flush();
    if (fenced)
        out += "</code></pre>\n";
    return out;
}
struct Source {
    std::vector<std::string> lines;
    std::string line(int n) const {
        if (n < 1 || n > int(lines.size()))
            return "";
        auto text = lines[n - 1];
        auto a = text.find_first_not_of(" \t");
        return a == std::string::npos ? "" : text.substr(a);
    }
    // The block of `#` comment lines ending directly above line `n`.
    std::vector<std::string> comment(int n) const {
        std::vector<std::string> out;
        for (int i = n - 1; i >= 1; --i) {
            auto text = line(i);
            if (text.empty() || text[0] != '#')
                break;
            text = text.substr(1);
            if (!text.empty() && text[0] == ' ')
                text = text.substr(1);
            out.insert(out.begin(), text);
        }
        return out;
    }
};
std::string signature(const std::string &line) {
    auto text = line;
    for (std::string prefix : {"public ", "private "})
        if (text.rfind(prefix, 0) == 0)
            text = text.substr(prefix.size());
    if (auto eq = text.find(" = "); eq != std::string::npos && text.rfind("function", 0) != 0 &&
        text.rfind("const ", 0) != 0)
        text = text.substr(0, eq);
    return text;
}
std::string anchor(const std::string &name) {
    std::string out;
    for (char c : name)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    return out;
}
std::string document(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error({1, 1, path}, "cannot read module");
    std::stringstream text;
    text << in.rdbuf();
    Source source;
    std::string line;
    std::istringstream lines(text.str());
    while (std::getline(lines, line))
        source.lines.push_back(line.empty() || line.back() != '\r' ? line : line.substr(0, line.size() - 1));
    auto program = Parser(lex(text.str())).parse();
    auto moduleName = fs::path(path).stem().string();
    std::string body, toc;
    // The module's own description is the first comment block of the file.
    std::vector<std::string> intro;
    for (auto &l : source.lines) {
        if (l.rfind("#", 0) != 0)
            break;
        auto t = l.substr(1);
        intro.push_back(!t.empty() && t[0] == ' ' ? t.substr(1) : t);
    }
    struct Entry {
        int line;
        std::string kind, name, html;
    };
    std::vector<Entry> entries;
    auto entry = [&](int at, const std::string &kind, const std::string &name, std::string extra = "") {
        if (program.privateNames.count(name))
            return;
        auto comment = source.comment(at);
        if (!intro.empty() && at - int(comment.size()) == 1)
            comment.clear(); // the file comment is not the first declaration's
        Entry e{at, kind, name, ""};
        e.html = "<section id=\"" + anchor(name) + "\"><h3><span class=\"kind\">" + kind +
                 "</span> " + escape(name) + "</h3>\n<pre class=\"signature\"><code>" +
                 escape(signature(source.line(at))) + "</code></pre>\n" + prose(comment) + extra +
                 "</section>\n";
        entries.push_back(std::move(e));
    };
    for (auto &f : program.functions)
        entry(f.loc.line, "function", f.name);
    for (auto &c : program.classes) {
        std::string members;
        for (auto &field : c.fields) {
            if (field.isPrivate)
                continue;
            members += "<li><code>" + escape(signature(source.line(field.loc.line))) + "</code>" +
                       prose(source.comment(field.loc.line)) + "</li>\n";
        }
        for (auto &m : c.methods) {
            if (m.isPrivate)
                continue;
            members += "<li><code>" + escape(signature(source.line(m.loc.line))) + "</code>" +
                       prose(source.comment(m.loc.line)) + "</li>\n";
        }
        if (!members.empty())
            members = "<ul class=\"members\">\n" + members + "</ul>\n";
        entry(c.loc.line, c.isInterface ? "interface" : "class", c.name, members);
    }
    for (auto &en : program.enums) {
        std::string members = "<ul class=\"members\">\n";
        for (auto &m : en.members)
            members += "<li><code>" + escape(m) + "</code></li>\n";
        entry(en.loc.line, "enum", en.name, members + "</ul>\n");
    }
    for (auto &s : program.body)
        if (s->kind == "declare" && s->op == "const")
            entry(s->loc.line, "const", s->name);
    std::sort(entries.begin(), entries.end(), [](auto &a, auto &b) { return a.line < b.line; });
    for (auto &e : entries) {
        toc += "<li><a href=\"#" + anchor(e.name) + "\">" + escape(e.name) + "</a> <span class=\"kind\">" + e.kind + "</span></li>\n";
        body += e.html;
    }
    return "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
           escape(moduleName) +
           "</title>\n<style>\n"
           ":root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fff;--muted:#6b6b70;--code:#f3f3f5;--line:#e3e3e8}\n"
           "@media (prefers-color-scheme: dark){:root{--fg:#e8e8ea;--bg:#161618;--muted:#9a9aa0;--code:#232327;--line:#2f2f35}}\n"
           "body{font:16px/1.55 system-ui,sans-serif;color:var(--fg);background:var(--bg);max-width:52rem;margin:0 auto;padding:2rem 1rem}\n"
           "code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.9em}\n"
           "pre{background:var(--code);padding:.6rem .8rem;border-radius:6px;overflow-x:auto}\n"
           ":not(pre)>code{background:var(--code);padding:.1em .3em;border-radius:4px}\n"
           "section{border-top:1px solid var(--line);padding-top:.5rem;margin-top:1.5rem}\n"
           ".kind{color:var(--muted);font-weight:normal;font-size:.85em}\nul.members{padding-left:1.2rem}\n"
           "</style></head><body>\n<h1>" +
           escape(moduleName) + "</h1>\n" + prose(intro) + "<ul>\n" + toc + "</ul>\n" + body +
           "</body></html>\n";
}
} // namespace
int docCommand(const std::vector<std::string> &args) {
    std::string input, output;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size())
            output = args[++i];
        else
            input = args[i];
    }
    if (input.empty()) {
        std::cerr << "usage: flywheel doc module.fw [-o output.html]\n";
        return 2;
    }
    if (output.empty())
        output = fs::path(input).stem().string() + ".html";
    try {
        auto html = document(input);
        std::ofstream(output, std::ios::binary) << html;
        std::cout << "wrote " << output << "\n";
        return 0;
    } catch (const Error &e) {
        std::cerr << (e.loc.file.empty() ? input : e.loc.file) << ":" << e.loc.line << ":"
                  << e.loc.column << ": error: " << e.what() << "\n";
        return 1;
    }
}
} // namespace fw
