#pragma once
#include <cstdint>
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
    bool spaced = true; // whitespace (or the start of input) precedes the token
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
    // match: catches holds case bodies; each case has value patterns, or a type pattern in
    // catchTypes with an optional binding in catchNames, and an optional guard.
    std::vector<std::vector<EP>> patterns;
    std::vector<EP> guards;
};
using SP = std::unique_ptr<Stmt>;
struct Param {
    std::string name, type;
    EP value;
    Location loc;
    bool isPrivate = false, readonly = false; // class fields only
};
struct Function {
    std::string name, result, owner;
    Location loc;
    std::vector<Param> params;
    std::vector<SP> body;
    bool abstract = false; // interface method signature without a body
    bool isPrivate = false; // methods only
    std::vector<std::string> typeParams;
    // Constraints on type parameters (`T implements Comparable`), by parameter name.
    std::map<std::string, std::string> typeBounds;
};
struct Class {
    std::string name, base;
    Location loc;
    std::vector<Param> fields;
    std::vector<Function> methods;
    std::vector<std::string> interfaces, typeParams;
    std::map<std::string, std::string> typeBounds;
    bool isInterface = false;
    // Enums with data become a base class whose subclasses are the variants: `variants` on the
    // base lists them, and each variant class names its enum in `variantOf`.
    std::vector<std::string> variants;
    std::string variantOf;
};
struct Enum {
    std::string name;
    Location loc;
    std::vector<std::string> members;
};
// A markup element family (`elements name ... end`): tag names with their allowed attributes.
struct FamilyTag {
    std::set<std::string> attributes;
    bool anyAttribute = false, isVoid = false;
};
struct ElementFamily {
    std::string name;
    Location loc;
    std::map<std::string, FamilyTag> tags;
};
struct Program {
    std::vector<Function> functions;
    std::vector<Class> classes;
    std::vector<Enum> enums;
    std::vector<ElementFamily> families;
    bool svg = false;
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
    std::vector<std::string> typeParameters(std::map<std::string, std::string> *bounds = nullptr);
    std::string typeArguments(bool allowSpace = false);
    std::string markupName(); // a tag or attribute name such as my-card or stroke-width
    EP expression(int precedence = 0);
    EP prefix();
    EP outer();
    EP markup();
    EP stringExpr(const Token &t);
    SP statement();
    std::vector<SP> block();
    std::vector<SP> pending; // extra statements produced by the last statement (destructuring)
    int temporaries = 0;
    Function function(const std::string &owner = "", bool signatureOnly = false);
    EP lambda();

  public:
    explicit Parser(std::vector<Token> t) : tokens(std::move(t)) {}
    Program parse();
};
Program loadProgram(const std::string &filename);
EP clone(const Expr &e);
SP clone(const Stmt &s);
Function clone(const Function &f);
Class clone(const Class &c);
struct Options {
    int optimization = 0;
    bool debug = false; // emit DWARF line tables
    bool coverage = false; // count executions of each source line (flywheel test --coverage)
    bool race = false;     // detect data races at run time (--race)
    std::string ir, object, desktopBackend;
    // What a top-level `return` of an element or fragment does: "desktop" shows it in a window,
    // "serve" serves it over HTTP at serveHost:servePort, "check" only type-checks it. Empty
    // (run and build) rejects markup; a top-level `return` of an int exits with that status.
    std::string entry, serveHost = "127.0.0.1";
    int64_t servePort = 8000;
    std::string *objectData = nullptr; // receives the object file in memory when set
};
void compile(Program &program, const std::string &filename, const Options &options);
// Re-indents source by block structure (4 spaces), keeping multi-line tokens unchanged.
std::string format(const std::string &source);
// Language server over stdio (JSON-RPC): diagnostics and document formatting.
int languageServer();
// Loads a program, preferring in-memory `overlay` text for paths it contains.
Program loadProgram(const std::string &filename,
                    const std::map<std::string, std::string> &overlay);
// Library modules written in Flywheel and embedded in the compiler, by module name.
const std::map<std::string, std::string> &libraryModules();
// Installed packages (see src/package.cpp; Pulley installs them). Flywheel only resolves them.
// The executable's path locates the installation-wide modules directory.
void setExecutablePath(const std::string &path);
std::string globalModulesDirectory();
// Directories searched for packages: the nearest .flywheel-modules, then the global one.
std::vector<std::string> moduleDirectories(const std::string &sourceFile);
// Packages visible to a source file: import name -> package directory.
std::map<std::string, std::string> packageRoots(const std::string &sourceFile);
// The main module file of a package: `main` in its pulley.toml, else <name>.fw, main.fw, lib.fw.
std::string packageMain(const std::string &directory, const std::string &name);
// `flywheel doc`: renders documentation comments of a module (and its imports) to HTML.
int docCommand(const std::vector<std::string> &args);
} // namespace fw
