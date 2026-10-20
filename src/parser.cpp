#include "parser.h"
#include "common.h"

namespace mp {
namespace {

class Parser {
public:
    explicit Parser(const std::vector<Token>& t) : t_(t) {}

    Module parse() {
        Module m;
        while (cur().type != Token::End) m.funcs.push_back(parseFunc());
        return m;
    }

private:
    const std::vector<Token>& t_;
    size_t p_ = 0;

    const Token& cur() const { return t_[p_]; }
    bool isP(const char* s) const { return cur().type == Token::Punct && cur().text == s; }
    bool isKw(const char* s) const { return cur().type == Token::Kw && cur().text == s; }
    [[noreturn]] void err(const std::string& m) const { throw CompileError(m, cur().line); }
    void expectP(const char* s) {
        if (!isP(s)) err(std::string("expected '") + s + "' but found '" + cur().text + "'");
        p_++;
    }
    std::string expectIdent() {
        if (cur().type != Token::Ident) err("expected identifier but found '" + cur().text + "'");
        return t_[p_++].text;
    }
    bool atType() const { return isKw("int") || isKw("float") || isKw("void"); }

    Ty parseType(bool allowVoid) {
        Ty ty;
        if (isKw("int")) ty = Ty::Int;
        else if (isKw("float")) ty = Ty::Float;
        else if (isKw("void") && allowVoid) { p_++; return Ty::Void; }
        else err("expected a type");
        p_++;
        if (isP("*")) { p_++; return ty == Ty::Int ? Ty::IntPtr : Ty::FloatPtr; }
        return ty;
    }

    Func parseFunc() {
        Func f;
        f.line = cur().line;
        if (isKw("extern")) { p_++; f.external = true; }
        if (isKw("kernel")) {
            if (f.external) err("a kernel cannot be extern");
            p_++; f.kernel = true; f.ret = Ty::Void;
        } else if (isKw("device")) {
            p_++;
            f.ret = parseType(true);
            if (isPtr(f.ret)) err("device functions cannot return pointers");
        } else err("expected 'kernel' or 'device'");
        f.name = expectIdent();
        expectP("(");
        if (!isP(")")) {
            do {
                Param pr;
                pr.ty = parseType(false);
                pr.name = expectIdent();
                f.params.push_back(pr);
            } while (isP(",") && (p_++, true));
        }
        expectP(")");
        if (f.kernel && f.params.size() > 8) err("kernels support at most 8 parameters");
        if (!f.kernel && f.params.size() > 4) err("device functions support at most 4 parameters");
        if (f.external) { expectP(";"); return f; }
        expectP("{");
        f.body = parseBlockBody();
        return f;
    }

    std::vector<StmtP> parseBlockBody() {  // after '{'
        std::vector<StmtP> v;
        while (!isP("}")) {
            if (cur().type == Token::End) err("unexpected end of file, missing '}'");
            v.push_back(parseStmt());
        }
        p_++;
        return v;
    }

    StmtP mk(Stmt::Kind k, int line) { auto s = std::make_unique<Stmt>(); s->kind = k; s->line = line; return s; }

    StmtP parseStmt() {
        int line = cur().line;
        if (isP("{")) { p_++; auto s = mk(Stmt::Block, line); s->body = parseBlockBody(); return s; }
        if (isKw("if")) {
            p_++; auto s = mk(Stmt::If, line);
            expectP("("); s->cond = parseExpr(); expectP(")");
            s->body.push_back(parseStmt());
            if (isKw("else")) { p_++; s->elseBody.push_back(parseStmt()); }
            return s;
        }
        if (isKw("while")) {
            p_++; auto s = mk(Stmt::While, line);
            expectP("("); s->cond = parseExpr(); expectP(")");
            s->body.push_back(parseStmt());
            return s;
        }
        if (isKw("for")) {  // desugar: { init; while (cond) { body; step; } }
            p_++;
            auto outer = mk(Stmt::Block, line);
            expectP("(");
            if (!isP(";")) outer->body.push_back(parseSimple());
            expectP(";");
            auto w = mk(Stmt::While, line);
            if (!isP(";")) w->cond = parseExpr();
            else { w->cond = std::make_unique<Expr>(); w->cond->kind = Expr::IntLit; w->cond->ival = 1; w->cond->line = line; }
            expectP(";");
            StmtP step;
            if (!isP(")")) step = parseSimple();
            expectP(")");
            w->body.push_back(parseStmt());
            if (step) w->body.push_back(std::move(step));
            outer->body.push_back(std::move(w));
            return outer;
        }
        if (isKw("return")) {
            p_++; auto s = mk(Stmt::Return, line);
            if (!isP(";")) s->e = parseExpr();
            expectP(";");
            return s;
        }
        auto s = parseSimple();
        expectP(";");
        return s;
    }

