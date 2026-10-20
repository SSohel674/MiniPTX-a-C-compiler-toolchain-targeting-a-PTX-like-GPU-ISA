#include "irgen.h"
#include <map>
#include "common.h"

namespace mp {
namespace {

uint8_t clsOf(Ty t) {
    switch (t) {
    case Ty::Int: return C_S32;
    case Ty::Float: return C_F32;
    case Ty::Bool: return C_PRED;
    case Ty::IntPtr: case Ty::FloatPtr: return C_U64;
    default: return C_VOID;
    }
}

struct Sig { Ty ret; std::vector<Ty> params; bool kernel; };
struct V { Operand o; Ty ty; };
struct Var { int vreg; Ty ty; };

class Gen {
public:
    Gen(const Module& m, const std::string& file) : mod_(m) { ir_.file = file; }

    IRModule run() {
        for (auto& f : mod_.funcs) {
            if (sigs_.count(f.name)) throw CompileError("redefinition of '" + f.name + "'", f.line);
            Sig s{f.ret, {}, f.kernel};
            for (auto& p : f.params) s.params.push_back(p.ty);
            sigs_[f.name] = s;
        }
        for (auto& f : mod_.funcs) {
            if (f.external) {
                ExternDecl d; d.name = f.name; d.retCls = clsOf(f.ret);
                for (auto& p : f.params) d.params.push_back({p.name, clsOf(p.ty)});
                ir_.externs.push_back(d);
            } else genFunc(f);
        }
        return std::move(ir_);
    }

private:
    const Module& mod_;
    IRModule ir_;
    std::map<std::string, Sig> sigs_;
    IRFunc* F = nullptr;
    const Func* curFn_ = nullptr;
    std::vector<std::map<std::string, Var>> scopes_;
    int curLine_ = 0;

    // ---- emit helpers
    int newV(uint8_t cls, const std::string& name = "") {
        if (F->vcls.size() >= 60000) throw CompileError("function too large", curLine_);
        F->vcls.push_back(cls); F->vname.push_back(name);
        return (int)F->vcls.size() - 1;
    }
    Operand R(int v) const { return mkReg(v, F->vcls[v]); }
    void push(const Instr& i) { F->code.push_back(i); }
    int emit(Op op, uint8_t type, uint8_t dstCls, Operand a, Operand b = Operand(), uint8_t cmp = 0) {
        int d = newV(dstCls);
        Instr i; i.op = op; i.type = type; i.cmp = cmp; i.dst = R(d); i.s[0] = a; i.s[1] = b; i.line = curLine_;
        push(i);
        return d;
    }
    void emitMov(int dst, Operand src) {
        Instr i; i.op = OP_MOV; i.type = F->vcls[dst]; i.dst = R(dst); i.s[0] = src; i.line = curLine_; push(i);
    }
    int newLabel() { return F->nlabels++; }
    void label(int id) {
        Instr i; i.op = IR_LABEL; i.s[0] = mkOp(K_LABEL, 0, 0, id); i.line = curLine_; push(i);
    }
    void bra(int id, const Operand* pred = nullptr, bool negate = false) {
        Instr i; i.op = OP_BRA; i.s[0] = mkOp(K_LABEL, 0, 0, id); i.line = curLine_;
        if (pred) { i.predmode = negate ? 2 : 1; i.pred = *pred; }
        push(i);
    }

    // ---- scopes
    Var* lookup(const std::string& n) {
        for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
            auto it = scopes_[i].find(n);
            if (it != scopes_[i].end()) return &it->second;
        }
        return nullptr;
    }
    void declare(const std::string& n, Var v, int line) {
        if (scopes_.back().count(n)) throw CompileError("redeclaration of '" + n + "'", line);
        scopes_.back()[n] = v;
    }

