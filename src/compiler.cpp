#include "flywheel/frontend.hpp"
#include <algorithm>
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
#include <set>
#include <unordered_map>

namespace fw {
namespace {
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
    Ty item() const {
        return Ty{name.substr(0, name.size() - 2)};
    }
    int bits() const {
        if (integer())
            return std::stoi(name.substr(1));
        return name == "float" ? 32 : 64;
    }
    bool object() const {
        return !numeric() && name != "bool" && name != "void" && name.rfind("fn:", 0) != 0 &&
               name.rfind("module:", 0) != 0;
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
    std::string owner;
    std::vector<Ty> returns;
    Location location;
    uint64_t tempGeneration = 0;
    [[noreturn]] void error(const std::string &s) {
        throw Error(location, s);
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
        if (t.numeric() || name == "bool" || name == "string" || name == "void" ||
            name == "element" || name == "fragment" || classes.count(name))
            return t;
        error("unknown or unsupported type '" + name + "'");
    }
    llvm::ConstantInt *i64(int64_t x) {
        return b.getInt64(x);
    }
    llvm::Value *call(const std::string &name, llvm::Type *result,
                      std::vector<llvm::Value *> args) {
        std::vector<llvm::Type *> types;
        for (auto a : args)
            types.push_back(a->getType());
        auto f = module.getOrInsertFunction(name, llvm::FunctionType::get(result, types, false));
        return b.CreateCall(f, args);
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
    void guard(llvm::Value *condition, const std::string &message) {
        auto ok = llvm::BasicBlock::Create(ctx, "checked", current);
        auto bad = llvm::BasicBlock::Create(ctx, "error", current);
        b.CreateCondBr(condition, ok, bad);
        b.SetInsertPoint(bad);
        call("fw_panic", b.getVoidTy(), {b.CreateGlobalString(message)});
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
        if (x.t.name == "null" && to.nullable())
            return {x.v, to};
        if (to.nullable() && to.name == x.t.name + "?")
            return {x.v, to};
        if (x.t.name == "fragment" && to.name == "element")
            error("fragment is not an element");
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
                auto out = b.CreateIntCast(x.v, ll(to), !x.t.unsigned_());
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
    Val stringify(Val x) {
        if (x.t.name == "string")
            return x;
        if (x.t.integer()) {
            auto y = b.CreateIntCast(x.v, b.getInt64Ty(), !x.t.unsigned_());
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
        error("cannot format value of type " + x.t.name);
    }
    Var *lookup(const std::string &name) {
        for (auto i = scopes.rbegin(); i != scopes.rend(); ++i) {
            auto v = i->find(name);
            if (v != i->end())
                return &v->second;
        }
        auto g = globals.find(name);
        return g == globals.end() ? nullptr : &g->second;
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
        auto result = b.CreateLoad(ll(v.type), v.address);
        if (v.type.object())
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
            return {objectCall("fw_keep", {frame, object}), t};
        }
        if (t.real()) {
            auto x = call("fw_get_real", b.getDoubleTy(), {obj.v, index});
            return {b.CreateFPCast(x, ll(t)), t};
        }
        auto x = call("fw_get_int", b.getInt64Ty(), {obj.v, index});
        return {b.CreateIntCast(x, ll(t), !t.unsigned_()), t};
    }
    void set(Val obj, llvm::Value *index, Val x) {
        if (x.t.object())
            call("fw_set_object", b.getVoidTy(), {obj.v, index, x.v});
        else if (x.t.real())
            call("fw_set_real", b.getVoidTy(),
                 {obj.v, index, b.CreateFPCast(x.v, b.getDoubleTy())});
        else
            call("fw_set_int", b.getVoidTy(),
                 {obj.v, index, b.CreateIntCast(x.v, b.getInt64Ty(), !x.t.unsigned_())});
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
    Val invoke(Expr &e);
    Val value(Expr &e, Ty expected = Ty{});
    Val defaultValue(Expr &e, Ty expected) {
        auto saved = std::move(scopes);
        auto savedAttrs = std::move(attrs);
        auto savedNames = std::move(componentNames);
        scopes.clear();
        scopes.emplace_back();
        attrs.clear();
        componentNames.clear();
        auto result = value(e, expected);
        scopes = std::move(saved);
        attrs = std::move(savedAttrs);
        componentNames = std::move(savedNames);
        return result;
    }
    void statement(Stmt &s);
    bool terminated() {
        return !b.GetInsertBlock()->empty() && b.GetInsertBlock()->back().isTerminator();
    }
    void block(std::vector<SP> &body, bool scope = true) {
        if (scope)
            scopes.emplace_back();
        for (auto &s : body) {
            if (terminated())
                error("unreachable statement");
            auto before = tempGeneration;
            statement(*s);
            if (!terminated() && tempGeneration != before)
                call("fw_sweep", b.getVoidTy(), {frame});
        }
        if (scope)
            scopes.pop_back();
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
        static const std::set<std::string> modules = {"html", "math", "fs",      "os",  "json",
                                                      "web",  "time", "testing", "core"};
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
        current = llvm::Function::Create(llvm::FunctionType::get(b.getInt32Ty(), false),
                                         llvm::GlobalValue::ExternalLinkage, "main", module);
        auto entry = llvm::BasicBlock::Create(ctx, "entry", current);
        b.SetInsertPoint(entry);
        frame = objectCall("fw_frame", {});
        scopes.emplace_back();
        top = true;
        returnType = Ty{"void"};
        block(p.body, false);
        if (!terminated()) {
            call("fw_drop", b.getVoidTy(), {frame});
            b.CreateRet(b.getInt32(0));
        }
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
        if (!opts.object.empty()) {
            std::error_code ec;
            llvm::raw_fd_ostream output(opts.object, ec, llvm::sys::fs::OF_None);
            if (ec)
                throw std::runtime_error(ec.message());
            llvm::legacy::PassManager pm;
            if (machine->addPassesToEmitFile(pm, output, nullptr,
                                             llvm::CodeGenFileType::ObjectFile))
                throw std::runtime_error("target cannot emit object files");
            pm.run(module);
            output.flush();
        }
    }
};
Val Compiler::value(Expr &e, Ty expected) {
    location = e.loc;
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
        auto obj = value(*e.args[0]);
        if (obj.t.name == "module:math" && e.text == "PI")
            return {llvm::ConstantFP::get(b.getDoubleTy(), 3.141592653589793), Ty{"double"}};
        if ((obj.t.name == "string" || obj.t.array()) && e.text == "length")
            return {call("fw_length", b.getInt64Ty(), {obj.v}), Ty{"i64"}};
        if (obj.t.name == "element" || obj.t.name == "fragment") {
            if (e.text == "tag")
                return {objectCall("fw_tag", {frame, obj.v}), Ty{"string"}};
            if (e.text == "children")
                error("element.children includes text and element nodes; its union type is not "
                      "implemented yet");
        }
        auto [c, i] = field(obj.t, e.text);
        return get(obj, i64(i), type(c->fields[i].type));
    }
    if (e.kind == "index") {
        auto obj = value(*e.args[0]);
        if (!obj.t.array())
            error("indexing requires an array");
        auto index = convert(value(*e.args[1]), Ty{"i64"});
        return get(obj, index.v, obj.t.item());
    }
    if (e.kind == "call")
        return invoke(e);
    if (e.kind == "array") {
        Ty item = expected.array() ? expected.item() : Ty{};
        std::vector<Val> items;
        for (auto &arg : e.args) {
            auto v = value(*arg, item);
            if (item.name == "void")
                item = v.t;
            else if (v.t != item && !expected.array()) {
                if (widening(item, v.t))
                    item = v.t;
                else if (!widening(v.t, item))
                    error("heterogeneous tuple inference is not implemented yet");
            }
            items.push_back(v);
        }
        if (item.name == "void")
            error("empty arrays require an expected array type");
        if (item.name == "null" || item.name == "void" ||
            (!item.object() && !item.numeric() && item.name != "bool"))
            error("unsupported array element type");
        Ty t{item.name + "[]"};
        Val array{objectCall("fw_new", {frame, i64(items.size()), i64(1)}), t};
        for (size_t i = 0; i < items.size(); ++i)
            set(array, i64(i), convert(items[i], item));
        return array;
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
        if (e.text == "in") {
            if (!c.t.array())
                error("membership currently requires an array");
            a = convert(a, c.t.item());
            std::string suffix = a.t.object() ? "object" : a.t.real() ? "real" : "int";
            llvm::Value *item = a.v;
            if (a.t.real())
                item = b.CreateFPCast(item, b.getDoubleTy());
            else if (!a.t.object())
                item = b.CreateIntCast(item, b.getInt64Ty(), !a.t.unsigned_());
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
Val Compiler::invoke(Expr &e) {
    auto &callee = *e.args[0];
    std::string name = callable(callee);
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
        Val obj{objectCall("fw_new", {frame, i64(c->fields.size()), i64(2)}), Ty{c->name}};
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
    if (name.empty() && callee.kind == "member") {
        receiverValue = value(*callee.args[0]);
        name = receiverValue.t.name + "." + callee.text;
        method = true;
    }
    if (classes.count(name)) {
        auto c = classes.at(name);
        if (e.args.size() - 1 > c->fields.size())
            error("too many constructor arguments for " + name);
        Val obj{objectCall("fw_new", {frame, i64(c->fields.size()), i64(2)}), Ty{name}};
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
        return literal(value(*e.args[1]).t.name);
    }
    if (name == "len") {
        if (e.args.size() != 2)
            error("len expects one argument");
        auto a = value(*e.args[1]);
        if (a.t.name != "string" && !a.t.array())
            error("len expects string or array");
        return {call("fw_length", b.getInt64Ty(), {a.v}), Ty{"i64"}};
    }
    static const std::set<std::string> casts = {"int", "i8",  "i16", "i32",   "i64",    "u8",
                                                "u16", "u32", "u64", "float", "double", "string"};
    if (casts.count(name)) {
        if (e.args.size() != 2)
            error("cast expects exactly one argument");
        auto a = value(*e.args[1]);
        if (a.t.name == "string" && name != "string") {
            auto t = type(name);
            Ty source = t.integer() ? Ty{t.unsigned_() ? "u64" : "i64"} : Ty{"double"};
            auto parsed = call(t.integer() ? (t.unsigned_() ? "fw_parse_uint" : "fw_parse_int")
                                           : "fw_parse_real",
                               ll(source), {a.v});
            return convert({parsed, source}, t, true);
        }
        return name == "string" ? stringify(a) : convert(a, type(name), true);
    }
    if (functions.count(name) || (name.empty() && callee.kind == "name") ||
        (!name.empty() && lookup(name))) {
        if (auto v = lookup(name); v && v->type.name.rfind("fn:", 0) == 0)
            name = v->type.name.substr(3);
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
        if (sig.result.object())
            objectCall("fw_adopt", {frame, result});
        return {result, sig.result};
    }
    for (size_t i = 1; i < e.args.size(); ++i)
        args.push_back(value(*e.args[i]));
    auto count = [&](size_t n) {
        if (args.size() != n)
            error(name + " expects " + std::to_string(n) + " argument(s)");
    };
    auto str = [&](size_t i) { return convert(args[i], Ty{"string"}).v; };
    auto real = [&](size_t i) { return convert(args[i], Ty{"double"}).v; };
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
    if (name == "web.serve") {
        count(3);
        if (args[1].t.name.rfind("fn:", 0) != 0)
            error("HTTP handler must be a function reference");
        auto &sig = signature(args[1].t.name.substr(3));
        if (!sig.ast->params.empty() || (!sig.ast->owner.empty()) ||
            (sig.result.name != "element" && sig.result.name != "fragment"))
            error("HTTP handler must take no arguments and return element or fragment");
        call("fw_serve", b.getVoidTy(), {str(0), args[1].v, convert(args[2], Ty{"i64"}).v});
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
            globals[s.name] = {g, x.t, s.op == "const"};
            if (x.t.object())
                call("fw_root", b.getVoidTy(), {frame, g});
            store(g, x);
        } else
            bind(s.name, x, s.op == "const");
        return;
    }
    if (s.kind == "assign") {
        Var *variable = nullptr;
        Val object;
        llvm::Value *index = nullptr;
        Ty target;
        if (s.target->kind == "name") {
            variable = lookup(s.target->text);
            if (!variable)
                error("assignment to undeclared identifier '" + s.target->text +
                      "'; use var or an explicit type");
            if (variable->constant)
                error("cannot assign to const '" + s.target->text + "'");
            target = variable->type;
        } else if (s.target->kind == "member" || s.target->kind == "inst") {
            object = s.target->kind == "inst" ? receiver() : value(*s.target->args[0]);
            auto [c, i] = field(object.t, s.target->text);
            index = i64(i);
            target = type(c->fields[i].type);
        } else if (s.target->kind == "index") {
            object = value(*s.target->args[0]);
            if (!object.t.array())
                error("index assignment requires array");
            index = convert(value(*s.target->args[1]), Ty{"i64"}).v;
            target = object.t.item();
        } else
            error("invalid assignment target");
        Val old;
        if (s.op != "=")
            old = variable ? load(*variable) : get(object, index, target);
        auto rhs = value(*s.expr, target);
        if (s.op != "=")
            rhs = binary(s.op.substr(0, 1), old, rhs);
        rhs = convert(rhs, target);
        if (variable)
            store(variable->address, rhs);
        else
            set(object, index, rhs);
        return;
    }
    if (s.kind == "expr") {
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
        block(s.body);
        loops.pop_back();
        if (!terminated())
            b.CreateBr(cond);
        b.SetInsertPoint(end);
        return;
    }
    if (s.kind == "for") {
        auto array = value(*s.expr);
        if (!array.t.array())
            error("for currently requires an array");
        auto anchor = allocate(array.t, "iterable.root");
        store(anchor, array);
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
        scopes.emplace_back();
        bind(s.name, get(array, i, array.t.item()));
        loops.emplace_back(end, next);
        block(s.body, false);
        loops.pop_back();
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
        return;
    }
    if (s.kind == "break" || s.kind == "continue") {
        if (loops.empty())
            error(s.kind + " requires a loop");
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
    auto savedIP = b.saveIP();
    auto savedCurrent = current;
    auto savedFrame = frame;
    auto savedAttrs = std::move(attrs);
    auto savedComponentNames = std::move(componentNames);
    attrs.clear();
    componentNames.clear();
    auto savedScopes = std::move(scopes);
    auto savedLoops = std::move(loops);
    auto savedOwner = owner;
    auto savedReturn = returnType;
    auto savedReturns = std::move(returns);
    bool savedInfer = infer, savedTop = top;
    auto savedLocation = location;
    owner = sig.ast->owner;
    top = false;
    infer = inference;
    returnType = inference ? Ty{"void"} : sig.result;
    returns.clear();
    scopes.clear();
    scopes.emplace_back();
    loops.clear();
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
    if (inference) {
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
        sig.result = result;
        current->eraseFromParent();
        sig.function = llvm::Function::Create(
            llvm::FunctionType::get(ll(result), paramTypes, false),
            llvm::GlobalValue::InternalLinkage,
            "fly." + (owner.empty() ? "" : owner + ".") + sig.ast->name, module);
    } else
        sig.done = true;
    current = savedCurrent;
    frame = savedFrame;
    scopes = std::move(savedScopes);
    attrs = std::move(savedAttrs);
    componentNames = std::move(savedComponentNames);
    loops = std::move(savedLoops);
    owner = savedOwner;
    returnType = savedReturn;
    returns = std::move(savedReturns);
    infer = savedInfer;
    top = savedTop;
    location = savedLocation;
    b.restoreIP(savedIP);
    sig.building = false;
}
} // namespace
void compile(Program &p, const std::string &filename, const Options &options) {
    Compiler c(p, filename);
    c.run(options);
}
} // namespace fw
