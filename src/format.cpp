#include "flywheel/frontend.hpp"
#include <algorithm>
#include <sstream>

namespace fw {
namespace {
struct Block {
    std::string kind; // "block", "interface", "match", or "case"
    int depth;        // delimiter depth of the opening line; deeper lines are continuations
};
// Index of the ')' matching the '(' at `open` within one line's tokens, or npos.
size_t closing(const std::vector<const Token *> &line, size_t open) {
    int depth = 0;
    for (size_t i = open; i < line.size(); ++i) {
        if (line[i]->kind != "op")
            continue;
        if (line[i]->text == "(")
            ++depth;
        else if (line[i]->text == ")" && --depth == 0)
            return i;
    }
    return std::string::npos;
}
// True when the line ends with the header of a block lambda: function(...) [returns T]
bool opensLambda(const std::vector<const Token *> &line, int &depth) {
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i]->text != "function" || line[i]->kind != "id" || line[i + 1]->text != "(")
            continue;
        auto end = closing(line, i + 1);
        if (end == std::string::npos)
            return false;
        size_t next = end + 1;
        bool arrow = false;
        for (size_t k = next; k < line.size(); ++k)
            if (line[k]->text == "=>" && line[k]->kind == "op")
                arrow = true;
        if (arrow)
            continue;
        if (next == line.size() || line[next]->text == "returns") {
            depth = line[i]->depth;
            return true;
        }
    }
    return false;
}
} // namespace
std::string format(const std::string &source) {
    bool crlf = source.find("\r\n") != std::string::npos;
    std::string text;
    for (char c : source)
        if (c != '\r')
            text += c;
    auto tokens = lex(text);
    std::vector<std::string> lines;
    {
        std::string line;
        std::istringstream in(text);
        while (std::getline(in, line))
            lines.push_back(line);
    }
    size_t count = lines.size();
    std::vector<std::vector<const Token *>> byLine(count + 2);
    std::vector<bool> inside(count + 2, false), endsInside(count + 2, false),
        continued(count + 2, false);
    const Token *previous = nullptr;
    for (auto &t : tokens) {
        if (t.kind == "eof")
            break;
        if (t.kind == "nl") {
            previous = &t;
            continue;
        }
        auto line = static_cast<size_t>(t.loc.line);
        if (line <= count) {
            if (byLine[line].empty() && previous && previous->kind != "nl" &&
                previous->loc.line < t.loc.line)
                continued[line] = true; // joined by '\' or a trailing operator
            byLine[line].push_back(&t);
        }
        auto newlines = static_cast<size_t>(std::count(t.text.begin(), t.text.end(), '\n'));
        if ((t.kind == "string" || t.kind == "markup") && newlines) {
            endsInside[line] = true;
            for (size_t k = 1; k <= newlines && line + k <= count; ++k) {
                inside[line + k] = true;
                if (k < newlines)
                    endsInside[line + k] = true;
            }
        }
        previous = &t;
    }
    std::vector<Block> stack;
    std::vector<std::string> out;
    auto trim = [](const std::string &s, bool right) {
        auto begin = s.find_first_not_of(" \t");
        if (begin == std::string::npos)
            return std::string();
        auto end = right ? s.find_last_not_of(" \t") + 1 : s.size();
        return s.substr(begin, end - begin);
    };
    auto indent = [](size_t level) { return std::string(level * 4, ' '); };
    int lastDepth = 0;
    for (size_t n = 1; n <= count; ++n) {
        auto &raw = lines[n - 1];
        if (inside[n]) {
            out.push_back(raw); // inside a multi-line string or markup literal
            continue;
        }
        auto &line = byLine[n];
        auto content = trim(raw, !endsInside[n]);
        if (line.empty()) {
            // Blank and comment-only lines follow the surrounding block.
            out.push_back(content.empty()
                              ? std::string()
                              : indent(stack.size() + (lastDepth > (stack.empty() ? 0 : stack.back().depth))) +
                                    content);
            continue;
        }
        size_t first = 0;
        while (first + 1 < line.size() &&
               (line[first]->text == "public" || line[first]->text == "private" ||
                line[first]->text == "readonly"))
            ++first;
        auto &keyword = line[first]->text;
        bool isKeyword = line[first]->kind == "id";
        size_t level = stack.size();
        int closed = -1; // delimiter depth of the block an `end` closes
        if (isKeyword && keyword == "end" && !stack.empty()) {
            if (stack.back().kind == "case")
                stack.pop_back();
            if (!stack.empty()) {
                closed = stack.back().depth;
                stack.pop_back();
            }
            level = stack.size();
        } else if (isKeyword && (keyword == "else" || keyword == "case") && !stack.empty() &&
                   stack.back().kind == "case") {
            stack.pop_back();
            level = stack.size();
            stack.push_back({"case", line[first]->depth});
        } else if (isKeyword && keyword == "case" && !stack.empty() &&
                   stack.back().kind == "match") {
            level = stack.size();
            stack.push_back({"case", line[first]->depth});
        } else if (isKeyword && (keyword == "else" || keyword == "catch" || keyword == "finally"))
            level = stack.empty() ? 0 : stack.size() - 1;
        int base = closed >= 0 ? closed : stack.empty() ? 0 : stack.back().depth;
        // An opening delimiter carries the depth inside it; the line itself starts one level out.
        auto front = line.front();
        bool opens = front->kind == "op" &&
                     (front->text == "(" || front->text == "[" || front->text == "{");
        size_t extra = (front->depth - (opens ? 1 : 0) > base ? 1 : 0) + (continued[n] ? 1 : 0);
        out.push_back(indent(level + extra) + content);
        lastDepth = line.back()->depth;
        auto depth = line[first]->depth;
        if (!isKeyword || keyword == "end" || keyword == "case") {
            // closes or continues an existing block
        } else if (keyword == "match")
            stack.push_back({"match", depth});
        else if (keyword == "class" || keyword == "enum" || keyword == "elements" ||
                 keyword == "while" || keyword == "for" || keyword == "try")
            stack.push_back({"block", depth});
        else if (keyword == "interface")
            stack.push_back({"interface", depth});
        else if (keyword == "function" && first + 1 < line.size() && line[first + 1]->kind == "id" &&
                 (stack.empty() || stack.back().kind != "interface"))
            stack.push_back({"block", depth});
        else if (keyword == "if" && line.back()->text == "then")
            stack.push_back({"block", depth});
        else if (keyword == "defer" && first + 1 == line.size())
            stack.push_back({"block", depth}); // `defer` alone opens a block ending in `end`
        int lambdaDepth = 0;
        if (!(isKeyword && keyword == "function" && first + 1 < line.size() &&
              line[first + 1]->kind == "id") &&
            opensLambda(line, lambdaDepth))
            stack.push_back({"block", lambdaDepth});
    }
    while (!out.empty() && out.back().empty())
        out.pop_back();
    std::string result;
    for (auto &l : out)
        result += l + (crlf ? "\r\n" : "\n");
    return result;
}
} // namespace fw
