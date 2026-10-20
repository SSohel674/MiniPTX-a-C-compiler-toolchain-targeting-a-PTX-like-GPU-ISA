#pragma once
#include <memory>
#include <string>
#include <vector>

namespace mp {

enum class Ty { Void, Int, Float, IntPtr, FloatPtr, Bool };

inline const char* tyName(Ty t) {
    switch (t) {
    case Ty::Void: return "void";
    case Ty::Int: return "int";
    case Ty::Float: return "float";
    case Ty::IntPtr: return "int*";
    case Ty::FloatPtr: return "float*";
    case Ty::Bool: return "bool";
    }
    return "?";
}
inline bool isPtr(Ty t) { return t == Ty::IntPtr || t == Ty::FloatPtr; }

struct Expr;
using ExprP = std::unique_ptr<Expr>;
struct Expr {
    enum Kind { IntLit, FloatLit, Var, Bin, Un, Index, Call, Cast } kind = IntLit;
    int line = 0;
    int64_t ival = 0;
    float fval = 0;
    std::string name;  // Var / Call / Index (array variable)
    std::string op;    // Bin / Un
    ExprP a, b;        // operands (Index: a = index expression)
    std::vector<ExprP> args;
    Ty castTy = Ty::Int;
};

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;
struct Stmt {
    enum Kind { Decl, Assign, IndexAssign, ExprStmt, If, While, Block, Return } kind = Block;
    int line = 0;
    Ty ty = Ty::Int;
    std::string name;
    ExprP e;     // initializer / rhs / return value / expression
    ExprP idx;   // IndexAssign index
    ExprP cond;  // If / While
    std::vector<StmtP> body, elseBody;
};

struct Param { Ty ty; std::string name; };

struct Func {
    bool kernel = false, external = false;
    Ty ret = Ty::Void;
    std::string name;
    std::vector<Param> params;
    std::vector<StmtP> body;
    int line = 0;
};

struct Module { std::vector<Func> funcs; };

}  // namespace mp
