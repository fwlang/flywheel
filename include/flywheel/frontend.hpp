#pragma once
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fw {
struct Location {
    int line = 1, column = 1;
    std::string file;
};
struct Error : std::runtime_error {
    Location loc;
    Error(Location p, const std::string &s) : std::runtime_error(s), loc(p) {}
};
struct Token {
    std::string text, kind;
    Location loc;
    int depth = 0; // open delimiters before the token; newlines inside lambdas use it
};
std::vector<Token> lex(const std::string &source);
struct Function;
struct Expr {
    std::string kind, text;
    bool grouped = false, outer = false;
    Location loc;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<std::string> names;
    std::shared_ptr<Function> lambda;
};
using EP = std::unique_ptr<Expr>;
struct Stmt {
    std::string kind, name, type, op;
    Location loc;
    EP expr, target;
    std::vector<std::unique_ptr<Stmt>> body, other, finalizer;
    std::vector<std::string> catchTypes, catchNames;
    std::vector<std::vector<std::unique_ptr<Stmt>>> catches;
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
    std::vector<std::string> importOrder;
    std::map<std::string, Location> importLocations;
    bool html = false;
    std::set<std::string> wildcards, privateNames;
};
class Parser {
    std::vector<Token> tokens;
    size_t pos = 0;
    std::vector<int> bodyDepths;
    size_t index(size_t n);
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
    EP lambda();

  public:
    explicit Parser(std::vector<Token> t) : tokens(std::move(t)) {}
    Program parse();
};
Program loadProgram(const std::string &filename);
struct Options {
    int optimization = 0;
    std::string ir, object, desktopBackend;
    std::string *objectData = nullptr; // receives the object file in memory when set
};
void compile(Program &program, const std::string &filename, const Options &options);
} // namespace fw
