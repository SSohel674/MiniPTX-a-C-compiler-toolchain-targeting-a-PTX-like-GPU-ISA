#include "compiler.h"
#include "codegen.h"
#include "irgen.h"
#include "lexer.h"
#include "parser.h"

namespace mp {

std::string compileToPtx(const std::string& src, const std::string& filename, int optLevel, OptStats* stats,
                         std::string* irDump) {
    auto toks = lex(src);
    Module m = parseModule(toks);
    IRModule ir = generateIR(m, filename);
    OptStats local;
    if (optLevel > 0)
        for (auto& f : ir.funcs) optimize(f, local);
    if (stats) *stats = local;
    if (irDump) *irDump = dumpIR(ir);
    return emitPtx(ir);
}

}  // namespace mp
