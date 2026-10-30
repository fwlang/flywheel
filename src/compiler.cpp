#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <llvm/Config/llvm-config.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <optional>
#include <deque>
#include <filesystem>
#include <functional>
#include <set>
#include <unordered_map>

namespace fw {
namespace {
std::string displayName(const std::string &text) {
    std::string result;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 8, "$native$") == 0) {
            i += 8;
            continue;
        }
        if (text.compare(i, 2, "$m") == 0) {
            size_t end = i + 2;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
                ++end;
            if (end > i + 2 && end < text.size() && text[end] == '$') {
                i = end + 1;
                continue;
            }
        }
        result += text[i++];
    }
    // `int` is the source spelling of i64.
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (auto at = result.find("i64"); at != std::string::npos; at = result.find("i64", at + 1))
        if ((at == 0 || !word(result[at - 1])) &&
            (at + 3 == result.size() || !word(result[at + 3])))
            result.replace(at, 3, "int");
    // function<R,A,B> is spelled function(A, B) returns R in source and diagnostics.
    for (auto at = result.find("function<"); at != std::string::npos;
         at = result.find("function<", at + 1)) {
        size_t end = at + 9;
        int depth = 1;
        std::vector<std::string> parts;
        size_t begin = end;
        for (; end < result.size() && depth; ++end) {
            char c = result[end];
            if (c == '<' || c == '[' || c == '(')
                ++depth;
            else if (c == '>' || c == ']' || c == ')')
                --depth;
            if ((c == ',' && depth == 1) || depth == 0) {
                parts.push_back(result.substr(begin, end - begin));
                begin = end + 1;
            }
        }
        if (depth || parts.empty())
            break;
        std::string spelled = "function(";
        for (size_t i = 1; i < parts.size(); ++i)
            spelled += (i > 1 ? ", " : "") + parts[i];
        spelled += ")";
        if (parts[0] != "void")
            spelled += " returns " + parts[0];
        // Suffixes after a function type need grouping: (function() returns int)[].
        if (end < result.size() && (result[end] == '[' || result[end] == '?'))
            spelled = "(" + spelled + ")";
        result.replace(at, end - at, spelled);
    }
    return result;
}
std::vector<std::string> typeParts(const std::string &text) {
    std::vector<std::string> result;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '<' || c == '[' || c == '(')
            ++depth;
        else if (c == '>' || c == ']' || c == ')')
            --depth;
        else if (c == ',' && depth == 0) {
            result.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size())
        result.push_back(text.substr(start));
    return result;
}
const std::set<std::string> errorTypes = {
    "Error",         "RuntimeError",   "FileError",    "IndexError",  "KeyError",
    "NumericError",  "AssertionError", "JsonError",    "ResultError", "CastError",
    "SelectorError", "InputError",     "ProcessError", "DesktopError", "RegexError",
    "NetworkError",  "ValueError",     "HttpError",    "DatabaseError"};
