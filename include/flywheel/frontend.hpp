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
    // declare: the negated alias of a bool (`bool enabled/disabled = true` names `disabled`).
    std::string alias;
    Location loc;
    EP expr, target;
    std::vector<std::unique_ptr<Stmt>> body, other, finalizer;
    std::vector<std::string> catchTypes, catchNames;
    std::vector<Location> catchLocations; // try: where each catch clause starts
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
// Function and Class declare noexcept moves because MSVC's std::map move constructor is not
// noexcept. Without these, std::vector<Function> would copy on reallocation, and copying fails
// on the unique_ptrs inside (error "'std::construct_at': no matching overloaded function").
struct Function {
    Function() = default;
    Function(Function &&) noexcept = default;
    Function &operator=(Function &&) noexcept = default;
    std::string name, result, owner;
    Location loc;
    std::vector<Param> params;
    std::vector<SP> body;
    bool abstract = false; // interface method signature without a body
    bool isPrivate = false; // methods only
    std::vector<std::string> typeParams;
    // Constraints on type parameters (`T implements Comparable`), by parameter name.
    std::map<std::string, std::string> typeBounds;
    // Functions of a natively imported C, C++, Rust, or Zig module: the C symbol of the generated
    // shim that the compiler calls instead of emitting a body (see src/native.cpp).
    std::string foreign;
};
struct Class {
    Class() = default;
    Class(Class &&) noexcept = default;
    Class &operator=(Class &&) noexcept = default;
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
// A C, C++, Rust, or Zig source imported with `import c/cxx/rust/zig name`: the functions found in
// it and the generated shim that exposes them to Flywheel through C symbols.
struct ForeignFunction {
    std::string name, result;                // Flywheel types: int, double, bool, string, void
    std::vector<std::string> params;         // Flywheel parameter types
    std::vector<std::string> nativeParams;   // the source's own parameter types
    std::string nativeResult, symbol;
};
struct ForeignModule {
    std::string language, source, shim, key; // key: hash identifying the source and shim
    std::vector<ForeignFunction> functions;
};
// Reads the functions of a foreign source file (language: c, cxx, rust, zig) and generates its shim.
ForeignModule scanForeign(const std::string &language, const std::string &path);
// Compiles foreign modules into link inputs. `shared` builds shared libraries for the in-process
// JIT; otherwise objects or static libraries for linking. Results are cached by content.
struct ForeignBuild {
    std::vector<std::string> inputs, flags;
};
ForeignBuild buildForeign(const std::vector<ForeignModule> &modules, bool shared);
// Sources named by URL (https://, http://, ftp://, ftps://, sftp://, scp://, ssh://, file://).
bool isUrl(const std::string &text);
// Downloads a URL's contents (file:// reads the local file); throws std::runtime_error.
std::string fetchUrl(const std::string &url);
// Like fetchUrl, but returns false instead of throwing when the resource does not exist.
bool tryFetchUrl(const std::string &url, std::string &contents);
// A directory under Flywheel's cache (XDG_CACHE_HOME, ~/.cache, or %LOCALAPPDATA%), created.
std::string cacheDirectory(const std::string &kind);
// A short stable hash (hex) of text, for cache keys and generated symbol names.
std::string contentHash(const std::string &text);
// The local path of a file:// URL, or "" for other text.
std::string fileUrlPath(const std::string &url);
// Stores `contents` in the download cache under the URL's file name and returns the local path.
std::string cacheDownload(const std::string &url, const std::string &contents);
// A unit namespace (`unitns time`) and its units (`unit time.s = 1`). Units apply to every
// numeric type: quantity values such as `15s` are instances of two hidden classes, one for
// integer amounts and one for floating-point amounts (src/units.cpp).
struct UnitNamespace {
    std::string name;
    Location loc;
    std::vector<std::string> units, factors; // in declaration order; factors as written
    std::vector<Location> unitLocations;
    std::string intClass, realClass; // hidden class names
    // Set by the module loader: a unique identifier for the namespace (its qualified hidden
    // class), and for an imported one how the importing module spells its hidden names
    // ("" or "metric.").
    std::string key, ref, module;
};
// The identifier that names namespace `ns` in equivalence methods (__fwu_as__<key>).
std::string unitKey(const UnitNamespace &ns);
// `unitconv 1metric_length.m / 1time.s = 1metric_speed.mps`: relates three namespaces.
struct UnitConversion {
    Location loc;
    // "*" or "/"; "=" for `unitconv 1length.ft = 0.3048metric.length.m`, two namespaces that
    // measure the same thing (only [0] and [1] are used; a namespace may be module-qualified).
    std::string op;
    std::string amounts[3], namespaces[3], units[3];
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
    // Native imports by alias: "c", "cxx", "rust", or "zig" (absent for Flywheel modules).
    std::map<std::string, std::string> importKinds;
    std::vector<ForeignModule> foreign; // merged program: every natively imported module
    std::vector<std::string> importOrder;
    std::map<std::string, Location> importLocations;
    bool html = false;
    std::set<std::string> wildcards, privateNames;
    std::vector<UnitNamespace> unitNamespaces;
    std::vector<UnitConversion> unitConversions; // consumed by lowerUnits
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
    bool aliasDeclaration(); // `bool name/negated = ...`
    std::string type();
    std::vector<std::string> typeParameters(std::map<std::string, std::string> *bounds = nullptr);
    std::string typeArguments(bool allowSpace = false);
    std::string markupName(); // a tag or attribute name such as my-card or stroke-width
    EP expression(int precedence = 0);
    EP operators(EP e, int precedence);
    EP which(EP sequence);
    int whichDepth = 0; // inside a `which` condition, `.name` and a leading comparison use the item
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
    // The module loader lowers units itself, once the imported modules' namespaces are known.
    bool deferUnits = false;
};
Program loadProgram(const std::string &filename);
// Lowers reactive variables (`reactive`, `computed`, `when`) and negated bool aliases into
// ordinary declarations, closures, and watcher lists (src/lower.cpp). Parser::parse runs it.
void lowerReactive(Program &program);
// Unit namespaces (`unitns`, `unit`, `unitconv`) become hidden quantity classes (src/units.cpp).
// `imported` are the namespaces of imported modules, which `unitconv` may relate; their hidden
// names are the unqualified ones, which the importing module resolves. When `strict` is false
// (parsing a file on its own), relations naming namespaces it does not know are skipped.
void lowerUnits(Program &program, const std::vector<UnitNamespace> &imported = {},
                bool strict = false);
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
    // "value" (export, debug, flywheel.run) accepts a return of any value and exits with 0.
    std::string entry, serveHost = "127.0.0.1";
    int64_t servePort = 8000;
    std::string *objectData = nullptr; // receives the object file in memory when set
    // The compiler executable, which the `flywheel` module runs (flywheel.run and friends).
    std::string compilerPath;
    // Receives warnings (such as a `catch "Name"` that nothing throws) when set; otherwise
    // they are printed to stderr as file:line:column: warning: message.
    std::vector<Error> *warnings = nullptr;
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
