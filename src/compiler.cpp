#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <llvm/Config/llvm-config.h>
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
    "SelectorError", "InputError",     "ProcessError", "DesktopError"};
struct Ty {
    std::string name = "void";
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
        return !numeric() && name != "bool" && name != "void" && name.rfind("fn:", 0) != 0 &&
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
};
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
    std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> loops;
    std::vector<std::map<std::string, Val>> attrs;
    std::set<std::string> componentStack;
    std::vector<std::string> componentNames;
    std::map<std::string, std::map<std::string, Ty>> componentTypes;
    Ty returnType;
    bool infer = false, top = false;
    std::string owner, desktopBackend;
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
    std::map<std::string, llvm::Function *> trampolines;
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
                        std::move(lambdaStack)};
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
        b.restoreIP(s.ip);
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
        if (t.name == "float")
            return b.getFloatTy();
        if (t.name == "double")
            return b.getDoubleTy();
        if (t.name == "void")
            return b.getVoidTy();
        return b.getPtrTy();
    }
    Ty type(std::string name) {
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
            if (!inner.object())
                error("nullable primitive types are not implemented yet");
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
                if (!key.integer() && key.name != "string" && key.name != "bool")
                    error("map keys must be integers, bool, or string");
            }
            return out;
        }
        if (t.numeric() || name == "bool" || name == "string" || name == "void" ||
            name == "element" || name == "fragment" || name == "json.value" ||
            name == "desktop.window" || errorTypes.count(name) || classes.count(name))
            return t;
        error("unknown or unsupported type '" + name + "'");
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
                if (var.type.object())
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
        for (auto &[alias, name] : p.imports) {
            (void)alias;
            if (name == "web")
                return true;
        }
        return false;
    }
    // web.request and web.response are ordinary classes; runtime/web.inc relies on this
    // field order when it builds requests and reads responses.
    void webClasses() {
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
            "fw_error_value",   "fw_error_take",    "fw_error_restore", "fw_web_shutdown"};
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
        if (x.t.classReference())
            error("constructor reference '" + displayName(x.t.name.substr(6)) +
                  "' is not an instance; call it with () or use the unparenthesized class name on "
                  "the assignment right-hand side");
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
        if (x.t.name.rfind("fn:", 0) == 0 && to.function())
            return functionValue(x.t.name.substr(3), to);
        if (to.weak() && (x.t.name == "null" || x.t.name == to.parts()[0].name ||
                          x.t.name == to.parts()[0].name + "?"))
            return {objectCall("fw_weak_new", {frame, x.v}), to};
        if (x.t.numeric() && to.numeric()) {
            bool fits = false;
            if (auto ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(x.v); ci && to.integer()) {
                auto v = ci->getValue();
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
    // Nullable references are unwrapped for member access; null raises CastError.
    Val unwrap(Val x) {
        if (!x.t.nullable() || x.t.name == "null")
            return x;
        Ty base{x.t.name.substr(0, x.t.name.size() - 1)};
        if (classes.count(base.name) || base.name == "element" || base.name == "fragment" ||
            base.name == "desktop.window")
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
        if (x.t.name == "null")
            return literal("null");
        if (x.t.name.rfind("fn:", 0) == 0)
            return literal("<function " + displayName(x.t.name.substr(3)) + ">");
        if (x.t.classReference())
            return literal("<class " + displayName(x.t.name.substr(6)) + ">");
        if (x.t.name.rfind("module:", 0) == 0)
            return literal("<module " + x.t.name.substr(7) + ">");
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
        auto address = allocate(outer->type, name);
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
        if (t.object())
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
    void bind(const std::string &name, Val v, bool constant = false) {
        if (scopes.back().count(name))
            error("duplicate declaration '" + name + "'");
        auto a = allocate(v.t, name);
        store(a, v);
        scopes.back()[name] = {a, v.t, constant};
    }
    Val load(Var &v) {
        if (v.initialized && !top)
            guard(b.CreateLoad(b.getInt1Ty(), v.initialized),
                  "module global read before initialization", "RuntimeError");
        auto result = b.CreateLoad(ll(v.type), v.address);
        if (v.type.object() && !borrowed)
            return {objectCall("fw_keep", {frame, result}), v.type};
        return {result, v.type};
    }
    std::pair<Class *, size_t> field(Ty t, const std::string &name) {
        auto c = classes.find(t.name);
        if (c == classes.end())
            error("type '" + t.name + "' has no field '" + name + "'");
        for (size_t i = 0; i < c->second->fields.size(); ++i)
            if (c->second->fields[i].name == name)
                return {c->second, i};
        error("unknown field '" + name + "' on " + t.name);
    }
    Val get(Val obj, llvm::Value *index, Ty t) {
        if (t.object()) {
            auto object = objectCall("fw_get_object", {obj.v, index});
            return {borrowed ? object : objectCall("fw_keep", {frame, object}), t};
        }
        if (t.real()) {
            auto x = call("fw_get_real", b.getDoubleTy(), {obj.v, index});
            return {b.CreateFPCast(x, ll(t)), t};
        }
        auto x = call("fw_get_int", b.getInt64Ty(), {obj.v, index});
        return {b.CreateIntCast(x, ll(t), !t.unsigned_()), t};
    }
    void set(Val obj, llvm::Value *index, Val x) {
        if (x.t.classReference())
            error("constructor references cannot be stored in collections or object fields");
        if (x.t.object())
            call("fw_set_object", b.getVoidTy(), {obj.v, index, x.v});
        else if (x.t.real())
            call("fw_set_real", b.getVoidTy(),
                 {obj.v, index, b.CreateFPCast(x.v, b.getDoubleTy())});
        else
            call("fw_set_int", b.getVoidTy(),
                 {obj.v, index,
                  b.CreateIntCast(x.v, b.getInt64Ty(), x.t.name != "bool" && !x.t.unsigned_()),
                  i64(x.t.name == "bool" ? 3
                      : x.t.unsigned_()  ? 1
                                         : 0)});
    }
    Val receiver() {
        auto v = lookup("$inst");
        if (!v)
            error("'inst' is valid only in an instance method");
        return load(*v);
    }
    Val binary(const std::string &op, Val a, Val c) {
        if (a.t.name == "string" && c.t.name == "string") {
            if (op == "+")
                return {objectCall("fw_concat", {frame, a.v, c.v}), a.t};
            if (op == "==" || op == "!=") {
                auto eq = b.CreateICmpNE(call("fw_equal", b.getInt64Ty(), {a.v, c.v}), i64(0));
                return {op == "==" ? eq : b.CreateNot(eq), Ty{"bool"}};
            }
        }
        if ((op == "==" || op == "!=") && a.t.object() && c.t.object()) {
            if (a.t != c.t &&
                !((a.t.name == "null" && c.t.nullable()) || (c.t.name == "null" && a.t.nullable())))
                error("incompatible equality operands");
            auto eq = b.CreateICmpEQ(a.v, c.v);
            return {op == "==" ? eq : b.CreateNot(eq), Ty{"bool"}};
        }
        if (a.t.name == "bool" && c.t.name == "bool" && (op == "==" || op == "!="))
            return {op == "==" ? b.CreateICmpEQ(a.v, c.v) : b.CreateICmpNE(a.v, c.v), Ty{"bool"}};
        if (!a.t.numeric() || !c.t.numeric())
            error("operator '" + op + "' requires compatible numeric operands");
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
            error("'//' requires integer operands");
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
        return {
            objectCall("fw_instance",
                       {frame, i64(c.fields.size()), b.CreateGlobalString(c.name),
                        b.CreateGlobalString(displayName(c.name)), b.CreateGlobalString(fields)}),
            Ty{c.name}};
    }
    Val collectionCast(Val source, Ty target);
    Ty inferCollectionCast(const std::string &constructor, Ty source);
    Val explicitCast(Val source, Ty target);
    std::optional<Val> domCall(const std::string &name, std::vector<Val> args);
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
        auto integer = key.t.object() ? i64(0)
                                      : b.CreateIntCast(key.v, b.getInt64Ty(),
                                                        key.t.name != "bool" && !key.t.unsigned_());
        auto object = key.t.object() ? key.v : llvm::ConstantPointerNull::get(b.getPtrTy());
        return call("fw_map_index", b.getInt64Ty(), {map.v, integer, object, i64(insert ? 1 : 0)});
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
    Ty lambdaBody(Function &f, const std::vector<Ty> &params, Ty result, bool inference,
                  LambdaContext &context);
    Val sequenceMethod(Val sequence, const std::string &method, Expr &call);
    void declareFunction(Function &f) {
        std::string key = f.owner.empty() ? f.name : f.owner + "." + f.name;
        if (functions.count(key) || (!f.owner.empty() && f.name == "init"))
            error(f.name == "init" ? "custom init constructors are not implemented yet"
                                   : "duplicate function '" + key + "'");
        Signature sig;
        sig.ast = &f;
        sig.result = f.result.empty() ? Ty{"$infer"} : type(f.result);
        if (sig.result.name != "$infer") {
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
        if (usesWeb())
            webClasses();
        for (auto &c : p.classes) {
            location = c.loc;
            if (classes.count(c.name))
                error("duplicate class");
            classes[c.name] = &c;
            std::set<std::string> fields;
            for (auto &f : c.fields)
                if (!fields.insert(f.name).second)
                    error("duplicate field");
        }
        static const std::set<std::string> modules = {"desktop", "html", "math",       "fs",
                                                      "os",      "json", "web",        "time",
                                                      "testing", "core", "collections"};
        for (auto &[alias, name] : p.imports) {
            if (!modules.count(name))
                error("module '" + name +
                      "' is not implemented yet (local modules are not supported)");
            (void)alias;
        }
        for (auto &f : p.functions)
            declareFunction(f);
        for (auto &c : p.classes)
            for (auto &f : c.methods)
                declareFunction(f);
    }
    void run(const Options &opts) {
        desktopBackend = opts.desktopBackend;
        current = llvm::Function::Create(llvm::FunctionType::get(b.getInt32Ty(), false),
                                         llvm::GlobalValue::ExternalLinkage, "main", module);
        auto entry = llvm::BasicBlock::Create(ctx, "entry", current);
        b.SetInsertPoint(entry);
        frame = objectCall("fw_frame", {});
        failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
        scopes.emplace_back();
        top = true;
        returnType = Ty{"void"};
        block(p.body, false);
        if (!terminated()) {
            if (usesWeb()) {
                call("fw_web_wait", b.getVoidTy(), {});
                call("fw_web_shutdown", b.getVoidTy(), {});
            }
            call("fw_drop", b.getVoidTy(), {frame});
            b.CreateRet(b.getInt32(0));
        }
        emitFailure();
        top = false;
        for (auto &[name, sig] : functions) {
            (void)name;
            if (sig.result.name == "$infer")
                buildFunction(sig, true);
            if (!sig.done)
                buildFunction(sig, false);
        }
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
    if ((source.t.numeric() || source.t.name == "bool") && target.numeric())
        return convert(source, target, true);
    return collectionCast(source, target);
}
Val Compiler::value(Expr &e, Ty expected) {
    location = e.loc;
    if (e.kind == "name" || e.kind == "member") {
        auto name = callable(e);
        bool referenceVariable = false;
        if (e.kind == "name")
            if (auto variable = lookup(name)) {
                referenceVariable = variable->type.classReference();
                if (referenceVariable) {
                    // Check global initialization even though the constructor identity is static.
                    auto reference = load(*variable);
                    if (e.grouped || !e.outer)
                        return reference;
                    name = variable->type.name.substr(6);
                } else
                    name.clear();
            }
        if (name == "desktop.Window")
            name = "desktop.window";
        if (referenceVariable || classes.count(name) || name == "desktop.window") {
            if (e.outer && !e.grouped) {
                Expr call;
                call.kind = "call";
                call.loc = e.loc;
                auto callee = std::make_unique<Expr>();
                callee->kind = "name";
                callee->text = name;
                callee->loc = e.loc;
                call.args.push_back(std::move(callee));
                return invoke(call, expected);
            }
            // Constructor identity lives in the static type; this payload is never ARC-managed.
            return {llvm::ConstantPointerNull::get(b.getPtrTy()), Ty{"class:" + name}};
        }
    }
    if (e.kind == "int") {
        try {
            size_t used;
            auto n = std::stoll(e.text, &used);
            if (used != e.text.size())
                error("invalid integer literal");
            return {i64(n), Ty{"i64"}};
        } catch (const std::exception &) {
            error("integer literal exceeds int range");
        }
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
    if (e.kind == "null")
        return {llvm::ConstantPointerNull::get(b.getPtrTy()), Ty{"null"}};
    if (e.kind == "string")
        return literal(e.text);
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
        auto [c, i] = field(obj.t, e.text);
        return get(obj, i64(i), type(c->fields[i].type));
    }
    if (e.kind == "member") {
        auto obj = unwrap(value(*e.args[0]));
        if (obj.t.name == "module:math" && e.text == "PI")
            return {llvm::ConstantFP::get(b.getDoubleTy(), 3.141592653589793), Ty{"double"}};
        if (obj.t.name == "module:desktop")
            error("module 'desktop' has no member '" + e.text +
                  "'; use desktop.window or desktop.Window");
        if (obj.t.result() && e.text == "is_ok")
            return get(obj, i64(0), Ty{"bool"});
        if ((obj.t.name == "string" || obj.t.sequence() || obj.t.map() || obj.t.tuple()) &&
            e.text == "length")
            return {call("fw_length", b.getInt64Ty(), {obj.v}), Ty{"i64"}};
        if (obj.t.name == "element" || obj.t.name == "fragment") {
            if (e.text == "tag")
                return {objectCall("fw_tag", {frame, obj.v}), Ty{"string"}};
            if (e.text == "attributes" || e.text == "children" || e.text == "text_content" ||
                e.text == "inner_html" || e.text == "outer_html" || e.text == "id" ||
                e.text == "class_name")
                return *domCall("html." + e.text, {obj});
        }
        if (errorTypes.count(obj.t.name) && e.text == "message")
            return {objectCall("fw_error_message", {frame, obj.v}), Ty{"string"}};
        auto [c, i] = field(obj.t, e.text);
        return get(obj, i64(i), type(c->fields[i].type));
    }
    if (e.kind == "index") {
        auto obj = value(*e.args[0]);
        auto [index, t] = indexOf(obj, *e.args[1], false);
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
        if (items.empty() && !expected.sequence() && !expected.tuple())
            error("empty arrays require an expected array type");
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
        Val out{objectCall("fw_map_new", {frame, i64(key.object()         ? 4
                                                     : key.name == "bool" ? 3
                                                     : key.unsigned_()    ? 1
                                                                          : 0)}),
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
        if (!a.t.numeric())
            error("unary operator requires a number");
        if (e.text == "+")
            return a;
        if (a.t.real())
            return {b.CreateFNeg(a.v), a.t};
        return binary("-", {llvm::ConstantInt::get(ll(a.t), 0), a.t}, a);
    }
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
            auto c = value(*e.args[1]);
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
            if (c.t.map()) {
                auto key = convert(a, c.t.parts()[0]);
                auto integer = key.t.object()
                                   ? i64(0)
                                   : b.CreateIntCast(key.v, b.getInt64Ty(),
                                                     key.t.name != "bool" && !key.t.unsigned_());
                auto object = key.t.object() ? key.v : llvm::ConstantPointerNull::get(b.getPtrTy());
                return {b.CreateICmpNE(call("fw_map_has", b.getInt64Ty(), {c.v, integer, object}),
                                       i64(0)),
                        Ty{"bool"}};
            }
            if (!c.t.sequence())
                error("membership requires an array, homogeneous tuple, or map");
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
    if (e.kind == "element") {
        if (p.components.count(e.text)) {
            if (!componentStack.insert(e.text).second)
                error("recursive component expansion is not supported");
            std::map<std::string, Val> supplied;
            for (size_t i = 0; i < e.names.size(); ++i)
                supplied[e.names[i]] = value(*e.args[i]);
            auto empty = literal("");
            Val inner{objectCall("fw_element", {frame, empty.v}), Ty{"fragment"}};
            for (size_t i = e.names.size(); i < e.args.size(); ++i) {
                auto child = value(*e.args[i]);
                if (child.t.name != "element" && child.t.name != "fragment")
                    child = stringify(child);
                call("fw_child", b.getVoidTy(), {inner.v, child.v});
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
        static const std::set<std::string> tags = {
            "html",  "head",    "title",    "body",       "div",    "span",  "h1",     "h2",
            "h3",    "h4",      "h5",       "h6",         "p",      "a",     "button", "input",
            "label", "form",    "textarea", "select",     "option", "ul",    "ol",     "li",
            "table", "thead",   "tbody",    "tr",         "td",     "th",    "br",     "hr",
            "img",   "meta",    "link",     "script",     "style",  "main",  "header", "footer",
            "nav",   "section", "article",  "aside",      "strong", "em",    "b",      "i",
            "code",  "pre",     "small",    "blockquote", "video",  "audio", "source"};
        if (!e.text.empty() && (!p.html || !tags.count(e.text)))
            error("unknown tag '" + e.text + "'");
        auto tag = literal(e.text);
        Val tree{objectCall("fw_element", {frame, tag.v}),
                 Ty{e.text.empty() ? "fragment" : "element"}};
        static const std::set<std::string> global = {"id",   "class", "style", "title",
                                                     "lang", "dir",   "role",  "tabindex"};
        static const std::map<std::string, std::set<std::string>> specific = {
            {"a", {"href", "target", "rel"}},
            {"img", {"src", "alt", "width", "height"}},
            {"input", {"type", "name", "value", "placeholder", "disabled", "checked", "required"}},
            {"button", {"type", "disabled", "name", "value"}},
            {"form", {"action", "method"}},
            {"label", {"for"}},
            {"meta", {"charset", "name", "content"}},
            {"link", {"href", "rel"}},
            {"script", {"src", "type"}},
            {"textarea", {"name", "placeholder", "disabled", "required", "rows", "cols"}},
            {"option", {"value", "selected", "disabled"}}};
        static const std::set<std::string> booleans = {"disabled", "checked", "required",
                                                       "selected", "hidden"};
        for (size_t i = 0; i < e.names.size(); ++i) {
            auto &name = e.names[i];
            if (name.rfind("on", 0) == 0)
                error("native event handlers require a client backend, which is not implemented");
            auto it = specific.find(e.text);
            if (!global.count(name) && name != "hidden" && name.rfind("data-", 0) != 0 &&
                name.rfind("aria-", 0) != 0 && (it == specific.end() || !it->second.count(name)))
                error("unknown attribute '" + name + "' on <" + e.text + ">");
            auto x = value(*e.args[i]);
            auto key = literal(name);
            if (booleans.count(name)) {
                if (x.t.name != "bool")
                    error("boolean HTML attribute '" + name + "' requires bool");
                auto yes = llvm::BasicBlock::Create(ctx, "attribute", current);
                auto after = llvm::BasicBlock::Create(ctx, "attribute.end", current);
                b.CreateCondBr(x.v, yes, after);
                b.SetInsertPoint(yes);
                auto v = literal("");
                call("fw_attribute", b.getVoidTy(), {tree.v, key.v, v.v});
                b.CreateBr(after);
                b.SetInsertPoint(after);
            } else {
                if (x.t.name != "string")
                    error("HTML attribute '" + name + "' requires string");
                call("fw_attribute", b.getVoidTy(), {tree.v, key.v, x.v});
            }
        }
        static const std::set<std::string> voidTags = {"br",   "hr",   "img",   "input",
                                                       "meta", "link", "source"};
        if (voidTags.count(e.text) && e.args.size() != e.names.size())
            error("void HTML elements cannot have children");
        for (size_t i = e.names.size(); i < e.args.size(); ++i) {
            auto child = value(*e.args[i]);
            if (child.t.name != "element" && child.t.name != "fragment")
                child = stringify(child);
            call("fw_child", b.getVoidTy(), {tree.v, child.v});
        }
        return tree;
    }
    error("unsupported expression '" + e.kind + "'");
}
std::optional<Val> Compiler::domCall(const std::string &name, std::vector<Val> args) {
    if (name.rfind("html.", 0) != 0)
        return std::nullopt;
    auto method = name.substr(5);
    static const std::set<std::string> methods = {"attributes",
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
Val Compiler::invoke(Expr &e, Ty expected) {
    auto &callee = *e.args[0];
    std::vector<Expr *> rest;
    for (size_t i = 1; i < e.args.size(); ++i)
        rest.push_back(e.args[i].get());
    if (callee.kind != "name" && callee.kind != "member") {
        auto function = value(callee);
        if (!function.t.function())
            error("value of type " + function.t.name + " is not callable");
        return callClosure(function, rest);
    }
    std::string name = callable(callee);
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
        classes.count(callee.args[0]->text)) {
        if (e.args.size() != 2)
            error("from_json expects one JSON string");
        auto c = classes.at(callee.args[0]->text);
        auto text = convert(value(*e.args[1]), Ty{"string"});
        auto json = objectCall("fw_json_parse", {frame, text.v});
        Val obj = newInstance(*c);
        for (size_t i = 0; i < c->fields.size(); ++i) {
            auto &f = c->fields[i];
            auto t = type(f.type);
            auto key = literal(f.name);
            llvm::BasicBlock *merge = nullptr;
            if (f.value) {
                auto present = llvm::BasicBlock::Create(ctx, "json.present", current);
                auto missing = llvm::BasicBlock::Create(ctx, "json.default", current);
                merge = llvm::BasicBlock::Create(ctx, "json.next", current);
                auto has =
                    b.CreateICmpNE(call("fw_json_has", b.getInt64Ty(), {json, key.v}), i64(0));
                b.CreateCondBr(has, present, missing);
                b.SetInsertPoint(missing);
                set(obj, i64(i), convert(defaultValue(*f.value, t), t));
                b.CreateBr(merge);
                b.SetInsertPoint(present);
            }
            auto child = objectCall("fw_json_get", {frame, json, key.v});
            Val x;
            if (t.name == "string")
                x = {objectCall("fw_json_string", {frame, child}), t};
            else if (t.integer())
                x = convert({call("fw_json_int", b.getInt64Ty(), {child}), Ty{"i64"}}, t, true);
            else if (t.real())
                x = convert({call("fw_json_real", b.getDoubleTy(), {child}), Ty{"double"}}, t,
                            true);
            else if (t.name == "bool")
                x = {b.CreateICmpNE(call("fw_json_bool", b.getInt64Ty(), {child}), i64(0)), t};
            else
                error("from_json currently supports scalar string, bool and numeric fields");
            set(obj, i64(i), x);
            if (merge) {
                b.CreateBr(merge);
                b.SetInsertPoint(merge);
            }
        }
        return obj;
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
    if (method && classes.count(receiverValue.t.name) && !functions.count(name)) {
        auto c = classes.at(receiverValue.t.name);
        for (size_t i = 0; i < c->fields.size(); ++i)
            if (c->fields[i].name == callee.text && type(c->fields[i].type).function())
                return callClosure(get(receiverValue, i64(static_cast<int64_t>(i)),
                                       type(c->fields[i].type)),
                                   rest);
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
    if (method && (receiverValue.t.array() || receiverValue.t.map())) {
        auto obj = receiverValue;
        auto methodName = callee.text;
        if (obj.t.array() && methodName == "push") {
            if (e.args.size() != 2)
                error("push expects one value");
            auto v = convert(value(*e.args[1], obj.t.item()), obj.t.item());
            auto index = call("fw_array_append", b.getInt64Ty(), {obj.v});
            set(obj, index, v);
            return {};
        }
        if (obj.t.array() && methodName == "pop") {
            if (e.args.size() != 1)
                error("pop expects no arguments");
            auto length = call("fw_length", b.getInt64Ty(), {obj.v});
            auto v = get(obj, b.CreateSub(length, i64(1)), obj.t.item());
            call("fw_array_pop", b.getVoidTy(), {obj.v});
            return v;
        }
        if (methodName == "clear") {
            if (e.args.size() != 1)
                error("clear expects no arguments");
            call("fw_collection_clear", b.getVoidTy(), {obj.v});
            return {};
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
            auto integer = key.t.object()
                               ? i64(0)
                               : b.CreateIntCast(key.v, b.getInt64Ty(),
                                                 key.t.name != "bool" && !key.t.unsigned_());
            auto object = key.t.object() ? key.v : llvm::ConstantPointerNull::get(b.getPtrTy());
            return {b.CreateICmpNE(call("fw_map_remove", b.getInt64Ty(), {obj.v, integer, object}),
                                   i64(0)),
                    Ty{"bool"}};
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
        if (a.t.name != "string" && !a.t.sequence() && !a.t.map() && !a.t.tuple())
            error("len expects string or array");
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
                return {objectCall("fw_map_new", {frame, i64(key.object()         ? 4
                                                             : key.name == "bool" ? 3
                                                             : key.unsigned_()    ? 1
                                                                                  : 0)}),
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
                                                "double", "bool", "string", "element", "fragment"};
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
        count(1);
        auto x = stringify(args[0]);
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
    if (name == "web.serve" || name == "web.route") {
        size_t offset = name == "web.route" ? 1 : 0;
        count(3 + offset);
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
                            convert(args[offset + 2], Ty{"i64"}).v}),
                Ty{"string"}};
    }
    if (name == "web.static") {
        count(3);
        return {objectCall("fw_serve_static", {frame, str(0), str(1), convert(args[2], Ty{"i64"}).v}),
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
    error("unknown or unimplemented callable '" + name + "'");
}
void Compiler::statement(Stmt &s) {
    location = s.loc;
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
            target = variable->type;
        } else if (s.target->kind == "member" || s.target->kind == "inst") {
            object = unwrap(s.target->kind == "inst" ? receiver() : value(*s.target->args[0]));
            if ((object.t.name == "element" || object.t.name == "fragment") &&
                (s.target->text == "text_content" || s.target->text == "id" ||
                 s.target->text == "class_name")) {
                domProperty = s.target->text;
                target = Ty{"string"};
            } else {
                auto [c, i] = field(object.t, s.target->text);
                index = i64(i);
                target = type(c->fields[i].type);
            }
        } else if (s.target->kind == "index") {
            object = value(*s.target->args[0]);
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
            rhs = binary(s.op.substr(0, 1), old, rhs);
        rhs = convert(rhs, target);
        if (object.t.map())
            index = mapIndex(object, mapKey, true);
        if (variable)
            store(variable->address, rhs);
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
        bool groupedConstructor =
            s.target->grouped &&
            (classes.count(name) || name == "desktop.window" || name == "desktop.Window" ||
             (variable && variable->type.classReference()));
        if (!groupedConstructor && (s.target->kind == "name" || s.target->kind == "member")) {
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
        block(s.body);
        bool yesEnds = terminated();
        if (!yesEnds)
            b.CreateBr(end);
        b.SetInsertPoint(no);
        block(s.other);
        bool noEnds = terminated();
        if (!noEnds)
            b.CreateBr(end);
        b.SetInsertPoint(end);
        if (yesEnds && noEnds)
            b.CreateUnreachable();
        return;
    }
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
        block(s.body);
        loops.pop_back();
        loopScopes.pop_back();
        if (!terminated())
            b.CreateBr(cond);
        b.SetInsertPoint(end);
        return;
    }
    if (s.kind == "for") {
        auto array = value(*s.expr);
        if (array.t.map())
            array = {objectCall("fw_map_keys", {frame, array.v}),
                     Ty{array.t.parts()[0].name + "[]"}};
        if (!array.t.sequence())
            error("for requires an array, homogeneous tuple, or map");
        scopes.emplace_back();
        bind("$iterable", array, true);
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
        bind(s.name, get(array, i, array.t.item()));
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
    frame = objectCall("fw_frame", {});
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
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
    for (size_t i = 0; i < context.captures.size(); ++i)
        set(closure, i64(static_cast<int64_t>(i)), load(context.captures[i].second));
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
    frame = objectCall("fw_frame", {});
    auto body = llvm::BasicBlock::Create(ctx, "body", current);
    context.initPoint = b.CreateBr(body);
    context.function = current;
    context.frame = frame;
    context.env = current->getArg(0);
    b.SetInsertPoint(body);
    failure = llvm::BasicBlock::Create(ctx, "propagate.error", current);
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