// Enum type names; enum values are i64 member indices.
std::set<std::string> &enumNames() {
    static std::set<std::string> names;
    return names;
}
struct Ty {
    std::string name = "void";
    bool enumeration() const {
        return enumNames().count(name) > 0;
    }
    bool operator==(const Ty &t) const {
        return name == t.name;
    }
    bool operator!=(const Ty &t) const {
        return !(*this == t);
    }
    bool integer() const {
        return name == "i8" || name == "i16" || name == "i32" || name == "i64" || name == "u8" ||
               name == "u16" || name == "u32" || name == "u64";
    }
    bool unsigned_() const {
        return integer() && name[0] == 'u';
    }
    bool real() const {
        return name == "float" || name == "double";
    }
    bool numeric() const {
        return integer() || real();
    }
    bool nullable() const {
        return name == "null" || (!name.empty() && name.back() == '?');
    }
    bool array() const {
        return name.size() > 2 && name.substr(name.size() - 2) == "[]";
    }
    bool homogeneousTuple() const {
        return name.rfind("tuple<", 0) == 0 && name.back() == '>';
    }
    bool sequence() const {
        return array() || homogeneousTuple();
    }
    bool tuple() const {
        return !name.empty() && !array() && name.front() == '[' && name.back() == ']';
    }
    bool map() const {
        return name.rfind("map<", 0) == 0 && name.back() == '>';
    }
    bool set() const {
        return name.rfind("set<", 0) == 0 && name.back() == '>';
    }
    bool result() const {
        return name.rfind("Result<", 0) == 0 && name.back() == '>';
    }
    // function<R,A...>: an ARC-managed closure taking A... and returning R.
    bool function() const {
        return name.rfind("function<", 0) == 0 && name.back() == '>';
    }
    bool weak() const {
        return name.rfind("weak<", 0) == 0 && name.back() == '>';
    }
    std::vector<Ty> parts() const {
        size_t first = tuple() ? 1 : name.find('<') + 1;
        std::vector<Ty> r;
        for (auto &p : typeParts(name.substr(first, name.size() - first - 1)))
            r.push_back(Ty{p});
        return r;
    }
    Ty item() const {
        return homogeneousTuple() ? parts()[0] : Ty{name.substr(0, name.size() - 2)};
    }
    int bits() const {
        if (integer())
            return std::stoi(name.substr(1));
        return name == "float" ? 32 : 64;
    }
    bool object() const {
        return !numeric() && name != "bool" && name != "char" && name != "void" &&
               !enumeration() && name.rfind("fn:", 0) != 0 &&
               name.rfind("module:", 0) != 0 && !classReference();
    }
    bool classReference() const {
        return name.rfind("class:", 0) == 0;
    }
};
struct Val {
    llvm::Value *v = nullptr;
    Ty t;
};
struct Var {
    llvm::Value *address;
    Ty type;
    bool constant = false;
    llvm::Value *initialized = nullptr;
    bool captured = false;
    // A narrowed view of a nullable variable (after `x != null`): `type` is the non-null type,
    // `storage` the declared one. Reads are null-checked. Only owners release at scope exit.
    bool narrowed = false, owner = true;
    // Narrowed by `x is T`; writes are rejected. Globals can change during calls, so their
    // narrowed reads re-check the class (`checked`).
    bool typeNarrowed = false, checked = false;
    // A variable that lambdas capture and someone assigns lives in a shared heap cell
    // (a one-slot Box) so every closure and the declaring scope see the same value.
    bool cell = false;
    Ty storage = Ty{};
};
bool primitive(const Ty &t) {
    return t.numeric() || t.name == "bool" || t.name == "char" || t.enumeration();
}
Ty nonNull(const Ty &t) {
    return t.nullable() && t.name != "null" ? Ty{t.name.substr(0, t.name.size() - 1)} : t;
}
// A lambda being compiled. Enclosing locals it reads are copied into its environment.
struct LambdaContext {
    std::vector<std::map<std::string, Var>> *outerScopes = nullptr;
    llvm::Function *function = nullptr;
    llvm::Value *frame = nullptr, *env = nullptr;
    llvm::Instruction *initPoint = nullptr;
    std::vector<std::pair<std::string, Var>> captures;
    std::map<std::string, Var> captured;
};
struct Signature {
    Function *ast = nullptr;
    Ty result;
    llvm::Function *function = nullptr;
    bool building = false, done = false;
};
class Compiler {
    Program &p;
    llvm::LLVMContext ctx;
    llvm::Module module;
    llvm::IRBuilder<> b;
    std::unique_ptr<llvm::TargetMachine> machine;
    llvm::Function *current = nullptr;
    llvm::Value *frame = nullptr;
    std::vector<std::map<std::string, Var>> scopes;
    std::map<std::string, Var> globals;
    std::map<std::string, Signature> functions;
    std::map<std::string, Class *> classes;
    std::map<std::string, Enum *> enums;
    std::map<std::string, int64_t> enumIds;
    std::map<std::string, Class *> templates;
    std::map<std::string, Function *> functionTemplates;
    std::map<std::string, Function *> methodTemplates; // "Class.method" -> generic method
    std::deque<Class> instances;
    std::deque<Function> functionInstances;
    std::map<std::string, int> classStates; // 1 while flattening, 2 when complete
    std::map<std::string, llvm::Constant *> classIds;
    std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> loops;
    std::vector<std::map<std::string, Val>> attrs;
    std::set<std::string> componentStack;
    std::vector<std::string> componentNames;
    std::map<std::string, std::map<std::string, Ty>> componentTypes;
    Ty returnType;
    bool infer = false, top = false;
    std::string owner, desktopBackend;
    std::string method; // name of the function or method being emitted
    // Coverage: one counter per executed source line, keyed (file, line).
    bool coverage = false, race = false;
    std::map<std::pair<std::string, int>, llvm::GlobalVariable *> coverageCounters;
    // --race: reports reads and writes of a module global to the race detector.
    void raceGlobal(Var &v, bool write) {
        if (!race || !v.initialized)
            return;
        auto name = v.address->getName().str();
        if (name.rfind("global.", 0) == 0)
            name = displayName(name.substr(7));
        call("fw_race_global", b.getVoidTy(), {v.address, b.CreateGlobalString(name), i64(write)});
    }
    void countLine(Location loc) {
        if (!coverage || loc.file.empty() || loc.file.rfind("/<flywheel>/", 0) == 0 ||
            !b.GetInsertBlock() || terminated())
            return;
        auto &counter = coverageCounters[{loc.file, loc.line}];
        if (!counter)
            counter = new llvm::GlobalVariable(module, b.getInt64Ty(), false,
                                               llvm::GlobalValue::InternalLinkage, i64(0),
                                               "fw.coverage");
        b.CreateStore(b.CreateAdd(b.CreateLoad(b.getInt64Ty(), counter), i64(1)), counter);
    }
    // Passes the counters and their file/line table to the runtime when main starts.
    void registerCoverage(llvm::Function *mainFunction) {
        std::vector<llvm::Constant *> pointers;
        std::string table;
        for (auto &[key, counter] : coverageCounters) {
            pointers.push_back(counter);
            table += key.first + "\t" + std::to_string(key.second) + "\n";
        }
        auto arrayType = llvm::ArrayType::get(b.getPtrTy(), pointers.size());
        auto array = new llvm::GlobalVariable(module, arrayType, true,
                                              llvm::GlobalValue::InternalLinkage,
                                              llvm::ConstantArray::get(arrayType, pointers),
                                              "fw.coverage.counters");
        auto saved = b.saveIP();
        auto debug = b.getCurrentDebugLocation();
        auto &entry = mainFunction->getEntryBlock();
        b.SetInsertPoint(&entry, entry.getFirstInsertionPt());
        b.SetCurrentDebugLocation(entry.getFirstInsertionPt()->getDebugLoc());
        auto f = module.getOrInsertFunction(
            "fw_coverage_register",
            llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy(), b.getInt64Ty(), b.getPtrTy()}, false));
        b.CreateCall(f, {array, i64(static_cast<int64_t>(pointers.size())), b.CreateGlobalString(table)});
        b.restoreIP(saved);
        b.SetCurrentDebugLocation(debug);
    }
    // Declaring class of each private or readonly field, keyed "Class.field".
    std::map<std::string, std::string> fieldOwners;
    std::vector<Ty> returns;
    Location location;
    uint64_t tempGeneration = 0;
    bool borrowed = false;
    struct Handler {
        llvm::BasicBlock *target;
        size_t depth;
    };
    struct Finalizer {
        std::vector<SP> *body;
        Handler outer;
        size_t loopDepth;
        size_t depth;
    };
    std::vector<Handler> handlers;
    std::vector<Finalizer> finalizers;
    std::vector<size_t> loopScopes;
    llvm::BasicBlock *failure = nullptr;
    struct ProtectedValue {
        llvm::Value *address;
        size_t depth;
    };
    std::vector<ProtectedValue> protectedValues;
    std::vector<LambdaContext *> lambdaStack;
    std::set<std::string> cells; // locals of the current function stored in shared cells
    std::map<std::string, llvm::Function *> trampolines;
    // -g: DWARF line tables for user functions, lambdas, and the program body.
    std::unique_ptr<llvm::DIBuilder> dib;
    llvm::DICompileUnit *unit = nullptr;
    std::map<std::string, llvm::DIFile *> debugFiles;
    std::map<std::string, llvm::DIType *> debugObjectTypes;
    std::string mainFile;
    llvm::DIFile *debugFile(const std::string &path) {
        auto name = path.empty() ? mainFile : path;
        auto &file = debugFiles[name];
        if (!file) {
            auto native = std::filesystem::path(name);
            file = dib->createFile(native.filename().string(), native.parent_path().string());
        }
        return file;
    }
    void debugFunction(llvm::Function *f, const std::string &name, Location loc) {
        if (!dib)
            return;
        auto file = debugFile(loc.file);
        auto line = static_cast<unsigned>(std::max(1, loc.line));
        auto type = dib->createSubroutineType(dib->getOrCreateTypeArray({}));
        f->setSubprogram(dib->createFunction(file, displayName(name), f->getName(), file, line,
                                             type, line, llvm::DINode::FlagZero,
                                             llvm::DISubprogram::SPFlagDefinition));
        debugLocation();
    }
    // Points the builder at `location` inside the current function's subprogram, if any.
    void debugLocation() {
        if (!dib)
            return;
        auto scope = current ? current->getSubprogram() : nullptr;
        if (!scope) {
            b.SetCurrentDebugLocation(llvm::DebugLoc());
            return;
        }
        b.SetCurrentDebugLocation(llvm::DILocation::get(
            ctx, static_cast<unsigned>(std::max(1, location.line)),
            static_cast<unsigned>(std::max(0, location.column)), scope));
    }
    uint64_t lambdaCount = 0;
    // Per-function code generation state, saved while another function body is emitted.
    struct FunctionState {
        llvm::IRBuilderBase::InsertPoint ip;
        bool borrowed, infer, top;
        llvm::Function *current;
        llvm::Value *frame;
        std::vector<std::map<std::string, Val>> attrs;
        std::vector<std::string> componentNames;
        std::vector<std::map<std::string, Var>> scopes;
        std::vector<Handler> handlers;
        std::vector<Finalizer> finalizers;
        std::vector<size_t> loopScopes;
        llvm::BasicBlock *failure;
        std::vector<ProtectedValue> protectedValues;
        std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> loops;
        std::string owner;
        Ty returnType;
        std::vector<Ty> returns;
        Location location;
        std::vector<LambdaContext *> lambdaStack;
        std::set<std::string> cells;
        llvm::DebugLoc debug;
        std::string method;
    };
    FunctionState saveFunction() {
        FunctionState s{b.saveIP(),
                        borrowed,
                        infer,
                        top,
                        current,
                        frame,
                        std::move(attrs),
                        std::move(componentNames),
                        std::move(scopes),
                        std::move(handlers),
                        std::move(finalizers),
                        std::move(loopScopes),
                        failure,
                        std::move(protectedValues),
                        std::move(loops),
                        owner,
                        returnType,
                        std::move(returns),
                        location,
                        std::move(lambdaStack),
                        std::move(cells),
                        b.getCurrentDebugLocation(),
                        method};
        b.SetCurrentDebugLocation(llvm::DebugLoc());
        borrowed = false;
        attrs.clear();
        componentNames.clear();
        scopes.clear();
        handlers.clear();
        finalizers.clear();
        loopScopes.clear();
        protectedValues.clear();
        loops.clear();
        returns.clear();
        lambdaStack.clear();
        cells.clear();
        return s;
    }
    void restoreFunction(FunctionState &s) {
        borrowed = s.borrowed;
        infer = s.infer;
        top = s.top;
        current = s.current;
        frame = s.frame;
        attrs = std::move(s.attrs);
        componentNames = std::move(s.componentNames);
        scopes = std::move(s.scopes);
        handlers = std::move(s.handlers);
        finalizers = std::move(s.finalizers);
        loopScopes = std::move(s.loopScopes);
        failure = s.failure;
        protectedValues = std::move(s.protectedValues);
        loops = std::move(s.loops);
        owner = s.owner;
        returnType = s.returnType;
        returns = std::move(s.returns);
        location = s.location;
        lambdaStack = std::move(s.lambdaStack);
        cells = std::move(s.cells);
        method = s.method;
        b.restoreIP(s.ip);
        b.SetCurrentDebugLocation(s.debug);
    }
    void releaseProtected(size_t depth) {
        for (auto &value : protectedValues)
            if (value.depth >= depth)
                call("fw_store", b.getVoidTy(),
                     {value.address, llvm::ConstantPointerNull::get(b.getPtrTy())});
    }
    [[noreturn]] void error(const std::string &s) {
        throw Error(location, displayName(s));
    }
    llvm::Type *ll(Ty t) {
        if (t.integer())
            return llvm::IntegerType::get(ctx, t.bits());
        if (t.name == "bool")
            return b.getInt1Ty();
        if (t.name == "char")
            return b.getInt32Ty();
        if (t.enumeration())
            return b.getInt64Ty();
        if (t.name == "float")
            return b.getFloatTy();
        if (t.name == "double")
            return b.getDoubleTy();
        if (t.name == "void")
            return b.getVoidTy();
        return b.getPtrTy();
    }
    Ty type(std::string name) {
        if (auto open = name.find('<'); open != std::string::npos && name.back() == '>' &&
                                        templates.count(name.substr(0, open))) {
            std::vector<Ty> arguments;
            for (auto &part : typeParts(name.substr(open + 1, name.size() - open - 2)))
                arguments.push_back(type(part));
            return Ty{instantiateClass(name.substr(0, open), arguments)};
        }
        if (templates.count(name))
            error("generic class " + displayName(name) + " needs type arguments, such as " +
                  displayName(name) + "<int>");
        for (std::string family : {"thread.task", "thread.channel"}) {
            if (name == family)
                error(family + " needs a type argument, such as " + family + "<int>");
            if (name.rfind(family + "<", 0) == 0 && name.back() == '>') {
                auto inner = name.substr(family.size() + 1, name.size() - family.size() - 2);
                auto payload = inner == "void" && family == "thread.task" ? Ty{} : type(inner);
                if (payload.name == "void" && family == "thread.channel")
                    error("channels cannot carry void");
                return Ty{family + "<" + payload.name + ">"};
            }
        }
        if (name == "thread.mutex")
            return Ty{name};
        if (name.rfind("array<", 0) == 0 && name.back() == '>') {
            auto parts = Ty{name}.parts();
            if (parts.size() != 1)
                error("array<T> requires one element type");
            return type(parts[0].name + "[]");
        }
        if (name == "int")
            name = "i64";
        if (name.size() > 2 && name.substr(name.size() - 2) == "[]")
            return Ty{type(name.substr(0, name.size() - 2)).name + "[]"};
        if (name.size() > 1 && name.back() == '?') {
            Ty inner = type(name.substr(0, name.size() - 1));
            if (inner.name == "void" || inner.nullable())
                error("'?' requires a non-void, non-nullable type");
            if (!inner.object() && !primitive(inner))
                error("type '" + inner.name + "' cannot be nullable");
            return Ty{inner.name + "?"};
        }
        Ty t{name};
        if (t.function()) {
            auto parts = t.parts();
            std::string r = "function<" + type(parts[0].name).name;
            for (size_t i = 1; i < parts.size(); ++i) {
                auto part = type(parts[i].name);
                if (part.name == "void")
                    error("function parameters cannot be void");
                r += "," + part.name;
            }
            return Ty{r + ">"};
        }
        if (t.weak()) {
            auto parts = t.parts();
            if (parts.size() != 1)
                error("weak<T> requires one type");
            auto target = type(parts[0].name);
            if (!target.object() || target.nullable() || target.weak())
                error("weak<T> requires a non-null reference type such as a class or element");
            return Ty{"weak<" + target.name + ">"};
        }
        if (t.set()) {
            auto parts = t.parts();
            if (parts.size() != 1)
                error("set<T> requires one element type");
            auto item = type(parts[0].name);
            if (!hashable(item))
                error("set elements must be integers, bool, char, enum, string, tuples of these, "
                      "or classes with hash() and equals()");
            return Ty{"set<" + item.name + ">"};
        }
        if (t.tuple() || t.homogeneousTuple() || t.map() || t.result()) {
            auto parts = t.parts();
            if ((t.map() || t.result()) && parts.size() != 2)
                error("generic collection/Result requires two types");
            if (t.homogeneousTuple() && parts.size() != 1)
                error("tuple<T> requires one element type");
            std::string r = t.tuple()              ? "["
                            : t.map()              ? "map<"
                            : t.homogeneousTuple() ? "tuple<"
                                                   : "Result<";
            for (size_t i = 0; i < parts.size(); ++i) {
                auto part = type(parts[i].name);
                if (part.name == "void")
                    error("collection and Result payloads cannot be void");
                if (i)
                    r += ",";
                r += part.name;
            }
            r += t.tuple() ? "]" : ">";
            Ty out{r};
            if (out.map()) {
                auto key = out.parts()[0];
                if (!hashable(key))
                    error("map keys must be integers, bool, char, enum, string, tuples of these, "
                          "or classes with hash() and equals()");
            }
            return out;
        }
        if (t.numeric() || name == "bool" || name == "char" || name == "string" || name == "void" ||
            name == "element" || name == "fragment" || name == "json.value" ||
            name == "desktop.window" || name == "io.file" || name == "net.socket" ||
            name == "net.listener" || name == "sqlite.database" || errorTypes.count(name) ||
            classes.count(name) ||
            t.enumeration())
            return t;
        error("unknown or unsupported type '" + name + "'");
    }
    // Runtime slot kind for map keys: 0 signed, 1 unsigned, 3 bool, 4 reference, 5 char.
    bool hashable(Ty t) {
        if (t.integer() || t.name == "bool" || t.name == "char" || t.name == "string" ||
            t.enumeration() || classes.count(t.name) || templates.count(t.name))
            return true;
        if (t.tuple() || t.homogeneousTuple()) {
            for (auto &part : t.parts())
                if (!hashable(type(part.name)) || classes.count(type(part.name).name))
                    return false;
            return true;
        }
        return false;
    }
    // Enum slots (kind 6) carry their enum id in the upper bits; 7 marks tuple keys and 8 class
    // keys hashed by their hash() method.
    int64_t mapKeyKind(Ty key) {
        if (key.tuple() || key.homogeneousTuple())
            return 7;
        if (classes.count(key.name)) {
            if (resolveMethod(key.name, "hash").empty() || resolveMethod(key.name, "equals").empty())
                error("class " + displayName(key.name) +
                      " is used as a map key or set element, so it must define "
                      "hash() returns int and equals(" + displayName(key.name) +
                      " other) returns bool");
            return 8;
        }
        return key.object()           ? 4
               : key.name == "bool"   ? 3
               : key.name == "char"   ? 5
               : key.enumeration()    ? 6 | (enumIds.at(key.name) << 8)
               : key.unsigned_()      ? 1
                                      : 0;
    }
    llvm::ConstantInt *i64(int64_t x) {
        return b.getInt64(x);
    }
    Handler errorHandler() {
        return handlers.empty() ? Handler{failure, 0} : handlers.back();
    }
    void releaseScopes(size_t depth) {
        for (size_t i = scopes.size(); i > depth; --i)
            for (auto &[name, var] : scopes[i - 1]) {
                (void)name;
                if (var.owner && (var.cell || (var.narrowed ? var.storage : var.type).object()))
                    call("fw_store", b.getVoidTy(),
                         {var.address, llvm::ConstantPointerNull::get(b.getPtrTy())});
            }
    }
    void checkError() {
        auto handler = errorHandler();
        if (!handler.target)
            return;
        auto status = call("fw_error_pending", b.getInt64Ty(), {});
        auto ok = llvm::BasicBlock::Create(ctx, "success", current);
        auto cleanup = llvm::BasicBlock::Create(ctx, "error.cleanup", current);
        b.CreateCondBr(b.CreateICmpNE(status, i64(0)), cleanup, ok);
        b.SetInsertPoint(cleanup);
        releaseProtected(handler.depth);
        if (handler.target != failure)
            releaseScopes(handler.depth);
        call("fw_sweep", b.getVoidTy(), {frame});
        b.CreateBr(handler.target);
        b.SetInsertPoint(ok);
    }
    bool usesWeb() const {
        return uses("web");
    }
    bool uses(const std::string &module) const {
        for (auto &[alias, name] : p.imports) {
            (void)alias;
            if (name == module)
                return true;
        }
        return false;
    }
    // Builtin classes are ordinary classes. Native code builds and reads them with this exact
    // field order: runtime/web.inc (web.*) and runtime/stdlib.inc (os.*, time.datetime).
    void builtinClasses() {
        std::vector<std::pair<std::string, std::string>> sources;
        if (uses("os"))
            sources.emplace_back("os", R"(class process
    string[] command
    string cwd = ""
    map<string, string> env = {}
    bool inherit_env = true
    string input = ""
    double timeout = 0.0
    bool capture = true
end
class result
    int status = 0
    string stdout = ""
    string stderr = ""
    bool timed_out = false
end
)");
        if (uses("time"))
            sources.emplace_back("time", R"(class datetime
    int year
    int month
    int day
    int hour = 0
    int minute = 0
    int second = 0
    int weekday = 0
    int yearday = 0
end
)");
        for (auto &[module, source] : sources) {
            auto program = Parser(lex(source)).parse();
            for (auto &c : program.classes) {
                c.name = module + "." + c.name;
                builtinClassNames.push_back(c.name);
                p.classes.push_back(std::move(c));
            }
        }
        if (usesWeb())
            webClasses();
    }
    std::vector<std::string> builtinClassNames;
    // web.request and web.response are ordinary classes; runtime/web.inc relies on this
    // field order when it builds requests and reads responses.
    void webClasses() {
        builtinClassNames.push_back("web.request");
        builtinClassNames.push_back("web.response");
        auto program = Parser(lex(R"(class Request
    string method = "GET"
    string path = "/"
    map<string, string> query = {}
    map<string, string> headers = {}
    string body = ""
    map<string, string> params = {}
    map<string, string> form = {}
end
class Response
    int status = 200
    string body = ""
    map<string, string> headers = {}
end
)"))
                           .parse();
        program.classes[0].name = "web.request";
        program.classes[1].name = "web.response";
        for (auto &c : program.classes)
            p.classes.push_back(std::move(c));
    }
    void emitFailure() {
        auto saved = b.saveIP();
        b.SetInsertPoint(failure);
        if (top && usesWeb())
            call("fw_web_shutdown", b.getVoidTy(), {});
        if (top)
            call("fw_error_report", b.getVoidTy(), {});
        call("fw_drop", b.getVoidTy(), {frame});
        if (top)
            b.CreateRet(b.getInt32(1));
        else if (infer || returnType.name == "void")
            b.CreateRetVoid();
        else
            b.CreateRet(llvm::Constant::getNullValue(ll(returnType)));
        b.restoreIP(saved);
    }
    llvm::Value *call(const std::string &name, llvm::Type *result,
                      std::vector<llvm::Value *> args) {
        std::vector<llvm::Type *> types;
        for (auto a : args)
            types.push_back(a->getType());
        auto f = module.getOrInsertFunction(name, llvm::FunctionType::get(result, types, false));
        auto resultValue = b.CreateCall(f, args);
        static const std::set<std::string> safe = {
            "fw_frame",         "fw_drop",          "fw_root",          "fw_sweep",
            "fw_store",         "fw_retain",        "fw_adopt",         "fw_keep",
            "fw_error_pending", "fw_error_matches", "fw_throw",         "fw_error_report",
            "fw_error_value",   "fw_error_take",    "fw_error_restore", "fw_web_shutdown",
            "fw_class_id",      "fw_enum_register", "fw_register_class", "fw_set_args",
            "fw_gc_collect",    "fw_gc_pending"};
        if (!safe.count(name))
            checkError();
        return resultValue;
    }
    llvm::Value *objectCall(const std::string &name, std::vector<llvm::Value *> args) {
        if (name != "fw_frame")
            ++tempGeneration;
        return call(name, b.getPtrTy(), std::move(args));
    }
    Val literal(const std::string &s) {
        return {objectCall("fw_literal",
                           {frame, b.CreateGlobalString(llvm::StringRef(s.data(), s.size())),
                            i64(static_cast<int64_t>(s.size()))}),
                Ty{"string"}};
    }
    void guard(llvm::Value *condition, const std::string &message,
               const std::string &errorType = "NumericError") {
        auto ok = llvm::BasicBlock::Create(ctx, "checked", current);
        auto bad = llvm::BasicBlock::Create(ctx, "error", current);
        b.CreateCondBr(condition, ok, bad);
        b.SetInsertPoint(bad);
        call("fw_panic", b.getVoidTy(),
             {b.CreateGlobalString(message), b.CreateGlobalString(errorType)});
        b.CreateUnreachable();
        b.SetInsertPoint(ok);
    }
    bool widening(Ty from, Ty to) {
        if (from == to)
            return true;
        if (from.integer() && to.integer()) {
            if (from.unsigned_() == to.unsigned_())
                return from.bits() < to.bits();
            return from.unsigned_() && !to.unsigned_() && from.bits() < to.bits();
        }
        if (from.name == "float" && to.name == "double")
            return true;
        if (from.integer() && to.real())
            return from.bits() - (from.unsigned_() ? 0 : 1) <= (to.name == "float" ? 24 : 53);
        return false;
    }
    Val convert(Val x, Ty to, bool explicit_ = false) {
        if (x.t == to)
            return x;
        if (x.t.classReference() && to.function())
            return constructorValue(x.t.name.substr(6), to);
        if (x.t.classReference())
            error("'" + displayName(x.t.name.substr(6)) +
                  "' is a class reference, not an instance; call it with () to construct one");
        if (to.nullable() && primitive(nonNull(to)) && x.t.name != "null") {
            // Present nullable primitives are boxed; widening applies to the payload.
            if (!primitive(x.t))
                error("cannot convert " + x.t.name + " to " + to.name);
            auto payload = convert(x, nonNull(to), explicit_);
            Val box{objectCall("fw_new", {frame, i64(1), i64(12)}), to};
            set(box, i64(0), payload);
            return box;
        }
        if (x.t.nullable() && primitive(nonNull(x.t)) && primitive(to)) {
            if (!explicit_)
                error("cannot implicitly convert " + x.t.name + " to " + to.name +
                      "; check for null, use '?\?', or cast explicitly");
            return convert(unboxChecked(x), to, true);
        }
        if (explicit_ && x.t.nullable() && x.t.name == to.name + "?") {
            guard(b.CreateICmpNE(x.v, llvm::ConstantPointerNull::get(b.getPtrTy())),
                  "cannot cast null to " + displayName(to.name), "CastError");
            return {x.v, to};
        }
        if (explicit_ && x.t.name == "bool" && to.numeric())
            return convert({b.CreateZExt(x.v, b.getInt8Ty()), Ty{"u8"}}, to, true);
        if (to.name == "Error" && errorTypes.count(x.t.name))
            return {x.v, to};
        if (x.t.name == "null" && to.nullable())
            return {x.v, to};
        if (to.nullable() && to.name == x.t.name + "?")
            return {x.v, to};
        if (x.t.name == "fragment" && to.name == "element")
            return {x.v, to};
        if (classes.count(nonNull(x.t).name) && classes.count(nonNull(to).name) &&
            derives(nonNull(x.t).name, nonNull(to).name) && (to.nullable() || !x.t.nullable()))
            return {x.v, to};
        if (x.t.name.rfind("fn:", 0) == 0 && to.function())
            return functionValue(x.t.name.substr(3), to);
        if (to.weak() && (x.t.name == "null" || x.t.name == to.parts()[0].name ||
                          x.t.name == to.parts()[0].name + "?"))
            return {objectCall("fw_weak_new", {frame, x.v}), to};
        if (x.t.numeric() && to.numeric()) {
            bool fits = false;
            if (auto ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(x.v); ci && to.integer()) {
                auto v = ci->getValue();
                if (x.t.unsigned_())
                    fits = v.getActiveBits() <= unsigned(to.bits() - (to.unsigned_() ? 0 : 1));
                else
                    fits = to.unsigned_()
                               ? (!v.isNegative() && v.getActiveBits() <= unsigned(to.bits()))
                               : v.isSignedIntN(to.bits());
            }
            if (!explicit_ && !widening(x.t, to) && !fits)
                error("cannot implicitly convert " + x.t.name + " to " + to.name +
                      "; use an explicit checked cast");
            if (x.t.integer() && to.integer()) {
                auto out = b.CreateIntCast(x.v, ll(to), x.t.name != "bool" && !x.t.unsigned_());
                if (explicit_ && !widening(x.t, to)) {
                    auto restored = b.CreateIntCast(out, ll(x.t), !to.unsigned_());
                    auto valid = b.CreateICmpEQ(restored, x.v);
                    if (x.t.unsigned_() != to.unsigned_()) {
                        auto nonnegative =
                            x.t.unsigned_()
                                ? b.CreateICmpSGE(out, llvm::ConstantInt::get(ll(to), 0))
                                : b.CreateICmpSGE(x.v, llvm::ConstantInt::get(ll(x.t), 0));
                        valid = b.CreateAnd(valid, nonnegative);
                    }
                    guard(valid, "integer cast out of range");
                }
                return {out, to};
            }
            if (x.t.integer())
                return {x.t.unsigned_() ? b.CreateUIToFP(x.v, ll(to)) : b.CreateSIToFP(x.v, ll(to)),
                        to};
            if (to.real()) {
                if (to.bits() < x.t.bits()) {
                    auto max = llvm::ConstantFP::get(ll(x.t), std::numeric_limits<float>::max());
                    auto min = llvm::ConstantFP::get(ll(x.t), -std::numeric_limits<float>::max());
                    guard(b.CreateAnd(b.CreateFCmpOGE(x.v, min), b.CreateFCmpOLE(x.v, max)),
                          "floating cast out of range");
                }
                return {b.CreateFPCast(x.v, ll(to)), to};
            }
            double low = to.unsigned_() ? 0 : -std::ldexp(1.0, to.bits() - 1);
            double high = std::ldexp(1.0, to.bits() - (to.unsigned_() ? 0 : 1));
            guard(b.CreateAnd(b.CreateFCmpOGE(x.v, llvm::ConstantFP::get(ll(x.t), low)),
                              b.CreateFCmpOLT(x.v, llvm::ConstantFP::get(ll(x.t), high))),
                  "floating to integer cast out of range");
            return {to.unsigned_() ? b.CreateFPToUI(x.v, ll(to)) : b.CreateFPToSI(x.v, ll(to)), to};
        }
        error("cannot convert " + x.t.name + " to " + to.name);
    }
    // Reads the payload of a nullable value; null raises CastError.
    Val unboxChecked(Val x) {
        auto inner = nonNull(x.t);
        guard(b.CreateICmpNE(x.v, llvm::ConstantPointerNull::get(b.getPtrTy())),
              "null value of type " + displayName(x.t.name) + " used as " +
                  displayName(inner.name),
              "CastError");
        if (primitive(inner))
            return get({x.v, x.t}, i64(0), inner);
        return {x.v, inner};
    }
    // Names proven non-null when `condition` evaluates to `positive`.
    void nonNullNames(Expr &condition, bool positive, std::vector<std::string> &names) {
        if (condition.kind == "unary" && condition.text == "not")
            return nonNullNames(*condition.args[0], !positive, names);
        if (condition.kind != "binary")
            return;
        if (condition.text == (positive ? "and" : "or")) {
            nonNullNames(*condition.args[0], positive, names);
            nonNullNames(*condition.args[1], positive, names);
            return;
        }
        if (condition.text != (positive ? "!=" : "=="))
            return;
        auto &left = *condition.args[0], &right = *condition.args[1];
        if (left.kind == "name" && right.kind == "null")
            names.push_back(left.text);
        else if (right.kind == "name" && left.kind == "null")
            names.push_back(right.text);
    }
    // Local names proven to hold class `T` (by `x is T`) when `condition` is `positive`.
    void instanceNames(Expr &condition, bool positive,
                       std::vector<std::pair<std::string, std::string>> &names) {
        if (condition.kind == "unary" && condition.text == "not")
            return instanceNames(*condition.args[0], !positive, names);
        if (condition.kind == "binary" && condition.text == (positive ? "and" : "or")) {
            instanceNames(*condition.args[0], positive, names);
            instanceNames(*condition.args[1], positive, names);
        } else if (positive && condition.kind == "is" && condition.args[0]->kind == "name")
            names.emplace_back(condition.args[0]->text, condition.text);
    }
    void narrowTypes(const std::vector<std::pair<std::string, std::string>> &names) {
        for (auto &[name, typeName] : names) {
            auto variable = lookup(name);
            auto t = type(typeName);
            if (!variable || variable->typeNarrowed || variable->cell || !classes.count(t.name) ||
                !variable->type.object())
                continue;
            Var view = *variable;
            view.narrowed = view.typeNarrowed = true;
            view.checked = variable->initialized != nullptr;
            view.storage = variable->narrowed ? variable->storage : variable->type;
            view.type = t;
            view.owner = false;
            scopes.back()[name] = view;
        }
    }
    // Adds narrowed views of `names` to the innermost scope.
    void narrow(const std::vector<std::string> &names) {
        for (auto &name : names) {
            auto variable = lookup(name);
            if (!variable || variable->narrowed || variable->cell || !variable->type.nullable() ||
                variable->type.name == "null" || variable->type.classReference())
                continue;
            Var view = *variable;
            view.narrowed = true;
            view.storage = variable->type;
            view.type = nonNull(variable->type);
            auto existing = scopes.back().find(name);
            if (existing != scopes.back().end()) {
                view.owner = existing->second.owner;
                existing->second = view;
            } else {
                view.owner = false;
                scopes.back()[name] = view;
            }
        }
    }
    // Nullable references are unwrapped for member access; null raises CastError.
    Val unwrap(Val x) {
        if (!x.t.nullable() || x.t.name == "null")
            return x;
        Ty base{x.t.name.substr(0, x.t.name.size() - 1)};
        if (classes.count(base.name) || base.name == "element" || base.name == "fragment" ||
            base.name == "desktop.window" || base.name == "io.file" || base.name == "net.socket" ||
            base.name == "net.listener" || base.name == "sqlite.database")
            return convert(x, base, true);
        return x;
    }
    // A named function used as a function value: a capture-free closure around a trampoline.
    Val functionValue(const std::string &name, Ty to) {
        auto &sig = signature(name);
        auto parts = to.parts();
        if (!sig.ast->owner.empty())
            error("methods cannot be used as function values yet; wrap the call in a lambda");
        bool same = sig.ast->params.size() + 1 == parts.size() && sig.result == parts[0];
        for (size_t i = 0; same && i < sig.ast->params.size(); ++i)
            same = type(sig.ast->params[i].type) == parts[i + 1];
        if (!same)
            error("function '" + name + "' does not match type " + to.name);
        auto &code = trampolines[name];
        if (!code) {
            std::vector<llvm::Type *> types{b.getPtrTy()};
            for (size_t i = 1; i < parts.size(); ++i)
                types.push_back(ll(parts[i]));
            code = llvm::Function::Create(llvm::FunctionType::get(ll(parts[0]), types, false),
                                          llvm::GlobalValue::InternalLinkage, "fly.ref." + name,
                                          module);
            llvm::IRBuilder<> body(llvm::BasicBlock::Create(ctx, "entry", code));
            std::vector<llvm::Value *> args;
            for (size_t i = 1; i < parts.size(); ++i)
                args.push_back(code->getArg(static_cast<unsigned>(i)));
            auto result = body.CreateCall(sig.function, args);
            if (parts[0].name == "void")
                body.CreateRetVoid();
            else
                body.CreateRet(result);
        }
        return {objectCall("fw_closure_new", {frame, code, i64(0)}), to};
    }
    Val callClosure(Val callee, std::vector<Expr *> arguments) {
        auto parts = callee.t.parts();
        if (arguments.size() + 1 != parts.size())
            error("function value expects " + std::to_string(parts.size() - 1) + " argument(s)");
        std::vector<Val> args;
        for (size_t i = 0; i < arguments.size(); ++i)
            args.push_back(convert(value(*arguments[i], parts[i + 1]), parts[i + 1]));
        return callClosure(callee, args);
    }
    Val callClosure(Val callee, std::vector<Val> args) {
        auto parts = callee.t.parts();
        std::vector<llvm::Type *> types{b.getPtrTy()};
        std::vector<llvm::Value *> values{callee.v};
        for (size_t i = 0; i < args.size(); ++i) {
            types.push_back(ll(parts[i + 1]));
            values.push_back(convert(args[i], parts[i + 1]).v);
        }
        auto code = call("fw_closure_code", b.getPtrTy(), {callee.v});
        auto result =
            b.CreateCall(llvm::FunctionType::get(ll(parts[0]), types, false), code, values);
        checkError();
        if (parts[0].object())
            objectCall("fw_adopt", {frame, result});
        return {result, parts[0]};
    }
    Val stringify(Val x) {
        if (x.t.name == "string")
            return x;
        if (errorTypes.count(x.t.name))
            return {objectCall("fw_error_message", {frame, x.v}), Ty{"string"}};
        if (x.t.name == "element" || x.t.name == "fragment")
            return {objectCall("fw_render", {frame, x.v}), Ty{"string"}};
        if (x.t.integer()) {
            auto y = b.CreateIntCast(x.v, b.getInt64Ty(), x.t.name != "bool" && !x.t.unsigned_());
            return {objectCall(x.t.unsigned_() ? "fw_uint_string" : "fw_int_string", {frame, y}),
                    Ty{"string"}};
        }
        if (x.t.real())
            return {objectCall("fw_real_string", {frame, b.CreateFPCast(x.v, b.getDoubleTy())}),
                    Ty{"string"}};
        if (x.t.name == "bool")
            return {objectCall("fw_bool_string", {frame, b.CreateZExt(x.v, b.getInt64Ty())}),
                    Ty{"string"}};
        if (x.t.name == "char")
            return {objectCall("fw_char_string", {frame, b.CreateZExt(x.v, b.getInt64Ty())}),
                    Ty{"string"}};
        if (x.t.enumeration())
            return {objectCall("fw_enum_name", {frame, i64(enumIds.at(x.t.name)), x.v}),
                    Ty{"string"}};
        if (x.t.name == "null")
            return literal("null");
        if (x.t.name.rfind("fn:", 0) == 0)
            return literal("<function " + displayName(x.t.name.substr(3)) + ">");
        if (x.t.classReference())
            return literal("<class " + displayName(x.t.name.substr(6)) + ">");
        if (x.t.name.rfind("module:", 0) == 0)
            return literal("<module " + x.t.name.substr(7) + ">");
        // A class with `function to_string returns string` formats itself.
        if (classes.count(x.t.name) && !resolveMethod(x.t.name, "to_string").empty() &&
            signature(resolveMethod(x.t.name, "to_string")).ast->params.empty()) {
            auto text = callNamedMethod(x, "to_string", {});
            if (text.t.name != "string")
                error(displayName(x.t.name) + ".to_string must return string");
            return text;
        }
        if (x.t.object())
            return {objectCall("fw_format", {frame, x.v}), Ty{"string"}};
        error("cannot format value of type " + x.t.name);
    }
    Var *lookup(const std::string &name) {
        for (auto i = scopes.rbegin(); i != scopes.rend(); ++i) {
            auto v = i->find(name);
            if (v != i->end())
                return &v->second;
        }
        if (!lambdaStack.empty())
            if (auto v = capture(lambdaStack.size() - 1, name))
                return v;
        auto g = globals.find(name);
        return g == globals.end() ? nullptr : &g->second;
    }
    // Finds an enclosing local for lambda `level` and copies it into the environment.
    Var *capture(size_t level, const std::string &name) {
        auto &context = *lambdaStack[level];
        if (auto it = context.captured.find(name); it != context.captured.end())
            return &it->second;
        std::optional<Var> outer;
        for (auto i = context.outerScopes->rbegin(); i != context.outerScopes->rend(); ++i)
            if (auto v = i->find(name); v != i->end()) {
                outer = v->second;
                break;
            }
        if (!outer && level > 0)
            if (auto v = capture(level - 1, name))
                outer = *v;
        if (!outer)
            return nullptr;
        if (outer->type.classReference() || outer->type.name.rfind("fn:", 0) == 0)
            error("lambdas cannot capture '" + name +
                  "' because it is a constructor or function reference; use the name directly");
        auto index = context.captures.size();
        context.captures.emplace_back(name, *outer);
        auto savedCurrent = current;
        auto savedFrame = frame;
        current = context.function;
        frame = context.frame;
        auto address = allocate(outer->cell ? Ty{"Error"} : outer->type, name);
        current = savedCurrent;
        frame = savedFrame;
        llvm::IRBuilder<> init(context.initPoint);
        auto get = [&](const std::string &function, llvm::Type *result) {
            return init.CreateCall(
                module.getOrInsertFunction(
                    function,
                    llvm::FunctionType::get(result, {b.getPtrTy(), b.getInt64Ty()}, false)),
                {context.env, i64(static_cast<int64_t>(index))});
        };
        auto t = outer->type;
        if (t.object() || outer->cell)
            init.CreateCall(module.getOrInsertFunction(
                                "fw_store", llvm::FunctionType::get(
                                                b.getVoidTy(), {b.getPtrTy(), b.getPtrTy()}, false)),
                            {address, get("fw_get_object", b.getPtrTy())});
        else if (t.real())
            init.CreateStore(init.CreateFPCast(get("fw_get_real", b.getDoubleTy()), ll(t)), address);
        else
            init.CreateStore(init.CreateIntCast(get("fw_get_int", b.getInt64Ty()), ll(t), false),
                             address);
        Var local{address, t, true};
        local.captured = true;
        if (outer->cell) {
            // The closure shares the cell, so assignments are visible on both sides.
            local.cell = true;
            local.captured = false;
            local.constant = outer->constant;
        }
        return &(context.captured[name] = local);
    }
    llvm::Value *allocate(Ty t, const std::string &name) {
        llvm::IRBuilder<> entry(&current->getEntryBlock(), current->getEntryBlock().begin());
        auto address = entry.CreateAlloca(ll(t), nullptr, name);
        if (t.object()) {
            // Every local reference has one initialized root, even for declarations in loops.
            auto instruction = llvm::cast<llvm::Instruction>(frame);
            llvm::IRBuilder<> setup(instruction->getParent(),
                                    std::next(instruction->getIterator()));
            setup.CreateStore(llvm::ConstantPointerNull::get(b.getPtrTy()), address);
            auto root = module.getOrInsertFunction(
                "fw_root",
                llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy(), b.getPtrTy()}, false));
            setup.CreateCall(root, {frame, address});
        }
        return address;
    }
    void store(llvm::Value *address, Val value) {
        if (value.t.object())
            call("fw_store", b.getVoidTy(), {address, value.v});
        else
            b.CreateStore(value.v, address);
    }
    // Names both read inside some lambda of `body` and assigned anywhere in it (by name, so a
    // shadowed name may get a cell unnecessarily, which is harmless).
    std::set<std::string> cellNames(std::vector<SP> &body) {
        std::set<std::string> assigned, captured;
        std::function<void(Expr &, bool)> expression;
        std::function<void(std::vector<SP> &, bool)> statements = [&](std::vector<SP> &list,
                                                                      bool inLambda) {
            for (auto &st : list) {
                if (st->kind == "assign" && st->target && st->target->kind == "name")
                    assigned.insert(st->target->text);
                if (st->expr)
                    expression(*st->expr, inLambda);
                if (st->target)
                    expression(*st->target, inLambda);
                for (auto *inner : {&st->body, &st->other, &st->finalizer})
                    statements(*inner, inLambda);
                for (auto &inner : st->catches)
                    statements(inner, inLambda);
                for (auto &values : st->patterns)
                    for (auto &v : values)
                        expression(*v, inLambda);
            }
        };
        expression = [&](Expr &e, bool inLambda) {
            if (e.kind == "name" && inLambda)
                captured.insert(e.text);
            for (auto &a : e.args)
                if (a)
                    expression(*a, inLambda);
            if (e.lambda)
                statements(e.lambda->body, true);
        };
        statements(body, false);
        std::set<std::string> out;
        for (auto &name : assigned)
            if (captured.count(name))
                out.insert(name);
        return out;
    }
    void bind(const std::string &name, Val v, bool constant = false) {
        if (scopes.back().count(name))
            error("duplicate declaration '" + name + "'");
        if (cells.count(name) && !constant) {
            Val cell{objectCall("fw_new", {frame, i64(1), i64(12)}), Ty{"Error"}};
            set(cell, i64(0), v);
            auto address = allocate(Ty{"Error"}, name);
            store(address, cell);
            Var var{address, v.t, constant};
            var.cell = true;
            scopes.back()[name] = var;
            return;
        }
        auto a = allocate(v.t, name);
        store(a, v);
        scopes.back()[name] = {a, v.t, constant};
        debugVariable(name, v.t, a);
    }
    // -g: describes a local to debuggers. Numbers, bools, and chars show their values; other
    // values are pointers to runtime objects (print them with `fw_debug_format(name)`).
    llvm::DIType *debugType(Ty t) {
        if (t.integer())
            return dib->createBasicType(t.name == "i64" ? "int" : t.name, t.bits(),
                                        t.unsigned_() ? llvm::dwarf::DW_ATE_unsigned
                                                      : llvm::dwarf::DW_ATE_signed);
        if (t.real())
            return dib->createBasicType(t.name, t.bits(), llvm::dwarf::DW_ATE_float);
        if (t.name == "bool")
            return dib->createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
        if (t.name == "char")
            return dib->createBasicType("char", 32, llvm::dwarf::DW_ATE_UTF);
        if (t.enumeration())
            return dib->createBasicType(displayName(t.name), 64, llvm::dwarf::DW_ATE_signed);
        auto &cached = debugObjectTypes[t.name];
        if (!cached)
            cached = dib->createPointerType(
                dib->createForwardDecl(llvm::dwarf::DW_TAG_structure_type, displayName(t.name),
                                       unit, nullptr, 0),
                64, 0, std::nullopt, displayName(t.name));
        return cached;
    }
    void debugVariable(const std::string &name, Ty t, llvm::Value *address) {
        if (!dib || !current || !current->getSubprogram() || name.empty() || name[0] == '$' ||
            name.rfind("__fw", 0) == 0 || t.name == "void" || t.name.rfind("fn:", 0) == 0 ||
            t.name.rfind("module:", 0) == 0 || t.classReference())
            return;
        auto alloca = llvm::dyn_cast<llvm::AllocaInst>(address);
        if (!alloca)
            return;
        auto scope = current->getSubprogram();
        auto line = static_cast<unsigned>(std::max(1, location.line));
        auto variable = dib->createAutoVariable(scope, displayName(name), debugFile(location.file),
                                                line, debugType(t), true);
        dib->insertDeclare(alloca, variable, dib->createExpression(),
                           llvm::DILocation::get(ctx, line, 0, scope),
                           llvm::InsertPosition(std::next(alloca->getIterator())));
    }
    Val load(Var &v) {
        if (v.initialized && !top)
            guard(b.CreateLoad(b.getInt1Ty(), v.initialized),
                  "module global read before initialization", "RuntimeError");
        raceGlobal(v, false);
        if (v.cell)
            return get({b.CreateLoad(b.getPtrTy(), v.address), Ty{"Error"}}, i64(0), v.type);
        if (v.narrowed) {
            auto raw = b.CreateLoad(b.getPtrTy(), v.address);
            Val stored{borrowed ? raw : objectCall("fw_keep", {frame, raw}), v.storage};
            if (v.typeNarrowed) {
                if (v.checked)
                    guard(isInstance(stored.v, v.type.name),
                          "narrowed value is no longer " + displayName(v.type.name), "CastError");
                return {stored.v, v.type};
            }
            return unboxChecked(stored);
        }
        auto result = b.CreateLoad(ll(v.type), v.address);
        if (v.type.object() && !borrowed)
            return {objectCall("fw_keep", {frame, result}), v.type};
        return {result, v.type};
    }
    std::pair<Class *, size_t> field(Ty t, const std::string &name) {
        if (t.classReference())
            error("'" + displayName(t.name.substr(6)) +
                  "' is a class reference, not an instance; call it with () to construct one");
        auto c = classes.find(t.name);
        if (c == classes.end())
            error("type '" + t.name + "' has no field '" + name + "'");
        for (size_t i = 0; i < c->second->fields.size(); ++i)
            if (c->second->fields[i].name == name) {
                if (c->second->fields[i].isPrivate)
                    checkAccess(fieldOwners.at(t.name + "." + name), "field", name);
                return {c->second, i};
            }
        error("unknown field '" + name + "' on " + t.name);
    }
    Val get(Val obj, llvm::Value *index, Ty t) {
        if (t.object())
            return {borrowed ? objectCall("fw_get_object", {obj.v, index})
                             : objectCall("fw_get_kept", {frame, obj.v, index}),
                    t};
        if (t.real()) {
            auto x = call("fw_get_real", b.getDoubleTy(), {obj.v, index});
            return {b.CreateFPCast(x, ll(t)), t};
        }
        auto x = call("fw_get_int", b.getInt64Ty(), {obj.v, index});
        return {b.CreateIntCast(x, ll(t), !t.unsigned_()), t};
    }
    // The slot kind of an integer-like value, as fw_set_int and fw_array_push_int take it.
    llvm::Value *slotKind(Ty t) {
        return i64(t.name == "bool"   ? 3
                   : t.name == "char" ? 5
                   : t.enumeration()  ? 6 | (enumIds.at(t.name) << 8)
                   : t.unsigned_()    ? 1
                                      : 0);
    }
    llvm::Value *slotInteger(Val x) {
        return b.CreateIntCast(x.v, b.getInt64Ty(), x.t.name != "bool" && !x.t.unsigned_());
    }
    void set(Val obj, llvm::Value *index, Val x) {
        if (x.t.classReference())
            x = convert(x, constructorType(x.t.name.substr(6)));
        if (x.t.object())
            call("fw_set_object", b.getVoidTy(), {obj.v, index, x.v});
        else if (x.t.real())
            call("fw_set_real", b.getVoidTy(),
                 {obj.v, index, b.CreateFPCast(x.v, b.getDoubleTy())});
        else
            call("fw_set_int", b.getVoidTy(), {obj.v, index, slotInteger(x), slotKind(x.t)});
    }
    // Appends in one runtime call, so a push is atomic for other threads.
    void push(Val array, Val x) {
        if (x.t.classReference())
            x = convert(x, constructorType(x.t.name.substr(6)));
        if (x.t.object())
            call("fw_array_push_object", b.getVoidTy(), {array.v, x.v});
        else if (x.t.real())
            call("fw_array_push_real", b.getVoidTy(),
                 {array.v, b.CreateFPCast(x.v, b.getDoubleTy())});
        else
            call("fw_array_push_int", b.getVoidTy(), {array.v, slotInteger(x), slotKind(x.t)});
    }
    Val receiver() {
        auto v = lookup("$inst");
        if (!v)
            error("'inst' is valid only in an instance method");
        return load(*v);
    }
    Val binary(const std::string &op, Val a, Val c) {
        // Operator methods on classes: `function operator +(T other)`. `!=` negates
        // `operator ==`; comparisons missing an operator derive from `operator <`.
        if (classes.count(a.t.name)) {
            auto has = [&](const std::string &name) {
                return !resolveMethod(a.t.name, name).empty();
            };
            auto negate = [&](Val v) { return Val{b.CreateNot(v.v), Ty{"bool"}}; };
            auto check = [&](Val v, const std::string &name) {
                if ((name == "operator==" || name.find('<') != std::string::npos ||
                     name.find('>') != std::string::npos) &&
                    v.t.name != "bool")
                    error(displayName(a.t.name) + "." + name + " must return bool");
                return v;
            };
            auto apply = [&](const std::string &name, Val x, Val y) {
                return check(callNamedMethod(x, name, {y}), name);
            };
            if (has("operator" + op))
                return apply("operator" + op, a, c);
            if (op == "!=" && has("operator=="))
                return negate(apply("operator==", a, c));
            bool sameClass = classes.count(c.t.name) && derives(c.t.name, a.t.name);
            if (has("operator<") && sameClass) {
                if (op == ">")
                    return apply("operator<", c, a);
                if (op == "<=")
                    return negate(apply("operator<", c, a));
                if (op == ">=")
                    return negate(apply("operator<", a, c));
            }
            static const std::set<std::string> arithmetic = {"+", "-", "*", "/", "//", "%",
                                                             "<", "<=", ">", ">="};
            if (arithmetic.count(op))
                error("class " + displayName(a.t.name) + " does not define operator " + op +
                      "; declare `function operator " + op + "(...)`");
        }
        if (op == "+" && ((a.t.name == "string" && c.t.name == "char") ||
                          (a.t.name == "char" && c.t.name == "string")))
            return binary(op, stringify(a), stringify(c));
        if (a.t.name == "char" && c.t.name == "char") {
            static const std::map<std::string, llvm::CmpInst::Predicate> predicates = {
                {"==", llvm::CmpInst::ICMP_EQ},  {"!=", llvm::CmpInst::ICMP_NE},
                {"<", llvm::CmpInst::ICMP_ULT},  {"<=", llvm::CmpInst::ICMP_ULE},
                {">", llvm::CmpInst::ICMP_UGT},  {">=", llvm::CmpInst::ICMP_UGE}};
            auto predicate = predicates.find(op);
            if (predicate == predicates.end())
                error("operator '" + op + "' is not defined for char; convert with int(c) first");
            return {b.CreateICmp(predicate->second, a.v, c.v), Ty{"bool"}};
        }
        if (a.t.name == "string" && c.t.name == "string") {
            if (op == "+")
                return {objectCall("fw_concat", {frame, a.v, c.v}), a.t};
            if (op == "==" || op == "!=") {
                auto eq = b.CreateICmpNE(call("fw_equal", b.getInt64Ty(), {a.v, c.v}), i64(0));
                return {op == "==" ? eq : b.CreateNot(eq), Ty{"bool"}};
            }
            // Strings order by Unicode code point (UTF-8 byte order).
            static const std::map<std::string, llvm::CmpInst::Predicate> order = {
                {"<", llvm::CmpInst::ICMP_SLT}, {"<=", llvm::CmpInst::ICMP_SLE},
                {">", llvm::CmpInst::ICMP_SGT}, {">=", llvm::CmpInst::ICMP_SGE}};
            if (auto it = order.find(op); it != order.end())
                return {b.CreateICmp(it->second,
                                     call("fw_string_order", b.getInt64Ty(), {a.v, c.v}), i64(0)),
                        Ty{"bool"}};
        }
        bool optionalA = a.t.nullable() && primitive(nonNull(a.t)),
             optionalC = c.t.nullable() && primitive(nonNull(c.t));
        if ((op == "==" || op == "!=") && (optionalA || optionalC) && a.t.name != "null" &&
            c.t.name != "null") {
            Ty t = optionalA ? a.t : c.t;
            auto x = convert(a, t), y = convert(c, t);
            auto eq = b.CreateICmpNE(call("fw_box_equal", b.getInt64Ty(), {x.v, y.v}), i64(0));
            return {op == "==" ? eq : b.CreateNot(eq), Ty{"bool"}};
        }
        auto variantType = [&](const Ty &t) {
            auto it = classes.find(nonNull(t).name);
            return it != classes.end() &&
                   (!it->second->variantOf.empty() || !it->second->variants.empty());
        };
        if ((op == "==" || op == "!=") && variantType(a.t) && variantType(c.t) &&
            resolveMethod(nonNull(a.t).name, "equals").empty()) {
            // Members of enums with data compare by member and field values.
            auto eq = b.CreateICmpNE(call("fw_variant_equal", b.getInt64Ty(), {a.v, c.v}), i64(0));
            return {op == "==" ? eq : b.CreateNot(eq), Ty{"bool"}};
        }
        if ((optionalA || optionalC) && op != "==" && op != "!=")
            error("operator '" + op + "' requires non-null operands; check for null or use '?\?'");
        if ((op == "==" || op == "!=") && classes.count(a.t.name) && classes.count(c.t.name) &&
            !resolveMethod(a.t.name, "equals").empty() &&
            (derives(a.t.name, c.t.name) || derives(c.t.name, a.t.name))) {
            auto eq = callNamedMethod(a, "equals", {c});
            if (eq.t.name != "bool")
                error(displayName(a.t.name) + ".equals must return bool");
            return {op == "==" ? eq.v : b.CreateNot(eq.v), Ty{"bool"}};
        }
        if ((op == "==" || op == "!=") && a.t.object() && c.t.object()) {
            auto related = derives(nonNull(a.t).name, nonNull(c.t).name) ||
                           derives(nonNull(c.t).name, nonNull(a.t).name);
            if (a.t != c.t && !related &&
                !((a.t.name == "null" && c.t.nullable()) || (c.t.name == "null" && a.t.nullable())))
                error("incompatible equality operands");
            auto eq = b.CreateICmpEQ(a.v, c.v);
            return {op == "==" ? eq : b.CreateNot(eq), Ty{"bool"}};
        }
        if (a.t.enumeration() || c.t.enumeration()) {
            if (a.t != c.t || (op != "==" && op != "!="))
                error("enums support only == and != with the same enum type");
            return {op == "==" ? b.CreateICmpEQ(a.v, c.v) : b.CreateICmpNE(a.v, c.v), Ty{"bool"}};
        }
        if (a.t.name == "bool" && c.t.name == "bool" && (op == "==" || op == "!="))
            return {op == "==" ? b.CreateICmpEQ(a.v, c.v) : b.CreateICmpNE(a.v, c.v), Ty{"bool"}};
        if (!a.t.numeric() || !c.t.numeric())
            error("operator '" + op + "' requires compatible numeric operands");
        if (op == "//" && (a.t.real() || c.t.real())) {
            // Floating integer division truncates toward zero, like integer '//', and returns int.
            a = convert(a, Ty{"double"}, true);
            c = convert(c, Ty{"double"}, true);
            guard(b.CreateFCmpUNE(c.v, llvm::ConstantFP::get(b.getDoubleTy(), 0.0)),
                  "division by zero");
            return convert({b.CreateFDiv(a.v, c.v), Ty{"double"}}, Ty{"i64"}, true);
        }
        Ty common = a.t;
        if (a.t != c.t) {
            if (widening(a.t, c.t))
                common = c.t;
            else if (!widening(c.t, a.t))
                error("no lossless common numeric type; use an explicit cast");
        }
        a = convert(a, common);
        c = convert(c, common);
        bool real = common.real();
        bool u = common.unsigned_();
        if (op == "/") {
            if (common.real())
                return {b.CreateFDiv(a.v, c.v), common};
            a = convert(a, Ty{"double"}, true);
            c = convert(c, Ty{"double"}, true);
            return {b.CreateFDiv(a.v, c.v), Ty{"double"}};
        }
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            llvm::Value *v = nullptr;
            if (real) {
                if (op == "==")
                    v = b.CreateFCmpOEQ(a.v, c.v);
                else if (op == "!=")
                    v = b.CreateFCmpUNE(a.v, c.v);
                else if (op == "<")
                    v = b.CreateFCmpOLT(a.v, c.v);
                else if (op == "<=")
                    v = b.CreateFCmpOLE(a.v, c.v);
                else if (op == ">")
                    v = b.CreateFCmpOGT(a.v, c.v);
                else
                    v = b.CreateFCmpOGE(a.v, c.v);
            } else {
                if (op == "==")
                    v = b.CreateICmpEQ(a.v, c.v);
                else if (op == "!=")
                    v = b.CreateICmpNE(a.v, c.v);
                else if (op == "<")
                    v = u ? b.CreateICmpULT(a.v, c.v) : b.CreateICmpSLT(a.v, c.v);
                else if (op == "<=")
                    v = u ? b.CreateICmpULE(a.v, c.v) : b.CreateICmpSLE(a.v, c.v);
                else if (op == ">")
                    v = u ? b.CreateICmpUGT(a.v, c.v) : b.CreateICmpSGT(a.v, c.v);
                else
                    v = u ? b.CreateICmpUGE(a.v, c.v) : b.CreateICmpSGE(a.v, c.v);
            }
            return {v, Ty{"bool"}};
        }
        if (real) {
            if (op == "+")
                return {b.CreateFAdd(a.v, c.v), common};
            if (op == "-")
                return {b.CreateFSub(a.v, c.v), common};
            if (op == "*")
                return {b.CreateFMul(a.v, c.v), common};
            if (op == "%")
                return {b.CreateFRem(a.v, c.v), common};
            error("unsupported numeric operator '" + op + "'");
        }
        if (op == "//" || op == "%") {
            auto zero = llvm::ConstantInt::get(ll(common), 0);
            guard(b.CreateICmpNE(c.v, zero), "integer division by zero");
            if (!u) {
                auto min =
                    llvm::ConstantInt::get(ctx, llvm::APInt::getSignedMinValue(common.bits()));
                auto minus =
                    llvm::ConstantInt::getSigned(llvm::cast<llvm::IntegerType>(ll(common)), -1);
                guard(
                    b.CreateNot(b.CreateAnd(b.CreateICmpEQ(a.v, min), b.CreateICmpEQ(c.v, minus))),
                    "integer division overflow");
            }
            return {op == "//" ? (u ? b.CreateUDiv(a.v, c.v) : b.CreateSDiv(a.v, c.v))
                               : (u ? b.CreateURem(a.v, c.v) : b.CreateSRem(a.v, c.v)),
                    common};
        }
        llvm::Intrinsic::ID intrinsic;
        if (op == "+")
            intrinsic =
                u ? llvm::Intrinsic::uadd_with_overflow : llvm::Intrinsic::sadd_with_overflow;
        else if (op == "-")
            intrinsic =
                u ? llvm::Intrinsic::usub_with_overflow : llvm::Intrinsic::ssub_with_overflow;
        else if (op == "*")
            intrinsic =
                u ? llvm::Intrinsic::umul_with_overflow : llvm::Intrinsic::smul_with_overflow;
        else
            error("unsupported numeric operator '" + op + "'");
        auto f = llvm::Intrinsic::getOrInsertDeclaration(&module, intrinsic, {ll(common)});

        auto result = b.CreateCall(f, {a.v, c.v});
        guard(b.CreateNot(b.CreateExtractValue(result, 1)), "integer overflow");
        return {b.CreateExtractValue(result, 0), common};
    }
    std::string callable(Expr &e) {
        if (e.kind == "name")
            return e.text;
        if (e.kind == "member" && e.args[0]->kind == "name") {
            auto n = e.args[0]->text;
            auto it = p.imports.find(n);
            if (it != p.imports.end())
                return it->second + "." + e.text;
        }
        return "";
    }
    Val newInstance(Class &c) {
        std::string fields;
        for (auto &f : c.fields) {
            if (!fields.empty())
                fields += ",";
            fields += f.name;
        }
        return {objectCall("fw_instance",
                           {frame, i64(c.fields.size()), classId(c.name),
                            b.CreateGlobalString(displayName(c.name)), b.CreateGlobalString(fields)}),
                Ty{c.name}};
    }
    Val collectionCast(Val source, Ty target);
    Ty inferCollectionCast(const std::string &constructor, Ty source);
    Val explicitCast(Val source, Ty target);
    std::optional<Val> domCall(const std::string &name, std::vector<Val> args);
    struct TagRules {
        std::set<std::string> attributes, booleans;
        bool any = false, isVoid = false;
    };
    std::map<std::string, TagRules> familyRules;
    std::set<std::string> instantiatedComponents;
    // Components that are never used still get their markup structure checked: tags, attribute
    // names, and void elements. Attribute expressions are typed only when instantiated.
    // Inserts a markup child: elements and fragments as nodes, sequences of them (or of other
    // values) item by item, and anything else as escaped text.
    void markupChild(Val parent, Val child) {
        if (child.t.sequence()) {
            eachItem(child, [&](Val item) { markupChild(parent, item); });
            return;
        }
        if (nonNull(child.t).name == "element" || nonNull(child.t).name == "fragment") {
            if (child.t.nullable()) {
                // A null optional child adds nothing.
                auto present = llvm::BasicBlock::Create(ctx, "child.present", current);
                auto done = llvm::BasicBlock::Create(ctx, "child.end", current);
                b.CreateCondBr(
                    b.CreateICmpNE(child.v, llvm::ConstantPointerNull::get(b.getPtrTy())), present,
                    done);
                b.SetInsertPoint(present);
                call("fw_child", b.getVoidTy(), {parent.v, child.v});
                b.CreateBr(done);
                b.SetInsertPoint(done);
                return;
            }
        } else
            child = stringify(child);
        call("fw_child", b.getVoidTy(), {parent.v, child.v});
    }
    void checkTemplate(Expr &e) {
        location = e.loc;
        if (e.kind == "element" && !p.components.count(e.text)) {
            auto rules = tagRules(e.text);
            if (!e.text.empty() && !rules)
                error("unknown tag '" + e.text + "'");
            for (auto &name : e.names)
                if (!(name.rfind("on", 0) == 0 && name.size() > 2) &&
                    !attributeAllowed(*rules, name))
                    error("unknown attribute '" + name + "' on <" + e.text + ">");
            if (rules && rules->isVoid && e.args.size() != e.names.size())
                error("void element <" + e.text + "> cannot have children");
        }
        for (auto &a : e.args)
            if (a)
                checkTemplate(*a);
    }
    const TagRules *tagRules(const std::string &tag);
    bool attributeAllowed(const TagRules &rules, const std::string &name);
    Val invoke(Expr &e, Ty expected = Ty{});
    Val value(Expr &e, Ty expected = Ty{});
    Val defaultValue(Expr &e, Ty expected) {
        auto saved = std::move(scopes);
        auto savedAttrs = std::move(attrs);
        auto savedNames = std::move(componentNames);
        auto savedLambdas = std::move(lambdaStack);
        scopes.clear();
        scopes.emplace_back();
        attrs.clear();
        componentNames.clear();
        lambdaStack.clear();
        auto result = value(e, expected);
        scopes = std::move(saved);
        attrs = std::move(savedAttrs);
        componentNames = std::move(savedNames);
        lambdaStack = std::move(savedLambdas);
        return result;
    }
    llvm::Value *mapIndex(Val map, Val key, bool insert) {
        return keyFind(map, key, insert ? 1 : 0);
    }
    // Key position in a map or set. mode 0 raises KeyError when missing, 1 inserts, 2 gives -1.
    llvm::Value *keyFind(Val container, Val key, int mode) {
        auto kind = mapKeyKind(key.t);
        if (kind == 7 || kind == 8) {
            llvm::Value *hash, *equal = llvm::ConstantPointerNull::get(b.getPtrTy());
            if (kind == 7)
                hash = call("fw_structural_hash", b.getInt64Ty(), {key.v});
            else {
                hash = convert(callNamedMethod(key, "hash", {}), Ty{"i64"}).v;
                equal = equalsCallback(key.t.name);
            }
            return call("fw_map_find_hashed", b.getInt64Ty(),
                        {container.v, hash, key.v, equal, i64(mode)});
        }
        auto integer = key.t.object() ? i64(0)
                                      : b.CreateIntCast(key.v, b.getInt64Ty(),
                                                        key.t.name != "bool" && !key.t.unsigned_());
        auto object = key.t.object() ? key.v : llvm::ConstantPointerNull::get(b.getPtrTy());
        if (mode == 2) {
            auto has = call("fw_map_has", b.getInt64Ty(), {container.v, integer, object});
            return b.CreateSelect(b.CreateICmpNE(has, i64(0)), i64(0), i64(-1));
        }
        return call("fw_map_index", b.getInt64Ty(), {container.v, integer, object, i64(mode)});
    }
    Val keyRemove(Val container, Val key) {
        auto kind = mapKeyKind(key.t);
        if (kind != 7 && kind != 8) {
            auto integer =
                key.t.object() ? i64(0)
                               : b.CreateIntCast(key.v, b.getInt64Ty(),
                                                 key.t.name != "bool" && !key.t.unsigned_());
            auto object = key.t.object() ? key.v : llvm::ConstantPointerNull::get(b.getPtrTy());
            return {b.CreateICmpNE(
                        call("fw_map_remove", b.getInt64Ty(), {container.v, integer, object}),
                        i64(0)),
                    Ty{"bool"}};
        }
        auto index = keyFind(container, key, 2);
        auto found = b.CreateICmpSGE(index, i64(0));
        auto yes = llvm::BasicBlock::Create(ctx, "remove.found", current);
        auto done = llvm::BasicBlock::Create(ctx, "remove.end", current);
        b.CreateCondBr(found, yes, done);
        b.SetInsertPoint(yes);
        call("fw_map_remove_at", b.getVoidTy(), {container.v, index});
        b.CreateBr(done);
        b.SetInsertPoint(done);
        return {found, Ty{"bool"}};
    }
    Val newSet(Ty t) {
        return {objectCall("fw_map_new", {frame, i64(mapKeyKind(t.parts()[0]) | (1LL << 40))}), t};
    }
    // Calls receiver.method(args...) with already evaluated values.
    Val callNamedMethod(Val receiver, const std::string &method, std::vector<Val> args) {
        scopes.emplace_back();
        Expr call;
        call.kind = "call";
        call.loc = location;
        auto callee = std::make_unique<Expr>();
        callee->kind = "member";
        callee->text = method;
        callee->loc = location;
        auto self = "$r" + std::to_string(hiddenCount++);
        bind(self, receiver, true);
        callee->args.push_back(syntheticName(self, location));
        call.args.push_back(std::move(callee));
        for (auto &arg : args) {
            auto name = "$p" + std::to_string(hiddenCount++);
            bind(name, arg, true);
            call.args.push_back(syntheticName(name, location));
        }
        auto result = invoke(call);
        releaseScopes(scopes.size() - 1);
        scopes.pop_back();
        return result;
    }
    // int64_t equal(key, candidate) for class keys: key.equals(candidate).
    llvm::Value *equalsCallback(const std::string &className) {
        return closureCode("equals." + className, Ty{"function<i64," + className + ">"},
                           [&](llvm::Value *env, Expr &call) {
                               (void)call;
                               auto self = callNamedMethod({env, Ty{className}}, "equals",
                                                           {load(*lookup("$a0"))});
                               if (self.t.name != "bool")
                                   error(displayName(className) + ".equals must return bool");
                               return Val{b.CreateZExt(self.v, b.getInt64Ty()), Ty{"i64"}};
                           });
    }
    // Emits `body(item)` for each item of a sequence value.
    void eachItem(Val sequence, const std::function<void(Val)> &body) {
        auto size = call("fw_length", b.getInt64Ty(), {sequence.v});
        auto index = allocate(Ty{"i64"}, "each");
        b.CreateStore(i64(0), index);
        auto cond = llvm::BasicBlock::Create(ctx, "each.condition", current);
        auto loop = llvm::BasicBlock::Create(ctx, "each.body", current);
        auto end = llvm::BasicBlock::Create(ctx, "each.end", current);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        auto i = b.CreateLoad(b.getInt64Ty(), index);
        b.CreateCondBr(b.CreateICmpSLT(i, size), loop, end);
        b.SetInsertPoint(loop);
        body(get(sequence, i, sequence.t.item()));
        b.CreateStore(b.CreateAdd(b.CreateLoad(b.getInt64Ty(), index), i64(1)), index);
        b.CreateBr(cond);
        b.SetInsertPoint(end);
    }
    std::optional<int64_t> constantIndex(Expr &e) {
        if (e.kind == "int") {
            try {
                return std::stoll(e.text);
            } catch (...) {
                return std::nullopt;
            }
        }
        if (e.kind == "unary" && (e.text == "+" || e.text == "-")) {
            auto n = constantIndex(*e.args[0]);
            if (!n)
                return std::nullopt;
            if (e.text == "+")
                return n;
            if (*n == std::numeric_limits<int64_t>::min())
                error("tuple index overflow");
            return -*n;
        }
        if (e.kind != "binary")
            return std::nullopt;
        auto a = constantIndex(*e.args[0]), c = constantIndex(*e.args[1]);
        if (!a || !c)
            return std::nullopt;
        llvm::APInt left(64, static_cast<uint64_t>(*a), true),
            right(64, static_cast<uint64_t>(*c), true), out(64, 0);
        bool overflow = false;
        if (e.text == "+")
            out = left.sadd_ov(right, overflow);
        else if (e.text == "-")
            out = left.ssub_ov(right, overflow);
        else if (e.text == "*")
            out = left.smul_ov(right, overflow);
        else if (e.text == "//" || e.text == "%") {
            if (*c == 0 || (*a == std::numeric_limits<int64_t>::min() && *c == -1))
                error("invalid constant tuple index division");
            out = e.text == "//" ? left.sdiv(right) : left.srem(right);
        } else
            return std::nullopt;
        if (overflow)
            error("tuple index overflow");
        return out.getSExtValue();
    }
    std::pair<llvm::Value *, Ty> indexOf(Val obj, Expr &e, bool insert) {
        if (obj.t.map()) {
            auto parts = obj.t.parts();
            auto key = convert(value(e, parts[0]), parts[0]);
            return {mapIndex(obj, key, insert), parts[1]};
        }
        if (obj.t.tuple()) {
            auto index = constantIndex(e);
            if (!index)
                error("tuple indices must be compile-time integers");
            auto parts = obj.t.parts();
            if (*index < 0 || size_t(*index) >= parts.size())
                error("tuple index out of bounds");
            return {i64(*index), parts[size_t(*index)]};
        }
        auto index = convert(value(e), Ty{"i64"});
        if (obj.t.sequence())
            return {index.v, obj.t.item()};
        error("indexing requires an array, tuple, or map");
    }
    void runFinalizers(size_t minimumLoop = 0) {
        auto savedFinalizers = finalizers;
        auto savedHandlers = handlers;
        auto savedScopes = scopes;
        for (size_t i = savedFinalizers.size(); i > 0; --i) {
            auto active = savedFinalizers[i - 1];
            if (active.loopDepth < minimumLoop)
                break;
            releaseScopes(active.depth);
            finalizers.assign(savedFinalizers.begin(),
                              savedFinalizers.begin() + static_cast<ptrdiff_t>(i - 1));
            handlers = {active.outer};
            scopes.resize(active.depth);
            block(*active.body);
            scopes = savedScopes;
            if (terminated())
                break;
        }
        finalizers = std::move(savedFinalizers);
        handlers = std::move(savedHandlers);
        scopes = std::move(savedScopes);
    }
    void statement(Stmt &s);
    bool terminated() {
        return !b.GetInsertBlock()->empty() && b.GetInsertBlock()->back().isTerminator();
    }
    bool borrowable(Expr &e) {
        if ((e.kind == "element" && p.components.count(e.text)) || e.kind == "lambda")
            return false;
        if (e.outer && !e.grouped && (e.kind == "name" || e.kind == "member")) {
            auto name = callable(e);
            auto variable = lookup(name);
            if (classes.count(name) || name == "desktop.window" || name == "desktop.Window" ||
                (variable && variable->type.classReference()))
                return false;
        }
        if (e.kind == "call") {
            auto name = callable(*e.args[0]);
            if (functions.count(name) || classes.count(name) || lookup(name))
                return false;
            static const std::set<std::string> pure = {"print",    "len",
                                                       "typeof",   "string",
                                                       "int",      "i8",
                                                       "i16",      "i32",
                                                       "i64",      "u8",
                                                       "u16",      "u32",
                                                       "u64",      "float",
                                                       "double",   "html.render",
                                                       "render",   "math.sqrt",
                                                       "math.sin", "math.cos",
                                                       "math.pow", "testing.live_objects"};
            if (!pure.count(name)) {
                auto &callee = *e.args[0];
                bool markupRender = false;
                if (callee.kind == "member" && callee.text == "render") {
                    auto &receiver = *callee.args[0];
                    markupRender = receiver.kind == "element";
                    if (receiver.kind == "name")
                        if (auto var = lookup(receiver.text))
                            markupRender =
                                var->type.name == "element" || var->type.name == "fragment";
                }
                if (!markupRender)
                    return false;
            }
        }
        for (auto &a : e.args)
            if (!borrowable(*a))
                return false;
        return true;
    }
    bool borrowable(Stmt &s) {
        if (s.kind == "assign" && s.target && s.target->kind == "member")
            return false;
        if (s.kind != "declare" && s.kind != "assign" && s.kind != "expr" && s.kind != "return")
            return false;
        return (!s.expr || borrowable(*s.expr)) && (!s.target || borrowable(*s.target));
    }
    void block(std::vector<SP> &body, bool scope = true) {
        if (scope)
            scopes.emplace_back();
        for (auto &s : body) {
            if (terminated())
                error("unreachable statement");
            auto before = tempGeneration;
            auto savedBorrowed = borrowed;
            borrowed = borrowable(*s);
            statement(*s);
            borrowed = savedBorrowed;
            if (!terminated() && tempGeneration != before)
                call("fw_sweep", b.getVoidTy(), {frame});
        }
        if (scope) {
            if (!terminated())
                releaseScopes(scopes.size() - 1);
            scopes.pop_back();
        }
    }
    Signature &signature(const std::string &name) {
        auto it = functions.find(name);
        if (it == functions.end())
            error("unknown function '" + name + "'");
        auto &sig = it->second;
        if (sig.result.name == "$infer")
            buildFunction(sig, true);
        return sig;
    }
    void buildFunction(Signature &sig, bool inference);
    Ty inferredResult(bool fallsThrough) {
        Ty result;
        for (auto t : returns) {
            if (result.name == "void" && t.name != "void")
                result = t;
            else if (t != result) {
                if (widening(t, result))
                    continue;
                if (widening(result, t))
                    result = t;
                else
                    error("inconsistent inferred return types");
            }
        }
        if (result.name != "void") {
            if (fallsThrough)
                error("inferred non-void function must return on all paths");
            for (auto t : returns)
                if (t.name == "void")
                    error("cannot mix value and bare returns");
        }
        if (result.name == "null")
            error("return null requires an explicit nullable return type");
        return result;
    }
    Val lambda(Expr &e, Ty expected);
    Val coalesce(Expr &e, Ty expected);
    Val typeTest(Expr &e);
    // Private members are visible only inside methods (and lambdas in methods) of their class.
    void checkAccess(const std::string &declaring, const std::string &kind,
                     const std::string &name) {
        if (owner != declaring)
            error(kind + " '" + name + "' is private to class " + displayName(declaring));
    }
    bool hasField(const std::string &className, const std::string &name) {
        auto c = classes.find(className);
        if (c == classes.end())
            return false;
        for (auto &f : c->second->fields)
            if (f.name == name)
                return true;
        return false;
    }
    static EP syntheticName(const std::string &name, Location loc) {
        auto e = std::make_unique<Expr>();
        e->kind = "name";
        e->text = name;
        e->loc = loc;
        return e;
    }
    // Builds `R code(ptr env, P...)` for a closure of type `function`; `emit` receives the
    // environment and the parameters bound as locals $a0, $a1, ... and returns the result.
    llvm::Function *closureCode(const std::string &key, Ty function,
                                const std::function<Val(llvm::Value *, Expr &)> &emit);
    Ty constructorType(const std::string &className);
    std::string instantiateClass(const std::string &base, const std::vector<Ty> &arguments);
    void checkBounds(const std::string &generic, const std::vector<std::string> &params,
                     const std::map<std::string, std::string> &bounds,
                     const std::vector<Ty> &arguments);
    std::string methodTemplate(const std::string &name, const std::string &method);
    std::optional<Val> genericMethodCall(Val receiver, const std::string &text, Expr &e,
                                         Ty expected);
    std::string instantiateFunction(const std::string &base, const std::vector<Ty> &arguments);
    std::optional<Val> genericCall(Expr &e, const std::string &name, Ty expected);
    uint64_t hiddenCount = 0;
    Val constructorValue(const std::string &className, Ty to);
    Val boundMethod(Val receiver, const std::string &method);
    Val callMethod(Val receiver, const std::string &staticType, const std::string &method, Expr &e,
                   bool direct);
    void validateClasses();
    void checkInit(Class &c, Function &init);
    void match(Stmt &s);
    bool destructuring(Expr &p);
    bool irrefutable(Expr &p, Ty subject);
    void destructure(Expr &p, Val subject, llvm::BasicBlock *fail,
                     std::vector<std::pair<std::string, Val>> &bindings);
    void iterate(Stmt &s, Val source);
    Ty lambdaBody(Function &f, const std::vector<Ty> &params, Ty result, bool inference,
                  LambdaContext &context);
    Val sequenceMethod(Val sequence, const std::string &method, Expr &call);
    Val stringMethod(Val text, const std::string &method, Expr &call);
    std::optional<Val> sequenceTool(Val sequence, const std::string &method, Expr &call);
    std::optional<Val> mathCall(const std::string &name, std::vector<Val> &args);
    std::optional<Val> stdlibCall(const std::string &name, std::vector<Val> &args, Ty expected);
    Val unwrapForBinding(Val v);
    Val expectError(Expr &e);
    std::optional<Val> resourceMethod(Val receiver, const std::string &method, Expr &e);
    Val jsonDecode(Ty t, Val json);
    llvm::Value *jsonDecodeFunction(Ty t);
    std::map<std::string, llvm::Function *> jsonDecoders;
    llvm::Function *lessTrampoline(Ty item, Ty function);
    llvm::Function *taskRunner(Ty function);
    // Named functions passed where a function value is expected become closures.
    Val asFunction(Val f) {
        if (f.t.name.rfind("fn:", 0) != 0)
            return f;
        auto &sig = signature(f.t.name.substr(3));
        std::string t = "function<" + sig.result.name;
        for (auto &param : sig.ast->params)
            t += "," + type(param.type).name;
        return convert(f, Ty{t + ">"});
    }
    void declareFunction(Function &f) {
        std::string key = f.owner.empty() ? f.name : f.owner + "." + f.name;
        location = f.loc;
        if (functions.count(key))
            error("duplicate function '" + displayName(key) + "'");
        if (f.name == "init" && !f.owner.empty() && !f.result.empty() && f.result != "void")
            error("init cannot return a value");
        if (!f.typeParams.empty() && !f.owner.empty()) {
            // Generic methods are instantiated per call like generic functions.
            methodTemplates[key] = &f;
            return;
        }
        Signature sig;
        sig.ast = &f;
        sig.result = f.result.empty() ? (f.name == "init" ? Ty{"void"} : Ty{"$infer"})
                                      : type(f.result);
        if (f.abstract)
            sig.done = true;
        else if (sig.result.name != "$infer") {
            std::vector<llvm::Type *> args;
            if (!f.owner.empty())
                args.push_back(b.getPtrTy());
            for (auto &a : f.params)
                args.push_back(ll(type(a.type)));
            sig.function =
                llvm::Function::Create(llvm::FunctionType::get(ll(sig.result), args, false),
                                       llvm::GlobalValue::InternalLinkage, "fly." + key, module);
        }
        functions[key] = sig;
    }

  public:
    Compiler(Program &program, const std::string &file) : p(program), module(file, ctx), b(ctx) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        auto triple = llvm::sys::getDefaultTargetTriple();
        std::string message;
        auto target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), message);

        if (!target)
            throw std::runtime_error(message);
        llvm::TargetOptions options;
        machine.reset(target->createTargetMachine(llvm::Triple(triple), "generic", "", options,

                                                  llvm::Reloc::PIC_));
        module.setTargetTriple(llvm::Triple(triple));

        module.setDataLayout(machine->createDataLayout());
        builtinClasses();
        for (auto &family : p.families)
            for (auto &[tag, info] : family.tags) {
                auto &rules = familyRules[tag];
                rules.attributes.insert(info.attributes.begin(), info.attributes.end());
                rules.any = rules.any || info.anyAttribute;
                rules.isVoid = rules.isVoid || info.isVoid;
            }
        enumNames().clear();
        for (auto &en : p.enums) {
            enums[en.name] = &en;
            enumIds[en.name] = static_cast<int64_t>(enumIds.size());
            enumNames().insert(en.name);
        }
        for (auto &c : p.classes) {
            location = c.loc;
            if (classes.count(c.name) || templates.count(c.name) || enums.count(c.name))
                error("duplicate class");
            if (!c.typeParams.empty())
                templates[c.name] = &c;
            else
                classes[c.name] = &c;
        }
        std::vector<Class *> declared;
        for (auto &[name, c] : classes) {
            (void)name;
            declared.push_back(c);
        }
        for (auto c : declared)
            finishClass(*c);
        static const std::set<std::string> modules = {
            "desktop", "html", "math", "fs",          "os", "json",  "web", "time",
            "testing", "core", "io",   "collections", "net", "regex", "thread", "gc",
            "svg",     "crypto", "encoding", "uuid", "compress", "sqlite", "toml", "yaml"};
        for (auto &[alias, name] : p.imports) {
            if (!modules.count(name))
                error("module '" + name +
                      "' is not implemented yet (local modules are not supported)");
            (void)alias;
        }
        for (auto &f : p.functions) {
            if (!f.typeParams.empty()) {
                if (functionTemplates.count(f.name))
                    error("duplicate function '" + displayName(f.name) + "'");
                functionTemplates[f.name] = &f;
            } else
                declareFunction(f);
        }
        for (auto &c : p.classes)
            if (c.typeParams.empty())
                for (auto &f : c.methods)
                    declareFunction(f);
    }
    // Flattens inherited fields (base fields first) and checks the class header.
    void finishClass(Class &c) {
        auto &state = classStates[c.name];
        if (state == 2)
            return;
        location = c.loc;
        if (state == 1)
            error("inheritance cycle through '" + displayName(c.name) + "'");
        state = 1;
        std::set<std::string> names;
        for (auto &f : c.fields) {
            if (!names.insert(f.name).second)
                error("duplicate field '" + f.name + "'");
            if (f.isPrivate || f.readonly)
                fieldOwners[c.name + "." + f.name] = c.name;
        }
        if (!c.base.empty()) {
            c.base = type(c.base).name;
            auto base = classes.find(c.base);
            if (base == classes.end() || base->second->isInterface)
                error("'" + displayName(c.base) + "' is not a class; use implements for interfaces");
            finishClass(*base->second);
            std::vector<Param> fields;
            for (auto &f : base->second->fields) {
                if (names.count(f.name))
                    error("field '" + f.name + "' is already declared by base class '" +
                          displayName(c.base) + "'");
                fields.push_back({f.name, f.type, f.value ? clone(*f.value) : nullptr, f.loc,
                                  f.isPrivate, f.readonly});
                if (f.isPrivate || f.readonly)
                    fieldOwners[c.name + "." + f.name] = fieldOwners.at(c.base + "." + f.name);
            }
            for (auto &f : c.fields)
                fields.push_back(std::move(f));
            c.fields = std::move(fields);
        }
        for (auto &i : c.interfaces) {
            i = type(i).name;
            auto it = classes.find(i);
            if (it == classes.end() || !it->second->isInterface)
                error("'" + displayName(i) + "' is not an interface");
            finishClass(*it->second);
        }
        state = 2;
    }
    // True when values of class/interface `from` may be used as `to`.
    bool derives(const std::string &from, const std::string &to) {
        if (from == to)
            return true;
        auto it = classes.find(from);
        if (it == classes.end())
            return false;
        auto &c = *it->second;
        if (!c.base.empty() && derives(c.base, to))
            return true;
        for (auto &i : c.interfaces)
            if (derives(i, to))
                return true;
        return false;
    }
    // Concrete classes whose instances can have static type `name`.
    std::vector<std::string> concreteClasses(const std::string &name) {
        std::vector<std::string> out;
        for (auto &[candidate, c] : classes)
            if (!c->isInterface && c->variants.empty() && derives(candidate, name))
                out.push_back(candidate);
        return out;
    }
    // The function key implementing `method` for class or interface `name` ("" if none).
    std::string resolveMethod(const std::string &name, const std::string &method) {
        auto it = classes.find(name);
        if (it == classes.end())
            return "";
        if (functions.count(name + "." + method))
            return name + "." + method;
        auto &c = *it->second;
        if (!c.base.empty())
            if (auto found = resolveMethod(c.base, method); !found.empty())
                return found;
        for (auto &i : c.interfaces)
            if (auto found = resolveMethod(i, method); !found.empty())
                return found;
        return "";
    }
    // One canonical name string per class doubles as its runtime class identity.
    llvm::Constant *classId(const std::string &name) {
        auto &id = classIds[name];
        if (!id)
            id = b.CreateGlobalString(name, "class." + name, 0, &module);
        return id;
    }
    llvm::Value *isInstance(llvm::Value *object, const std::string &type) {
        auto id = call("fw_class_id", b.getPtrTy(), {object});
        llvm::Value *match = b.getFalse();
        for (auto &c : concreteClasses(type))
            match = b.CreateOr(match, b.CreateICmpEQ(id, classId(c)));
        return match;
    }
    void run(const Options &opts) {
        desktopBackend = opts.desktopBackend;
        coverage = opts.coverage;
        race = opts.race;
        if (opts.debug) {
            dib = std::make_unique<llvm::DIBuilder>(module);
            mainFile = module.getModuleIdentifier();
            unit = dib->createCompileUnit(llvm::dwarf::DW_LANG_C, debugFile(mainFile), "flywheel",
                                          opts.optimization > 0, "", 0);
            module.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                                 llvm::DEBUG_METADATA_VERSION);
            module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
        }
        current = llvm::Function::Create(
            llvm::FunctionType::get(b.getInt32Ty(), {b.getInt32Ty(), b.getPtrTy()}, false),
            llvm::GlobalValue::ExternalLinkage, "main", module);
        auto entry = llvm::BasicBlock::Create(ctx, "entry", current);
        auto mainFunction = current;
        b.SetInsertPoint(entry);
        if (dib)
            debugFunction(current, "main", {1, 1, mainFile});
        if (race)
            call("fw_race_enable", b.getVoidTy(), {});
        frame = objectCall("fw_frame", {});
        call("fw_set_args", b.getVoidTy(),
             {b.CreateSExt(current->getArg(0), b.getInt64Ty()), current->getArg(1)});
        for (auto &name : builtinClassNames)
            call("fw_register_class", b.getVoidTy(), {b.CreateGlobalString(name), classId(name)});
        failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
        scopes.emplace_back();
        top = true;
        returnType = Ty{"void"};
        for (auto &[name, en] : enums) {
            std::string names;
            for (auto &m : en->members)
                names += (names.empty() ? "" : ",") + m;
            call("fw_enum_register", b.getVoidTy(),
                 {i64(enumIds.at(name)), b.CreateGlobalString(names)});
        }
        validateClasses();
        cells = cellNames(p.body);
        block(p.body, false);
        if (!terminated()) {
            if (usesWeb()) {
                call("fw_web_wait", b.getVoidTy(), {});
                call("fw_web_shutdown", b.getVoidTy(), {});
            }
            call("fw_drop", b.getVoidTy(), {frame});
            // Globals are released with the frame; collect any cycles they kept alive.
            call("fw_gc_collect", b.getInt64Ty(), {});
            b.CreateRet(b.getInt32(0));
        }
        emitFailure();
        top = false;
        // Building a body can instantiate generic functions and classes, adding signatures.
        for (bool pending = true; pending;) {
            pending = false;
            std::vector<std::string> names;
            for (auto &[name, sig] : functions)
                if (!sig.ast->abstract && !sig.done)
                    names.push_back(name);
            for (auto &name : names) {
                auto &sig = functions.at(name);
                if (sig.done)
                    continue;
                pending = true;
                if (sig.result.name == "$infer")
                    buildFunction(sig, true);
                if (!sig.done)
                    buildFunction(sig, false);
            }
        }
        for (auto &[name, body] : p.components)
            if (!instantiatedComponents.count(name))
                checkTemplate(*body);
        if (coverage)
            registerCoverage(mainFunction);
        if (dib)
            dib->finalize();
        if (llvm::verifyModule(module, &llvm::errs()))
            throw std::runtime_error("internal error: LLVM verification failed");
        if (opts.optimization) {
            llvm::PassBuilder pb(machine.get());
            llvm::LoopAnalysisManager lam;
            llvm::FunctionAnalysisManager fam;
            llvm::CGSCCAnalysisManager cgam;
            llvm::ModuleAnalysisManager mam;
            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cgam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cgam, mam);
            auto level = opts.optimization == 1   ? llvm::OptimizationLevel::O1
                         : opts.optimization == 2 ? llvm::OptimizationLevel::O2
                                                  : llvm::OptimizationLevel::O3;
            auto pipeline = pb.buildPerModuleDefaultPipeline(level);
            pipeline.run(module, mam);
            if (llvm::verifyModule(module, &llvm::errs()))
                throw std::runtime_error("internal error: optimized LLVM verification failed");
        }
        if (!opts.ir.empty()) {
            std::error_code ec;
            llvm::raw_fd_ostream output(opts.ir, ec);
            if (ec)
                throw std::runtime_error(ec.message());
            module.print(output, nullptr);
        }
        auto emit = [&](llvm::raw_pwrite_stream &output) {
            // -O0 uses the fast instruction selector and skips machine-level optimization.
            machine->setOptLevel(opts.optimization == 0   ? llvm::CodeGenOptLevel::None
                                 : opts.optimization == 1 ? llvm::CodeGenOptLevel::Less
                                 : opts.optimization == 2 ? llvm::CodeGenOptLevel::Default
                                                          : llvm::CodeGenOptLevel::Aggressive);
            llvm::legacy::PassManager pm;
            if (machine->addPassesToEmitFile(pm, output, nullptr,
                                             llvm::CodeGenFileType::ObjectFile))
                throw std::runtime_error("target cannot emit object files");
            pm.run(module);
        };
        if (opts.objectData) {
            llvm::SmallVector<char, 0> buffer;
            llvm::raw_svector_ostream output(buffer);
            emit(output);
            opts.objectData->assign(buffer.begin(), buffer.end());
        } else if (!opts.object.empty()) {
            std::error_code ec;
            llvm::raw_fd_ostream output(opts.object, ec, llvm::sys::fs::OF_None);
            if (ec)
                throw std::runtime_error(ec.message());
            emit(output);
            output.flush();
        }
    }
};
Ty Compiler::inferCollectionCast(const std::string &constructor, Ty source) {
    auto common = [&](std::vector<Ty> parts) -> Ty {
        if (parts.empty())
            error("empty collection needs an explicit element type");
        auto result = parts[0];
        for (auto &part : parts) {
            if (result == part)
                continue;
            if (widening(result, part))
                result = part;
            else if (widening(part, result))
                continue;
            else if ((result.name == "element" || result.name == "fragment") &&
                     (part.name == "element" || part.name == "fragment"))
                result = Ty{"element"};
            else
                error("heterogeneous collection needs an explicit target element type");
        }
        return result;
    };
    if (constructor == "map") {
        if (source.map())
            return source;
        auto pairTypes = source.tuple()      ? source.parts()
                         : source.sequence() ? std::vector<Ty>{source.item()}
                                             : std::vector<Ty>{};
        std::vector<Ty> keys, values;
        for (auto &pair : pairTypes) {
            if (pair.tuple()) {
                auto parts = pair.parts();
                if (parts.size() != 2)
                    error("map casts require two-item pairs");
                keys.push_back(parts[0]);
                values.push_back(parts[1]);
            } else if (pair.sequence()) {
                keys.push_back(pair.item());
                values.push_back(pair.item());
            } else
                error("map casts require a sequence of pairs");
        }
        return type("map<" + common(keys).name + "," + common(values).name + ">");
    }
    Ty item;
    if (source.map()) {
        auto parts = source.parts();
        item = Ty{"[" + parts[0].name + "," + parts[1].name + "]"};
    } else if (source.sequence())
        item = source.item();
    else if (source.tuple()) {
        if (constructor == "tuple")
            return source;
        item = common(source.parts());
    } else
        item = source;
    return type(constructor == "array" ? item.name + "[]" : "tuple<" + item.name + ">");
}
Val Compiler::collectionCast(Val source, Ty target) {
    if (source.t.classReference() || source.t.name == "void" ||
        source.t.name.rfind("fn:", 0) == 0 || source.t.name.rfind("module:", 0) == 0)
        error("unsupported cast source " + source.t.name);
    Val box{objectCall("fw_new", {frame, i64(1), i64(2)}), Ty{"Error"}};
    set(box, i64(0), source);
    Val converted{
        objectCall("fw_collection_cast", {frame, box.v, b.CreateGlobalString(source.t.name),
                                          b.CreateGlobalString(target.name)}),
        Ty{"Error"}};
    return get(converted, i64(0), target);
}
Val Compiler::explicitCast(Val source, Ty target) {
    if (target.name == "string")
        return stringify(source);
    if (source.t.name == "void" || source.t.name.rfind("module:", 0) == 0)
        error("unsupported cast source " + source.t.name);
    if (source.t.name.rfind("fn:", 0) == 0 || source.t.classReference()) {
        if (target.name == "bool")
            return {b.getTrue(), Ty{"bool"}};
        error("unsupported cast source " + source.t.name);
    }
    if (source.t == target && !target.array() && !target.tuple() && !target.homogeneousTuple() &&
        !target.map())
        return source;
    if (target.name == "char") {
        if (source.t.name == "string")
            return {b.CreateTrunc(call("fw_string_to_char", b.getInt64Ty(), {source.v}),
                                  b.getInt32Ty()),
                    target};
        if (!source.t.integer())
            error("char() converts an integer code point or a one-character string");
        auto code = convert(source, Ty{"i64"}, true);
        return {b.CreateTrunc(call("fw_int_to_char", b.getInt64Ty(), {code.v}), b.getInt32Ty()),
                target};
    }
    if (source.t.enumeration() && target.numeric())
        return convert({source.v, Ty{"i64"}}, target, true);
    if (source.t.name == "char" && target.numeric())
        return convert({b.CreateZExt(source.v, b.getInt64Ty()), Ty{"i64"}}, target, true);
    if ((source.t.numeric() || source.t.name == "bool") && target.numeric())
        return convert(source, target, true);
    return collectionCast(source, target);
}
Val Compiler::value(Expr &e, Ty expected) {
    location = e.loc;
    debugLocation();
    if (e.kind == "name" || e.kind == "member") {
        auto name = callable(e);
        if (e.kind == "name")
            if (auto variable = lookup(name)) {
                // Check global initialization even though the constructor identity is static.
                if (variable->type.classReference())
                    return load(*variable);
                name.clear();
            }
        if (name == "desktop.Window")
            name = "desktop.window";
        if (auto open = name.find('<'); open != std::string::npos) {
            auto base = name.substr(0, open);
            std::vector<Ty> arguments;
            for (auto &part : typeParts(name.substr(open + 1, name.size() - open - 2)))
                arguments.push_back(type(part));
            if (templates.count(base))
                name = instantiateClass(base, arguments);
            else if (functionTemplates.count(base)) {
                auto key = instantiateFunction(base, arguments);
                return {signature(key).function, Ty{"fn:" + key}};
            }
        } else if (functionTemplates.count(name))
            error("generic function " + displayName(name) +
                  " needs type arguments to be used as a value, such as " + displayName(name) +
                  "<int>");
        // A member of an enum with data that has no fields is a value: Shape.Empty.
        if (classes.count(name) && !classes.at(name)->variantOf.empty() &&
            classes.at(name)->fields.empty() && !lookup(name)) {
            Expr construct;
            construct.kind = "call";
            construct.loc = e.loc;
            construct.args.push_back(clone(e));
            return invoke(construct, expected);
        }
        // A bare class name is a constructor reference; construction always uses a call.
        if (classes.count(name) || name == "desktop.window")
            return {llvm::ConstantPointerNull::get(b.getPtrTy()), Ty{"class:" + name}};
    }
    if (e.kind == "int") {
        llvm::APInt n;
        if (llvm::StringRef(e.text).getAsInteger(10, n) || n.getActiveBits() > 64)
            error("integer literal exceeds u64 range");
        // Literals above the signed 64-bit maximum are u64 constants.
        if (n.getActiveBits() == 64)
            return {b.getInt64(n.getZExtValue()), Ty{"u64"}};
        return {i64(static_cast<int64_t>(n.getZExtValue())), Ty{"i64"}};
    }
    if (e.kind == "real") {
        try {
            auto n = std::stod(e.text);
            if (!std::isfinite(n))
                error("floating literal out of range");
            return {llvm::ConstantFP::get(b.getDoubleTy(), n), Ty{"double"}};
        } catch (...) {
            error("floating literal out of range");
        }
    }
    if (e.kind == "bool")
        return {b.getInt1(e.text == "true"), Ty{"bool"}};
    if (e.kind == "char") {
        uint32_t scalar = 0;
        auto c = static_cast<unsigned char>(e.text[0]);
        size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        scalar = n == 1 ? c : c & (0x3f >> (n - 1));
        for (size_t i = 1; i < n; ++i)
            scalar = (scalar << 6) | (static_cast<unsigned char>(e.text[i]) & 63);
        return {b.getInt32(scalar), Ty{"char"}};
    }
    if (e.kind == "null")
        return {llvm::ConstantPointerNull::get(b.getPtrTy()), Ty{"null"}};
    if (e.kind == "string")
        return literal(e.text);
    if (e.kind == "format") {
        // `{value:spec}` in an interpolated string.
        auto x = unwrap(value(*e.args[0]));
        auto spec = literal(e.text);
        if (x.t.integer())
            return {objectCall("fw_format_int",
                               {frame, convert(x, x.t.unsigned_() ? Ty{"u64"} : Ty{"i64"}).v,
                                i64(x.t.unsigned_()), spec.v}),
                    Ty{"string"}};
        if (x.t.real())
            return {objectCall("fw_format_real", {frame, convert(x, Ty{"double"}).v, spec.v}),
                    Ty{"string"}};
        return {objectCall("fw_format_text", {frame, stringify(x).v, spec.v}), Ty{"string"}};
    }
    if (e.kind == "interpolate") {
        Val out = literal("");
        for (auto &x : e.args) {
            auto str = stringify(value(*x));
            out = {objectCall("fw_concat", {frame, out.v, str.v}), Ty{"string"}};
        }
        return out;
    }
    if (e.kind == "name") {
        if (auto v = lookup(e.text))
            return load(*v);
        if (p.imports.count(e.text))
            return {nullptr, Ty{"module:" + p.imports.at(e.text)}};
        if (functions.count(e.text)) {
            auto &sig = signature(e.text);
            return {sig.function, Ty{"fn:" + e.text}};
        }
        error("undeclared identifier '" + e.text + "'; declarations require var, const, or a type");
    }
    if (e.kind == "attr") {
        if (attrs.empty())
            error("'attr' is valid only in a component template");
        auto it = attrs.back().find(e.text);
        if (it == attrs.back().end())
            error("missing required component attribute '" + e.text + "'");
        auto &types = componentTypes[componentNames.back()];
        if (types.count(e.text) && types.at(e.text) != it->second.t)
            error("inconsistent component attribute type for '" + e.text + "'");
        types[e.text] = it->second.t;
        return it->second;
    }
    if (e.kind == "inst") {
        auto obj = receiver();
        if (!hasField(obj.t.name, e.text) && !resolveMethod(obj.t.name, e.text).empty())
            return boundMethod(obj, e.text);
        auto [c, i] = field(obj.t, e.text);
        return get(obj, i64(i), type(c->fields[i].type));
    }
    if (e.kind == "member" && e.args[0]->kind == "name" && enums.count(e.args[0]->text) &&
        !lookup(e.args[0]->text)) {
        auto &en = *enums.at(e.args[0]->text);
        auto it = std::find(en.members.begin(), en.members.end(), e.text);
        if (it == en.members.end())
            error("enum " + displayName(en.name) + " has no member '" + e.text + "'");
        return {i64(it - en.members.begin()), Ty{en.name}};
    }
    if (e.kind == "is" || e.kind == "as")
        return typeTest(e);
    if (e.kind == "super")
        error("'super' must be followed by a method call, such as super.init(...)");
    if (e.kind == "member") {
        auto obj = unwrap(value(*e.args[0]));
        if (obj.t.enumeration()) {
            if (e.text == "value")
                return {obj.v, Ty{"i64"}};
            if (e.text == "name")
                return stringify(obj);
            error("enum values have .name and .value");
        }
        if (obj.t.name == "module:math") {
            static const std::map<std::string, double> constants = {
                {"PI", 3.141592653589793},
                {"TAU", 6.283185307179586},
                {"E", 2.718281828459045},
                {"INF", std::numeric_limits<double>::infinity()},
                {"NAN", std::numeric_limits<double>::quiet_NaN()}};
            if (constants.count(e.text))
                return {llvm::ConstantFP::get(b.getDoubleTy(), constants.at(e.text)),
                        Ty{"double"}};
        }
        if (obj.t.name == "module:desktop")
            error("module 'desktop' has no member '" + e.text +
                  "'; use desktop.window or desktop.Window");
        if (obj.t.result() && e.text == "is_ok")
            return get(obj, i64(0), Ty{"bool"});
        if ((obj.t.name == "string" || obj.t.sequence() || obj.t.map() || obj.t.set() ||
             obj.t.tuple()) &&
            e.text == "length")
            return {call("fw_length", b.getInt64Ty(), {obj.v}), Ty{"i64"}};
        if (obj.t.name == "element" || obj.t.name == "fragment") {
            if (e.text == "tag")
                return {objectCall("fw_tag", {frame, obj.v}), Ty{"string"}};
            if (e.text == "attributes" || e.text == "children" || e.text == "text_content" ||
                e.text == "inner_html" || e.text == "outer_html" || e.text == "id" ||
                e.text == "class_name")
                return *domCall("html." + e.text, {obj});
            if (e.text == "parent" || e.text == "next_sibling" || e.text == "previous_sibling")
                return {objectCall("fw_dom_relative",
                                   {frame, obj.v,
                                    i64(e.text == "parent" ? 0 : e.text == "next_sibling" ? 1 : 2)}),
                        Ty{"element?"}};
        }
        if (errorTypes.count(obj.t.name) && e.text == "message")
            return {objectCall("fw_error_message", {frame, obj.v}), Ty{"string"}};
        if (classes.count(obj.t.name) && !hasField(obj.t.name, e.text) &&
            !resolveMethod(obj.t.name, e.text).empty())
            return boundMethod(obj, e.text);
        auto [c, i] = field(obj.t, e.text);
        return get(obj, i64(i), type(c->fields[i].type));
    }
    if (e.kind == "index") {
        auto obj = value(*e.args[0]);
        auto &position = *e.args[1];
        if ((obj.t.name == "string" || obj.t.sequence()) && position.kind == "binary" &&
            !position.grouped && (position.text == ".." || position.text == "..<")) {
            auto from = convert(value(*position.args[0]), Ty{"i64"});
            auto to = convert(value(*position.args[1]), Ty{"i64"});
            auto mode = i64(position.text == ".." ? 2 : 1);
            if (obj.t.name == "string")
                return {objectCall("fw_string_slice", {frame, obj.v, from.v, to.v, mode}), obj.t};
            return {objectCall("fw_array_slice", {frame, obj.v, from.v, to.v, mode}), obj.t};
        }
        if (obj.t.name == "string") {
            auto index = convert(value(position), Ty{"i64"});
            return {b.CreateTrunc(call("fw_string_char_at", b.getInt64Ty(), {obj.v, index.v}),
                                  b.getInt32Ty()),
                    Ty{"char"}};
        }
        if (obj.t.name == "json.value") {
            // json[0] indexes an array; json["key"] reads an object field.
            auto key = value(position);
            if (key.t.name == "string")
                return {objectCall("fw_json_get", {frame, obj.v, key.v}), obj.t};
            return {objectCall("fw_json_at", {frame, obj.v, convert(key, Ty{"i64"}).v}), obj.t};
        }
        if (classes.count(obj.t.name)) {
            if (resolveMethod(obj.t.name, "operator[]").empty())
                error("class " + displayName(obj.t.name) +
                      " cannot be indexed; declare `function operator [](...)`");
            return callNamedMethod(obj, "operator[]", {value(position)});
        }
        auto [index, t] = indexOf(obj, position, false);
        return get(obj, index, t);
    }
    if (e.kind == "call")
        return invoke(e, expected);
    if (e.kind == "lambda")
        return lambda(e, expected);
    if (e.kind == "array") {
        std::vector<Val> items;
        auto parts = expected.tuple() ? expected.parts() : std::vector<Ty>{};
        if (expected.tuple() && parts.size() != e.args.size())
            error("tuple length does not match declared type");
        Ty item = expected.sequence() ? expected.item() : Ty{};
        bool tuple = expected.tuple();
        for (size_t i = 0; i < e.args.size(); ++i) {
            auto v = value(*e.args[i], expected.tuple() ? parts[i] : item);
            if (v.t.classReference())
                v = convert(v, item.function() ? item : constructorType(v.t.name.substr(6)));
            items.push_back(v);
            if (item.name == "void")
                item = v.t;
            else if (v.t != item && !expected.sequence() && !tuple) {
                if (widening(item, v.t))
                    item = v.t;
                else if (!widening(v.t, item))
                    tuple = true;
            }
        }
        if (items.empty() && !expected.sequence() && !expected.tuple()) {
            // `function() returns int[]` is a function returning an array; point at the grouping.
            if (expected.function() && expected.parts()[0].array()) {
                auto parts = expected.parts();
                std::string grouped = "function<" + parts[0].name.substr(0, parts[0].name.size() - 2);
                for (size_t i = 1; i < parts.size(); ++i)
                    grouped += "," + parts[i].name;
                error("[] is not a " + expected.name +
                      "; for an array of functions, group the function type: (" + grouped + ">)[]");
            }
            error("empty arrays require an expected array type");
        }
        Ty t;
        if (tuple) {
            t.name = "[";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i)
                    t.name += ",";
                t.name += (expected.tuple() ? parts[i] : items[i].t).name;
            }
            t.name += "]";
        } else
            t.name = expected.homogeneousTuple() ? "tuple<" + item.name + ">" : item.name + "[]";
        Val out{objectCall("fw_new", {frame, i64(items.size()),
                                      i64(t.tuple() || t.homogeneousTuple() ? 7 : 1)}),
                t};
        for (size_t i = 0; i < items.size(); ++i) {
            auto target = tuple ? t.parts()[i] : item;
            if (target.name == "void" || target.name == "null")
                error("collection item needs a concrete type");
            set(out, i64(i), convert(items[i], target));
        }
        return out;
    }
    if (e.kind == "set" || (e.kind == "map" && e.args.empty() && expected.set())) {
        Ty item = expected.set() ? expected.parts()[0] : Ty{};
        std::vector<Val> items;
        for (auto &a : e.args) {
            auto v = value(*a, item);
            if (item.name == "void")
                item = v.t;
            items.push_back(v);
        }
        auto out = newSet(type("set<" + item.name + ">"));
        for (auto &v : items)
            keyFind(out, convert(v, item), 1);
        return out;
    }
    if (e.kind == "map") {
        if (e.args.empty() && !expected.map())
            error("empty maps require an expected map type");
        std::vector<Val> keys, values;
        Ty key = expected.map() ? expected.parts()[0] : Ty{},
           item = expected.map() ? expected.parts()[1] : Ty{};
        for (size_t i = 0; i < e.args.size(); i += 2) {
            auto k = value(*e.args[i], key);
            auto v = value(*e.args[i + 1], item);
            if (key.name == "void")
                key = k.t;
            if (item.name == "void")
                item = v.t;
            keys.push_back(k);
            values.push_back(v);
        }
        Ty t = type("map<" + key.name + "," + item.name + ">");
        Val out{objectCall("fw_map_new", {frame, i64(mapKeyKind(key))}),
                t};
        for (size_t i = 0; i < keys.size(); ++i) {
            auto index = mapIndex(out, convert(keys[i], key), true);
            set(out, index, convert(values[i], item));
        }
        return out;
    }
    if (e.kind == "unary") {
        if (e.text == "-" && e.args[0]->kind == "int" && e.args[0]->text == "9223372036854775808")
            return {i64(std::numeric_limits<int64_t>::min()), Ty{"i64"}};
        auto a = value(*e.args[0]);
        if (e.text == "not") {
            if (a.t.name != "bool")
                error("'not' requires bool (no implicit truthiness)");
            return {b.CreateNot(a.v), a.t};
        }
        if (classes.count(a.t.name) && e.text == "-") {
            if (resolveMethod(a.t.name, "operator-()").empty())
                error("class " + displayName(a.t.name) +
                      " does not define unary minus; declare `function operator -()`");
            return callNamedMethod(a, "operator-()", {});
        }
        if (!a.t.numeric())
            error("unary operator requires a number");
        if (e.text == "+")
            return a;
        if (a.t.real())
            return {b.CreateFNeg(a.v), a.t};
        return binary("-", {llvm::ConstantInt::get(ll(a.t), 0), a.t}, a);
    }
    if (e.kind == "binary" && e.text == "??")
        return coalesce(e, expected);
    if (e.kind == "binary") {
        auto a = value(*e.args[0]);
        if (e.text == "and" || e.text == "or") {
            if (a.t.name != "bool")
                error("boolean operators require bool operands");
            auto left = b.GetInsertBlock();
            auto right = llvm::BasicBlock::Create(ctx, "boolean.rhs", current);
            auto merge = llvm::BasicBlock::Create(ctx, "boolean.end", current);
            bool and_ = e.text == "and";
            b.CreateCondBr(a.v, and_ ? right : merge, and_ ? merge : right);
            b.SetInsertPoint(right);
            // `x != null and x > 0` reads x as non-null on the right-hand side.
            std::vector<std::string> proven;
            nonNullNames(*e.args[0], and_, proven);
            std::vector<std::pair<std::string, std::string>> instances;
            if (and_)
                instanceNames(*e.args[0], true, instances);
            scopes.emplace_back();
            narrow(proven);
            narrowTypes(instances);
            Val c;
            try {
                c = value(*e.args[1]);
            } catch (...) {
                scopes.pop_back();
                throw;
            }
            scopes.pop_back();
            if (c.t.name != "bool")
                error("boolean operators require bool operands");
            auto tail = b.GetInsertBlock();
            b.CreateBr(merge);
            b.SetInsertPoint(merge);
            auto phi = b.CreatePHI(b.getInt1Ty(), 2);
            phi->addIncoming(b.getInt1(!and_), left);
            phi->addIncoming(c.v, tail);
            return {phi, Ty{"bool"}};
        }
        auto c = value(*e.args[1]);
        if (e.text == ".." || e.text == "..<") {
            if (!a.t.integer() || !c.t.integer())
                error("range endpoints must be integers");
            a = convert(a, Ty{"i64"});
            c = convert(c, Ty{"i64"});
            return {objectCall("fw_tuple_range", {frame, a.v, c.v, i64(e.text == ".." ? 1 : 0)}),
                    Ty{"tuple<i64>"}};
        }
        if (e.text == "*" &&
            ((a.t.integer() &&
              (c.t.array() || c.t.tuple() || c.t.homogeneousTuple() || c.t.name == "string")) ||
             (c.t.integer() &&
              (a.t.array() || a.t.tuple() || a.t.homogeneousTuple() || a.t.name == "string")))) {
            Val source = a.t.integer() ? c : a, count = a.t.integer() ? a : c;
            auto target = source.t;
            if (target.tuple()) {
                auto constant = constantIndex(*(a.t.integer() ? e.args[0] : e.args[1]));
                auto parts = target.parts();
                if (parts.empty())
                    target.name = "[]";
                else if (!constant) {
                    if (!std::all_of(parts.begin(), parts.end(),
                                     [&](Ty part) { return part == parts[0]; }))
                        error("heterogeneous tuple repetition needs a constant count; cast to a "
                              "homogeneous collection first");
                    target.name = "tuple<" + parts[0].name + ">";
                } else {
                    if (*constant < 0)
                        error("tuple repetition count must be non-negative");
                    if (static_cast<uint64_t>(*constant) > 4096 / parts.size())
                        error("repeated tuple type exceeds 4096 positions");
                    target.name = "[";
                    for (int64_t n = 0; n < *constant; ++n)
                        for (auto &part : parts) {
                            if (target.name.size() > 1)
                                target.name += ",";
                            target.name += part.name;
                        }
                    target.name += "]";
                }
            }
            count = convert(count, Ty{"i64"});
            return {objectCall("fw_repeat", {frame, source.v, count.v}), target};
        }
        if (e.text == "*" && (a.t.map() || c.t.map()))
            error("maps cannot be repeated because keys must be unique");
        if (e.text == "in") {
            if (c.t.map() || c.t.set()) {
                auto key = convert(a, c.t.parts()[0]);
                return {b.CreateICmpSGE(keyFind(c, key, 2), i64(0)), Ty{"bool"}};
            }
            if (c.t.name == "string") {
                if (a.t.name != "string" && a.t.name != "char")
                    error("string membership requires a string or char");
                return {b.CreateICmpNE(call("fw_string_search", b.getInt64Ty(),
                                            {c.v, stringify(a).v, i64(1)}),
                                       i64(0)),
                        Ty{"bool"}};
            }
            if (!c.t.sequence())
                error("membership requires an array, homogeneous tuple, string, or map");
            a = convert(a, c.t.item());
            std::string suffix = a.t.object() ? "object" : a.t.real() ? "real" : "int";
            llvm::Value *item = a.v;
            if (a.t.real())
                item = b.CreateFPCast(item, b.getDoubleTy());
            else if (!a.t.object())
                item =
                    b.CreateIntCast(item, b.getInt64Ty(), a.t.name != "bool" && !a.t.unsigned_());
            return {
                b.CreateICmpNE(call("fw_contains_" + suffix, b.getInt64Ty(), {c.v, item}), i64(0)),
                Ty{"bool"}};
        }
        return binary(e.text, a, c);
    }
    if (e.kind == "chain") {
        // Each comparison is evaluated only while the previous ones hold.
        auto left = value(*e.args[0]);
        auto merge = llvm::BasicBlock::Create(ctx, "chain.end", current);
        std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>> exits;
        Val result;
        for (size_t i = 1; i < e.args.size(); ++i) {
            auto right = value(*e.args[i]);
            result = binary(e.names[i - 1], left, right);
            if (i + 1 == e.args.size())
                break;
            auto next = llvm::BasicBlock::Create(ctx, "chain.next", current);
            exits.emplace_back(b.GetInsertBlock(), b.getFalse());
            b.CreateCondBr(result.v, next, merge);
            b.SetInsertPoint(next);
            left = right;
        }
        exits.emplace_back(b.GetInsertBlock(), result.v);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        auto phi = b.CreatePHI(b.getInt1Ty(), static_cast<unsigned>(exits.size()));
        for (auto &[block, v] : exits)
            phi->addIncoming(v, block);
        return {phi, Ty{"bool"}};
    }
    if (e.kind == "element") {
        if (p.components.count(e.text)) {
            instantiatedComponents.insert(e.text);
            if (!componentStack.insert(e.text).second)
                error("recursive component expansion is not supported");
            std::map<std::string, Val> supplied;
            for (size_t i = 0; i < e.names.size(); ++i)
                supplied[e.names[i]] = value(*e.args[i]);
            auto empty = literal("");
            Val inner{objectCall("fw_element", {frame, empty.v}), Ty{"fragment"}};
            for (size_t i = e.names.size(); i < e.args.size(); ++i) {
                markupChild(inner, value(*e.args[i]));
            }
            if (supplied.count("inner"))
                error("'inner' is reserved for component child content");
            supplied["inner"] = inner;
            if (!supplied.count("style"))
                supplied["style"] = empty;
            attrs.push_back(std::move(supplied));
            componentNames.push_back(e.text);
            auto result = value(*p.components.at(e.text));
            attrs.pop_back();
            componentNames.pop_back();
            componentStack.erase(e.text);
            return result;
        }
        auto rules = tagRules(e.text);
        if (!e.text.empty() && !rules)
            error("unknown tag '" + e.text + "'");
        auto tag = literal(e.text);
        Val tree{objectCall("fw_element", {frame, tag.v}),
                 Ty{e.text.empty() ? "fragment" : "element"}};
        for (size_t i = 0; i < e.names.size(); ++i) {
            auto &name = e.names[i];
            if (name.rfind("on", 0) == 0 && name.size() > 2) {
                // String handlers are inline JavaScript. Function handlers run Flywheel code in
                // desktop windows; HTML rendered for the web omits them.
                auto handler = asFunction(value(*e.args[i], Ty{"function<void>"}));
                auto key = literal(name);
                if (handler.t.name == "string") {
                    call("fw_attribute", b.getVoidTy(), {tree.v, key.v, handler.v});
                    continue;
                }
                if (handler.t.name != "function<void>" && handler.t.name != "function<void,string>")
                    error("event attribute '" + name +
                          "' expects JavaScript text, a function() returns void, or a "
                          "function(string value) returns void");
                call("fw_element_event", b.getVoidTy(),
                     {tree.v, key.v, handler.v, i64(handler.t.parts().size() - 1)});
                continue;
            }
            if (!attributeAllowed(*rules, name))
                error("unknown attribute '" + name + "' on <" + e.text + ">");
            // A valueless attribute (`<input disabled>`) is true for a boolean attribute.
            bool valueless = e.args[i]->kind == "string" && e.args[i]->outer;
            auto x = valueless && rules->booleans.count(name) ? Val{b.getTrue(), Ty{"bool"}}
                                                               : value(*e.args[i]);
            auto key = literal(name);
            if (rules->booleans.count(name)) {
                if (x.t.name != "bool")
                    error("boolean attribute '" + name + "' requires bool");
                auto yes = llvm::BasicBlock::Create(ctx, "attribute", current);
                auto after = llvm::BasicBlock::Create(ctx, "attribute.end", current);
                b.CreateCondBr(x.v, yes, after);
                b.SetInsertPoint(yes);
                auto v = literal("");
                call("fw_attribute", b.getVoidTy(), {tree.v, key.v, v.v});
                b.CreateBr(after);
                b.SetInsertPoint(after);
            } else {
                // Numbers and chars render with their usual text; other values need string().
                if (x.t.numeric() || x.t.name == "char" || x.t.enumeration())
                    x = stringify(x);
                if (x.t.name != "string")
                    error("attribute '" + name + "' requires a string or number, not " +
                          displayName(x.t.name));
                call("fw_attribute", b.getVoidTy(), {tree.v, key.v, x.v});
            }
        }
        if (rules && rules->isVoid && e.args.size() != e.names.size())
            error("void element <" + e.text + "> cannot have children");
        for (size_t i = e.names.size(); i < e.args.size(); ++i) {
            markupChild(tree, value(*e.args[i]));
        }
        return tree;
    }
    error("unsupported expression '" + e.kind + "'");
}
// Allowed attributes per tag for HTML (import html.*), SVG (import svg.*), and declared
// element families. data-*, aria-*, and on* event attributes are accepted on every tag.
const Compiler::TagRules *Compiler::tagRules(const std::string &tag) {
    static const std::set<std::string> htmlBooleans = {
        "disabled", "checked",   "required", "selected", "hidden",   "readonly",
        "multiple", "autofocus", "autoplay", "controls", "loop",     "muted",
        "open",     "novalidate", "reversed", "defer",   "async",    "allowfullscreen",
        "default",  "ismap",     "playsinline", "nomodule", "formnovalidate", "inert",
        "itemscope"};
    static const std::map<std::string, TagRules> html = [] {
        std::set<std::string> global = {
            "accesskey", "autocapitalize", "autofocus", "class", "contenteditable", "dir",
            "draggable", "enterkeyhint", "hidden", "id", "inert", "inputmode", "is", "itemid",
            "itemprop", "itemref", "itemscope", "itemtype", "lang", "nonce", "popover", "role",
            "slot", "spellcheck", "style", "tabindex", "title", "translate"};
        std::map<std::string, std::set<std::string>> specific = {
            {"a", {"href", "target", "rel", "download", "hreflang", "ping", "referrerpolicy",
                   "type"}},
            {"area", {"alt", "coords", "shape", "href", "target", "rel", "download"}},
            {"audio", {"src", "controls", "autoplay", "loop", "muted", "preload", "crossorigin"}},
            {"video", {"src", "controls", "autoplay", "loop", "muted", "preload", "poster",
                       "width", "height", "playsinline", "crossorigin"}},
            {"base", {"href", "target"}},
            {"blockquote", {"cite"}},
            {"q", {"cite"}},
            {"del", {"cite", "datetime"}},
            {"ins", {"cite", "datetime"}},
            {"button", {"type", "disabled", "name", "value", "form", "formaction", "formmethod",
                        "formenctype", "formnovalidate", "formtarget", "popovertarget",
                        "popovertargetaction"}},
            {"canvas", {"width", "height"}},
            {"col", {"span"}},
            {"colgroup", {"span"}},
            {"data", {"value"}},
            {"details", {"open", "name"}},
            {"dialog", {"open"}},
            {"embed", {"src", "type", "width", "height"}},
            {"fieldset", {"disabled", "form", "name"}},
            {"form", {"action", "method", "enctype", "target", "novalidate", "autocomplete",
                      "name", "accept-charset", "rel"}},
            {"iframe", {"src", "srcdoc", "name", "sandbox", "allow", "allowfullscreen", "width",
                        "height", "loading", "referrerpolicy"}},
            {"img", {"src", "alt", "width", "height", "srcset", "sizes", "loading", "decoding",
                     "crossorigin", "usemap", "ismap", "referrerpolicy", "fetchpriority"}},
            {"input", {"type", "name", "value", "placeholder", "disabled", "checked", "required",
                       "readonly", "min", "max", "step", "minlength", "maxlength", "pattern",
                       "size", "multiple", "accept", "autocomplete", "list", "form", "src",
                       "alt", "width", "height", "capture", "dirname", "formaction",
                       "formmethod"}},
            {"label", {"for", "form"}},
            {"li", {"value"}},
            {"link", {"href", "rel", "type", "media", "sizes", "crossorigin", "integrity", "as",
                      "hreflang", "referrerpolicy"}},
            {"map", {"name"}},
            {"meta", {"charset", "name", "content", "http-equiv", "property", "media"}},
            {"meter", {"value", "min", "max", "low", "high", "optimum"}},
            {"object", {"data", "type", "name", "width", "height", "form"}},
            {"ol", {"reversed", "start", "type"}},
            {"optgroup", {"label", "disabled"}},
            {"option", {"value", "selected", "disabled", "label"}},
            {"output", {"for", "form", "name"}},
            {"progress", {"value", "max"}},
            {"script", {"src", "type", "async", "defer", "crossorigin", "integrity", "nomodule",
                        "referrerpolicy"}},
            {"select", {"name", "disabled", "required", "multiple", "size", "form",
                        "autocomplete"}},
            {"source", {"src", "type", "srcset", "sizes", "media", "width", "height"}},
            {"style", {"media"}},
            {"td", {"colspan", "rowspan", "headers"}},
            {"th", {"colspan", "rowspan", "headers", "scope", "abbr"}},
            {"template", {"shadowrootmode"}},
            {"textarea", {"name", "placeholder", "disabled", "required", "readonly", "rows",
                          "cols", "minlength", "maxlength", "wrap", "form", "autocomplete"}},
            {"time", {"datetime"}},
            {"track", {"src", "kind", "srclang", "label", "default"}}};
        std::set<std::string> voids = {"area", "base", "br",   "col",   "embed", "hr", "img",
                                       "input", "link", "meta", "source", "track", "wbr"};
        std::map<std::string, TagRules> out;
        for (auto tag : {"a", "abbr", "address", "area", "article", "aside", "audio", "b", "base",
                         "bdi", "bdo", "blockquote", "body", "br", "button", "canvas", "caption",
                         "cite", "code", "col", "colgroup", "data", "datalist", "dd", "del",
                         "details", "dfn", "dialog", "div", "dl", "dt", "em", "embed", "fieldset",
                         "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5",
                         "h6", "head", "header", "hgroup", "hr", "html", "i", "iframe", "img",
                         "input", "ins", "kbd", "label", "legend", "li", "link", "main", "map",
                         "mark", "menu", "meta", "meter", "nav", "noscript", "object", "ol",
                         "optgroup", "option", "output", "p", "picture", "pre", "progress", "q",
                         "rp", "rt", "ruby", "s", "samp", "script", "search", "section",
                         "select", "slot", "small", "source", "span", "strong", "style", "sub",
                         "summary", "sup", "table", "tbody", "td", "template", "textarea",
                         "tfoot", "th", "thead", "time", "title", "tr", "track", "u", "ul",
                         "var", "video", "wbr"}) {
            TagRules rules;
            rules.attributes = global;
            if (auto it = specific.find(tag); it != specific.end())
                rules.attributes.insert(it->second.begin(), it->second.end());
            rules.isVoid = voids.count(tag) > 0;
            rules.booleans = htmlBooleans;
            out[tag] = std::move(rules);
        }
        return out;
    }();
    static const std::map<std::string, TagRules> svg = [] {
        std::set<std::string> attributes = {
            "id", "class", "style", "lang", "tabindex", "role", "viewBox", "xmlns", "xmlns:xlink",
            "width", "height", "x", "y", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry", "dx",
            "dy", "d", "points", "pathLength", "fill", "fill-opacity", "fill-rule", "stroke",
            "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-dasharray",
            "stroke-dashoffset", "stroke-opacity", "stroke-miterlimit", "opacity", "transform",
            "href", "xlink:href", "offset", "stop-color", "stop-opacity", "gradientUnits",
            "gradientTransform", "spreadMethod", "fx", "fy", "font-size", "font-family",
            "font-weight", "text-anchor", "dominant-baseline", "letter-spacing", "lengthAdjust",
            "textLength", "preserveAspectRatio", "clip-path", "clipPathUnits", "mask",
            "maskUnits", "marker-start", "marker-mid", "marker-end", "refX", "refY",
            "markerWidth", "markerHeight", "markerUnits", "orient", "patternUnits",
            "patternContentUnits", "patternTransform", "visibility", "display", "filter",
            "vector-effect", "shape-rendering", "color"};
        std::map<std::string, TagRules> out;
        for (auto tag : {"svg", "g", "path", "circle", "ellipse", "line", "polyline", "polygon",
                         "rect", "text", "tspan", "textPath", "defs", "use", "symbol",
                         "linearGradient", "radialGradient", "stop", "clipPath", "mask",
                         "pattern", "image", "title", "desc", "marker", "foreignObject",
                         "switch", "view"}) {
            TagRules rules;
            rules.attributes = attributes;
            out[tag] = std::move(rules);
        }
        return out;
    }();
    if (p.html)
        if (auto it = html.find(tag); it != html.end())
            return &it->second;
    if (p.svg)
        if (auto it = svg.find(tag); it != svg.end())
            return &it->second;
    if (auto it = familyRules.find(tag); it != familyRules.end())
        return &it->second;
    return nullptr;
}
bool Compiler::attributeAllowed(const TagRules &rules, const std::string &name) {
    return rules.any || rules.attributes.count(name) || name.rfind("data-", 0) == 0 ||
           name.rfind("aria-", 0) == 0;
}
std::optional<Val> Compiler::domCall(const std::string &name, std::vector<Val> args) {
    if (name.rfind("html.", 0) != 0)
        return std::nullopt;
    auto method = name.substr(5);
    if (method == "parse") {
        // html.parse(text): a fragment holding the parsed nodes.
        if (args.size() != 1)
            error("html.parse expects HTML text");
        return Val{objectCall("fw_html_parse", {frame, convert(args[0], Ty{"string"}).v}),
                   Ty{"fragment"}};
    }
    static const std::set<std::string> methods = {"attributes",
                                                  "closest",
                                                  "query",
                                                  "query_all",
                                                  "get_by_id",
                                                  "matches",
                                                  "children",
                                                  "text_content",
                                                  "get_text",
                                                  "inner_html",
                                                  "outer_html",
                                                  "id",
                                                  "class_name",
                                                  "get_attribute",
                                                  "has_attribute",
                                                  "set_attribute",
                                                  "remove_attribute",
                                                  "set_text",
                                                  "append",
                                                  "prepend",
                                                  "remove_child",
                                                  "clear",
                                                  "clone",
                                                  "has_class",
                                                  "add_class",
                                                  "remove_class",
                                                  "toggle_class"};
    if (!methods.count(method))
        return std::nullopt;
    if (args.empty())
        error(name + " requires a tree as its first argument");
    auto root = args[0];
    if (root.t.name == "element?" || root.t.name == "fragment?")
        root = convert(root, Ty{root.t.name.substr(0, root.t.name.size() - 1)}, true);
    if (root.t.name != "element" && root.t.name != "fragment")
        error(name + " requires an element or fragment tree");
    auto count = [&](size_t n) {
        if (args.size() != n)
            error(name + " expects " + std::to_string(n - 1) + " argument(s) after its tree");
    };
    auto text = [&](size_t i) { return convert(args[i], Ty{"string"}).v; };
    auto flag = [&](const std::string &function, std::vector<llvm::Value *> values) -> Val {
        return {b.CreateICmpNE(call(function, b.getInt64Ty(), std::move(values)), i64(0)),
                Ty{"bool"}};
    };
    if (method == "query" || method == "query_all") {
        count(2);
        return Val{objectCall("fw_dom_query", {frame, root.v, text(1), i64(method == "query_all")}),
                   Ty{method == "query_all" ? "element[]" : "element?"}};
    }
    if (method == "closest") {
        count(2);
        return Val{objectCall("fw_dom_closest", {frame, root.v, text(1)}), Ty{"element?"}};
    }
    if (method == "get_by_id") {
        count(2);
        return Val{objectCall("fw_dom_get_by_id", {frame, root.v, text(1)}), Ty{"element?"}};
    }
    if (method == "matches") {
        count(2);
        return flag("fw_dom_matches", {root.v, text(1)});
    }
    if (method == "attributes") {
        count(1);
        return Val{objectCall("fw_dom_attributes", {frame, root.v}), Ty{"map<string,string>"}};
    }
    if (method == "children") {
        count(1);
        return Val{objectCall("fw_dom_children", {frame, root.v}), Ty{"element[]"}};
    }
    if (method == "text_content" || method == "get_text" || method == "inner_html" ||
        method == "outer_html" || method == "id" || method == "class_name") {
        count(1);
        int code = method == "inner_html"   ? 1
                   : method == "outer_html" ? 2
                   : method == "id"         ? 3
                   : method == "class_name" ? 4
                                            : 0;
        return Val{objectCall("fw_dom_text", {frame, root.v, i64(code)}), Ty{"string"}};
    }
    if (method == "get_attribute") {
        count(2);
        return Val{objectCall("fw_dom_get_attribute", {frame, root.v, text(1)}), Ty{"string?"}};
    }
    if (method == "has_attribute" || method == "remove_attribute") {
        count(2);
        return flag(method == "has_attribute" ? "fw_dom_has_attribute" : "fw_dom_remove_attribute",
                    {root.v, text(1)});
    }
    if (method == "set_attribute") {
        count(3);
        call("fw_dom_set_attribute", b.getVoidTy(), {root.v, text(1), text(2)});
        return Val{};
    }
    if (method == "set_text") {
        count(2);
        call("fw_dom_set_text", b.getVoidTy(), {root.v, text(1)});
        return Val{};
    }
    if (method == "append" || method == "prepend") {
        count(2);
        auto child = args[1];
        if (child.t.name != "element" && child.t.name != "fragment" && child.t.name != "element?" &&
            child.t.name != "fragment?")
            child = stringify(child);
        call("fw_dom_append", b.getVoidTy(), {root.v, child.v, i64(method == "prepend")});
        return Val{};
    }
    if (method == "remove_child") {
        count(2);
        if (args[1].t.name != "element" && args[1].t.name != "fragment" &&
            args[1].t.name != "element?" && args[1].t.name != "fragment?")
            error("remove_child requires an element or fragment reference");
        return flag("fw_dom_remove_child", {root.v, args[1].v});
    }
    if (method == "clear") {
        count(1);
        call("fw_dom_clear", b.getVoidTy(), {root.v});
        return Val{};
    }
    if (method == "clone") {
        if (args.size() != 1 && args.size() != 2)
            error("clone expects an optional bool deep flag");
        auto deep = args.size() == 2 ? convert(args[1], Ty{"bool"}).v : b.getTrue();
        return Val{objectCall("fw_dom_clone", {frame, root.v, b.CreateZExt(deep, b.getInt64Ty())}),
                   root.t};
    }
    count(2);
    int operation = method == "has_class"      ? 0
                    : method == "add_class"    ? 1
                    : method == "remove_class" ? 2
                                               : 3;
    return flag("fw_dom_class", {root.v, text(1), i64(operation)});
}
// testing.expect_error(ErrorType, function): calls the function, requires it to raise that
// error type (or a subtype), and returns the error. Compiled as a generated try/catch.
Val Compiler::expectError(Expr &e) {
    if (e.args.size() != 3 || e.args[1]->kind != "name")
        error("testing.expect_error expects an error type and a function, such as "
              "testing.expect_error(ValueError, function() => int(\"x\"))");
    auto errorType = e.args[1]->text;
    if (!errorTypes.count(errorType) && !classes.count(errorType))
        error("'" + displayName(errorType) + "' is not an error type");
    auto function = asFunction(value(*e.args[2]));
    if (!function.t.function() || function.t.parts().size() != 1)
        error("testing.expect_error needs a function without parameters");
    auto id = std::to_string(hiddenCount++);
    auto f = "__fw_expect_f" + id, caught = "__fw_expect_caught" + id, err = "__fw_expect_e" + id;
    bind(f, function, true);
    auto call = function.t.parts()[0].name == "void" ? f + "()"
                                                     : "var __fw_expect_r" + id + " = " + f + "()";
    auto text = "__FW_T? " + caught + " = null\ntry\n    " + call + "\ncatch __FW_T " + err +
                "\n    " + caught + " = " + err + "\nend\nif " + caught +
                " == null then\n    throw AssertionError(\"expected __FW_NAME, but nothing was "
                "raised\")\nend\n";
    auto pos = text.find("__FW_NAME");
    text.replace(pos, 9, displayName(errorType));
    auto generated = Parser(lex(text)).parse();
    std::function<void(Stmt &)> place = [&](Stmt &st) {
        st.loc = e.loc;
        if (st.type == "__FW_T?")
            st.type = errorType + "?";
        for (auto &t : st.catchTypes)
            if (t == "__FW_T")
                t = errorType;
        for (auto *list : {&st.body, &st.other, &st.finalizer})
            for (auto &inner : *list)
                place(*inner);
        for (auto &c : st.catches)
            for (auto &inner : c)
                place(*inner);
    };
    for (auto &st : generated.body) {
        place(*st);
        statement(*st);
    }
    auto result = syntheticName(caught, e.loc);
    return value(*result);
}
Val Compiler::invoke(Expr &e, Ty expected) {
    auto &callee = *e.args[0];
    std::vector<Expr *> rest;
    for (size_t i = 1; i < e.args.size(); ++i)
        rest.push_back(e.args[i].get());
    if (callee.kind == "inst" && !owner.empty() && !resolveMethod(owner, callee.text).empty())
        return callMethod(receiver(), owner, callee.text, e, false);
    if (callee.kind == "inst" && !owner.empty())
        if (auto result = genericMethodCall(receiver(), callee.text, e, expected))
            return *result;
    if (callee.kind == "member" && callee.args[0]->kind == "super") {
        if (owner.empty())
            error("'super' is valid only in a method");
        auto &c = *classes.at(owner);
        if (c.base.empty())
            error("'super' requires a base class; " + displayName(owner) + " has none");
        auto self = receiver();
        if (callee.text == "init" && resolveMethod(c.base, "init").empty()) {
            // The base class's generated constructor: assign its fields positionally.
            auto &base = *classes.at(c.base);
            if (e.args.size() - 1 > base.fields.size())
                error("too many arguments to super.init for " + displayName(c.base));
            for (size_t i = 0; i < base.fields.size(); ++i) {
                auto &f = base.fields[i];
                Ty t = type(f.type);
                Val v;
                if (i + 1 < e.args.size())
                    v = value(*e.args[i + 1], t);
                else if (f.value)
                    v = defaultValue(*f.value, t);
                else
                    error("missing super.init argument for field '" + f.name + "'");
                set(self, i64(static_cast<int64_t>(i)), convert(v, t));
            }
            return {};
        }
        if (resolveMethod(c.base, callee.text).empty())
            error("base class " + displayName(c.base) + " has no method '" + callee.text + "'");
        return callMethod({self.v, Ty{c.base}}, c.base, callee.text, e, true);
    }
    if (callee.kind == "member" && callee.args[0]->kind == "name" &&
        enums.count(callee.args[0]->text) && !lookup(callee.args[0]->text)) {
        auto &en = *enums.at(callee.args[0]->text);
        Ty t{en.name};
        if (callee.text == "values") {
            if (e.args.size() != 1)
                error("values expects no arguments");
            Val out{objectCall("fw_new", {frame, i64(static_cast<int64_t>(en.members.size())),
                                          i64(1)}),
                    Ty{en.name + "[]"}};
            for (size_t i = 0; i < en.members.size(); ++i)
                set(out, i64(static_cast<int64_t>(i)), {i64(static_cast<int64_t>(i)), t});
            return out;
        }
        if (callee.text == "from_name") {
            if (e.args.size() != 2)
                error("from_name expects one string");
            auto text = convert(value(*e.args[1]), Ty{"string"});
            std::string names;
            for (auto &m : en.members)
                names += (names.empty() ? "" : ",") + m;
            auto index = call("fw_enum_index", b.getInt64Ty(),
                              {b.CreateGlobalString(names), text.v});
            auto found = llvm::BasicBlock::Create(ctx, "enum.found", current);
            auto missing = llvm::BasicBlock::Create(ctx, "enum.missing", current);
            auto done = llvm::BasicBlock::Create(ctx, "enum.end", current);
            b.CreateCondBr(b.CreateICmpSGE(index, i64(0)), found, missing);
            b.SetInsertPoint(found);
            auto present = convert({index, t}, Ty{en.name + "?"});
            auto foundEnd = b.GetInsertBlock();
            b.CreateBr(done);
            b.SetInsertPoint(missing);
            b.CreateBr(done);
            b.SetInsertPoint(done);
            auto phi = b.CreatePHI(b.getPtrTy(), 2);
            phi->addIncoming(present.v, foundEnd);
            phi->addIncoming(llvm::ConstantPointerNull::get(b.getPtrTy()), missing);
            return {phi, Ty{en.name + "?"}};
        }
        error("enum " + displayName(en.name) + " supports values() and from_name(text)");
    }
    if (callee.kind != "name" && callee.kind != "member") {
        auto function = value(callee);
        if (!function.t.function())
            error("value of type " + function.t.name + " is not callable");
        return callClosure(function, rest);
    }
    std::string name = callable(callee);
    if (name == "testing.expect_error")
        return expectError(e);
    if (callee.kind == "name" && !lookup(name))
        if (auto result = genericCall(e, name, expected))
            return *result;
    if (callee.kind == "name") {
        if (auto variable = lookup(name); variable && variable->type.classReference()) {
            load(*variable);
            name = variable->type.name.substr(6);
        } else if (variable && variable->type.function())
            return callClosure(load(*variable), rest);
    }
    std::vector<Val> args;
    // Receivers are evaluated before arguments. Module references have no runtime evaluation.
    Val receiverValue;
    bool method = false;
    if (callee.kind == "member" && callee.text == "from_json" && callee.args[0]->kind == "name" &&
        (classes.count(callee.args[0]->text) ||
         (callee.args[0]->text.find('<') != std::string::npos &&
          templates.count(callee.args[0]->text.substr(0, callee.args[0]->text.find('<')))))) {
        if (e.args.size() != 2)
            error("from_json expects one JSON string");
        auto target = type(callee.args[0]->text);
        auto text = convert(value(*e.args[1]), Ty{"string"});
        return jsonDecode(target, {objectCall("fw_json_parse", {frame, text.v}), Ty{"json.value"}});
    }
    if (name.empty() && callee.kind == "member" &&
        !(callee.args[0]->kind == "name" && callee.args[0]->text == "Result")) {
        receiverValue = value(*callee.args[0]);
        if (receiverValue.t.nullable() &&
            classes.count(receiverValue.t.name.substr(0, receiverValue.t.name.size() - 1)))
            receiverValue = unwrap(receiverValue);
        name = receiverValue.t.name + "." + callee.text;
        method = true;
    }
    if (method && classes.count(receiverValue.t.name) &&
        !resolveMethod(receiverValue.t.name, callee.text).empty())
        return callMethod(receiverValue, receiverValue.t.name, callee.text, e, false);
    if (method && classes.count(receiverValue.t.name))
        if (auto result = genericMethodCall(receiverValue, callee.text, e, expected))
            return *result;
    if (method && classes.count(receiverValue.t.name) && !functions.count(name)) {
        auto c = classes.at(receiverValue.t.name);
        for (size_t i = 0; i < c->fields.size(); ++i)
            if (c->fields[i].name == callee.text && type(c->fields[i].type).function())
                return callClosure(get(receiverValue, i64(static_cast<int64_t>(i)),
                                       type(c->fields[i].type)),
                                   rest);
    }
    if (method && (receiverValue.t.array() || receiverValue.t.map() || receiverValue.t.set()) &&
        (callee.text == "freeze" || callee.text == "is_frozen")) {
        // xs.freeze() makes a collection immutable in place and returns it.
        if (e.args.size() != 1)
            error(callee.text + " expects no arguments");
        auto flag = call("fw_freeze", b.getInt64Ty(), {receiverValue.v, i64(callee.text == "freeze")});
        if (callee.text == "is_frozen")
            return {b.CreateICmpNE(flag, i64(0)), Ty{"bool"}};
        return receiverValue;
    }
    if (method && receiverValue.t.weak()) {
        if (e.args.size() != 1 || (callee.text != "get" && callee.text != "alive"))
            error("weak references support get() and alive()");
        auto target = objectCall("fw_weak_get", {frame, receiverValue.v});
        if (callee.text == "alive")
            return {b.CreateICmpNE(target, llvm::ConstantPointerNull::get(b.getPtrTy())),
                    Ty{"bool"}};
        return {target, Ty{receiverValue.t.parts()[0].name + "?"}};
    }
    if (method && receiverValue.t.function() && callee.text == "call")
        return callClosure(receiverValue, rest);
    if (method && receiverValue.t.sequence() &&
        (callee.text == "map" || callee.text == "filter" || callee.text == "each" ||
         callee.text == "reduce" || callee.text == "any" || callee.text == "all"))
        return sequenceMethod(receiverValue, callee.text, e);
    if (method && receiverValue.t.classReference())
        error("constructor reference '" + displayName(receiverValue.t.name.substr(6)) +
              "' has no instance methods; construct an instance with () first");
    if (method && (receiverValue.t.name == "element" || receiverValue.t.name == "fragment" ||
                   receiverValue.t.name == "element?" || receiverValue.t.name == "fragment?")) {
        std::vector<Val> values{receiverValue};
        for (size_t i = 1; i < e.args.size(); ++i)
            values.push_back(value(*e.args[i]));
        if (auto result = domCall("html." + callee.text, std::move(values)))
            return *result;
        if (receiverValue.t.nullable())
            receiverValue =
                convert(receiverValue,
                        Ty{receiverValue.t.name.substr(0, receiverValue.t.name.size() - 1)}, true);
    }
    if (method &&
        (receiverValue.t.name == "desktop.window" || receiverValue.t.name == "desktop.window?")) {
        if (receiverValue.t.nullable())
            receiverValue = convert(receiverValue, Ty{"desktop.window"}, true);
        if (callee.text == "load") {
            if (e.args.size() != 2)
                error("desktop.window.load expects one element or fragment");
            auto tree = value(*e.args[1]);
            if (tree.t.name != "element" && tree.t.name != "fragment")
                error("desktop.window.load expects an element or fragment");
            call("fw_desktop_load", b.getVoidTy(), {receiverValue.v, tree.v});
            return {};
        }
        if (callee.text == "run") {
            if (e.args.size() != 1)
                error("desktop.window.run expects no arguments");
            call("fw_desktop_run", b.getVoidTy(),
                 {receiverValue.v, b.CreateGlobalString(desktopBackend)});
            return {};
        }
        if (callee.text == "bind") {
            if (e.args.size() != 3)
                error("desktop.window.bind expects a name and a function(string) returns string");
            auto bindingName = convert(value(*e.args[1]), Ty{"string"});
            Ty expectedType{"function<string,string>"};
            auto handler = asFunction(value(*e.args[2], expectedType));
            if (handler.t != expectedType)
                error("desktop.window.bind expects a function(string) returns string");
            call("fw_desktop_bind", b.getVoidTy(), {receiverValue.v, bindingName.v, handler.v});
            return {};
        }
        if (callee.text == "eval") {
            if (e.args.size() != 2)
                error("desktop.window.eval expects JavaScript text");
            call("fw_desktop_eval", b.getVoidTy(),
                 {receiverValue.v, convert(value(*e.args[1]), Ty{"string"}).v});
            return {};
        }
        error("unknown desktop.window method '" + callee.text + "'");
    }
    if (method && receiverValue.t.result()) {
        if (e.args.size() != 1)
            error("Result inspection expects no arguments");
        auto ok = get(receiverValue, i64(0), Ty{"bool"});
        if (callee.text == "is_ok")
            return ok;
        if (callee.text == "is_error")
            return {b.CreateNot(ok.v), Ty{"bool"}};
        if (callee.text == "unwrap" || callee.text == "error") {
            bool success = callee.text == "unwrap";
            guard(success ? ok.v : b.CreateNot(ok.v),
                  "attempted to unwrap the wrong Result variant", "ResultError");
            return get(receiverValue, i64(1), receiverValue.t.parts()[success ? 0 : 1]);
        }
    }
    if (method && receiverValue.t.name == "string")
        return stringMethod(receiverValue, callee.text, e);
    if (method)
        if (auto result = resourceMethod(receiverValue, callee.text, e))
            return *result;
    if (method && receiverValue.t.sequence())
        if (auto result = sequenceTool(receiverValue, callee.text, e))
            return *result;
    if (method && receiverValue.t.set()) {
        auto self = receiverValue;
        auto item = self.t.parts()[0];
        auto &m = callee.text;
        size_t supplied = e.args.size() - 1;
        auto expect = [&](size_t n) {
            if (supplied != n)
                error("set." + m + " expects " + std::to_string(n) + " argument(s)");
        };
        if (m == "add" || m == "remove" || m == "contains") {
            expect(1);
            auto x = convert(value(*e.args[1], item), item);
            if (m == "remove")
                return keyRemove(self, x);
            if (m == "contains")
                return {b.CreateICmpSGE(keyFind(self, x, 2), i64(0)), Ty{"bool"}};
            keyFind(self, x, 1);
            return {};
        }
        if (m == "clear") {
            expect(0);
            call("fw_collection_clear", b.getVoidTy(), {self.v});
            return {};
        }
        if (m == "items") {
            expect(0);
            return {objectCall("fw_map_keys", {frame, self.v}), Ty{item.name + "[]"}};
        }
        if (m == "union" || m == "intersection" || m == "difference" || m == "is_subset") {
            expect(1);
            auto other = value(*e.args[1], self.t);
            if (other.t != self.t)
                error("set." + m + " expects another " + displayName(self.t.name));
            Val mine{objectCall("fw_map_keys", {frame, self.v}), Ty{item.name + "[]"}};
            Val theirs{objectCall("fw_map_keys", {frame, other.v}), Ty{item.name + "[]"}};
            if (m == "is_subset") {
                auto result = allocate(Ty{"bool"}, "subset");
                b.CreateStore(b.getTrue(), result);
                eachItem(mine, [&](Val v) {
                    auto present = b.CreateICmpSGE(keyFind(other, v, 2), i64(0));
                    b.CreateStore(b.CreateAnd(b.CreateLoad(b.getInt1Ty(), result), present), result);
                });
                return {b.CreateLoad(b.getInt1Ty(), result), Ty{"bool"}};
            }
            auto out = newSet(self.t);
            auto addIf = [&](Val v, Val container, bool wanted) {
                auto present = b.CreateICmpSGE(keyFind(container, v, 2), i64(0));
                auto yes = llvm::BasicBlock::Create(ctx, "set.add", current);
                auto done = llvm::BasicBlock::Create(ctx, "set.next", current);
                b.CreateCondBr(wanted ? present : b.CreateNot(present), yes, done);
                b.SetInsertPoint(yes);
                keyFind(out, v, 1);
                b.CreateBr(done);
                b.SetInsertPoint(done);
            };
            if (m == "union") {
                eachItem(mine, [&](Val v) { keyFind(out, v, 1); });
                eachItem(theirs, [&](Val v) { keyFind(out, v, 1); });
            } else
                eachItem(mine, [&](Val v) { addIf(v, other, m == "intersection"); });
            return out;
        }
        error("unknown set method '" + m + "'");
    }
    if (method && (receiverValue.t.array() || receiverValue.t.map())) {
        auto obj = receiverValue;
        auto methodName = callee.text;
        if (obj.t.array() && methodName == "push") {
            if (e.args.size() != 2)
                error("push expects one value");
            push(obj, convert(value(*e.args[1], obj.t.item()), obj.t.item()));
            return {};
        }
        if (obj.t.array() && methodName == "pop") {
            if (e.args.size() != 1)
                error("pop expects no arguments");
            auto t = obj.t.item();
            if (t.object())
                return {objectCall("fw_array_pop_object", {frame, obj.v}), t};
            if (t.real())
                return {b.CreateFPCast(call("fw_array_pop_real", b.getDoubleTy(), {obj.v}), ll(t)),
                        t};
            return {b.CreateIntCast(call("fw_array_pop_int", b.getInt64Ty(), {obj.v}), ll(t),
                                    !t.unsigned_()),
                    t};
        }
        if (methodName == "clear") {
            if (e.args.size() != 1)
                error("clear expects no arguments");
            call("fw_collection_clear", b.getVoidTy(), {obj.v});
            return {};
        }
        if (obj.t.map() && methodName == "values") {
            if (e.args.size() != 1)
                error("values expects no arguments");
            return {objectCall("fw_map_values", {frame, obj.v}), Ty{obj.t.parts()[1].name + "[]"}};
        }
        if (obj.t.map() && methodName == "keys") {
            if (e.args.size() != 1)
                error("keys expects no arguments");
            return {objectCall("fw_map_keys", {frame, obj.v}), Ty{obj.t.parts()[0].name + "[]"}};
        }
        if (obj.t.map() && methodName == "remove") {
            if (e.args.size() != 2)
                error("remove expects one key");
            auto key = convert(value(*e.args[1], obj.t.parts()[0]), obj.t.parts()[0]);
            return keyRemove(obj, key);
        }
    }
    if (method && callee.text == "render" &&
        (receiverValue.t.name == "element" || receiverValue.t.name == "fragment")) {
        if (e.args.size() != 1)
            error("render expects no arguments");
        return stringify(receiverValue);
    }
    if (callee.kind == "member" && callee.args[0]->kind == "name" &&
        callee.args[0]->text == "Result") {
        if (!expected.result())
            error("Result.ok/error requires an expected Result<T,E> type");
        if (e.args.size() != 2 || (callee.text != "ok" && callee.text != "error"))
            error("use Result.ok(value) or Result.error(value)");
        bool ok = callee.text == "ok";
        auto parts = expected.parts();
        auto payload = convert(value(*e.args[1], parts[ok ? 0 : 1]), parts[ok ? 0 : 1]);
        Val result{objectCall("fw_new", {frame, i64(2), i64(8)}), expected};
        set(result, i64(0), {b.getInt1(ok), Ty{"bool"}});
        set(result, i64(1), payload);
        return result;
    }
    if (errorTypes.count(name)) {
        if (e.args.size() != 2)
            error("error constructor expects a message string");
        auto message = convert(value(*e.args[1]), Ty{"string"});
        return {objectCall("fw_error_new", {frame, message.v, b.CreateGlobalString(name)}),
                Ty{name}};
    }
    if (classes.count(name)) {
        auto c = classes.at(name);
        if (c->isInterface)
            error("interface " + displayName(name) + " cannot be constructed");
        if (!c->variants.empty())
            error("enum " + displayName(name) + " is constructed through a member, such as " +
                  displayName(c->variants[0]) + "(...)");
        if (!resolveMethod(name, "init").empty()) {
            // Fields with defaults are set first; init assigns the rest.
            Val obj = newInstance(*c);
            for (size_t i = 0; i < c->fields.size(); ++i)
                if (c->fields[i].value) {
                    Ty t = type(c->fields[i].type);
                    set(obj, i64(static_cast<int64_t>(i)),
                        convert(defaultValue(*c->fields[i].value, t), t));
                }
            callMethod(obj, name, "init", e, true);
            return obj;
        }
        if (e.args.size() - 1 > c->fields.size())
            error("too many constructor arguments for " + name);
        Val obj = newInstance(*c);
        for (size_t i = 0; i < c->fields.size(); ++i) {
            auto &f = c->fields[i];
            Ty t = type(f.type);
            Val x;
            if (i + 1 < e.args.size())
                x = value(*e.args[i + 1], t);
            else if (f.value) {
                x = defaultValue(*f.value, t);
            } else
                error("missing required constructor field '" + f.name + "'");
            set(obj, i64(i), convert(x, t));
        }
        return obj;
    }
    if (name == "typeof") {
        if (e.args.size() != 2)
            error("typeof expects one argument");
        return literal(displayName(value(*e.args[1]).t.name));
    }
    if (name == "len") {
        if (e.args.size() != 2)
            error("len expects one argument");
        auto a = value(*e.args[1]);
        if (a.t.name != "string" && !a.t.sequence() && !a.t.map() && !a.t.set() && !a.t.tuple())
            error("len expects a string or collection");
        return {call("fw_length", b.getInt64Ty(), {a.v}), Ty{"i64"}};
    }
    if (name == "weak") {
        if (e.args.size() != 2)
            error("weak expects one reference");
        auto target = value(*e.args[1]);
        if (target.t.nullable() && target.t.name != "null")
            target.t = Ty{target.t.name.substr(0, target.t.name.size() - 1)};
        auto t = type("weak<" + target.t.name + ">");
        return {objectCall("fw_weak_new", {frame, target.v}), t};
    }
    std::string constructor = name.substr(0, name.find('<'));
    if (constructor == "set") {
        if (e.args.size() > 2)
            error("set() expects at most one sequence");
        Ty target = name.find('<') != std::string::npos ? type(name)
                    : expected.set()                    ? expected
                                                        : Ty{};
        Val source;
        if (e.args.size() == 2) {
            source = value(*e.args[1]);
            if (source.t.set() || source.t.map())
                source = {objectCall("fw_map_keys", {frame, source.v}),
                          Ty{source.t.parts()[0].name + "[]"}};
            if (source.t.name == "string")
                source = {objectCall("fw_string_chars", {frame, source.v}), Ty{"char[]"}};
            if (!source.t.sequence())
                error("set() expects an array, tuple, string, map, or set");
            if (target.name == "void")
                target = type("set<" + source.t.item().name + ">");
        }
        if (target.name == "void")
            error("empty set() needs an expected type or set<T>()");
        auto out = newSet(target);
        auto item = target.parts()[0];
        if (source.v)
            eachItem(source, [&](Val v) { keyFind(out, convert(v, item), 1); });
        return out;
    }
    if (constructor == "array" || constructor == "tuple" || constructor == "map") {
        if (e.args.size() > 2)
            error("collection casts expect at most one value");
        bool typed = name.find('<') != std::string::npos;
        Ty target =
            typed ? type(name)
            : ((constructor == "array" && expected.array()) ||
               (constructor == "tuple" && (expected.tuple() || expected.homogeneousTuple())) ||
               (constructor == "map" && expected.map()))
                ? expected
                : Ty{};
        if (e.args.size() == 1) {
            if (target.name == "void" && constructor == "tuple")
                target = Ty{"[]"};
            if (target.name == "void")
                error("empty collection casts need an expected type or explicit type arguments");
            if (target.map()) {
                auto key = target.parts()[0];
                return {objectCall("fw_map_new", {frame, i64(mapKeyKind(key))}),
                        target};
            }
            return {objectCall("fw_new", {frame, i64(0), i64(target.array() ? 1 : 7)}), target};
        }
        auto source = value(*e.args[1]);
        if (target.name == "void")
            target = inferCollectionCast(constructor, source.t);
        return collectionCast(source, target);
    }
    static const std::set<std::string> casts = {"int",    "i8",   "i16",    "i32",     "i64",
                                                "u8",     "u16",  "u32",    "u64",     "float",
                                                "double", "bool", "string", "element", "fragment",
                                                "char"};
    if (casts.count(name)) {
        if (e.args.size() != 2)
            error("cast expects exactly one argument");
        return explicitCast(value(*e.args[1]), type(name));
    }
    if (functions.count(name) || (name.empty() && callee.kind == "name") ||
        (!name.empty() && lookup(name))) {
        if (auto v = lookup(name); v && v->type.name.rfind("fn:", 0) == 0)
            name = v->type.name.substr(3);
        else if (v)
            error("value of type " + v->type.name + " is not callable");
        auto &sig = signature(name);
        if (!sig.ast->owner.empty() && !method)
            error("method requires a receiver");
        size_t supplied = e.args.size() - 1;
        if (supplied > sig.ast->params.size())
            error("too many arguments to " + name);
        std::vector<llvm::Value *> actual;
        if (method)
            actual.push_back(receiverValue.v);
        for (size_t i = 0; i < sig.ast->params.size(); ++i) {
            auto &param = sig.ast->params[i];
            Ty t = type(param.type);
            Val v;
            if (i < supplied)
                v = value(*e.args[i + 1], t);
            else if (param.value) {
                v = defaultValue(*param.value, t);
            } else
                error("missing argument '" + param.name + "'");
            actual.push_back(convert(v, t).v);
        }
        auto result = b.CreateCall(sig.function, actual);
        checkError();
        if (sig.result.object())
            objectCall("fw_adopt", {frame, result});
        return {result, sig.result};
    }
    for (size_t i = 1; i < e.args.size(); ++i)
        args.push_back(value(*e.args[i], name == "os.command" ? Ty{"string[]"} : Ty{}));
    auto count = [&](size_t n) {
        if (args.size() != n)
            error(name + " expects " + std::to_string(n) + " argument(s)");
    };
    auto str = [&](size_t i) { return convert(args[i], Ty{"string"}).v; };
    auto real = [&](size_t i) { return convert(args[i], Ty{"double"}).v; };
    if (name == "desktop.run") {
        if (args.size() != 1 || args[0].t.name != "desktop.window[]")
            error("desktop.run expects an array of windows");
        call("fw_desktop_run_all", b.getVoidTy(), {args[0].v, b.CreateGlobalString(desktopBackend)});
        return {};
    }
    if (name == "desktop.quit") {
        count(0);
        call("fw_desktop_quit", b.getVoidTy(), {});
        return {};
    }
    if (name == "desktop.window" || name == "desktop.Window") {
        if (args.size() > 3)
            error("desktop.window expects optional title, width, and height");
        auto title = args.empty() ? literal("Flywheel").v : str(0);
        auto width = args.size() < 2 ? i64(960) : convert(args[1], Ty{"i64"}).v;
        auto height = args.size() < 3 ? i64(640) : convert(args[2], Ty{"i64"}).v;
        return {objectCall("fw_desktop_window", {frame, title, width, height}),
                Ty{"desktop.window"}};
    }
    if (name == "input" || name == "core.input") {
        if (args.size() > 1)
            error(name + " expects zero or one string argument");
        auto prompt = args.empty() ? literal("").v : str(0);
        return {objectCall("fw_input", {frame, prompt}), Ty{"string"}};
    }
    if (name == "os.shell" || name == "os.command") {
        count(1);
        if (name == "os.shell")
            return {call("fw_shell", b.getInt64Ty(), {str(0)}), Ty{"i64"}};
        if (!args[0].t.array() || args[0].t.item().name != "string")
            error("os.command expects an array of strings");
        return {call("fw_command", b.getInt64Ty(), {args[0].v}), Ty{"i64"}};
    }
    if (name == "print" || name == "core.print") {
        if (args.empty())
            error("print expects at least one value");
        // Several values print on one line separated by spaces.
        auto x = stringify(args[0]);
        for (size_t i = 1; i < args.size(); ++i) {
            auto space = literal(" ");
            x = {objectCall("fw_concat", {frame, x.v, space.v}), Ty{"string"}};
            x = {objectCall("fw_concat", {frame, x.v, stringify(args[i]).v}), Ty{"string"}};
        }
        call("fw_print", b.getVoidTy(), {x.v});
        return {};
    }
    if (name == "render" || name == "html.render") {
        count(1);
        if (args[0].t.name != "element" && args[0].t.name != "fragment")
            error("render expects element or fragment");
        return {objectCall("fw_render", {frame, args[0].v}), Ty{"string"}};
    }
    if (auto result = domCall(name, args))
        return *result;
    // Listeners bind 127.0.0.1 unless a host is given, such as "0.0.0.0" or "::".
    auto host = [&](size_t i) { return args.size() > i ? str(i) : literal("127.0.0.1").v; };
    if (name == "web.serve" || name == "web.route") {
        size_t offset = name == "web.route" ? 1 : 0;
        if (args.size() != 3 + offset && args.size() != 4 + offset)
            error(name + " expects " + std::to_string(3 + offset) +
                  " arguments and an optional host");
        auto method = offset ? str(0) : literal("GET").v;
        auto handler = args[offset + 1];
        if (handler.t.name.rfind("fn:", 0) == 0) {
            auto &sig = signature(handler.t.name.substr(3));
            std::string t = "function<" + sig.result.name;
            for (auto &param : sig.ast->params)
                t += "," + type(param.type).name;
            if (sig.ast->owner.empty() && sig.ast->params.size() <= 1)
                handler = convert(handler, Ty{t + ">"});
        }
        auto parts = handler.t.function() ? handler.t.parts() : std::vector<Ty>{};
        static const std::set<std::string> results = {"element", "fragment", "string",
                                                      "web.response"};
        if (parts.empty() || !results.count(parts[0].name) || parts.size() > 2 ||
            (parts.size() == 2 && parts[1].name != "web.request"))
            error("HTTP handler must take no arguments or one web.request and return element, "
                  "fragment, string, or web.response");
        return {objectCall("fw_serve",
                           {frame, method, str(offset), handler.v,
                            i64(static_cast<int64_t>(parts.size() - 1)),
                            convert(args[offset + 2], Ty{"i64"}).v, host(offset + 3)}),
                Ty{"string"}};
    }
    if (name == "web.static") {
        if (args.size() != 3 && args.size() != 4)
            error("web.static expects route, directory, port, and an optional host");
        return {objectCall("fw_serve_static",
                           {frame, str(0), str(1), convert(args[2], Ty{"i64"}).v, host(3)}),
                Ty{"string"}};
    }
    if (name == "web.tls") {
        if (args.size() != 3 && args.size() != 4)
            error("web.tls expects certificate file, key file, port, and an optional host");
        return {objectCall("fw_web_tls",
                           {frame, str(0), str(1), convert(args[2], Ty{"i64"}).v, host(3)}),
                Ty{"string"}};
    }
    if (name == "web.text" || name == "web.html" || name == "web.json" || name == "web.redirect") {
        if (args.size() != 1 && args.size() != 2)
            error(name + " expects a body and an optional status");
        Val body = args[0];
        std::string type = name == "web.html" ? "text/html; charset=utf-8"
                           : name == "web.json" ? "application/json"
                                                : "text/plain; charset=utf-8";
        if (name == "web.html" && (body.t.name == "element" || body.t.name == "fragment"))
            body = stringify(body);
        else if (name == "web.json" && body.t.name == "json.value")
            body = {objectCall("fw_json_stringify", {frame, body.v}), Ty{"string"}};
        body = convert(body, Ty{"string"});
        auto status = args.size() == 2 ? convert(args[1], Ty{"i64"})
                                       : Val{i64(name == "web.redirect" ? 302 : 200), Ty{"i64"}};
        Val headers{objectCall("fw_map_new", {frame, i64(4)}), Ty{"map<string,string>"}};
        auto header = [&](const std::string &key, Val value) {
            set(headers, mapIndex(headers, literal(key), true), value);
        };
        Val response = newInstance(*classes.at("web.response"));
        if (name == "web.redirect") {
            header("location", body);
            body = literal("");
        }
        header("content-type", literal(type));
        set(response, i64(0), status);
        set(response, i64(1), body);
        set(response, i64(2), headers);
        return response;
    }
    if (name == "web.get" || name == "web.post" || name == "web.put" || name == "web.patch" ||
        name == "web.delete" || name == "web.head" || name == "web.fetch") {
        // HTTP client calls: web.get(url, headers = {}), web.post(url, body, headers = {}),
        // web.fetch(method, url, body = "", headers = {}, timeout = 30.0).
        bool fetch = name == "web.fetch";
        bool withBody = name == "web.post" || name == "web.put" || name == "web.patch";
        size_t low = fetch ? 2 : withBody ? 2 : 1, high = fetch ? 5 : withBody ? 3 : 2;
        if (args.size() < low || args.size() > high)
            error(name + (fetch ? " expects method, url, and optional body, headers, timeout"
                          : withBody ? " expects url, body, and optional headers"
                                     : " expects url and optional headers"));
        size_t at = 0;
        auto method = fetch ? convert(args[at++], Ty{"string"})
                            : literal(name == "web.delete" ? "DELETE" : [&] {
                                  std::string m = name.substr(4);
                                  for (auto &c : m)
                                      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                                  return m;
                              }());
        auto url = convert(args[at++], Ty{"string"});
        Val body = literal("");
        bool jsonBody = false;
        if ((fetch || withBody) && at < args.size()) {
            body = args[at++];
            if (body.t.name == "json.value") {
                body = {objectCall("fw_json_stringify", {frame, body.v}), Ty{"string"}};
                jsonBody = true;
            }
            body = convert(body, Ty{"string"});
        }
        Val headers;
        if (at < args.size()) {
            if (args[at].t.name != "map<string,string>")
                error(name + " headers must be map<string, string>");
            headers = args[at++];
        } else
            headers = {objectCall("fw_map_new", {frame, i64(4)}), Ty{"map<string,string>"}};
        if (jsonBody)
            set(headers, mapIndex(headers, literal("Content-Type"), true),
                literal("application/json"));
        auto timeout = at < args.size() ? convert(args[at], Ty{"double"}, true).v
                                         : llvm::ConstantFP::get(b.getDoubleTy(), 30.0);
        Val response = newInstance(*classes.at("web.response"));
        call("fw_http_fetch", b.getVoidTy(),
             {frame, method.v, url.v, body.v, headers.v, timeout, response.v});
        return response;
    }
    if (name == "web.workers") {
        count(1);
        call("fw_web_workers", b.getVoidTy(), {convert(args[0], Ty{"i64"}).v});
        return {};
    }
    if (name == "web.wait") {
        count(0);
        call("fw_web_wait", b.getVoidTy(), {});
        return {};
    }
    if (name == "json.parse") {
        count(1);
        return {objectCall("fw_json_parse", {frame, str(0)}), Ty{"json.value"}};
    }
    if (name == "json.stringify" || name == "json.kind" || name == "json.get" ||
        name == "json.has" || name == "json.as_string" || name == "json.as_int" ||
        name == "json.as_double" || name == "json.as_bool") {
        bool key = name == "json.get" || name == "json.has";
        count(key ? 2 : 1);
        if (args[0].t.name != "json.value")
            error("expected json.parse result");
        if (name == "json.stringify" || name == "json.kind")
            return {objectCall(name == "json.kind" ? "fw_json_kind" : "fw_json_stringify",
                               {frame, args[0].v}),
                    Ty{"string"}};
        if (name == "json.get")
            return {objectCall("fw_json_get", {frame, args[0].v, str(1)}), Ty{"json.value"}};
        if (name == "json.has")
            return {
                b.CreateICmpNE(call("fw_json_has", b.getInt64Ty(), {args[0].v, str(1)}), i64(0)),
                Ty{"bool"}};
        if (name == "json.as_string")
            return {objectCall("fw_json_string", {frame, args[0].v}), Ty{"string"}};
        if (name == "json.as_double")
            return {call("fw_json_real", b.getDoubleTy(), {args[0].v}), Ty{"double"}};
        auto n = call(name == "json.as_bool" ? "fw_json_bool" : "fw_json_int", b.getInt64Ty(),
                      {args[0].v});
        return name == "json.as_bool" ? Val{b.CreateICmpNE(n, i64(0)), Ty{"bool"}}
                                      : Val{n, Ty{"i64"}};
    }
    if (name == "fs.exists") {
        count(1);
        return {b.CreateICmpNE(call("fw_exists", b.getInt64Ty(), {str(0)}), i64(0)), Ty{"bool"}};
    }
    if (name == "fs.try_read") {
        count(1);
        return {objectCall("fw_try_read", {frame, str(0)}), Ty{"Result<string,FileError>"}};
    }
    if (name == "fs.read") {
        count(1);
        return {objectCall("fw_read", {frame, str(0)}), Ty{"string"}};
    }
    if (name == "fs.write") {
        count(2);
        call("fw_write", b.getVoidTy(), {str(0), str(1)});
        return {};
    }
    if (name == "fs.remove") {
        count(1);
        call("fw_remove", b.getVoidTy(), {str(0)});
        return {};
    }
    if (name == "os.env") {
        count(1);
        return {objectCall("fw_env", {frame, str(0)}), Ty{"string"}};
    }
    if (name == "time.monotonic") {
        count(0);
        return {call("fw_clock", b.getDoubleTy(), {}), Ty{"double"}};
    }
    if (name == "time.sleep") {
        count(1);
        call("fw_sleep", b.getVoidTy(), {real(0)});
        return {};
    }
    if (auto result = mathCall(name, args))
        return *result;
    if (auto result = stdlibCall(name, args, expected))
        return *result;
    if (name == "math.sqrt" || name == "math.sin" || name == "math.cos") {
        count(1);
        return {call("fw_" + name.substr(5), b.getDoubleTy(), {real(0)}), Ty{"double"}};
    }
    if (name == "math.pow") {
        count(2);
        return {call("fw_pow", b.getDoubleTy(), {real(0), real(1)}), Ty{"double"}};
    }
    if (name == "collections.range") {
        if (args.size() != 1 && args.size() != 2)
            error("range expects end or start,end");
        auto start = args.size() == 2 ? convert(args[0], Ty{"i64"}).v : i64(0);
        auto end = convert(args.back(), Ty{"i64"}).v;
        return {objectCall("fw_range", {frame, start, end}), Ty{"i64[]"}};
    }
    if (name == "testing.live_objects") {
        count(0);
        return {call("fw_live_objects", b.getInt64Ty(), {}), Ty{"i64"}};
    }
    if (name == "testing.assert") {
        if (args.size() != 1 && args.size() != 2)
            error("testing.assert expects bool and optional string");
        if (args[0].t.name != "bool")
            error("assert requires bool");
        auto message = args.size() == 2 ? convert(args[1], Ty{"string"}) : literal("");
        call("fw_assert", b.getVoidTy(), {b.CreateZExt(args[0].v, b.getInt64Ty()), message.v});
        return {};
    }
    if (name == "testing.assert_equal") {
        if (args.size() != 2 && args.size() != 3)
            error("testing.assert_equal expects actual, expected, and an optional message");
        auto equal = binary("==", args[0], args[1]);
        auto text = literal("expected ");
        auto message = stringify(args[1]);
        Val out{objectCall("fw_concat", {frame, text.v, message.v}), Ty{"string"}};
        auto got = literal(", got ");
        out = {objectCall("fw_concat", {frame, out.v, got.v}), Ty{"string"}};
        out = {objectCall("fw_concat", {frame, out.v, stringify(args[0]).v}), Ty{"string"}};
        if (args.size() == 3) {
            auto separator = literal(": ");
            auto prefix = convert(args[2], Ty{"string"});
            auto head = Val{objectCall("fw_concat", {frame, prefix.v, separator.v}), Ty{"string"}};
            out = {objectCall("fw_concat", {frame, head.v, out.v}), Ty{"string"}};
        }
        call("fw_assert", b.getVoidTy(), {b.CreateZExt(equal.v, b.getInt64Ty()), out.v});
        return {};
    }
    if (name == "testing.fail") {
        count(1);
        call("fw_assert", b.getVoidTy(), {i64(0), str(0)});
        return {};
    }
    error("unknown or unimplemented callable '" + name + "'");
}
void Compiler::statement(Stmt &s) {
    location = s.loc;
    if (s.kind != "declare" || s.expr)
        countLine(s.loc);
    debugLocation();
    if (s.kind == "throw") {
        auto x = value(*s.expr);
        if (x.t.classReference() || x.t.name == "void" || x.t.nullable() ||
            x.t.name.rfind("fn:", 0) == 0 || x.t.name.rfind("module:", 0) == 0)
            error("throw requires a non-null value");
        auto payload = x.v;
        if (!x.t.object()) {
            Val box{objectCall("fw_new", {frame, i64(1), i64(2)}), Ty{"Error"}};
            set(box, i64(0), x);
            payload = box.v;
        }
        call("fw_throw", b.getVoidTy(), {b.CreateGlobalString(x.t.name), payload});
        auto target = errorHandler();
        releaseProtected(target.depth);
        if (target.target != failure)
            releaseScopes(target.depth);
        call("fw_sweep", b.getVoidTy(), {frame});
        b.CreateBr(target.target);
        return;
    }
    if (s.kind == "try") {
        auto outer = errorHandler();
        size_t depth = scopes.size();
        auto dispatch = llvm::BasicBlock::Create(ctx, "catch.dispatch", current);
        auto failed = llvm::BasicBlock::Create(ctx, "finally.error", current);
        auto end = llvm::BasicBlock::Create(ctx, "try.end", current);
        Finalizer active{&s.finalizer, outer, loops.size(), depth};
        handlers.push_back({dispatch, depth});
        finalizers.push_back(active);
        block(s.body);
        finalizers.pop_back();
        handlers.pop_back();
        bool reachesEnd = false;
        if (!terminated()) {
            handlers.push_back(outer);
            block(s.finalizer);
            handlers.pop_back();
            if (!terminated()) {
                b.CreateBr(end);
                reachesEnd = true;
            }
        }
        b.SetInsertPoint(dispatch);
        for (size_t i = 0; i < s.catches.size(); ++i) {
            auto t = type(s.catchTypes[i]);
            if (t.name == "void")
                error("catch cannot use void");
            auto match = llvm::BasicBlock::Create(ctx, "catch.body", current);
            auto next = llvm::BasicBlock::Create(ctx, "catch.next", current);
            auto test = call("fw_error_matches", b.getInt64Ty(), {b.CreateGlobalString(t.name)});
            b.CreateCondBr(b.CreateICmpNE(test, i64(0)), match, next);
            b.SetInsertPoint(match);
            scopes.emplace_back();
            auto caught =
                objectCall(t.name == "Error" ? "fw_error_take" : "fw_error_value", {frame});
            Val caughtValue{caught, t};
            if (!t.object())
                caughtValue = get({caught, Ty{"Error"}}, i64(0), t);
            bind(s.catchNames[i], caughtValue);
            handlers.push_back({failed, depth});
            finalizers.push_back(active);
            block(s.catches[i], false);
            finalizers.pop_back();
            handlers.pop_back();
            if (!terminated())
                releaseScopes(depth);
            scopes.pop_back();
            if (!terminated()) {
                handlers.push_back(outer);
                block(s.finalizer);
                handlers.pop_back();
                if (!terminated()) {
                    b.CreateBr(end);
                    reachesEnd = true;
                }
            }
            b.SetInsertPoint(next);
        }
        b.CreateBr(failed);
        b.SetInsertPoint(failed);
        auto exception = objectCall("fw_error_take", {frame});
        auto anchor = allocate(Ty{"Error"}, "pending.exception");
        store(anchor, {exception, Ty{"Error"}});
        protectedValues.push_back({anchor, depth});
        handlers.push_back(outer);
        block(s.finalizer);
        handlers.pop_back();
        protectedValues.pop_back();
        if (!terminated()) {
            call("fw_error_restore", b.getVoidTy(), {exception});
            store(anchor, {llvm::ConstantPointerNull::get(b.getPtrTy()), Ty{"Error"}});
            if (outer.target != failure)
                releaseScopes(outer.depth);
            call("fw_sweep", b.getVoidTy(), {frame});
            b.CreateBr(outer.target);
        }
        b.SetInsertPoint(end);
        if (!reachesEnd)
            b.CreateUnreachable();
        return;
    }
    if (s.kind == "declare") {
        Ty t = s.type.empty() ? Ty{} : type(s.type);
        auto x = value(*s.expr, t);
        if (!s.type.empty())
            x = convert(x, t);
        if (x.t.name == "void" || x.t.name == "null" || x.t.name.rfind("module:", 0) == 0)
            error("declaration needs a concrete non-void type");
        if (top && scopes.size() == 1) {
            if (globals.count(s.name) || functions.count(s.name) || classes.count(s.name) ||
                p.imports.count(s.name))
                error("duplicate declaration '" + s.name + "'");
            auto g =
                new llvm::GlobalVariable(module, ll(x.t), false, llvm::GlobalValue::InternalLinkage,
                                         llvm::Constant::getNullValue(ll(x.t)), "global." + s.name);
            auto initialized = new llvm::GlobalVariable(module, b.getInt1Ty(), false,
                                                        llvm::GlobalValue::InternalLinkage,
                                                        b.getFalse(), "initialized." + s.name);
            globals[s.name] = {g, x.t, s.op == "const", initialized};
            if (x.t.object())
                call("fw_root", b.getVoidTy(), {frame, g});
            store(g, x);
            b.CreateStore(b.getTrue(), initialized);
        } else
            bind(s.name, x, s.op == "const");
        return;
    }
    if (s.kind == "destructure") {
        // After `var (a, b) = items`: the element count must match the names.
        auto items = lookup(s.name);
        auto t = items->type;
        auto count = std::stoll(s.op);
        if (t.tuple()) {
            if (static_cast<int64_t>(t.parts().size()) != count)
                error("cannot destructure a tuple of " + std::to_string(t.parts().size()) +
                      " elements into " + s.op + " names");
        } else if (t.sequence())
            call("fw_check_length", b.getVoidTy(), {load(*items).v, i64(count)});
        else
            error("destructuring needs a tuple or array, not " + displayName(t.name));
        return;
    }
    if (s.kind == "assign") {
        Var *variable = nullptr;
        Val object, mapKey;
        std::string domProperty;
        llvm::Value *index = nullptr;
        Ty target;
        if (s.target->kind == "name") {
            variable = lookup(s.target->text);
            if (!variable)
                error("assignment to undeclared identifier '" + s.target->text +
                      "'; use var or an explicit type");
            if (variable->captured)
                error("cannot assign to captured variable '" + s.target->text +
                      "'; lambdas capture copies, so share state through an object or collection");
            if (variable->constant)
                error("cannot assign to const '" + s.target->text + "'");
            if (variable->typeNarrowed)
                error("cannot assign to '" + s.target->text +
                      "' while it is narrowed by 'is'; use a new variable");
            target = variable->narrowed ? variable->storage : variable->type;
        } else if (s.target->kind == "member" || s.target->kind == "inst") {
            object = unwrap(s.target->kind == "inst" ? receiver() : value(*s.target->args[0]));
            if ((object.t.name == "element" || object.t.name == "fragment") &&
                (s.target->text == "text_content" || s.target->text == "id" ||
                 s.target->text == "class_name")) {
                domProperty = s.target->text;
                target = Ty{"string"};
            } else {
                auto [c, i] = field(object.t, s.target->text);
                // Readonly fields are set by constructors and by init of their own class.
                if (c->fields[i].readonly &&
                    (method != "init" ||
                     owner != fieldOwners.at(object.t.name + "." + s.target->text)))
                    error("field '" + s.target->text + "' is readonly; it can be set only in " +
                          "the constructor or init of " +
                          displayName(fieldOwners.at(object.t.name + "." + s.target->text)));
                index = i64(i);
                target = type(c->fields[i].type);
            }
        } else if (s.target->kind == "index") {
            object = value(*s.target->args[0]);
            if (object.t.name == "string")
                error("strings are immutable; build a new string instead");
            if (classes.count(object.t.name)) {
                // obj[key] = value calls `operator []=`; compound forms read with `operator []`.
                if (resolveMethod(object.t.name, "operator[]=").empty())
                    error("class " + displayName(object.t.name) +
                          " does not support index assignment; declare `function operator []=(K "
                          "key, V value)`");
                auto key = value(*s.target->args[1]);
                auto &setter = signature(resolveMethod(object.t.name, "operator[]="));
                auto valueType = type(setter.ast->params[1].type);
                Val rhs;
                if (s.op != "=") {
                    auto old = callNamedMethod(object, "operator[]", {key});
                    rhs = binary(s.op.substr(0, s.op.size() - 1), old, value(*s.expr, valueType));
                } else
                    rhs = value(*s.expr, valueType);
                callNamedMethod(object, "operator[]=", {key, rhs});
                return;
            }
            if (object.t.map()) {
                mapKey =
                    convert(value(*s.target->args[1], object.t.parts()[0]), object.t.parts()[0]);
                target = object.t.parts()[1];
                if (s.op != "=")
                    index = mapIndex(object, mapKey, false);
            } else {
                auto indexed = indexOf(object, *s.target->args[1], false);
                index = indexed.first;
                target = indexed.second;
            }
        } else
            error("invalid assignment target");
        Val old;
        if (s.op != "=")
            old = variable               ? load(*variable)
                  : !domProperty.empty() ? *domCall("html." + domProperty, {object})
                                         : get(object, index, target);
        auto rhs = value(*s.expr, target);
        if (s.op != "=")
            rhs = binary(s.op.substr(0, s.op.size() - 1), old, rhs);
        if (variable && variable->narrowed && s.op == "=" && rhs.t == variable->type)
            rhs = convert(rhs, variable->storage);
        rhs = convert(rhs, target);
        if (object.t.map())
            index = mapIndex(object, mapKey, true);
        if (variable && variable->cell)
            set({b.CreateLoad(b.getPtrTy(), variable->address), Ty{"Error"}}, i64(0), rhs);
        else if (variable) {
            raceGlobal(*variable, true);
            store(variable->address, rhs);
        }
        else if (!domProperty.empty()) {
            if (domProperty == "text_content")
                call("fw_dom_set_text", b.getVoidTy(), {object.v, rhs.v});
            else {
                auto key = literal(domProperty == "id" ? "id" : "class");
                call("fw_dom_set_attribute", b.getVoidTy(), {object.v, key.v, rhs.v});
            }
        } else
            set(object, index, rhs);
        return;
    }
    if (s.kind == "expr") {
        auto name = callable(*s.target);
        auto variable = lookup(name);
        if ((s.target->kind == "name" || s.target->kind == "member") &&
            (classes.count(name) || name == "desktop.window" || name == "desktop.Window" ||
             (variable && variable->type.classReference())))
            error("'" + displayName(name) +
                  "' is a class reference; call it with () to construct an instance");
        if (s.target->kind == "name" || s.target->kind == "member") {
            Expr call;
            call.kind = "call";
            call.loc = s.loc;
            call.args.push_back(std::move(s.target));
            try {
                invoke(call);
            } catch (...) {
                s.target = std::move(call.args[0]);
                throw;
            }
            s.target = std::move(call.args[0]);
        } else
            value(*s.target);
        return;
    }
    if (s.kind == "return") {
        if (top)
            error("return is only valid inside a function");
        Val x = s.expr ? value(*s.expr, returnType) : Val{};
        if (!finalizers.empty()) {
            if (x.t.object()) {
                auto anchor = allocate(x.t, "return.root");
                store(anchor, x);
                protectedValues.push_back({anchor, finalizers.front().depth});
            }
            runFinalizers();
            if (x.t.object())
                protectedValues.pop_back();
            if (terminated())
                return;
        }
        if (infer) {
            returns.push_back(x.t);
            call("fw_drop", b.getVoidTy(), {frame});
            b.CreateRetVoid();
            return;
        }
        if (returnType.name == "void") {
            if (x.t.name != "void")
                error("void function cannot return a value");
            call("fw_drop", b.getVoidTy(), {frame});
            b.CreateRetVoid();
        } else {
            if (!s.expr)
                error("non-void function must return a value");
            x = convert(x, returnType);
            if (x.t.object())
                call("fw_retain", b.getVoidTy(), {x.v});
            call("fw_drop", b.getVoidTy(), {frame});
            b.CreateRet(x.v);
        }
        return;
    }
    if (s.kind == "if") {
        auto c = value(*s.expr);
        if (c.t.name != "bool")
            error("if condition must be bool (no implicit truthiness)");
        auto yes = llvm::BasicBlock::Create(ctx, "if.then", current);
        auto no = llvm::BasicBlock::Create(ctx, "if.else", current);
        auto end = llvm::BasicBlock::Create(ctx, "if.end", current);
        b.CreateCondBr(c.v, yes, no);
        b.SetInsertPoint(yes);
        std::vector<std::string> whenTrue, whenFalse;
        nonNullNames(*s.expr, true, whenTrue);
        nonNullNames(*s.expr, false, whenFalse);
        std::vector<std::pair<std::string, std::string>> instances;
        instanceNames(*s.expr, true, instances);
        auto branch = [&](std::vector<SP> &body, const std::vector<std::string> &proven) {
            scopes.emplace_back();
            narrow(proven);
            if (&body == &s.body)
                narrowTypes(instances);
            block(body);
            scopes.pop_back();
        };
        branch(s.body, whenTrue);
        bool yesEnds = terminated();
        if (!yesEnds)
            b.CreateBr(end);
        b.SetInsertPoint(no);
        branch(s.other, whenFalse);
        bool noEnds = terminated();
        if (!noEnds)
            b.CreateBr(end);
        b.SetInsertPoint(end);
        if (yesEnds && noEnds)
            b.CreateUnreachable();
        // `if x == null then return end` proves x non-null for the rest of the block.
        else if (yesEnds && s.other.empty())
            narrow(whenFalse);
        return;
    }
    if (s.kind == "match")
        return match(s);
    if (s.kind == "while") {
        auto cond = llvm::BasicBlock::Create(ctx, "while.condition", current);
        auto body = llvm::BasicBlock::Create(ctx, "while.body", current);
        auto end = llvm::BasicBlock::Create(ctx, "while.end", current);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        auto before = tempGeneration;
        auto c = value(*s.expr);
        if (c.t.name != "bool")
            error("while condition must be bool");
        if (before != tempGeneration)
            call("fw_sweep", b.getVoidTy(), {frame});
        b.CreateCondBr(c.v, body, end);
        b.SetInsertPoint(body);
        loops.emplace_back(end, cond);
        loopScopes.push_back(scopes.size());
        // `while x != null do` reads x as non-null in the body (re-checked on each read).
        std::vector<std::string> proven;
        nonNullNames(*s.expr, true, proven);
        scopes.emplace_back();
        narrow(proven);
        block(s.body);
        scopes.pop_back();
        loops.pop_back();
        loopScopes.pop_back();
        if (!terminated())
            b.CreateBr(cond);
        b.SetInsertPoint(end);
        return;
    }
    if (s.kind == "for") {
        auto array = value(*s.expr);
        if (classes.count(array.t.name))
            return iterate(s, array);
        bool pairs = !s.op.empty();
        Val values;
        if (array.t.map()) {
            if (pairs)
                values = {objectCall("fw_map_values", {frame, array.v}),
                          Ty{array.t.parts()[1].name + "[]"}};
            array = {objectCall("fw_map_keys", {frame, array.v}),
                     Ty{array.t.parts()[0].name + "[]"}};
        } else if (array.t.set()) {
            if (pairs)
                error("sets have no index; iterate with one name");
            array = {objectCall("fw_map_keys", {frame, array.v}),
                     Ty{array.t.parts()[0].name + "[]"}};
        }
        if (array.t.name == "string")
            array = {objectCall("fw_string_chars", {frame, array.v}), Ty{"char[]"}};
        if (!array.t.sequence())
            error("for requires an array, tuple, string, map, set, or a class with iterator() "
                  "or next()");
        scopes.emplace_back();
        bind("$iterable", array, true);
        if (values.v)
            bind("$values", values, true);
        auto size = call("fw_length", b.getInt64Ty(), {array.v});
        auto index = allocate(Ty{"i64"}, "iterator");
        b.CreateStore(i64(0), index);
        auto cond = llvm::BasicBlock::Create(ctx, "for.condition", current);
        auto body = llvm::BasicBlock::Create(ctx, "for.body", current);
        auto next = llvm::BasicBlock::Create(ctx, "for.next", current);
        auto end = llvm::BasicBlock::Create(ctx, "for.end", current);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        auto i = b.CreateLoad(b.getInt64Ty(), index);
        b.CreateCondBr(b.CreateICmpSLT(i, size), body, end);
        b.SetInsertPoint(body);
        auto loopDepth = scopes.size();
        scopes.emplace_back();
        if (!pairs)
            bind(s.name, get(array, i, array.t.item()));
        else if (values.v) {
            bind(s.name, get(array, i, array.t.item()));
            bind(s.op, get(values, i, values.t.item()));
        } else {
            bind(s.name, {i, Ty{"i64"}});
            bind(s.op, get(array, i, array.t.item()));
        }
        loops.emplace_back(end, next);
        loopScopes.push_back(loopDepth);
        block(s.body, false);
        loops.pop_back();
        loopScopes.pop_back();
        if (!terminated())
            releaseScopes(loopDepth);
        scopes.pop_back();
        if (!terminated())
            b.CreateBr(next);
        b.SetInsertPoint(next);
        if (array.t.item().object())
            call("fw_sweep", b.getVoidTy(), {frame});
        auto old = b.CreateLoad(b.getInt64Ty(), index);
        b.CreateStore(b.CreateAdd(old, i64(1)), index);
        b.CreateBr(cond);
        b.SetInsertPoint(end);
        releaseScopes(scopes.size() - 1);
        scopes.pop_back();
        return;
    }
    if (s.kind == "break" || s.kind == "continue") {
        if (loops.empty())
            error(s.kind + " requires a loop");
        runFinalizers(loops.size());
        if (terminated())
            return;
        releaseProtected(loopScopes.back());
        releaseScopes(loopScopes.back());
        call("fw_sweep", b.getVoidTy(), {frame});
        b.CreateBr(s.kind == "break" ? loops.back().first : loops.back().second);
        return;
    }
    error("unsupported statement '" + s.kind + "'");
}
void Compiler::buildFunction(Signature &sig, bool inference) {
    if (sig.building)
        error("recursive functions require an explicit 'returns T'");
    sig.building = true;
    auto saved = saveFunction();
    owner = sig.ast->owner;
    method = sig.ast->name;
    top = false;
    infer = inference;
    returnType = inference ? Ty{"void"} : sig.result;
    scopes.emplace_back();
    std::vector<llvm::Type *> paramTypes;
    if (!owner.empty())
        paramTypes.push_back(b.getPtrTy());
    for (auto &param : sig.ast->params)
        paramTypes.push_back(ll(type(param.type)));
    if (inference)
        current = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), paramTypes, false),
                                         llvm::GlobalValue::InternalLinkage, "infer", module);
    else
        current = sig.function;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", current));
    location = sig.ast->loc;
    if (!inference)
        debugFunction(current, (owner.empty() ? "" : owner + ".") + sig.ast->name, sig.ast->loc);
    frame = objectCall("fw_frame", {});
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
    cells = cellNames(sig.ast->body);
    size_t index = 0;
    if (!owner.empty())
        bind("$inst", {current->getArg(index++), Ty{owner}});
    bool hasDefault = false;
    for (auto &param : sig.ast->params) {
        location = param.loc;
        if (param.value)
            hasDefault = true;
        else if (hasDefault)
            error("required parameters cannot follow default parameters");
        bind(param.name, {current->getArg(index++), type(param.type)});
    }
    block(sig.ast->body, false);
    bool fallsThrough = !terminated();
    if (fallsThrough) {
        if (!inference && sig.result.name != "void")
            error("non-void function '" + sig.ast->name + "' must return on all paths");
        call("fw_drop", b.getVoidTy(), {frame});
        b.CreateRetVoid();
    }
    emitFailure();
    if (inference) {
        auto result = inferredResult(fallsThrough);
        sig.result = result;
        current->eraseFromParent();
        sig.function = llvm::Function::Create(
            llvm::FunctionType::get(ll(result), paramTypes, false),
            llvm::GlobalValue::InternalLinkage,
            "fly." + (owner.empty() ? "" : owner + ".") + sig.ast->name, module);
    } else
        sig.done = true;
    restoreFunction(saved);
    sig.building = false;
}
Val Compiler::sequenceMethod(Val sequence, const std::string &method, Expr &e) {
    auto item = sequence.t.item();
    bool reduce = method == "reduce", predicate = method == "filter" || method == "any" ||
                                                  method == "all";
    if (e.args.size() != (reduce ? 3u : 2u))
        error(method + (reduce ? " expects an initial value and a function" : " expects a function"));
    Val initial = reduce ? value(*e.args[1]) : Val{};
    Ty hint{predicate          ? "function<bool," + item.name + ">"
            : method == "each" ? "function<void," + item.name + ">"
            : reduce ? "function<" + initial.t.name + "," + initial.t.name + "," + item.name + ">"
                     : ""};
    auto function = value(*e.args.back(), hint);
    if (function.t.name.rfind("fn:", 0) == 0) {
        auto &sig = signature(function.t.name.substr(3));
        std::string t = "function<" + sig.result.name;
        for (auto &param : sig.ast->params)
            t += "," + type(param.type).name;
        function = convert(function, Ty{t + ">"});
    }
    if (!function.t.function() || function.t.parts().size() != (reduce ? 3u : 2u))
        error(method + " expects a function taking " + (reduce ? "an accumulator and " : "") +
              "one " + item.name);
    auto parts = function.t.parts();
    auto result = parts[0];
    if (predicate && result.name != "bool")
        error(method + " expects a function returning bool");
    if (method == "map" && result.name == "void")
        error("map expects a function returning a value");
    Ty accumulator = reduce ? parts[1] : Ty{};
    if (reduce && result != accumulator)
        error("reduce function must return its accumulator type");
    Val out;
    if (method == "map" || method == "filter")
        out = {objectCall("fw_new", {frame, i64(0), i64(1)}),
               Ty{(method == "map" ? result : item).name + "[]"}};
    Var state{nullptr, reduce ? accumulator : Ty{"bool"}};
    if (reduce || method == "any" || method == "all") {
        state.address = allocate(state.type, method);
        store(state.address, reduce ? convert(initial, accumulator)
                                    : Val{b.getInt1(method == "all"), Ty{"bool"}});
    }
    auto size = call("fw_length", b.getInt64Ty(), {sequence.v});
    auto index = allocate(Ty{"i64"}, "index");
    b.CreateStore(i64(0), index);
    auto cond = llvm::BasicBlock::Create(ctx, method + ".condition", current);
    auto body = llvm::BasicBlock::Create(ctx, method + ".body", current);
    auto next = llvm::BasicBlock::Create(ctx, method + ".next", current);
    auto end = llvm::BasicBlock::Create(ctx, method + ".end", current);
    b.CreateBr(cond);
    b.SetInsertPoint(cond);
    auto i = b.CreateLoad(b.getInt64Ty(), index);
    b.CreateCondBr(b.CreateICmpSLT(i, size), body, end);
    b.SetInsertPoint(body);
    auto x = get(sequence, i, item);
    auto r = reduce ? callClosure(function, {load(state), x}) : callClosure(function, {x});
    auto append = [&](Val v) { set(out, call("fw_array_append", b.getInt64Ty(), {out.v}), v); };
    if (method == "map")
        append(r);
    else if (reduce)
        store(state.address, r);
    else if (method == "filter") {
        auto keep = llvm::BasicBlock::Create(ctx, "filter.keep", current);
        b.CreateCondBr(r.v, keep, next);
        b.SetInsertPoint(keep);
        append(x);
    } else if (predicate) {
        auto decided = llvm::BasicBlock::Create(ctx, method + ".decided", current);
        bool any = method == "any";
        b.CreateCondBr(r.v, any ? decided : next, any ? next : decided);
        b.SetInsertPoint(decided);
        b.CreateStore(b.getInt1(any), state.address);
        b.CreateBr(end);
    }
    if (!terminated())
        b.CreateBr(next);
    b.SetInsertPoint(next);
    b.CreateStore(b.CreateAdd(b.CreateLoad(b.getInt64Ty(), index), i64(1)), index);
    b.CreateBr(cond);
    b.SetInsertPoint(end);
    if (method == "map" || method == "filter")
        return out;
    if (method == "each")
        return {};
    auto final = load(state);
    if (state.type.object())
        store(state.address, {llvm::ConstantPointerNull::get(b.getPtrTy()), state.type});
    return final;
}
std::optional<Val> Compiler::stdlibCall(const std::string &name, std::vector<Val> &args,
                                        Ty expected) {
    auto dot = name.find('.');
    if (dot == std::string::npos)
        return std::nullopt;
    auto module = name.substr(0, dot);
    auto function = name.substr(dot + 1);
    static const std::set<std::string> modules = {
        "fs",     "time",     "regex", "io",       "json",   "os",  "net",
        "gc",     "thread",   "crypto", "encoding", "uuid", "compress", "sqlite", "toml", "yaml"};
    if (!modules.count(module))
        return std::nullopt;
    auto arity = [&](size_t low, size_t high) {
        if (args.size() < low || args.size() > high)
            error(name + " expects " +
                  (low == high ? std::to_string(low) + " argument(s)"
                               : std::to_string(low) + " to " + std::to_string(high) +
                                     " arguments"));
    };
    auto str = [&](size_t i) { return convert(args[i], Ty{"string"}).v; };
    auto integer = [&](size_t i) { return convert(args[i], Ty{"i64"}).v; };
    auto real = [&](size_t i) {
        if (!args[i].t.numeric())
            error(name + " expects a number");
        return convert(args[i], Ty{"double"}, true).v;
    };
    auto object = [&](const std::string &f, std::vector<llvm::Value *> values, Ty t) {
        values.insert(values.begin(), frame);
        return Val{objectCall(f, std::move(values)), t};
    };
    auto flag = [&](const std::string &f, std::vector<llvm::Value *> values) {
        return Val{b.CreateICmpNE(call(f, b.getInt64Ty(), std::move(values)), i64(0)), Ty{"bool"}};
    };
    auto stringList = [&](size_t i) {
        if (args[i].t.name != "string[]")
            error(name + " expects string[]");
        return args[i].v;
    };
    if (module == "fs") {
        static const std::map<std::string, int> paths = {{"basename", 0}, {"dirname", 1},
                                                         {"extension", 2}, {"stem", 3},
                                                         {"absolute", 4}, {"normalize", 5}};
        if (auto it = paths.find(function); it != paths.end()) {
            arity(1, 1);
            return object("fw_fs_path", {str(0), i64(it->second)}, Ty{"string"});
        }
        if (function == "list" || function == "walk") {
            arity(1, 1);
            return object("fw_fs_list", {str(0), i64(function == "walk")}, Ty{"string[]"});
        }
        if (function == "mkdir") {
            arity(1, 1);
            call("fw_fs_mkdir", b.getVoidTy(), {str(0)});
            return Val{};
        }
        if (function == "append") {
            arity(2, 2);
            call("fw_fs_append", b.getVoidTy(), {str(0), str(1)});
            return Val{};
        }
        if (function == "is_dir" || function == "is_file") {
            arity(1, 1);
            return flag("fw_fs_query", {str(0), i64(function == "is_file")});
        }
        if (function == "size") {
            arity(1, 1);
            return Val{call("fw_fs_query", b.getInt64Ty(), {str(0), i64(2)}), Ty{"i64"}};
        }
        if (function == "copy" || function == "move") {
            arity(2, 2);
            call("fw_fs_copy", b.getVoidTy(), {str(0), str(1), i64(function == "move")});
            return Val{};
        }
        if (function == "remove_all") {
            arity(1, 1);
            return Val{call("fw_fs_remove_all", b.getInt64Ty(), {str(0)}), Ty{"i64"}};
        }
        if (function == "join") {
            if (args.size() == 1 && args[0].t.name == "string[]")
                return object("fw_fs_join", {args[0].v}, Ty{"string"});
            if (args.empty())
                error("fs.join expects path parts");
            Val parts{objectCall("fw_new", {frame, i64(static_cast<int64_t>(args.size())), i64(1)}),
                      Ty{"string[]"}};
            for (size_t i = 0; i < args.size(); ++i)
                set(parts, i64(static_cast<int64_t>(i)), {str(i), Ty{"string"}});
            return object("fw_fs_join", {parts.v}, Ty{"string"});
        }
        if (function == "cwd" || function == "temp_dir") {
            arity(0, 0);
            return object("fw_fs_location", {i64(function == "temp_dir")}, Ty{"string"});
        }
        return std::nullopt;
    }
    if (module == "time") {
        if (function == "now") {
            arity(0, 0);
            return Val{call("fw_time_now", b.getDoubleTy(), {}), Ty{"double"}};
        }
        if (function == "format" || function == "format_local") {
            arity(1, 2);
            bool local = function == "format_local";
            auto pattern = args.size() == 2 ? str(1)
                                            : literal(local ? "%Y-%m-%d %H:%M:%S"
                                                            : "%Y-%m-%dT%H:%M:%SZ")
                                                  .v;
            return object("fw_time_format", {real(0), pattern, i64(local)}, Ty{"string"});
        }
        if (function == "parse") {
            arity(1, 2);
            auto pattern = args.size() == 2 ? str(1) : literal("%Y-%m-%dT%H:%M:%SZ").v;
            return Val{call("fw_time_parse", b.getDoubleTy(), {str(0), pattern}), Ty{"double"}};
        }
        if (function == "date" || function == "local_date") {
            arity(1, 1);
            return object("fw_time_date", {real(0), i64(function == "local_date")},
                          Ty{"time.datetime"});
        }
        if (function == "from_date") {
            arity(3, 6);
            std::vector<llvm::Value *> values;
            for (size_t i = 0; i < 5; ++i)
                values.push_back(i < args.size() ? integer(i) : i64(0));
            values.push_back(args.size() == 6 ? real(5)
                                              : llvm::ConstantFP::get(b.getDoubleTy(), 0.0));
            return Val{call("fw_time_from_date", b.getDoubleTy(), values), Ty{"double"}};
        }
        return std::nullopt;
    }
    if (module == "crypto") {
        static const std::map<std::string, int> hashes = {{"sha256", 0}, {"sha1", 1}, {"md5", 2}};
        auto base = function;
        bool raw = false, keyed = false;
        if (base.size() > 6 && base.compare(base.size() - 6, 6, "_bytes") == 0) {
            raw = true;
            base.resize(base.size() - 6);
        }
        if (base.rfind("hmac_", 0) == 0) {
            keyed = true;
            base = base.substr(5);
        }
        if (auto it = hashes.find(base); it != hashes.end()) {
            arity(keyed ? 2 : 1, keyed ? 2 : 1);
            auto key = keyed ? str(0) : llvm::ConstantPointerNull::get(b.getPtrTy());
            return object("fw_hash", {str(keyed ? 1 : 0), i64(it->second), i64(raw), key},
                          Ty{"string"});
        }
        if (function == "random_bytes" || function == "random_hex") {
            arity(1, 1);
            return object("fw_random_bytes", {integer(0), i64(function == "random_hex")},
                          Ty{"string"});
        }
        if (function == "token") {
            arity(0, 1);
            auto bytes = object("fw_random_bytes", {args.empty() ? i64(32) : integer(0), i64(0)},
                                Ty{"string"});
            return object("fw_encode", {bytes.v, i64(1), i64(0)}, Ty{"string"});
        }
        if (function == "random_int") {
            arity(2, 2);
            return Val{call("fw_secure_int", b.getInt64Ty(), {integer(0), integer(1)}), Ty{"i64"}};
        }
        if (function == "secure_equal") {
            arity(2, 2);
            return flag("fw_secure_equal", {str(0), str(1)});
        }
        return std::nullopt;
    }
    if (module == "encoding") {
        static const std::map<std::string, int> formats = {
            {"base64", 0}, {"base64url", 1}, {"hex", 2}, {"url", 3}, {"form", 4}};
        auto underscore = function.rfind('_');
        if (underscore != std::string::npos) {
            auto format = formats.find(function.substr(0, underscore));
            auto direction = function.substr(underscore + 1);
            if (format != formats.end() && (direction == "encode" || direction == "decode")) {
                arity(1, 1);
                return object("fw_encode",
                              {str(0), i64(format->second), i64(direction == "decode")},
                              Ty{"string"});
            }
        }
        return std::nullopt;
    }
    if (module == "toml" || module == "yaml") {
        if (function == "parse") {
            arity(1, 1);
            return object(module == "toml" ? "fw_toml_parse" : "fw_yaml_parse", {str(0)},
                          Ty{"json.value"});
        }
        return std::nullopt;
    }
    if (module == "sqlite") {
        if (function == "open") {
            arity(1, 1);
            return object("fw_sqlite_open", {str(0)}, Ty{"sqlite.database"});
        }
        return std::nullopt;
    }
    if (module == "uuid") {
        if (function == "v4" || function == "v7") {
            arity(0, 0);
            return object("fw_uuid", {i64(function == "v4" ? 4 : 7)}, Ty{"string"});
        }
        if (function == "is_valid") {
            arity(1, 1);
            return flag("fw_uuid_valid", {str(0)});
        }
        return std::nullopt;
    }
    if (module == "compress") {
        static const std::map<std::string, int> compressors = {
            {"gzip", 0}, {"zlib", 1}, {"deflate", 2}};
        static const std::map<std::string, int> decompressors = {
            {"gunzip", 0}, {"unzlib", 1}, {"inflate", 2}};
        if (auto it = compressors.find(function); it != compressors.end()) {
            arity(1, 2);
            return object("fw_compress", {str(0), i64(it->second), args.size() == 2 ? integer(1) : i64(6)},
                          Ty{"string"});
        }
        if (auto it = decompressors.find(function); it != decompressors.end()) {
            arity(1, 1);
            return object("fw_decompress", {str(0), i64(it->second)}, Ty{"string"});
        }
        if (function == "crc32") {
            arity(1, 1);
            return Val{call("fw_crc32", b.getInt64Ty(), {str(0)}), Ty{"i64"}};
        }
        if (function == "zip") {
            arity(1, 1);
            if (args[0].t.name != "map<string,string>")
                error("compress.zip expects map<string, string> (file name -> contents)");
            return object("fw_zip", {args[0].v}, Ty{"string"});
        }
        if (function == "unzip") {
            arity(1, 1);
            return object("fw_unzip", {str(0)}, Ty{"map<string,string>"});
        }
        return std::nullopt;
    }
    if (module == "regex") {
        if (function == "matches" || function == "contains") {
            arity(2, 2);
            return flag("fw_regex_test", {str(0), str(1), i64(function == "matches")});
        }
        if (function == "find") {
            arity(2, 2);
            return object("fw_regex_find", {str(0), str(1)}, Ty{"string?"});
        }
        if (function == "find_all" || function == "groups") {
            arity(2, 2);
            return object("fw_regex_list", {str(0), str(1), i64(function == "groups")},
                          Ty{function == "groups" ? "string[]?" : "string[]"});
        }
        if (function == "replace") {
            arity(3, 3);
            return object("fw_regex_replace", {str(0), str(1), str(2)}, Ty{"string"});
        }
        if (function == "split") {
            arity(2, 2);
            return object("fw_regex_split", {str(0), str(1)}, Ty{"string[]"});
        }
        return std::nullopt;
    }
    if (module == "io") {
        if (function == "write" || function == "write_error") {
            arity(1, 1);
            call("fw_io_write", b.getVoidTy(),
                 {stringify(args[0]).v, i64(function == "write_error")});
            return Val{};
        }
        if (function == "flush") {
            arity(0, 0);
            call("fw_io_flush", b.getVoidTy(), {});
            return Val{};
        }
        if (function == "read_line") {
            arity(0, 0);
            return object("fw_io_read_line", {}, Ty{"string?"});
        }
        if (function == "read_all") {
            arity(0, 0);
            return object("fw_io_read_all", {}, Ty{"string"});
        }
        if (function == "open") {
            arity(1, 2);
            return object("fw_io_open", {str(0), args.size() == 2 ? str(1) : literal("r").v},
                          Ty{"io.file"});
        }
        return std::nullopt;
    }
    if (module == "json") {
        auto json = [&](size_t i) {
            if (args[i].t.name != "json.value")
                error(name + " expects a json.value");
            return args[i].v;
        };
        if (function == "at") {
            arity(2, 2);
            return object("fw_json_at", {json(0), integer(1)}, Ty{"json.value"});
        }
        if (function == "length") {
            arity(1, 1);
            return Val{call("fw_json_length", b.getInt64Ty(), {json(0)}), Ty{"i64"}};
        }
        if (function == "items" || function == "keys") {
            arity(1, 1);
            return object("fw_json_list", {json(0), i64(function == "keys")},
                          Ty{function == "keys" ? "string[]" : "json.value[]"});
        }
        if (function == "is_null") {
            arity(1, 1);
            return flag("fw_json_is_null", {json(0)});
        }
        if (function == "encode") {
            arity(1, 2);
            if (args[0].t.classReference() || args[0].t.name.rfind("fn:", 0) == 0 ||
                args[0].t.function() || args[0].t.name == "void")
                error("json.encode cannot encode " + displayName(args[0].t.name));
            Val box{objectCall("fw_new", {frame, i64(1), i64(2)}), Ty{"Error"}};
            set(box, i64(0), args[0]);
            return object("fw_json_encode", {box.v, args.size() == 2 ? integer(1) : i64(0)},
                          Ty{"string"});
        }
        if (function == "decode" || function.rfind("decode<", 0) == 0) {
            arity(1, 1);
            Ty target = function == "decode" ? expected
                                             : type(function.substr(7, function.size() - 8));
            if (target.name == "void")
                error("json.decode needs a target type: declare the result type or write "
                      "json.decode<T>(text)");
            auto source = args[0];
            if (source.t.name == "string")
                source = {objectCall("fw_json_parse", {frame, source.v}), Ty{"json.value"}};
            if (source.t.name != "json.value")
                error("json.decode expects a JSON string or json.value");
            return jsonDecode(target, source);
        }
        return std::nullopt;
    }
    if (module == "os") {
        if (function == "args") {
            arity(0, 0);
            return object("fw_os_args", {}, Ty{"string[]"});
        }
        if (function == "exit") {
            arity(0, 1);
            call("fw_os_exit", b.getVoidTy(), {args.empty() ? i64(0) : integer(0)});
            return Val{};
        }
        if (function == "run") {
            arity(1, 1);
            auto process = newInstance(*classes.at("os.process"));
            auto &c = *classes.at("os.process");
            set(process, i64(0), {stringList(0), Ty{"string[]"}});
            for (size_t i = 1; i < c.fields.size(); ++i) {
                Ty t = type(c.fields[i].type);
                set(process, i64(static_cast<int64_t>(i)),
                    convert(defaultValue(*c.fields[i].value, t), t));
            }
            return object("fw_os_run", {process.v}, Ty{"os.result"});
        }
        if (function == "pipeline") {
            arity(1, 2);
            if (args[0].t.name != "string[][]")
                error("os.pipeline expects an array of commands (string[][])");
            return object("fw_os_pipeline",
                          {args[0].v, args.size() == 2 ? str(1) : literal("").v}, Ty{"os.result"});
        }
        return std::nullopt;
    }
    if (module == "gc") {
        if (function == "collect") {
            arity(0, 0);
            return Val{call("fw_gc_collect", b.getInt64Ty(), {}), Ty{"i64"}};
        }
        if (function == "pending") {
            arity(0, 0);
            return Val{call("fw_gc_pending", b.getInt64Ty(), {}), Ty{"i64"}};
        }
        return std::nullopt;
    }
    if (module == "thread") {
        if (function == "spawn") {
            arity(1, 1);
            auto task = asFunction(args[0]);
            if (!task.t.function() || task.t.parts().size() != 1)
                error("thread.spawn expects a function taking no arguments");
            auto result = task.t.parts()[0];
            return object("fw_thread_spawn", {task.v, taskRunner(task.t)},
                          Ty{"thread.task<" + result.name + ">"});
        }
        if (function == "mutex") {
            arity(0, 0);
            return object("fw_thread_mutex", {}, Ty{"thread.mutex"});
        }
        if (function == "cores") {
            arity(0, 0);
            return Val{call("fw_thread_cores", b.getInt64Ty(), {}), Ty{"i64"}};
        }
        if (function == "channel" || function.rfind("channel<", 0) == 0) {
            arity(0, 1);
            Ty t = function == "channel" ? expected : type("thread." + function);
            if (t.name.rfind("thread.channel<", 0) != 0)
                error("thread.channel needs an element type: declare the variable type or "
                      "write thread.channel<T>()");
            return object("fw_channel_new", {args.empty() ? i64(0) : integer(0)}, t);
        }
        return std::nullopt;
    }
    if (function == "resolve") {
        arity(1, 1);
        return object("fw_net_resolve", {str(0)}, Ty{"string[]"});
    }
    if (function == "connect") {
        arity(2, 3);
        auto timeout = args.size() == 3 ? real(2) : llvm::ConstantFP::get(b.getDoubleTy(), 0.0);
        return object("fw_net_connect", {str(0), integer(1), timeout}, Ty{"net.socket"});
    }
    if (function == "listen") {
        arity(2, 2);
        return object("fw_net_listen", {str(0), integer(1)}, Ty{"net.listener"});
    }
    return std::nullopt;
}
// A value bound to an SQL parameter: numbers, bools, chars, strings, enums (by index), and
// nullable forms of these.
Val Compiler::unwrapForBinding(Val v) {
    auto base = nonNull(v.t);
    if (!(base.numeric() || base.name == "bool" || base.name == "char" || base.name == "string" ||
          base.enumeration() || v.t.name == "null"))
        error("SQL parameters must be numbers, bools, strings, or null; got " +
              displayName(v.t.name));
    if (base.name == "u64")
        error("u64 values cannot be SQL parameters; SQLite integers are signed 64-bit");
    return v;
}
std::optional<Val> Compiler::resourceMethod(Val receiver, const std::string &method, Expr &e) {
    auto t = nonNull(receiver.t).name;
    bool generic = t.back() == '>';
    bool task = generic && t.rfind("thread.task<", 0) == 0,
         channel = generic && t.rfind("thread.channel<", 0) == 0;
    if (t != "io.file" && t != "net.socket" && t != "net.listener" && t != "os.process" &&
        t != "thread.mutex" && t != "sqlite.database" && !task && !channel)
        return std::nullopt;
    if (receiver.t.nullable())
        receiver = convert(receiver, Ty{t}, true);
    size_t supplied = e.args.size() - 1;
    auto arity = [&](size_t low, size_t high) {
        if (supplied < low || supplied > high)
            error(t + "." + method + " expects " +
                  (low == high ? std::to_string(low) + " argument(s)"
                               : std::to_string(low) + " to " + std::to_string(high) +
                                     " arguments"));
    };
    auto argument = [&](size_t i, Ty type) { return convert(value(*e.args[i], type), type).v; };
    auto object = [&](const std::string &f, std::vector<llvm::Value *> values, Ty type) {
        values.insert(values.begin(), frame);
        return Val{objectCall(f, std::move(values)), type};
    };
    if (task) {
        auto result = Ty{receiver.t.parts()[0]};
        if (method == "done") {
            arity(0, 0);
            return Val{b.CreateICmpNE(call("fw_thread_done", b.getInt64Ty(), {receiver.v}), i64(0)),
                       Ty{"bool"}};
        }
        if (method != "join")
            error("thread.task has join and done");
        arity(0, 0);
        auto box = objectCall("fw_thread_join", {frame, receiver.v});
        if (result.name == "void")
            return Val{};
        return get({box, Ty{"Error"}}, i64(0), result);
    }
    if (t == "thread.mutex") {
        static const std::map<std::string, int> operations = {
            {"lock", 0}, {"unlock", 1}, {"try_lock", 2}};
        auto it = operations.find(method);
        if (it == operations.end())
            error("thread.mutex has lock, unlock, and try_lock");
        arity(0, 0);
        auto status = call("fw_mutex_operation", b.getInt64Ty(), {receiver.v, i64(it->second)});
        if (method == "try_lock")
            return Val{b.CreateICmpNE(status, i64(0)), Ty{"bool"}};
        return Val{};
    }
    if (channel) {
        auto item = receiver.t.parts()[0];
        if (method == "send") {
            arity(1, 1);
            Val box{objectCall("fw_new", {frame, i64(1), i64(2)}), Ty{"Error"}};
            set(box, i64(0), convert(value(*e.args[1], item), item));
            call("fw_channel_send", b.getVoidTy(), {receiver.v, box.v});
            return Val{};
        }
        if (method == "close") {
            arity(0, 0);
            call("fw_channel_close", b.getVoidTy(), {receiver.v});
            return Val{};
        }
        if (method != "receive" && method != "try_receive")
            error("thread.channel has send, receive, try_receive, and close");
        arity(0, 0);
        // A null holder means closed and drained (receive) or empty (try_receive).
        auto holder = objectCall("fw_channel_receive",
                                 {frame, receiver.v, i64(method == "receive")});
        Ty optional{item.name + "?"};
        auto present = llvm::BasicBlock::Create(ctx, "channel.value", current);
        auto missing = llvm::BasicBlock::Create(ctx, "channel.empty", current);
        auto done = llvm::BasicBlock::Create(ctx, "channel.end", current);
        b.CreateCondBr(b.CreateICmpNE(holder, llvm::ConstantPointerNull::get(b.getPtrTy())),
                       present, missing);
        b.SetInsertPoint(present);
        auto payload = convert(get({holder, Ty{"Error"}}, i64(0), item), optional);
        auto presentEnd = b.GetInsertBlock();
        b.CreateBr(done);
        b.SetInsertPoint(missing);
        b.CreateBr(done);
        b.SetInsertPoint(done);
        auto phi = b.CreatePHI(b.getPtrTy(), 2);
        phi->addIncoming(payload.v, presentEnd);
        phi->addIncoming(llvm::ConstantPointerNull::get(b.getPtrTy()), missing);
        return Val{phi, optional};
    }
    if (t == "os.process") {
        if (method != "run")
            return std::nullopt;
        arity(0, 0);
        return object("fw_os_run", {receiver.v}, Ty{"os.result"});
    }
    if (t == "sqlite.database") {
        if (method == "execute" || method == "query") {
            // db.execute(sql, values...) / db.query(sql, values...): `?` parameters bind the
            // values in order.
            if (supplied < 1)
                error("sqlite.database." + method + " expects SQL and optional values");
            auto sql = argument(1, Ty{"string"});
            Val params{objectCall("fw_new", {frame, i64(static_cast<int64_t>(supplied - 1)), i64(1)}),
                       Ty{"json.value[]"}};
            for (size_t i = 2; i <= supplied; ++i) {
                auto v = unwrapForBinding(value(*e.args[i]));
                set(params, i64(static_cast<int64_t>(i - 2)), v);
            }
            if (method == "execute")
                return Val{call("fw_sqlite_execute", b.getInt64Ty(), {receiver.v, sql, params.v}),
                           Ty{"i64"}};
            return object("fw_sqlite_query", {receiver.v, sql, params.v}, Ty{"json.value"});
        }
        if (method == "last_insert_id") {
            arity(0, 0);
            return Val{call("fw_sqlite_last_insert", b.getInt64Ty(), {receiver.v}), Ty{"i64"}};
        }
        if (method == "close") {
            arity(0, 0);
            call("fw_sqlite_close", b.getVoidTy(), {receiver.v});
            return Val{};
        }
        error("sqlite.database has execute, query, last_insert_id, and close");
    }
    if (t == "io.file") {
        static const std::map<std::string, std::pair<int, std::string>> reads = {
            {"read_line", {0, "string?"}}, {"read_all", {1, "string"}}, {"lines", {2, "string[]"}}};
        if (auto it = reads.find(method); it != reads.end()) {
            arity(0, 0);
            return object("fw_io_file_read", {receiver.v, i64(it->second.first)},
                          Ty{it->second.second});
        }
        if (method == "write") {
            arity(1, 1);
            call("fw_io_file_write", b.getVoidTy(), {receiver.v, stringify(value(*e.args[1])).v});
            return Val{};
        }
        if (method == "close") {
            arity(0, 0);
            call("fw_io_file_close", b.getVoidTy(), {receiver.v});
            return Val{};
        }
        error("io.file has read_line, read_all, lines, write, and close");
    }
    if (method == "close") {
        arity(0, 0);
        call("fw_net_close", b.getVoidTy(), {receiver.v});
        return Val{};
    }
    if (method == "address" || method == "peer") {
        arity(0, 0);
        return object("fw_net_address", {receiver.v, i64(method == "peer")}, Ty{"string"});
    }
    if (method == "port") {
        arity(0, 0);
        return Val{call("fw_net_port", b.getInt64Ty(), {receiver.v}), Ty{"i64"}};
    }
    if (t == "net.listener") {
        if (method != "accept")
            error("net.listener has accept, port, address, and close");
        arity(0, 0);
        return object("fw_net_accept", {receiver.v}, Ty{"net.socket"});
    }
    if (method == "send") {
        arity(1, 1);
        call("fw_net_send", b.getVoidTy(), {receiver.v, argument(1, Ty{"string"})});
        return Val{};
    }
    if (method == "receive") {
        arity(0, 1);
        auto limit = supplied ? argument(1, Ty{"i64"}) : i64(65536);
        return object("fw_net_receive", {receiver.v, limit, i64(0)}, Ty{"string"});
    }
    if (method == "receive_line") {
        arity(0, 0);
        return object("fw_net_receive", {receiver.v, i64(0), i64(1)}, Ty{"string?"});
    }
    if (method == "set_timeout") {
        arity(1, 1);
        auto seconds = value(*e.args[1]);
        if (!seconds.t.numeric())
            error("set_timeout expects seconds");
        call("fw_net_timeout", b.getVoidTy(), {receiver.v, convert(seconds, Ty{"double"}, true).v});
        return Val{};
    }
    error("net.socket has send, receive, receive_line, set_timeout, peer, address, port, and "
          "close");
}
// Generated JSON decoding. Scalars decode inline; collections, nullable values and classes
// use one function per type so recursive types work.
Val Compiler::jsonDecode(Ty t, Val json) {
    if (t.name == "json.value")
        return json;
    if (t.name == "string")
        return {objectCall("fw_json_string", {frame, json.v}), t};
    if (t.integer())
        return convert({call("fw_json_int", b.getInt64Ty(), {json.v}), Ty{"i64"}}, t, true);
    if (t.real())
        return convert({call("fw_json_real", b.getDoubleTy(), {json.v}), Ty{"double"}}, t, true);
    if (t.name == "bool")
        return {b.CreateICmpNE(call("fw_json_bool", b.getInt64Ty(), {json.v}), i64(0)), t};
    if (t.name == "char") {
        auto text = objectCall("fw_json_string", {frame, json.v});
        return {b.CreateTrunc(call("fw_string_to_char", b.getInt64Ty(), {text}), b.getInt32Ty()), t};
    }
    if (t.enumeration()) {
        auto text = objectCall("fw_json_string", {frame, json.v});
        std::string names;
        for (auto &m : enums.at(t.name)->members)
            names += (names.empty() ? "" : ",") + m;
        auto index = call("fw_enum_index", b.getInt64Ty(), {b.CreateGlobalString(names), text});
        guard(b.CreateICmpSGE(index, i64(0)), "unknown " + displayName(t.name) + " member in JSON",
              "JsonError");
        return {index, t};
    }
    auto code = jsonDecodeFunction(t);
    auto result = b.CreateCall(
        llvm::FunctionType::get(ll(t), {b.getPtrTy()}, false), code, {json.v});
    checkError();
    if (t.object())
        objectCall("fw_adopt", {frame, result});
    return {result, t};
}
llvm::Value *Compiler::jsonDecodeFunction(Ty t) {
    auto &code = jsonDecoders[t.name];
    if (code)
        return code;
    bool supported = t.nullable() || t.array() || t.homogeneousTuple() || t.tuple() || t.set() ||
                     t.map() || (classes.count(t.name) && !classes.at(t.name)->isInterface);
    if (!supported)
        error("cannot decode " + displayName(t.name) + " from JSON");
    auto saved = saveFunction();
    top = false;
    infer = false;
    owner.clear();
    returnType = t;
    current = llvm::Function::Create(llvm::FunctionType::get(ll(t), {b.getPtrTy()}, false),
                                     llvm::GlobalValue::InternalLinkage, "fly.json." + t.name,
                                     module);
    code = current;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", current));
    frame = objectCall("fw_frame", {});
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
    scopes.emplace_back();
    Val json{current->getArg(0), Ty{"json.value"}};
    Val result;
    if (t.nullable()) {
        auto inner = nonNull(t);
        auto present = llvm::BasicBlock::Create(ctx, "json.present", current);
        auto missing = llvm::BasicBlock::Create(ctx, "json.null", current);
        b.CreateCondBr(b.CreateICmpNE(call("fw_json_is_null", b.getInt64Ty(), {json.v}), i64(0)),
                       missing, present);
        b.SetInsertPoint(missing);
        call("fw_drop", b.getVoidTy(), {frame});
        b.CreateRet(llvm::ConstantPointerNull::get(b.getPtrTy()));
        b.SetInsertPoint(present);
        result = convert(jsonDecode(inner, json), t);
    } else if (t.map()) {
        auto parts = t.parts();
        Val out{objectCall("fw_map_new", {frame, i64(mapKeyKind(parts[0]))}), t};
        Val keys{objectCall("fw_json_list", {frame, json.v, i64(1)}), Ty{"string[]"}};
        eachItem(keys, [&](Val key) {
            auto item = jsonDecode(parts[1], {objectCall("fw_json_get", {frame, json.v, key.v}),
                                              Ty{"json.value"}});
            // JSON object keys are strings: enum keys use member names; numbers, bools, and
            // chars convert like explicit casts from string.
            Val typedKey = key;
            if (parts[0].enumeration()) {
                std::string names;
                for (auto &m : enums.at(parts[0].name)->members)
                    names += (names.empty() ? "" : ",") + m;
                auto index =
                    call("fw_enum_index", b.getInt64Ty(), {b.CreateGlobalString(names), key.v});
                guard(b.CreateICmpSGE(index, i64(0)),
                      "unknown " + displayName(parts[0].name) + " member in JSON key", "JsonError");
                typedKey = {index, parts[0]};
            } else if (parts[0].integer() || parts[0].name == "bool" || parts[0].name == "char")
                typedKey = explicitCast(key, parts[0]);
            else if (parts[0].name != "string")
                error("cannot decode map keys of type " + displayName(parts[0].name) +
                      " from JSON object keys");
            set(out, mapIndex(out, typedKey, true), convert(item, parts[1]));
        });
        result = out;
    } else if (t.tuple()) {
        auto parts = t.parts();
        auto n = call("fw_json_length", b.getInt64Ty(), {json.v});
        guard(b.CreateICmpEQ(n, i64(static_cast<int64_t>(parts.size()))),
              "JSON array length does not match " + displayName(t.name), "JsonError");
        Val out{objectCall("fw_new", {frame, i64(static_cast<int64_t>(parts.size())), i64(7)}), t};
        for (size_t i = 0; i < parts.size(); ++i) {
            auto item = jsonDecode(parts[i], {objectCall("fw_json_at", {frame, json.v,
                                                                        i64(static_cast<int64_t>(i))}),
                                              Ty{"json.value"}});
            set(out, i64(static_cast<int64_t>(i)), convert(item, parts[i]));
        }
        result = out;
    } else if (t.array() || t.homogeneousTuple() || t.set()) {
        auto item = t.set() ? t.parts()[0] : t.item();
        Val out = t.set() ? newSet(t)
                          : Val{objectCall("fw_new", {frame, i64(0), i64(t.array() ? 1 : 7)}), t};
        Val items{objectCall("fw_json_list", {frame, json.v, i64(0)}), Ty{"json.value[]"}};
        eachItem(items, [&](Val element) {
            auto decoded = convert(jsonDecode(item, element), item);
            if (t.set())
                keyFind(out, decoded, 1);
            else
                set(out, call("fw_array_append", b.getInt64Ty(), {out.v}), decoded);
        });
        result = out;
    } else {
        // Classes: fields decode by name; defaults and null fill missing optional fields.
        auto &c = *classes.at(t.name);
        Val obj = newInstance(c);
        for (size_t i = 0; i < c.fields.size(); ++i) {
            auto &f = c.fields[i];
            Ty ft = type(f.type);
            auto key = literal(f.name);
            llvm::BasicBlock *merge = nullptr;
            if (f.value || ft.nullable()) {
                auto present = llvm::BasicBlock::Create(ctx, "json.field", current);
                auto missing = llvm::BasicBlock::Create(ctx, "json.default", current);
                merge = llvm::BasicBlock::Create(ctx, "json.next", current);
                b.CreateCondBr(
                    b.CreateICmpNE(call("fw_json_has", b.getInt64Ty(), {json.v, key.v}), i64(0)),
                    present, missing);
                b.SetInsertPoint(missing);
                set(obj, i64(static_cast<int64_t>(i)),
                    f.value ? convert(defaultValue(*f.value, ft), ft)
                            : Val{llvm::ConstantPointerNull::get(b.getPtrTy()), ft});
                b.CreateBr(merge);
                b.SetInsertPoint(present);
            }
            auto field = jsonDecode(ft, {objectCall("fw_json_get", {frame, json.v, key.v}),
                                         Ty{"json.value"}});
            set(obj, i64(static_cast<int64_t>(i)), convert(field, ft));
            if (merge) {
                b.CreateBr(merge);
                b.SetInsertPoint(merge);
            }
        }
        result = obj;
    }
    if (result.t.object())
        call("fw_retain", b.getVoidTy(), {result.v});
    call("fw_drop", b.getVoidTy(), {frame});
    b.CreateRet(result.v);
    emitFailure();
    restoreFunction(saved);
    return code;
}
Val Compiler::stringMethod(Val text, const std::string &method, Expr &e) {
    size_t supplied = e.args.size() - 1;
    auto arity = [&](size_t low, size_t high) {
        if (supplied < low || supplied > high)
            error("string." + method + " expects " +
                  (low == high ? std::to_string(low) + " argument(s)"
                               : std::to_string(low) + " to " + std::to_string(high) +
                                     " arguments"));
    };
    auto argument = [&](size_t i, Ty t) { return convert(value(*e.args[i], t), t); };
    auto textArgument = [&](size_t i) {
        auto v = value(*e.args[i], Ty{"string"});
        return v.t.name == "char" ? stringify(v) : convert(v, Ty{"string"});
    };
    auto result = [&](const std::string &function, std::vector<llvm::Value *> values, Ty t) {
        values.insert(values.begin(), frame);
        return Val{objectCall(function, std::move(values)), t};
    };
    static const std::map<std::string, int> transforms = {
        {"upper", 0}, {"lower", 1}, {"trim", 2}, {"trim_start", 3}, {"trim_end", 4}, {"reverse", 5}};
    static const std::map<std::string, int> searches = {{"find", 0},        {"contains", 1},
                                                        {"starts_with", 2}, {"ends_with", 3},
                                                        {"count", 4},       {"find_last", 5}};
    auto null = llvm::ConstantPointerNull::get(b.getPtrTy());
    if (auto it = transforms.find(method); it != transforms.end()) {
        arity(0, 0);
        return result("fw_string_transform", {text.v, i64(it->second)}, Ty{"string"});
    }
    if (auto it = searches.find(method); it != searches.end()) {
        arity(1, 1);
        auto found =
            call("fw_string_search", b.getInt64Ty(), {text.v, textArgument(1).v, i64(it->second)});
        if (it->second >= 1 && it->second <= 3)
            return {b.CreateICmpNE(found, i64(0)), Ty{"bool"}};
        return {found, Ty{"i64"}};
    }
    if (method == "split") {
        arity(0, 1);
        llvm::Value *separator = supplied ? textArgument(1).v : null;
        return result("fw_string_split", {text.v, separator, i64(0)}, Ty{"string[]"});
    }
    if (method == "lines") {
        arity(0, 0);
        return result("fw_string_split", {text.v, null, i64(1)}, Ty{"string[]"});
    }
    if (method == "chars") {
        arity(0, 0);
        return result("fw_string_chars", {text.v}, Ty{"char[]"});
    }
    if (method == "replace") {
        arity(2, 2);
        auto from = textArgument(1);
        auto to = textArgument(2);
        return result("fw_string_replace", {text.v, from.v, to.v}, Ty{"string"});
    }
    if (method == "slice") {
        arity(1, 2);
        auto from = argument(1, Ty{"i64"});
        auto to = supplied == 2 ? argument(2, Ty{"i64"}) : Val{i64(0), Ty{"i64"}};
        return result("fw_string_slice", {text.v, from.v, to.v, i64(supplied == 2 ? 1 : 0)},
                      Ty{"string"});
    }
    if (method == "pad_start" || method == "pad_end") {
        arity(1, 2);
        auto width = argument(1, Ty{"i64"});
        auto fill = supplied == 2 ? textArgument(2) : literal(" ");
        return result("fw_string_pad", {text.v, width.v, fill.v, i64(method == "pad_end")},
                      Ty{"string"});
    }
    if (method == "join") {
        arity(1, 1);
        auto items = value(*e.args[1]);
        if (!items.t.sequence())
            error("string.join expects an array or homogeneous tuple");
        return result("fw_array_join", {items.v, text.v}, Ty{"string"});
    }
    if (method == "repeat") {
        arity(1, 1);
        return result("fw_repeat", {text.v, argument(1, Ty{"i64"}).v}, Ty{"string"});
    }
    if (method == "is_empty") {
        arity(0, 0);
        return {b.CreateICmpEQ(call("fw_length", b.getInt64Ty(), {text.v}), i64(0)), Ty{"bool"}};
    }
    error("unknown string method '" + method + "'");
}
std::optional<Val> Compiler::sequenceTool(Val sequence, const std::string &method, Expr &e) {
    static const std::set<std::string> tools = {"sort",   "reverse",   "slice",  "index_of",
                                                "contains", "insert",  "remove_at", "remove",
                                                "extend", "copy",      "join"};
    if (!tools.count(method))
        return std::nullopt;
    size_t supplied = e.args.size() - 1;
    auto item = sequence.t.item();
    auto arity = [&](size_t low, size_t high) {
        if (supplied < low || supplied > high)
            error(method + " expects " +
                  (low == high ? std::to_string(low) + " argument(s)"
                               : std::to_string(low) + " to " + std::to_string(high) +
                                     " arguments"));
    };
    static const std::set<std::string> mutating = {"sort",      "reverse", "insert",
                                                   "remove_at", "remove",  "extend"};
    if (mutating.count(method) && !sequence.t.array())
        error("tuples are immutable; '" + method + "' requires an array");
    auto argument = [&](size_t i, Ty t) { return convert(value(*e.args[i], t), t); };
    auto search = [&](size_t i) {
        Val box{objectCall("fw_new", {frame, i64(1), i64(2)}), Ty{"Error"}};
        set(box, i64(0), argument(i, item));
        return call("fw_array_index_of", b.getInt64Ty(), {sequence.v, box.v});
    };
    if (method == "sort") {
        arity(0, 1);
        if (supplied == 0) {
            if (!item.numeric() && item.name != "char" && item.name != "bool" &&
                item.name != "string")
                error("sort() without a comparator requires numbers, chars, bools, or strings; "
                      "pass a function(" + displayName(item.name) + ", " +
                      displayName(item.name) + ") returns bool");
            call("fw_array_sort", b.getVoidTy(), {sequence.v});
            return Val{};
        }
        Ty hint{"function<bool," + item.name + "," + item.name + ">"};
        auto less = asFunction(value(*e.args[1], hint));
        if (less.t != hint)
            error("sort expects a function(" + displayName(item.name) + ", " +
                  displayName(item.name) + ") returns bool");
        call("fw_array_sort_by", b.getVoidTy(),
             {sequence.v, less.v, lessTrampoline(item, less.t)});
        return Val{};
    }
    if (method == "reverse") {
        arity(0, 0);
        call("fw_array_reverse", b.getVoidTy(), {sequence.v});
        return Val{};
    }
    if (method == "slice") {
        arity(1, 2);
        auto from = argument(1, Ty{"i64"});
        auto to = supplied == 2 ? argument(2, Ty{"i64"}) : Val{i64(0), Ty{"i64"}};
        return Val{objectCall("fw_array_slice", {frame, sequence.v, from.v, to.v,
                                                 i64(supplied == 2 ? 1 : 0)}),
                   sequence.t};
    }
    if (method == "index_of" || method == "contains") {
        arity(1, 1);
        auto index = search(1);
        if (method == "contains")
            return Val{b.CreateICmpSGE(index, i64(0)), Ty{"bool"}};
        return Val{index, Ty{"i64"}};
    }
    if (method == "insert") {
        arity(2, 2);
        auto position = argument(1, Ty{"i64"});
        auto x = argument(2, item);
        set(sequence, call("fw_array_insert", b.getInt64Ty(), {sequence.v, position.v}), x);
        return Val{};
    }
    if (method == "remove_at") {
        arity(1, 1);
        auto position = argument(1, Ty{"i64"});
        auto removed = get(sequence, position.v, item);
        call("fw_array_remove_at", b.getVoidTy(), {sequence.v, position.v});
        return removed;
    }
    if (method == "remove") {
        arity(1, 1);
        auto index = search(1);
        auto found = b.CreateICmpSGE(index, i64(0));
        auto yes = llvm::BasicBlock::Create(ctx, "remove.found", current);
        auto done = llvm::BasicBlock::Create(ctx, "remove.end", current);
        b.CreateCondBr(found, yes, done);
        b.SetInsertPoint(yes);
        call("fw_array_remove_at", b.getVoidTy(), {sequence.v, index});
        b.CreateBr(done);
        b.SetInsertPoint(done);
        return Val{found, Ty{"bool"}};
    }
    if (method == "extend") {
        arity(1, 1);
        auto other = value(*e.args[1], sequence.t);
        if (!other.t.sequence() || other.t.item() != item)
            error("extend expects a sequence of " + displayName(item.name));
        call("fw_array_extend", b.getVoidTy(), {sequence.v, other.v});
        return Val{};
    }
    if (method == "copy") {
        arity(0, 0);
        return Val{objectCall("fw_array_copy", {frame, sequence.v}), sequence.t};
    }
    arity(0, 1);
    auto separator = supplied ? argument(1, Ty{"string"}) : literal("");
    return Val{objectCall("fw_array_join", {frame, sequence.v, separator.v}), Ty{"string"}};
}
// less(closure, array, i, j) for fw_array_sort_by; returns 0 after a callback error.
llvm::Function *Compiler::lessTrampoline(Ty item, Ty function) {
    auto &code = trampolines["less:" + function.name];
    if (code)
        return code;
    auto saved = saveFunction();
    top = false;
    infer = false;
    returnType = Ty{"i64"};
    current = llvm::Function::Create(
        llvm::FunctionType::get(b.getInt64Ty(),
                                {b.getPtrTy(), b.getPtrTy(), b.getInt64Ty(), b.getInt64Ty()},
                                false),
        llvm::GlobalValue::InternalLinkage, "fly.less", module);
    code = current;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", current));
    frame = objectCall("fw_frame", {});
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
    scopes.emplace_back();
    Val array{current->getArg(1), Ty{item.name + "[]"}};
    auto x = get(array, current->getArg(2), item);
    auto y = get(array, current->getArg(3), item);
    auto result = callClosure(Val{current->getArg(0), function}, std::vector<Val>{x, y});
    auto answer = b.CreateZExt(result.v, b.getInt64Ty());
    call("fw_drop", b.getVoidTy(), {frame});
    b.CreateRet(answer);
    emitFailure();
    restoreFunction(saved);
    return code;
}
// run(closure, result) for fw_thread_spawn: calls the task and stores its result in slot 0.
llvm::Function *Compiler::taskRunner(Ty function) {
    auto &code = trampolines["task:" + function.name];
    if (code)
        return code;
    auto saved = saveFunction();
    top = false;
    infer = false;
    owner.clear();
    returnType = Ty{"void"};
    current = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy(), b.getPtrTy()}, false),
        llvm::GlobalValue::InternalLinkage, "fly.task", module);
    code = current;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", current));
    frame = objectCall("fw_frame", {});
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
    scopes.emplace_back();
    auto result = callClosure(Val{current->getArg(0), function}, std::vector<Val>{});
    if (result.t.name != "void")
        set({current->getArg(1), Ty{"Error"}}, i64(0), result);
    call("fw_drop", b.getVoidTy(), {frame});
    b.CreateRetVoid();
    emitFailure();
    restoreFunction(saved);
    return code;
}
std::optional<Val> Compiler::mathCall(const std::string &name, std::vector<Val> &args) {
    if (name.rfind("math.", 0) != 0)
        return std::nullopt;
    auto method = name.substr(5);
    auto count = [&](size_t n) {
        if (args.size() != n)
            error(name + " expects " + std::to_string(n) + " argument(s)");
    };
    auto real = [&](size_t i) {
        if (!args[i].t.numeric())
            error(name + " requires numeric arguments");
        return convert(args[i], Ty{"double"}, true).v;
    };
    auto common = [&](std::vector<Val> values) {
        Ty t = values[0].t;
        for (auto &v : values) {
            if (!v.t.numeric())
                error(name + " requires numeric arguments");
            if (widening(t, v.t))
                t = v.t;
            else if (!widening(v.t, t))
                error(name + " needs arguments with a lossless common numeric type");
        }
        return t;
    };
    auto select = [&](const std::string &op, Val a, Val c) {
        auto less = binary(op, a, c);
        return Val{b.CreateSelect(less.v, a.v, c.v), a.t};
    };
    static const std::map<std::string, int> unary = {
        {"floor", 0}, {"ceil", 1}, {"round", 2},  {"trunc", 3},  {"log10", 5}, {"log2", 6},
        {"exp", 7},   {"tan", 8},  {"asin", 9},   {"acos", 10},  {"atan", 11}};
    if (auto it = unary.find(method); it != unary.end()) {
        count(1);
        return Val{call("fw_math_unary", b.getDoubleTy(), {real(0), i64(it->second)}),
                   Ty{"double"}};
    }
    if (method == "log") {
        if (args.size() != 1 && args.size() != 2)
            error("math.log expects a value and an optional base");
        auto natural = call("fw_math_unary", b.getDoubleTy(), {real(0), i64(4)});
        if (args.size() == 1)
            return Val{natural, Ty{"double"}};
        auto base = call("fw_math_unary", b.getDoubleTy(), {real(1), i64(4)});
        return Val{b.CreateFDiv(natural, base), Ty{"double"}};
    }
    if (method == "atan2" || method == "hypot") {
        count(2);
        return Val{call("fw_math_binary", b.getDoubleTy(),
                        {real(0), real(1), i64(method == "atan2" ? 0 : 1)}),
                   Ty{"double"}};
    }
    if (method == "is_nan" || method == "is_finite") {
        count(1);
        auto x = real(0);
        if (method == "is_nan")
            return Val{b.CreateFCmpUNO(x, x), Ty{"bool"}};
        auto magnitude = call("fw_math_unary", b.getDoubleTy(), {x, i64(12)});
        return Val{b.CreateFCmpOLT(magnitude, llvm::ConstantFP::getInfinity(b.getDoubleTy())),
                   Ty{"bool"}};
    }
    if (method == "abs") {
        count(1);
        auto x = args[0];
        if (!x.t.numeric())
            error("math.abs requires a number");
        if (x.t.real())
            return Val{b.CreateFPCast(call("fw_math_unary", b.getDoubleTy(),
                                           {real(0), i64(12)}),
                                      ll(x.t)),
                       x.t};
        if (x.t.unsigned_())
            return x;
        auto zero = Val{llvm::ConstantInt::get(ll(x.t), 0), x.t};
        auto negative = b.CreateICmpSLT(x.v, zero.v);
        auto flip = llvm::BasicBlock::Create(ctx, "abs.negative", current);
        auto done = llvm::BasicBlock::Create(ctx, "abs.end", current);
        auto entry = b.GetInsertBlock();
        b.CreateCondBr(negative, flip, done);
        b.SetInsertPoint(flip);
        auto negated = binary("-", zero, x);
        auto flipped = b.GetInsertBlock();
        b.CreateBr(done);
        b.SetInsertPoint(done);
        auto phi = b.CreatePHI(ll(x.t), 2);
        phi->addIncoming(x.v, entry);
        phi->addIncoming(negated.v, flipped);
        return Val{phi, x.t};
    }
    if (method == "min" || method == "max") {
        if (args.size() < 2)
            error(name + " expects at least two values");
        auto t = common(args);
        auto best = convert(args[0], t);
        for (size_t i = 1; i < args.size(); ++i)
            best = select(method == "min" ? "<=" : ">=", best, convert(args[i], t));
        return best;
    }
    if (method == "clamp") {
        count(3);
        auto t = common(args);
        auto low = convert(args[1], t), high = convert(args[2], t);
        guard(binary("<=", low, high).v, "math.clamp requires low <= high");
        return select(">=", select("<=", convert(args[0], t), high), low);
    }
    if (method == "random") {
        count(0);
        return Val{call("fw_random", b.getDoubleTy(), {}), Ty{"double"}};
    }
    if (method == "random_int") {
        count(2);
        return Val{call("fw_random_int", b.getInt64Ty(),
                        {convert(args[0], Ty{"i64"}).v, convert(args[1], Ty{"i64"}).v}),
                   Ty{"i64"}};
    }
    if (method == "seed") {
        count(1);
        call("fw_random_seed", b.getVoidTy(), {convert(args[0], Ty{"i64"}).v});
        return Val{};
    }
    return std::nullopt;
}
// a ?? b: a's payload when a is not null, otherwise b (evaluated only then).
Val Compiler::coalesce(Expr &e, Ty expected) {
    auto a = value(*e.args[0], expected.name == "void" ? expected : Ty{expected.name + "?"});
    if (a.t.name == "null")
        return value(*e.args[1], expected);
    if (!a.t.nullable())
        error("'?\?' requires a nullable left operand, not " + a.t.name);
    auto inner = nonNull(a.t);
    auto present = llvm::BasicBlock::Create(ctx, "coalesce.present", current);
    auto missing = llvm::BasicBlock::Create(ctx, "coalesce.missing", current);
    auto merge = llvm::BasicBlock::Create(ctx, "coalesce.end", current);
    b.CreateCondBr(b.CreateICmpNE(a.v, llvm::ConstantPointerNull::get(b.getPtrTy())), present,
                   missing);
    b.SetInsertPoint(missing);
    auto fallback = value(*e.args[1], inner);
    // A nullable fallback keeps the result nullable; otherwise the result is the payload type.
    Ty result = fallback.t.nullable() ? a.t : inner;
    fallback = convert(fallback, result);
    auto missingEnd = b.GetInsertBlock();
    b.CreateBr(merge);
    b.SetInsertPoint(present);
    auto payload = result == a.t ? a : unboxChecked(a);
    auto presentEnd = b.GetInsertBlock();
    b.CreateBr(merge);
    b.SetInsertPoint(merge);
    auto phi = b.CreatePHI(ll(result), 2);
    phi->addIncoming(payload.v, presentEnd);
    phi->addIncoming(fallback.v, missingEnd);
    return {phi, result};
}
llvm::Function *Compiler::closureCode(const std::string &key, Ty function,
                                      const std::function<Val(llvm::Value *, Expr &)> &emit) {
    auto &code = trampolines[key];
    if (code)
        return code;
    auto parts = function.parts();
    auto saved = saveFunction();
    top = false;
    infer = false;
    owner.clear();
    returnType = parts[0];
    std::vector<llvm::Type *> types{b.getPtrTy()};
    for (size_t i = 1; i < parts.size(); ++i)
        types.push_back(ll(parts[i]));
    current = llvm::Function::Create(llvm::FunctionType::get(ll(parts[0]), types, false),
                                     llvm::GlobalValue::InternalLinkage, "fly.closure." + key,
                                     module);
    code = current;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", current));
    frame = objectCall("fw_frame", {});
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
    scopes.emplace_back();
    Expr synthetic;
    synthetic.kind = "call";
    synthetic.loc = location;
    synthetic.args.emplace_back();
    for (size_t i = 1; i < parts.size(); ++i) {
        auto name = "$a" + std::to_string(i - 1);
        bind(name, {current->getArg(static_cast<unsigned>(i)), parts[i]});
        synthetic.args.push_back(syntheticName(name, location));
    }
    auto result = emit(current->getArg(0), synthetic);
    if (parts[0].name == "void") {
        call("fw_drop", b.getVoidTy(), {frame});
        b.CreateRetVoid();
    } else {
        result = convert(result, parts[0]);
        if (result.t.object())
            call("fw_retain", b.getVoidTy(), {result.v});
        call("fw_drop", b.getVoidTy(), {frame});
        b.CreateRet(result.v);
    }
    emitFailure();
    restoreFunction(saved);
    return code;
}
namespace {
// Replaces identifiers naming type parameters inside a type spelling.
std::string substituteType(const std::string &text,
                           const std::map<std::string, std::string> &bindings) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c) || c == '_' || c == '$' || c >= 0x80) {
            size_t end = i + 1;
            while (end < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' ||
                    text[end] == '$' || text[end] == '.' ||
                    static_cast<unsigned char>(text[end]) >= 0x80))
                ++end;
            auto word = text.substr(i, end - i);
            auto it = bindings.find(word);
            out += it == bindings.end() ? word : it->second;
            i = end;
        } else
            out += text[i++];
    }
    return out;
}
void substitute(Function &f, const std::map<std::string, std::string> &bindings);
void substitute(Expr &e, const std::map<std::string, std::string> &bindings) {
    if (e.kind == "is" || e.kind == "as" || e.kind == "name")
        e.text = substituteType(e.text, bindings);
    for (auto &a : e.args)
        if (a)
            substitute(*a, bindings);
    if (e.lambda)
        substitute(*e.lambda, bindings);
}
void substitute(std::vector<SP> &body, const std::map<std::string, std::string> &bindings) {
    for (auto &st : body) {
        st->type = substituteType(st->type, bindings);
        for (auto &t : st->catchTypes)
            t = substituteType(t, bindings);
        if (st->expr)
            substitute(*st->expr, bindings);
        if (st->target)
            substitute(*st->target, bindings);
        substitute(st->body, bindings);
        substitute(st->other, bindings);
        substitute(st->finalizer, bindings);
        for (auto &c : st->catches)
            substitute(c, bindings);
        for (auto &values : st->patterns)
            for (auto &v : values)
                substitute(*v, bindings);
        for (auto &g : st->guards)
            if (g)
                substitute(*g, bindings);
    }
}
void substitute(Function &f, const std::map<std::string, std::string> &bindings) {
    f.result = substituteType(f.result, bindings);
    for (auto &param : f.params) {
        param.type = substituteType(param.type, bindings);
        if (param.value)
            substitute(*param.value, bindings);
    }
    substitute(f.body, bindings);
}
// Binds type parameters by matching a declared parameter type against an argument type.
void unify(std::string pattern, const std::string &concrete, const std::set<std::string> &params,
           std::map<std::string, std::string> &bound) {
    if (params.count(pattern)) {
        bound.emplace(pattern, concrete);
        return;
    }
    if (pattern.rfind("array<", 0) == 0 && pattern.back() == '>')
        pattern = pattern.substr(6, pattern.size() - 7) + "[]";
    auto ends = [](const std::string &text, const std::string &suffix) {
        return text.size() >= suffix.size() &&
               text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends(pattern, "[]") && ends(concrete, "[]"))
        return unify(pattern.substr(0, pattern.size() - 2), concrete.substr(0, concrete.size() - 2),
                     params, bound);
    if (ends(pattern, "?"))
        return unify(pattern.substr(0, pattern.size() - 1),
                     ends(concrete, "?") ? concrete.substr(0, concrete.size() - 1) : concrete,
                     params, bound);
    auto open = pattern.find('<'), concreteOpen = concrete.find('<');
    bool tuples = !pattern.empty() && pattern.front() == '[' && !concrete.empty() &&
                  concrete.front() == '[';
    if (tuples || (open != std::string::npos && concreteOpen != std::string::npos &&
                   pattern.substr(0, open) == concrete.substr(0, concreteOpen))) {
        size_t a = tuples ? 1 : open + 1, c = tuples ? 1 : concreteOpen + 1;
        auto left = typeParts(pattern.substr(a, pattern.size() - a - 1));
        auto right = typeParts(concrete.substr(c, concrete.size() - c - 1));
        for (size_t i = 0; i < left.size() && i < right.size(); ++i)
            unify(left[i], right[i], params, bound);
    }
}
} // namespace
// Checks type arguments against constraints such as `T implements Comparable`. Besides classes
// and interfaces, a bound may be `numeric`, `comparable` (supports <), or `hashable`.
void Compiler::checkBounds(const std::string &generic, const std::vector<std::string> &params,
                           const std::map<std::string, std::string> &bounds,
                           const std::vector<Ty> &arguments) {
    for (size_t i = 0; i < params.size() && i < arguments.size(); ++i) {
        auto it = bounds.find(params[i]);
        if (it == bounds.end())
            continue;
        auto &bound = it->second;
        auto t = arguments[i];
        bool ok;
        if (bound == "numeric")
            ok = t.numeric();
        else if (bound == "comparable")
            ok = t.numeric() || t.name == "string" || t.name == "char" ||
                 (classes.count(t.name) && !resolveMethod(t.name, "operator<").empty());
        else if (bound == "hashable")
            ok = hashable(t);
        else {
            auto target = type(bound);
            if (!classes.count(target.name))
                error("constraint " + displayName(bound) + " on " + params[i] +
                      " must be a class, an interface, numeric, comparable, or hashable");
            ok = classes.count(t.name) && derives(t.name, target.name);
        }
        if (!ok)
            error("type argument " + displayName(t.name) + " for " + params[i] + " of " +
                  displayName(generic) + " does not satisfy '" + params[i] +
                  (bound == "numeric" || bound == "comparable" || bound == "hashable"
                       ? " is " : " implements ") +
                  displayName(bound) + "'");
    }
}
// The generic method `method` of class `name` or a base class: its "Class.method" key or "".
std::string Compiler::methodTemplate(const std::string &name, const std::string &method) {
    auto it = classes.find(name);
    if (it == classes.end())
        return "";
    if (methodTemplates.count(name + "." + method))
        return name + "." + method;
    if (!it->second->base.empty())
        return methodTemplate(it->second->base, method);
    return "";
}
// A call of a generic method, `obj.convert<int>(x)` or with type arguments inferred from the
// arguments. Each instantiation is a non-virtual method of the declaring class.
std::optional<Val> Compiler::genericMethodCall(Val receiver, const std::string &text, Expr &e,
                                               Ty expected) {
    auto open = text.find('<');
    auto name = text.substr(0, open);
    auto key = methodTemplate(receiver.t.name, name);
    if (key.empty())
        return std::nullopt;
    auto &templ = *methodTemplates.at(key);
    auto ownerClass = key.substr(0, key.size() - name.size() - 1);
    std::vector<Ty> arguments;
    std::vector<Val> values;
    if (open != std::string::npos) {
        for (auto &part : typeParts(text.substr(open + 1, text.size() - open - 2)))
            arguments.push_back(type(part));
        if (arguments.size() != templ.typeParams.size())
            error(displayName(key) + " expects " + std::to_string(templ.typeParams.size()) +
                  " type argument(s)");
    } else {
        std::set<std::string> names(templ.typeParams.begin(), templ.typeParams.end());
        std::map<std::string, std::string> bound;
        for (size_t i = 1; i < e.args.size(); ++i) {
            auto v = asFunction(value(*e.args[i]));
            if (v.t.classReference())
                v = convert(v, constructorType(v.t.name.substr(6)));
            if (i - 1 < templ.params.size())
                unify(templ.params[i - 1].type, v.t.name, names, bound);
            values.push_back(v);
        }
        for (auto &param : templ.typeParams) {
            if (!bound.count(param))
                error("cannot infer type parameter " + param + " of " + displayName(key) +
                      "; write " + name + "<...>(...)");
            arguments.push_back(type(bound.at(param)));
        }
    }
    std::string instance = name + "<";
    std::map<std::string, std::string> bindings;
    for (size_t i = 0; i < arguments.size(); ++i) {
        instance += (i ? "," : "") + arguments[i].name;
        bindings[templ.typeParams[i]] = arguments[i].name;
    }
    instance += ">";
    if (!functions.count(ownerClass + "." + instance)) {
        checkBounds(key, templ.typeParams, templ.typeBounds, arguments);
        auto saved = location;
        functionInstances.push_back(clone(templ));
        auto &f = functionInstances.back();
        f.name = instance;
        f.typeParams.clear();
        substitute(f, bindings);
        declareFunction(f);
        location = saved;
    }
    Expr call;
    call.kind = "call";
    call.loc = e.loc;
    auto callee = std::make_unique<Expr>();
    callee->kind = "member";
    callee->text = instance;
    callee->loc = e.loc;
    call.args.push_back(std::move(callee));
    scopes.emplace_back();
    if (open != std::string::npos)
        for (size_t i = 1; i < e.args.size(); ++i)
            call.args.push_back(clone(*e.args[i]));
    else
        for (auto &v : values) {
            auto hidden = "$g" + std::to_string(hiddenCount++);
            bind(hidden, v, true);
            call.args.push_back(syntheticName(hidden, e.loc));
        }
    auto result = callMethod({receiver.v, Ty{ownerClass}}, ownerClass, instance, call, true);
    releaseScopes(scopes.size() - 1);
    scopes.pop_back();
    (void)expected;
    return result;
}
std::string Compiler::instantiateClass(const std::string &base, const std::vector<Ty> &arguments) {
    auto &templ = *templates.at(base);
    if (arguments.size() != templ.typeParams.size())
        error(displayName(base) + " expects " + std::to_string(templ.typeParams.size()) +
              " type argument(s)");
    std::string key = base + "<";
    std::map<std::string, std::string> bindings;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i].name == "void")
            error("type arguments cannot be void");
        key += (i ? "," : "") + arguments[i].name;
        bindings[templ.typeParams[i]] = arguments[i].name;
    }
    key += ">";
    if (classes.count(key))
        return key;
    checkBounds(base, templ.typeParams, templ.typeBounds, arguments);
    if (key.size() > 2000)
        error("generic instantiation of " + displayName(base) + " is too deeply nested");
    auto saved = location;
    instances.push_back(clone(templ));
    auto &c = instances.back();
    c.name = key;
    c.typeParams.clear();
    c.base = substituteType(c.base, bindings);
    for (auto &i : c.interfaces)
        i = substituteType(i, bindings);
    for (auto &f : c.fields) {
        f.type = substituteType(f.type, bindings);
        if (f.value)
            substitute(*f.value, bindings);
    }
    classes[key] = &c;
    finishClass(c);
    for (auto &m : c.methods) {
        substitute(m, bindings);
        m.owner = key;
        declareFunction(m);
    }
    location = saved;
    return key;
}
std::string Compiler::instantiateFunction(const std::string &base,
                                          const std::vector<Ty> &arguments) {
    auto &templ = *functionTemplates.at(base);
    if (arguments.size() != templ.typeParams.size())
        error(displayName(base) + " expects " + std::to_string(templ.typeParams.size()) +
              " type argument(s)");
    std::string key = base + "<";
    std::map<std::string, std::string> bindings;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i].name == "void")
            error("type arguments cannot be void");
        key += (i ? "," : "") + arguments[i].name;
        bindings[templ.typeParams[i]] = arguments[i].name;
    }
    key += ">";
    if (functions.count(key))
        return key;
    checkBounds(base, templ.typeParams, templ.typeBounds, arguments);
    auto saved = location;
    functionInstances.push_back(clone(templ));
    auto &f = functionInstances.back();
    f.name = key;
    f.typeParams.clear();
    substitute(f, bindings);
    declareFunction(f);
    location = saved;
    return key;
}
// Calls of generic functions and classes: explicit `f<int>(x)` or arguments inferred from
// the argument types (and, for constructors, from the expected type).
std::optional<Val> Compiler::genericCall(Expr &e, const std::string &name, Ty expected) {
    if (classes.count(name) || functions.count(name))
        return std::nullopt; // already an instantiation
    auto open = name.find('<');
    auto base = name.substr(0, open);
    bool isClass = templates.count(base) > 0;
    if (!isClass && !functionTemplates.count(base))
        return std::nullopt;
    std::vector<Ty> arguments;
    std::vector<Val> values;
    if (open != std::string::npos) {
        for (auto &part : typeParts(name.substr(open + 1, name.size() - open - 2)))
            arguments.push_back(type(part));
    } else {
        auto &params = isClass ? templates.at(base)->typeParams : functionTemplates.at(base)->typeParams;
        std::vector<std::string> declared;
        if (isClass) {
            auto &templ = *templates.at(base);
            auto init = std::find_if(templ.methods.begin(), templ.methods.end(),
                                     [](Function &f) { return f.name == "init"; });
            if (init != templ.methods.end())
                for (auto &param : init->params)
                    declared.push_back(param.type);
            else
                for (auto &f : templ.fields)
                    declared.push_back(f.type);
        } else
            for (auto &param : functionTemplates.at(base)->params)
                declared.push_back(param.type);
        std::set<std::string> names(params.begin(), params.end());
        std::map<std::string, std::string> bound;
        if (isClass && expected.name.rfind(base + "<", 0) == 0 && expected.name.back() == '>') {
            auto parts = typeParts(expected.name.substr(base.size() + 1,
                                                        expected.name.size() - base.size() - 2));
            for (size_t i = 0; i < parts.size() && i < params.size(); ++i)
                bound.emplace(params[i], parts[i]);
        }
        for (size_t i = 1; i < e.args.size(); ++i) {
            auto v = asFunction(value(*e.args[i]));
            if (v.t.classReference())
                v = convert(v, constructorType(v.t.name.substr(6)));
            if (i - 1 < declared.size())
                unify(declared[i - 1], v.t.name, names, bound);
            values.push_back(v);
        }
        for (auto &param : params) {
            if (!bound.count(param))
                error("cannot infer type parameter " + param + " of " + displayName(base) +
                      "; write " + displayName(base) + "<...>(...)");
            arguments.push_back(type(bound.at(param)));
        }
    }
    auto key = isClass ? instantiateClass(base, arguments) : instantiateFunction(base, arguments);
    Expr call;
    call.kind = "call";
    call.loc = e.loc;
    call.args.push_back(syntheticName(key, e.loc));
    if (open != std::string::npos) {
        for (size_t i = 1; i < e.args.size(); ++i)
            call.args.push_back(clone(*e.args[i]));
        return invoke(call, expected);
    }
    // Arguments were evaluated for inference; pass them through hidden locals.
    scopes.emplace_back();
    for (auto &v : values) {
        auto hidden = "$g" + std::to_string(hiddenCount++);
        bind(hidden, v, true);
        call.args.push_back(syntheticName(hidden, e.loc));
    }
    auto result = invoke(call, expected);
    releaseScopes(scopes.size() - 1);
    scopes.pop_back();
    return result;
}
// The natural function type of a constructor: init parameters, or all fields in order.
Ty Compiler::constructorType(const std::string &className) {
    std::string t = "function<" + className;
    if (className == "desktop.window")
        return Ty{t + ",string,i64,i64>"};
    auto c = classes.find(className);
    if (c == classes.end() || c->second->isInterface)
        error("'" + displayName(className) + "' cannot be used as a constructor");
    auto init = resolveMethod(className, "init");
    if (!init.empty())
        for (auto &param : signature(init).ast->params)
            t += "," + type(param.type).name;
    else
        for (auto &f : c->second->fields)
            t += "," + type(f.type).name;
    return Ty{t + ">"};
}
// A constructor used as a function value; trailing parameters with defaults may be omitted.
Val Compiler::constructorValue(const std::string &className, Ty to) {
    auto natural = constructorType(className).parts();
    auto wanted = to.parts();
    bool ok = wanted[0].name == className || (classes.count(nonNull(wanted[0]).name) &&
                                              derives(className, nonNull(wanted[0]).name));
    ok = ok && wanted.size() <= natural.size();
    for (size_t i = 1; ok && i < wanted.size(); ++i)
        ok = wanted[i] == natural[i];
    if (ok && wanted.size() < natural.size() && className != "desktop.window") {
        auto init = resolveMethod(className, "init");
        for (size_t i = wanted.size() - 1; ok && i + 1 < natural.size(); ++i)
            ok = init.empty() ? classes.at(className)->fields[i].value != nullptr
                              : signature(init).ast->params[i].value != nullptr;
    }
    if (!ok)
        error("constructor " + displayName(className) + " does not match type " + to.name);
    auto code = closureCode("new." + to.name + "." + className, to, [&](llvm::Value *, Expr &call) {
        call.args[0] = syntheticName(className, location);
        return invoke(call);
    });
    return {objectCall("fw_closure_new", {frame, code, i64(0)}), to};
}
// receiver.method as a value: a closure holding the receiver that dispatches like a call.
Val Compiler::boundMethod(Val receiver, const std::string &method) {
    auto key = resolveMethod(receiver.t.name, method);
    auto &sig = signature(key);
    if (sig.ast->isPrivate)
        checkAccess(key.substr(0, key.size() - method.size() - 1), "method", method);
    std::string t = "function<" + sig.result.name;
    for (auto &param : sig.ast->params)
        t += "," + type(param.type).name;
    Ty function{t + ">"};
    auto className = receiver.t.name;
    auto code = closureCode("method." + className + "." + method, function,
                            [&](llvm::Value *env, Expr &call) {
                                bind("$self", get({env, Ty{"Error"}}, i64(0), Ty{className}));
                                auto callee = std::make_unique<Expr>();
                                callee->kind = "member";
                                callee->text = method;
                                callee->loc = location;
                                callee->args.push_back(syntheticName("$self", location));
                                call.args[0] = std::move(callee);
                                return invoke(call);
                            });
    Val closure{objectCall("fw_closure_new", {frame, code, i64(1)}), function};
    set(closure, i64(0), receiver);
    return closure;
}
Val Compiler::typeTest(Expr &e) {
    auto x = value(*e.args[0]);
    auto t = type(e.text);
    auto base = nonNull(x.t);
    auto target = nonNull(t);
    bool classValue = classes.count(base.name) > 0, classTarget = classes.count(target.name) > 0;
    if (e.kind == "is") {
        if (!classTarget) {
            // Non-class types are known statically; for nullable values `x is T` tests presence.
            if (x.t.nullable() && base == t)
                return {b.CreateICmpNE(x.v, llvm::ConstantPointerNull::get(b.getPtrTy())),
                        Ty{"bool"}};
            return {b.getInt1(x.t == t), Ty{"bool"}};
        }
        if (!classValue)
            return {b.getFalse(), Ty{"bool"}};
        if (derives(base.name, target.name) && !x.t.nullable())
            return {b.getTrue(), Ty{"bool"}};
        return {isInstance(x.v, target.name), Ty{"bool"}};
    }
    if (!classTarget || !classValue)
        error("'as' casts between classes and interfaces; use " + displayName(target.name) +
              "(value) for other conversions");
    if (derives(base.name, target.name))
        return convert(x, t);
    if (!derives(target.name, base.name))
        error("cannot cast " + displayName(x.t.name) + " to unrelated type " +
              displayName(t.name));
    auto ok = isInstance(x.v, target.name);
    if (t.nullable())
        return {b.CreateSelect(ok, x.v, llvm::ConstantPointerNull::get(b.getPtrTy())), t};
    guard(ok, "cannot cast " + displayName(x.t.name) + " to " + displayName(t.name), "CastError");
    return {x.v, t};
}
// Calls `method` on a receiver of static class/interface type. Unless `direct`, calls dispatch
// on the receiver's runtime class when subclasses or implementations differ.
Val Compiler::callMethod(Val receiver, const std::string &staticType, const std::string &method,
                         Expr &e, bool direct) {
    auto key = resolveMethod(staticType, method);
    if (key.empty())
        error("type " + displayName(staticType) + " has no method '" + method + "'");
    auto &sig = signature(key);
    if (sig.ast->isPrivate)
        checkAccess(key.substr(0, key.size() - method.size() - 1), "method", method);
    size_t supplied = e.args.size() - 1;
    if (supplied > sig.ast->params.size())
        error("too many arguments to " + displayName(key));
    std::vector<llvm::Value *> actual{receiver.v};
    for (size_t i = 0; i < sig.ast->params.size(); ++i) {
        auto &param = sig.ast->params[i];
        Ty t = type(param.type);
        Val v;
        if (i < supplied)
            v = value(*e.args[i + 1], t);
        else if (param.value)
            v = defaultValue(*param.value, t);
        else
            error("missing argument '" + param.name + "'");
        actual.push_back(convert(v, t).v);
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> impls;
    if (direct)
        impls.push_back({key, {}});
    else
        for (auto &c : concreteClasses(staticType)) {
            auto impl = resolveMethod(c, method);
            auto found = std::find_if(impls.begin(), impls.end(),
                                      [&](auto &entry) { return entry.first == impl; });
            if (found == impls.end())
                impls.push_back({impl, {c}});
            else
                found->second.push_back(c);
        }
    auto invokeImpl = [&](const std::string &impl) -> llvm::Value * {
        auto &target = signature(impl);
        auto result = b.CreateCall(target.function, actual);
        checkError();
        return result;
    };
    llvm::Value *result = nullptr;
    if (impls.empty()) {
        // No class implements the interface method, so no receiver can reach this call.
        call("fw_panic", b.getVoidTy(),
             {b.CreateGlobalString("no implementation of " + displayName(key)),
              b.CreateGlobalString("RuntimeError")});
        result = sig.result.name == "void" ? nullptr : llvm::Constant::getNullValue(ll(sig.result));
    } else if (impls.size() == 1)
        result = invokeImpl(impls[0].first);
    else {
        auto id = call("fw_class_id", b.getPtrTy(), {receiver.v});
        auto merge = llvm::BasicBlock::Create(ctx, "dispatch.end", current);
        std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>> results;
        for (size_t i = 0; i < impls.size(); ++i) {
            auto body = llvm::BasicBlock::Create(ctx, "dispatch.call", current);
            auto next = llvm::BasicBlock::Create(ctx, "dispatch.next", current);
            if (i + 1 == impls.size())
                b.CreateBr(body);
            else {
                llvm::Value *match = b.getFalse();
                for (auto &c : impls[i].second)
                    match = b.CreateOr(match, b.CreateICmpEQ(id, classId(c)));
                b.CreateCondBr(match, body, next);
            }
            b.SetInsertPoint(body);
            auto value = invokeImpl(impls[i].first);
            results.emplace_back(b.GetInsertBlock(), value);
            b.CreateBr(merge);
            b.SetInsertPoint(next);
        }
        b.CreateUnreachable();
        b.SetInsertPoint(merge);
        if (sig.result.name != "void") {
            auto phi = b.CreatePHI(ll(sig.result), static_cast<unsigned>(results.size()));
            for (auto &[block, value] : results)
                phi->addIncoming(value, block);
            result = phi;
        }
    }
    if (sig.result.name == "void")
        return {};
    if (sig.result.object())
        objectCall("fw_adopt", {frame, result});
    return {result, sig.result};
}
// Checks overrides and interface conformance, and definite initialization in init methods.
void Compiler::validateClasses() {
    auto same = [&](const std::string &a, const std::string &b) {
        auto &x = signature(a);
        auto &y = signature(b);
        if (x.result != y.result || x.ast->params.size() != y.ast->params.size())
            return false;
        for (size_t i = 0; i < x.ast->params.size(); ++i)
            if (type(x.ast->params[i].type) != type(y.ast->params[i].type))
                return false;
        return true;
    };
    std::vector<std::pair<std::string, Class *>> all(classes.begin(), classes.end());
    for (auto &[name, c] : all) {
        location = c->loc;
        for (auto &m : c->methods) {
            if (m.name == "init" || c->base.empty())
                continue;
            auto inherited = resolveMethod(c->base, m.name);
            location = m.loc;
            if (!inherited.empty() && !same(name + "." + m.name, inherited))
                error("method '" + m.name + "' overrides " + displayName(inherited) +
                      " with a different signature");
        }
        if (c->isInterface)
            continue;
        for (auto &[other, i] : all) {
            if (!i->isInterface || !derives(name, other))
                continue;
            for (auto &m : i->methods) {
                auto impl = resolveMethod(name, m.name);
                location = c->loc;
                if (impl.empty() || signature(impl).ast->abstract)
                    error("class " + displayName(name) + " must implement " +
                          displayName(other) + "." + m.name);
                if (!same(impl, other + "." + m.name))
                    error(displayName(impl) + " does not match the signature of " +
                          displayName(other) + "." + m.name);
            }
        }
        if (functions.count(name + ".init"))
            checkInit(*c, *functions.at(name + ".init").ast);
        else if (!c->base.empty() && !resolveMethod(c->base, "init").empty()) {
            auto &base = *classes.at(c->base);
            for (size_t i = base.fields.size(); i < c->fields.size(); ++i)
                if (!c->fields[i].value)
                    error("class " + displayName(name) + " adds required field '" +
                          c->fields[i].name + "', so it must define init");
        }
    }
}
// Required fields (no default) must be assigned by top-level statements of init, or by a
// top-level super.init(...), before init reads `inst` or can return.
void Compiler::checkInit(Class &c, Function &init) {
    location = init.loc;
    std::vector<std::string> required;
    for (auto &f : c.fields)
        if (!f.value)
            required.push_back(f.name);
    std::set<std::string> assigned;
    std::function<bool(Expr &)> usesInst = [&](Expr &e) {
        if (e.kind == "inst" || e.kind == "super")
            return true;
        for (auto &a : e.args)
            if (a && usesInst(*a))
                return true;
        if (e.lambda)
            for (auto &st : e.lambda->body)
                if (st->expr && usesInst(*st->expr))
                    return true;
        return false;
    };
    std::function<bool(Stmt &, bool)> scan = [&](Stmt &st, bool returns) {
        if (returns && st.kind == "return")
            return true;
        if ((st.expr && usesInst(*st.expr)) || (st.target && usesInst(*st.target)))
            return true;
        for (auto *body : {&st.body, &st.other, &st.finalizer})
            for (auto &inner : *body)
                if (scan(*inner, returns))
                    return true;
        for (auto &body : st.catches)
            for (auto &inner : body)
                if (scan(*inner, returns))
                    return true;
        return false;
    };
    auto missing = [&]() -> std::string {
        for (auto &f : required)
            if (!assigned.count(f))
                return f;
        return "";
    };
    for (auto &st : init.body) {
        location = st->loc;
        auto pending = missing();
        if (pending.empty())
            break;
        if (st->kind == "assign" && st->op == "=" && st->target->kind == "inst") {
            if (usesInst(*st->expr))
                error("init reads 'inst' before assigning required field '" + pending + "'");
            assigned.insert(st->target->text);
            continue;
        }
        auto superInit = [&](Expr &target) {
            Expr *callee = target.kind == "call" ? target.args[0].get() : &target;
            return callee->kind == "member" && callee->text == "init" &&
                   callee->args[0]->kind == "super";
        };
        if (st->kind == "expr" && superInit(*st->target)) {
            for (size_t i = 1; i < st->target->args.size(); ++i)
                if (usesInst(*st->target->args[i]))
                    error("super.init arguments cannot read 'inst'");
            for (auto &f : classes.at(c.base)->fields)
                assigned.insert(f.name);
            continue;
        }
        if (scan(*st, false))
            error("init uses 'inst' before assigning required field '" + pending + "'");
        if (scan(*st, true))
            error("init can return before assigning required field '" + pending + "'");
    }
    location = init.loc;
    if (auto pending = missing(); !pending.empty())
        error("init must assign required field '" + pending + "'");
}
// for x in object: object.iterator() (or the object itself) provides next() returning T?;
// the loop ends when next() returns null.
void Compiler::iterate(Stmt &s, Val source) {
    if (!s.op.empty())
        error("iterator loops bind one name");
    auto iterator = resolveMethod(source.t.name, "iterator").empty()
                        ? source
                        : callNamedMethod(source, "iterator", {});
    if (!classes.count(iterator.t.name) || resolveMethod(iterator.t.name, "next").empty())
        error("for over " + displayName(source.t.name) +
              " requires iterator() or next() returning a nullable value");
    scopes.emplace_back();
    bind("$iterator", iterator, true);
    auto cond = llvm::BasicBlock::Create(ctx, "iterate.next", current);
    auto body = llvm::BasicBlock::Create(ctx, "iterate.body", current);
    auto end = llvm::BasicBlock::Create(ctx, "iterate.end", current);
    b.CreateBr(cond);
    b.SetInsertPoint(cond);
    call("fw_sweep", b.getVoidTy(), {frame});
    auto item = callNamedMethod(load(*lookup("$iterator")), "next", {});
    if (!item.t.nullable() || item.t.name == "null")
        error("next() must return a nullable value such as T?; null ends the loop");
    b.CreateCondBr(b.CreateICmpNE(item.v, llvm::ConstantPointerNull::get(b.getPtrTy())), body,
                   end);
    b.SetInsertPoint(body);
    auto loopDepth = scopes.size();
    scopes.emplace_back();
    bind(s.name, unboxChecked(item));
    loops.emplace_back(end, cond);
    loopScopes.push_back(loopDepth);
    block(s.body, false);
    loops.pop_back();
    loopScopes.pop_back();
    if (!terminated()) {
        releaseScopes(loopDepth);
        b.CreateBr(cond);
    }
    scopes.pop_back();
    b.SetInsertPoint(end);
    releaseScopes(scopes.size() - 1);
    scopes.pop_back();
}
// A pattern that unpacks: `[x, y]` or `Shape.Circle(r)`.
bool Compiler::destructuring(Expr &p) {
    return p.kind == "array" ||
           (p.kind == "call" && p.args[0]->kind == "name" && classes.count(p.args[0]->text) &&
            !classes.at(p.args[0]->text)->variantOf.empty());
}
// True when a destructuring pattern matches every value of its shape: it only binds names.
bool Compiler::irrefutable(Expr &p, Ty subject) {
    for (size_t i = p.kind == "call" ? 1 : 0; i < p.args.size(); ++i) {
        auto &a = *p.args[i];
        if (a.kind == "bind")
            continue;
        if (destructuring(a) && a.kind == "array" && irrefutable(a, Ty{}))
            continue;
        return false;
    }
    // Arrays of unknown length can fail the length check; tuples cannot.
    return p.kind == "call" || nonNull(subject).tuple();
}
// Emits the tests of a destructuring pattern, branching to `fail` on a mismatch, and collects
// the names it binds. Code continues in the success path.
void Compiler::destructure(Expr &p, Val subject, llvm::BasicBlock *fail,
                           std::vector<std::pair<std::string, Val>> &bindings) {
    location = p.loc;
    auto check = [&](llvm::Value *condition) {
        auto pass = llvm::BasicBlock::Create(ctx, "pattern.pass", current);
        b.CreateCondBr(condition, pass, fail);
        b.SetInsertPoint(pass);
    };
    if (subject.t.nullable()) {
        if (subject.t.name == "null")
            error("a destructuring pattern cannot match null");
        check(b.CreateICmpNE(subject.v, llvm::ConstantPointerNull::get(b.getPtrTy())));
        subject = unboxChecked(subject);
    }
    std::vector<Val> parts;
    size_t first = 0;
    if (p.kind == "call") {
        auto variant = p.args[0]->text;
        auto &c = *classes.at(variant);
        if (!classes.count(subject.t.name) ||
            (!derives(variant, subject.t.name) && !derives(subject.t.name, variant)))
            error(displayName(variant) + " can never match " + displayName(subject.t.name));
        if (p.args.size() - 1 != c.fields.size())
            error(displayName(variant) + " has " + std::to_string(c.fields.size()) +
                  " field(s); the pattern lists " + std::to_string(p.args.size() - 1));
        if (variant != subject.t.name)
            check(isInstance(subject.v, variant));
        Val typed{subject.v, Ty{variant}};
        for (size_t i = 0; i < c.fields.size(); ++i)
            parts.push_back(get(typed, i64(static_cast<int64_t>(i)), type(c.fields[i].type)));
        first = 1;
    } else {
        auto count = static_cast<int64_t>(p.args.size());
        if (subject.t.tuple()) {
            auto types = subject.t.parts();
            if (static_cast<int64_t>(types.size()) != count)
                error("a tuple of " + std::to_string(types.size()) +
                      " elements never matches a pattern of " + std::to_string(count));
            for (int64_t i = 0; i < count; ++i)
                parts.push_back(get(subject, i64(i), types[i]));
        } else if (subject.t.sequence()) {
            check(b.CreateICmpEQ(call("fw_length", b.getInt64Ty(), {subject.v}), i64(count)));
            for (int64_t i = 0; i < count; ++i)
                parts.push_back(get(subject, i64(i), subject.t.item()));
        } else
            error("an array pattern needs a tuple or array, not " + displayName(subject.t.name));
    }
    for (size_t i = first; i < p.args.size(); ++i) {
        auto &a = *p.args[i];
        auto part = parts[i - first];
        if (a.kind == "bind") {
            if (a.text != "_")
                bindings.push_back({a.text, part});
        } else if (destructuring(a))
            destructure(a, part, fail, bindings);
        else {
            // A value to compare, or a member of an enum with data without fields.
            if (a.kind == "name" && classes.count(a.text) && !classes.at(a.text)->variantOf.empty()) {
                if (!classes.count(nonNull(part.t).name))
                    error(displayName(a.text) + " can never match " + displayName(part.t.name));
                if (part.t.nullable())
                    check(b.CreateICmpNE(part.v, llvm::ConstantPointerNull::get(b.getPtrTy())));
                check(isInstance(part.v, a.text));
                continue;
            }
            auto expected = value(a, nonNull(part.t));
            check(binary("==", part, expected).v);
        }
    }
}
void Compiler::match(Stmt &s) {
    auto subject = value(*s.expr);
    if (subject.t.name == "void" || subject.t.name == "null" ||
        subject.t.name.rfind("module:", 0) == 0)
        error("match requires a value");
    auto base = nonNull(subject.t);
    size_t outer = scopes.size();
    scopes.emplace_back();
    bind("$match" + std::to_string(outer), subject, true);
    auto end = llvm::BasicBlock::Create(ctx, "match.end", current);
    bool reachesEnd = false;
    std::set<std::string> covered;
    auto enumMember = [&](Expr &v) -> std::string {
        if (!base.enumeration())
            return "";
        auto &members = enums.at(base.name)->members;
        if (v.kind == "member" && v.args[0]->kind == "name" && v.args[0]->text == base.name)
            return v.text;
        if (v.kind == "name" && !lookup(v.text) &&
            std::find(members.begin(), members.end(), v.text) != members.end())
            return v.text;
        return "";
    };
    for (size_t i = 0; i < s.catches.size(); ++i) {
        location = s.loc;
        auto matched = llvm::BasicBlock::Create(ctx, "match.case", current);
        auto next = llvm::BasicBlock::Create(ctx, "match.next", current);
        auto typeName = s.catchTypes[i];
        auto &values = s.patterns[i];
        bool unguarded = !s.guards[i];
        if (typeName.empty() && values.size() == 1 &&
            (values[0]->kind == "name" || values[0]->kind == "member")) {
            auto name = values[0]->kind == "name" ? values[0]->text : callable(*values[0]);
            if (classes.count(name) && !lookup(name))
                typeName = name;
        }
        Val binding;
        std::vector<std::pair<std::string, Val>> bindings;
        if (typeName.empty() && values.size() == 1 && destructuring(*values[0])) {
            if (unguarded && irrefutable(*values[0], base))
                for (auto &c : concreteClasses(values[0]->kind == "call"
                                                   ? values[0]->args[0]->text
                                                   : base.name))
                    covered.insert(values[0]->kind == "call" ? "class:" + c : "all");
            if (unguarded && irrefutable(*values[0], base) && values[0]->kind == "array")
                covered.insert("all");
            destructure(*values[0], subject, next, bindings);
            b.CreateBr(matched);
            b.SetInsertPoint(matched);
        } else if (!typeName.empty()) {
            llvm::Value *condition = nullptr;
            if ((typeName == "ok" || typeName == "error") && subject.t.result()) {
                auto ok = get(subject, i64(0), Ty{"bool"});
                condition = typeName == "ok" ? ok.v : b.CreateNot(ok.v);
                b.CreateCondBr(condition, matched, next);
                b.SetInsertPoint(matched);
                binding = get(subject, i64(1), subject.t.parts()[typeName == "ok" ? 0 : 1]);
                if (unguarded)
                    covered.insert(typeName);
            } else {
                auto t = type(typeName);
                if (classes.count(t.name)) {
                    if (!classes.count(base.name))
                        error("type pattern " + displayName(t.name) +
                              " requires a class or interface value");
                    if (!derives(t.name, base.name) && !derives(base.name, t.name))
                        error(displayName(t.name) + " can never match " +
                              displayName(subject.t.name));
                    b.CreateCondBr(isInstance(subject.v, t.name), matched, next);
                    b.SetInsertPoint(matched);
                    binding = {subject.v, t};
                    if (unguarded)
                        for (auto &c : concreteClasses(t.name))
                            covered.insert("class:" + c);
                } else if (subject.t.nullable() && t == base) {
                    b.CreateCondBr(
                        b.CreateICmpNE(subject.v, llvm::ConstantPointerNull::get(b.getPtrTy())),
                        matched, next);
                    b.SetInsertPoint(matched);
                    binding = unboxChecked(subject);
                    if (unguarded)
                        covered.insert("present");
                } else if (t == subject.t) {
                    b.CreateBr(matched);
                    b.SetInsertPoint(matched);
                    binding = subject;
                    if (unguarded)
                        covered.insert("all");
                } else
                    error("pattern type " + displayName(t.name) + " does not match " +
                          displayName(subject.t.name));
            }
        } else {
            for (size_t k = 0; k < values.size(); ++k) {
                auto &v = *values[k];
                location = v.loc;
                llvm::Value *condition;
                if (v.kind == "null") {
                    if (!subject.t.nullable())
                        error("null pattern requires a nullable value");
                    condition =
                        b.CreateICmpEQ(subject.v, llvm::ConstantPointerNull::get(b.getPtrTy()));
                    if (unguarded)
                        covered.insert("null");
                } else if (v.kind == "binary" && !v.grouped && (v.text == ".." || v.text == "..<") &&
                           (base.numeric() || base.name == "char")) {
                    auto x = subject.t.nullable() ? unboxChecked(subject) : subject;
                    auto low = binary("<=", value(*v.args[0], base), x);
                    auto high = binary(v.text == ".." ? "<=" : "<", x, value(*v.args[1], base));
                    condition = b.CreateAnd(low.v, high.v);
                } else if (auto member = enumMember(v); !member.empty()) {
                    auto &members = enums.at(base.name)->members;
                    auto index = std::find(members.begin(), members.end(), member) - members.begin();
                    condition = binary("==", subject, {i64(index), base}).v;
                    if (unguarded)
                        covered.insert(member);
                } else {
                    auto pattern = value(v, base);
                    condition = binary("==", subject, pattern).v;
                    if (unguarded && v.kind == "bool")
                        covered.insert(v.text);
                }
                auto alternative = k + 1 == values.size()
                                       ? next
                                       : llvm::BasicBlock::Create(ctx, "match.value", current);
                b.CreateCondBr(condition, matched, alternative);
                b.SetInsertPoint(alternative);
            }
            b.SetInsertPoint(matched);
        }
        size_t depth = scopes.size();
        scopes.emplace_back();
        if (!s.catchNames[i].empty())
            bind(s.catchNames[i], binding);
        for (auto &[name, v] : bindings)
            bind(name, v);
        if (s.guards[i]) {
            auto guardValue = value(*s.guards[i]);
            if (guardValue.t.name != "bool")
                error("case guard must be bool");
            auto body = llvm::BasicBlock::Create(ctx, "match.guarded", current);
            auto rejected = llvm::BasicBlock::Create(ctx, "match.rejected", current);
            b.CreateCondBr(guardValue.v, body, rejected);
            b.SetInsertPoint(rejected);
            releaseScopes(depth);
            b.CreateBr(next);
            b.SetInsertPoint(body);
        }
        block(s.catches[i], false);
        if (!terminated()) {
            releaseScopes(depth);
            b.CreateBr(end);
            reachesEnd = true;
        }
        scopes.pop_back();
        b.SetInsertPoint(next);
    }
    location = s.loc;
    if (s.op == "else") {
        block(s.other);
        if (!terminated()) {
            b.CreateBr(end);
            reachesEnd = true;
        }
    } else {
        std::vector<std::string> missing;
        bool exhaustive = covered.count("all") > 0;
        if (!exhaustive && subject.t.nullable() && !covered.count("null"))
            missing.push_back("null");
        if (!exhaustive && subject.t.nullable() && covered.count("present"))
            exhaustive = missing.empty();
        if (!exhaustive) {
            if (base.enumeration()) {
                for (auto &m : enums.at(base.name)->members)
                    if (!covered.count(m))
                        missing.push_back(m);
            } else if (base.name == "bool") {
                for (auto v : {"true", "false"})
                    if (!covered.count(v))
                        missing.push_back(v);
            } else if (base.result()) {
                for (auto v : {"ok", "error"})
                    if (!covered.count(v))
                        missing.push_back(v);
            } else if (classes.count(base.name)) {
                for (auto &c : concreteClasses(base.name))
                    if (!covered.count("class:" + c))
                        missing.push_back(displayName(c));
            } else
                missing.push_back("other values");
        }
        if (!exhaustive && !missing.empty()) {
            std::string list;
            for (auto &m : missing)
                list += (list.empty() ? "" : ", ") + m;
            error("match is not exhaustive (missing " + list + "); add an else branch");
        }
        b.CreateUnreachable();
    }
    b.SetInsertPoint(end);
    if (reachesEnd)
        releaseScopes(outer);
    else
        b.CreateUnreachable();
    scopes.pop_back();
}
Val Compiler::lambda(Expr &e, Ty expected) {
    auto &f = *e.lambda;
    location = e.loc;
    std::vector<Ty> params;
    for (auto &param : f.params) {
        auto t = type(param.type);
        if (t.name == "void")
            error("lambda parameters cannot be void");
        params.push_back(t);
    }
    Ty result;
    if (!f.result.empty())
        result = type(f.result);
    else if (expected.function() && expected.parts().size() == params.size() + 1)
        result = expected.parts()[0];
    else {
        LambdaContext probe;
        result = lambdaBody(f, params, Ty{}, true, probe);
    }
    LambdaContext context;
    lambdaBody(f, params, result, false, context);
    std::string name = "function<" + result.name;
    for (auto &param : params)
        name += "," + param.name;
    Val closure{objectCall("fw_closure_new", {frame, context.function,
                                              i64(static_cast<int64_t>(context.captures.size()))}),
                Ty{name + ">"}};
    for (size_t i = 0; i < context.captures.size(); ++i) {
        auto &captured = context.captures[i].second;
        set(closure, i64(static_cast<int64_t>(i)),
            captured.cell ? Val{b.CreateLoad(b.getPtrTy(), captured.address), Ty{"Error"}}
                          : load(captured));
    }
    return closure;
}
Ty Compiler::lambdaBody(Function &f, const std::vector<Ty> &params, Ty result, bool inference,
                        LambdaContext &context) {
    auto saved = saveFunction();
    context.outerScopes = &saved.scopes;
    lambdaStack = saved.lambdaStack;
    lambdaStack.push_back(&context);
    top = false;
    infer = inference;
    returnType = inference ? Ty{"void"} : result;
    scopes.emplace_back();
    std::vector<llvm::Type *> types{b.getPtrTy()};
    for (auto &param : params)
        types.push_back(ll(param));
    current = llvm::Function::Create(
        llvm::FunctionType::get(inference ? b.getVoidTy() : ll(result), types, false),
        llvm::GlobalValue::InternalLinkage, "fly.lambda." + std::to_string(lambdaCount++), module);
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", current));
    location = f.loc;
    if (!inference)
        debugFunction(current, "lambda", f.loc);
    frame = objectCall("fw_frame", {});
    auto body = llvm::BasicBlock::Create(ctx, "body", current);
    context.initPoint = b.CreateBr(body);
    context.function = current;
    context.frame = frame;
    context.env = current->getArg(0);
    b.SetInsertPoint(body);
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
    cells = cellNames(f.body);
    for (size_t i = 0; i < params.size(); ++i) {
        location = f.params[i].loc;
        bind(f.params[i].name, {current->getArg(static_cast<unsigned>(i + 1)), params[i]});
    }
    block(f.body, false);
    bool fallsThrough = !terminated();
    if (fallsThrough) {
        if (!inference && result.name != "void")
            error("non-void lambda must return on all paths");
        call("fw_drop", b.getVoidTy(), {frame});
        b.CreateRetVoid();
    }
    emitFailure();
    if (inference) {
        result = inferredResult(fallsThrough);
        current->eraseFromParent();
        context.function = nullptr;
    }
    restoreFunction(saved);
    return result;
}
} // namespace
void compile(Program &p, const std::string &filename, const Options &options) {
    Compiler c(p, filename);
    c.run(options);
}
} // namespace fw