    // ---- functions
    void genFunc(const Func& fn) {
        ir_.funcs.emplace_back();
        F = &ir_.funcs.back();
        curFn_ = &fn;
        F->name = fn.name; F->kernel = fn.kernel; F->retCls = clsOf(fn.ret); F->line = fn.line;
        scopes_.clear(); scopes_.emplace_back();
        curLine_ = fn.line;
        for (size_t i = 0; i < fn.params.size(); i++) {
            uint8_t c = clsOf(fn.params[i].ty);
            F->params.push_back({fn.params[i].name, c});
            int v = newV(c, fn.params[i].name);
            Instr in; in.op = OP_LD_PARAM; in.type = c; in.dst = R(v); in.s[0] = mkOp(K_PARAM, c, (uint16_t)i, 0); in.line = curLine_;
            push(in);
            declare(fn.params[i].name, {v, fn.params[i].ty}, fn.line);
        }
        genScoped(fn.body);
        if (F->code.empty() || F->code.back().op != OP_RET) {
            Instr r; r.op = OP_RET; r.type = F->retCls; r.line = curLine_;
            if (F->retCls != C_VOID) r.s[0] = mkImm(0, F->retCls);
            push(r);
        }
        F = nullptr;
    }

    void genScoped(const std::vector<StmtP>& body) {
        scopes_.emplace_back();
        for (auto& s : body) genStmt(*s);
        scopes_.pop_back();
    }

    // ---- conversions
    V toBool(V v, int line) {
        if (v.ty == Ty::Bool) return v;
        if (v.ty == Ty::Int) { int d = emit(OP_SETP, C_S32, C_PRED, v.o, mkImm(0, C_S32), CMP_NE); return {R(d), Ty::Bool}; }
        if (v.ty == Ty::Float) { int d = emit(OP_SETP, C_F32, C_PRED, v.o, mkImm(0, C_F32), CMP_NE); return {R(d), Ty::Bool}; }
        throw CompileError(std::string("cannot use '") + tyName(v.ty) + "' as a condition", line);
    }
    V conv(V v, Ty want, int line) {
        if (v.ty == want) return v;
        if (v.ty == Ty::Void) throw CompileError("void value used in expression", line);
        if (want == Ty::Float && v.ty == Ty::Int) { int d = emit(OP_CVT, C_F32, C_F32, v.o, Operand(), C_S32); return {R(d), Ty::Float}; }
        if (want == Ty::Int && v.ty == Ty::Float) { int d = emit(OP_CVT, C_S32, C_S32, v.o, Operand(), C_F32); return {R(d), Ty::Int}; }
        if (want == Ty::Bool) return toBool(v, line);
        throw CompileError(std::string("cannot convert '") + tyName(v.ty) + "' to '" + tyName(want) + "'", line);
    }

    // ---- expressions
    V genExpr(const Expr& e) {
        switch (e.kind) {
        case Expr::IntLit: return {mkImm((int32_t)e.ival, C_S32), Ty::Int};
        case Expr::FloatLit: return {mkImm(fToBits(e.fval), C_F32), Ty::Float};
        case Expr::Var: {
            if (Var* v = lookup(e.name)) return {R(v->vreg), v->ty};
            static const std::map<std::string, int> builtins = {
                {"threadIdx", SP_TID}, {"blockIdx", SP_CTAID}, {"blockDim", SP_NTID}, {"gridDim", SP_NCTAID}};
            auto it = builtins.find(e.name);
            if (it != builtins.end()) {
                int d = emit(OP_MOV, C_S32, C_S32, mkOp(K_SPECIAL, C_S32, 0, it->second));
                return {R(d), Ty::Int};
            }
            throw CompileError("use of undeclared identifier '" + e.name + "'", e.line);
        }
        case Expr::Cast: return conv(genExpr(*e.a), e.castTy, e.line);
        case Expr::Un: {
            V a = genExpr(*e.a);
            if (e.op == "!") { a = toBool(a, e.line); int d = emit(OP_NOT, C_PRED, C_PRED, a.o); return {R(d), Ty::Bool}; }
            if (a.ty == Ty::Int) { int d = emit(OP_NEG, C_S32, C_S32, a.o); return {R(d), Ty::Int}; }
            if (a.ty == Ty::Float) { int d = emit(OP_NEG, C_F32, C_F32, a.o); return {R(d), Ty::Float}; }
            throw CompileError("invalid operand to unary '-'", e.line);
        }
        case Expr::Bin: return genBin(e);
        case Expr::Index: {
            auto [addr, elem] = genAddr(e);
            uint8_t c = clsOf(elem);
            int d = emit(OP_LD_GLOBAL, c, c, addr);
            return {R(d), elem};
        }
        case Expr::Call: return genCall(e);
        }
        throw CompileError("bad expression", e.line);
    }