    static ExprP clone(const Expr& e) {
        auto c = std::make_unique<Expr>();
        c->kind = e.kind; c->line = e.line; c->ival = e.ival; c->fval = e.fval; c->name = e.name; c->op = e.op;
        c->castTy = e.castTy;
        if (e.a) c->a = clone(*e.a);
        if (e.b) c->b = clone(*e.b);
        for (auto& x : e.args) c->args.push_back(clone(*x));
        return c;
    }

    ExprP mkBin(const std::string& op, ExprP a, ExprP b, int line) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Bin; e->op = op; e->a = std::move(a); e->b = std::move(b); e->line = line;
        return e;
    }

    StmtP makeAssign(ExprP lhs, ExprP rhs, int line) {
        if (lhs->kind == Expr::Var) {
            auto s = mk(Stmt::Assign, line); s->name = lhs->name; s->e = std::move(rhs); return s;
        }
        if (lhs->kind == Expr::Index) {
            auto s = mk(Stmt::IndexAssign, line); s->name = lhs->name; s->idx = std::move(lhs->a); s->e = std::move(rhs); return s;
        }
        throw CompileError("invalid assignment target", line);
    }

    StmtP parseSimple() {
        int line = cur().line;
        if (isKw("int") || isKw("float")) {
            auto s = mk(Stmt::Decl, line);
            s->ty = parseType(false);
            if (isPtr(s->ty)) err("pointer variables can only be kernel/function parameters");
            s->name = expectIdent();
            if (isP("=")) { p_++; s->e = parseExpr(); }
            return s;
        }
        ExprP lhs = parseExpr();
        static const char* compound[] = {"+=", "-=", "*=", "/="};
        if (isP("=")) {
            p_++;
            return makeAssign(std::move(lhs), parseExpr(), line);
        }
        for (const char* c : compound)
            if (isP(c)) {
                std::string op(1, c[0]);
                p_++;
                ExprP rhs = parseExpr();
                ExprP full = mkBin(op, clone(*lhs), std::move(rhs), line);
                return makeAssign(std::move(lhs), std::move(full), line);
            }
        if (isP("++") || isP("--")) {
            std::string op(1, cur().text[0]);
            p_++;
            auto one = std::make_unique<Expr>(); one->kind = Expr::IntLit; one->ival = 1; one->line = line;
            ExprP full = mkBin(op, clone(*lhs), std::move(one), line);
            return makeAssign(std::move(lhs), std::move(full), line);
        }
        if (lhs->kind != Expr::Call) throw CompileError("expression statement has no effect", line);
        auto s = mk(Stmt::ExprStmt, line);
        s->e = std::move(lhs);
        return s;
    }

    static int prec(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 0;
    }

    ExprP parseExpr() { return parseBin(1); }

    ExprP parseBin(int minPrec) {
        ExprP lhs = parseUnary();
        while (cur().type == Token::Punct && prec(cur().text) >= minPrec) {
            std::string op = cur().text;
            int line = cur().line, pr = prec(op);
            p_++;
            ExprP rhs = parseBin(pr + 1);
            lhs = mkBin(op, std::move(lhs), std::move(rhs), line);
        }
        return lhs;
    }

    ExprP parseUnary() {
        int line = cur().line;
        if (isP("-") || isP("!")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Un; e->op = cur().text; e->line = line; p_++;
            e->a = parseUnary();
            return e;
        }
        if (isP("(") && t_[p_ + 1].type == Token::Kw && (t_[p_ + 1].text == "int" || t_[p_ + 1].text == "float") &&
            t_[p_ + 2].type == Token::Punct && t_[p_ + 2].text == ")") {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Cast; e->line = line;
            e->castTy = t_[p_ + 1].text == "int" ? Ty::Int : Ty::Float;
            p_ += 3;
            e->a = parseUnary();
            return e;
        }
        return parsePrimary();
    }

    ExprP parsePrimary() {
        int line = cur().line;
        auto e = std::make_unique<Expr>();
        e->line = line;
        if (cur().type == Token::Int) { e->kind = Expr::IntLit; e->ival = cur().ival; p_++; return e; }
        if (cur().type == Token::Float) { e->kind = Expr::FloatLit; e->fval = cur().fval; p_++; return e; }
        if (isP("(")) { p_++; auto x = parseExpr(); expectP(")"); return x; }
        if (cur().type == Token::Ident) {
            e->name = cur().text; p_++;
            if (isP("(")) {
                p_++; e->kind = Expr::Call;
                if (!isP(")")) {
                    do { e->args.push_back(parseExpr()); } while (isP(",") && (p_++, true));
                }
                expectP(")");
                return e;
            }
            if (isP("[")) {
                p_++; e->kind = Expr::Index; e->a = parseExpr(); expectP("]");
                return e;
            }
            e->kind = Expr::Var;
            return e;
        }
        err("unexpected token '" + cur().text + "' in expression");
    }
};

}  // namespace

Module parseModule(const std::vector<Token>& tokens) { return Parser(tokens).parse(); }

}  // namespace mp
