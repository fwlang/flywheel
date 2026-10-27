#pragma once
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fw {
struct Location {
    int line = 1, column = 1;
};
struct Error : std::runtime_error {
    Location loc;
    Error(Location p, const std::string &s) : std::runtime_error(s), loc(p) {}
};
struct Token {
    std::string text, kind;
    Location loc;
};
std::vector<Token> lex(const std::string &source);
struct Expr {
    std::string kind, text;
    Location loc;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<std::string> names;
};
using EP = std::unique_ptr<Expr>;
struct Stmt {
    std::string kind, name, type, op;
    Location loc;
    EP expr, target;
    std::vector<std::unique_ptr<Stmt>> body, other;
};
using SP = std::unique_ptr<Stmt>;
struct Param {
    std::string name, type;
    EP value;
    Location loc;
};
struct Function {
    std::string name, result, owner;
    Location loc;
    std::vector<Param> params;
    std::vector<SP> body;
};
struct Class {
    std::string name;
    Location loc;
    std::vector<Param> fields;
    std::vector<Function> methods;
};
struct Program {
    std::vector<Function> functions;
    std::vector<Class> classes;
    std::vector<SP> body;
    std::map<std::string, EP> components;
    std::map<std::string, std::string> imports;
    bool html = false;
};
class Parser {
    std::vector<Token> tokens;
    size_t pos = 0;
    Token &peek(size_t n = 0);
    Token take();
    bool accept(const std::string &s);
    Token expect(const std::string &s);
    Token identifier();
    void newlines();
    void finish();
    bool typedDeclaration();
    std::string type();
    EP expression(int precedence = 0);
    EP prefix();
    EP outer();
    EP markup();
    EP stringExpr(const Token &t);
    SP statement();
    std::vector<SP> block();
    Function function(const std::string &owner = "");

  public:
    explicit Parser(std::vector<Token> t) : tokens(std::move(t)) {}
    Program parse();
};
struct Options {
    int optimization = 0;
    std::string ir, object;
};
void compile(Program &program, const std::string &filename, const Options &options);
} // namespace fw