    std::pair<Operand, Ty> genAddr(const Expr& e) {  // e.kind == Index
        Var* v = lookup(e.name);
        if (!v) throw CompileError("use of undeclared identifier '" + e.name + "'", e.line);
        if (!isPtr(v->ty)) throw CompileError("'" + e.name + "' is not a pointer", e.line);
        V idx = genExpr(*e.a);
        if (idx.ty != Ty::Int) throw CompileError("array index must be int", e.line);
        int off = emit(OP_MUL_WIDE, C_U64, C_U64, idx.o, mkImm(4, C_S32));
        int addr = emit(OP_ADD, C_U64, C_U64, R(v->vreg), R(off));
        return {R(addr), v->ty == Ty::IntPtr ? Ty::Int : Ty::Float};
    }

    V genBin(const Expr& e) {
        const std::string& op = e.op;
        V a = genExpr(*e.a), b = genExpr(*e.b);
        if (op == "&&" || op == "||") {
            a = toBool(a, e.line); b = toBool(b, e.line);
            int d = emit(op == "&&" ? OP_AND : OP_OR, C_PRED, C_PRED, a.o, b.o);
            return {R(d), Ty::Bool};
        }
        if (isPtr(a.ty) || isPtr(b.ty)) throw CompileError("pointer arithmetic is not supported (use a[i])", e.line);
        if (a.ty == Ty::Void || b.ty == Ty::Void) throw CompileError("void value used in expression", e.line);
        if (a.ty == Ty::Bool || b.ty == Ty::Bool) throw CompileError("boolean operand in arithmetic/comparison", e.line);
        Ty t = (a.ty == Ty::Float || b.ty == Ty::Float) ? Ty::Float : Ty::Int;
        a = conv(a, t, e.line); b = conv(b, t, e.line);
        uint8_t c = clsOf(t);
        static const std::map<std::string, Cmp> cmps = {{"<", CMP_LT}, {"<=", CMP_LE}, {">", CMP_GT},
                                                        {">=", CMP_GE}, {"==", CMP_EQ}, {"!=", CMP_NE}};
        auto ci = cmps.find(op);
        if (ci != cmps.end()) { int d = emit(OP_SETP, c, C_PRED, a.o, b.o, ci->second); return {R(d), Ty::Bool}; }
        Op o;
        if (op == "+") o = OP_ADD; else if (op == "-") o = OP_SUB; else if (op == "*") o = OP_MUL;
        else if (op == "/") o = OP_DIV;
        else if (op == "%") { if (t != Ty::Int) throw CompileError("'%' requires int operands", e.line); o = OP_REM; }
        else throw CompileError("unknown operator '" + op + "'", e.line);
        int d = emit(o, c, c, a.o, b.o);
        return {R(d), t};
    }

    V genCall(const Expr& e) {
        auto it = sigs_.find(e.name);
        if (it == sigs_.end()) throw CompileError("call to undeclared function '" + e.name + "'", e.line);
        const Sig& sg = it->second;
        if (sg.kernel) throw CompileError("cannot call kernel '" + e.name + "' from device code", e.line);
        if (e.args.size() != sg.params.size())
            throw CompileError("'" + e.name + "' expects " + std::to_string(sg.params.size()) + " argument(s), got " +
                               std::to_string(e.args.size()), e.line);
        Instr in; in.op = OP_CALL; in.type = clsOf(sg.ret); in.line = curLine_;
        int ci = -1;
        for (size_t i = 0; i < F->callees.size(); i++) if (F->callees[i] == e.name) ci = (int)i;
        if (ci < 0) { F->callees.push_back(e.name); ci = (int)F->callees.size() - 1; }
        in.s[0] = mkOp(K_SYM, 0, 0, ci);
        for (size_t i = 0; i < e.args.size(); i++) {
            V a = conv(genExpr(*e.args[i]), sg.params[i], e.line);
            in.s[1 + i] = a.o;
        }
        if (sg.ret != Ty::Void) { int d = newV(clsOf(sg.ret)); in.dst = R(d); push(in); return {R(d), sg.ret}; }
        push(in);
        return {Operand(), Ty::Void};
    }

    // ---- statements
    void genStmt(const Stmt& s) {
        curLine_ = s.line;
        switch (s.kind) {
        case Stmt::Decl: {
            uint8_t c = clsOf(s.ty);
            V init = s.e ? conv(genExpr(*s.e), s.ty, s.line) : V{mkImm(0, c), s.ty};
            int v = newV(c, s.name);
            emitMov(v, init.o);
            declare(s.name, {v, s.ty}, s.line);
            break;
        }
        case Stmt::Assign: {
            Var* v = lookup(s.name);
            if (!v) throw CompileError("use of undeclared identifier '" + s.name + "'", s.line);
            if (isPtr(v->ty)) throw CompileError("cannot assign to pointer '" + s.name + "'", s.line);
            V val = conv(genExpr(*s.e), v->ty, s.line);
            emitMov(v->vreg, val.o);
            break;
        }
        case Stmt::IndexAssign: {
            auto addrOf = [&]() {
                Var* v = lookup(s.name);
                if (!v) throw CompileError("use of undeclared identifier '" + s.name + "'", s.line);
                if (!isPtr(v->ty)) throw CompileError("'" + s.name + "' is not a pointer", s.line);
                V idx = genExpr(*s.idx);
                if (idx.ty != Ty::Int) throw CompileError("array index must be int", s.line);
                int off = emit(OP_MUL_WIDE, C_U64, C_U64, idx.o, mkImm(4, C_S32));
                int addr = emit(OP_ADD, C_U64, C_U64, R(v->vreg), R(off));
                return std::make_pair(R(addr), v->ty == Ty::IntPtr ? Ty::Int : Ty::Float);
            };
            auto [addr, elem] = addrOf();
            V val = conv(genExpr(*s.e), elem, s.line);
            Instr st; st.op = OP_ST_GLOBAL; st.type = clsOf(elem); st.s[0] = addr; st.s[1] = val.o; st.line = curLine_;
            push(st);
            break;
        }
        case Stmt::ExprStmt: genExpr(*s.e); break;
        case Stmt::Block: genScoped(s.body); break;
        case Stmt::If: {
            V c = toBool(genExpr(*s.cond), s.line);
            bool hasElse = !s.elseBody.empty();
            int Lelse = newLabel(), Lend = newLabel();
            bra(hasElse ? Lelse : Lend, &c.o, true);
            genScoped(s.body);
            if (hasElse) {
                curLine_ = s.line; bra(Lend); label(Lelse);
                genScoped(s.elseBody);
            }
            label(Lend);
            break;
        }
        case Stmt::While: {
            int Ltop = newLabel(), Lend = newLabel();
            label(Ltop);
            curLine_ = s.line;
            V c = toBool(genExpr(*s.cond), s.line);
            bra(Lend, &c.o, true);
            genScoped(s.body);
            curLine_ = s.line;
            bra(Ltop);
            label(Lend);
            break;
        }
        case Stmt::Return: {
            Instr r; r.op = OP_RET; r.type = F->retCls; r.line = curLine_;
            if (curFn_->kernel) {
                if (s.e) throw CompileError("kernels cannot return a value", s.line);
            } else if (F->retCls == C_VOID) {
                if (s.e) throw CompileError("void function should not return a value", s.line);
            } else {
                if (!s.e) throw CompileError("non-void function must return a value", s.line);
                V v = conv(genExpr(*s.e), curFn_->ret, s.line);
                r.s[0] = v.o;
            }
            push(r);
            break;
        }
        }
    }
};

}  // namespace

IRModule generateIR(const Module& m, const std::string& file) { return Gen(m, file).run(); }

}  // namespace mp
