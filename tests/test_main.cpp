// Self-contained test runner (no external framework): `miniptx_tests` or `ctest`.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include "assembler.h"
#include "common.h"
#include "compiler.h"
#include "linker.h"
#include "simulator.h"

#ifndef MINIPTX_EXAMPLES_DIR
#define MINIPTX_EXAMPLES_DIR "examples"
#endif

using namespace mp;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << ": " #c "\n"; } } while (0)

static std::string readFile(const std::string& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("cannot open " + p);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static std::string ex(const std::string& n) { return std::string(MINIPTX_EXAMPLES_DIR) + "/" + n; }

static Object build(const std::vector<std::string>& files, int opt, OptStats* st = nullptr) {
    std::vector<Object> objs;
    for (auto& f : files) objs.push_back(assemble(compileToPtx(readFile(ex(f)), f, opt, st)));
    return linkObjects(objs);
}

// small harness: float / int buffers
struct Run {
    Machine m;
    std::vector<uint64_t> args;
    Run(const Object& o) : m(o) {}
    uint64_t bufF(const std::vector<float>& v) {
        uint64_t a = m.alloc(v.size() * 4); m.write(a, v.data(), v.size() * 4); args.push_back(a); return a;
    }
    uint64_t bufI(const std::vector<int32_t>& v) {
        uint64_t a = m.alloc(v.size() * 4); m.write(a, v.data(), v.size() * 4); args.push_back(a); return a;
    }
    void sf(float f) { args.push_back((uint32_t)fToBits(f)); }
    void si(int32_t i) { args.push_back((uint32_t)i); }
    void go(const Object& o, const char* kernel, uint32_t grid, uint32_t block) {
        m.launch(o.findSym(kernel), grid, block, args);
        m.run();
    }
    std::vector<float> getF(uint64_t a, size_t n) { std::vector<float> v(n); m.read(a, v.data(), n * 4); return v; }
    std::vector<int32_t> getI(uint64_t a, size_t n) { std::vector<int32_t> v(n); m.read(a, v.data(), n * 4); return v; }
};

static void testVecAdd() {
    for (int opt = 0; opt <= 1; opt++) {
        Object o = build({"vecadd.kc"}, opt);
        const int n = 130;
        std::vector<float> a(n), b(n), z(n, -1.f);
        for (int i = 0; i < n; i++) { a[i] = (float)i; b[i] = 0.5f * i; }
        Run r(o);
        r.bufF(a); r.bufF(b); uint64_t c = r.bufF(z); r.si(n);
        r.go(o, "vecadd", 3, 50);  // 150 threads, 20 guarded off
        auto out = r.getF(c, n);
        bool ok = true;
        for (int i = 0; i < n; i++) ok &= out[i] == a[i] + b[i];
        CHECK(ok);
    }
}

static void testSaxpyDivergence() {
    Object o = build({"saxpy.kc"}, 1);
    const int n = 70;
    std::vector<float> x(n), y(n, 1.f);
    for (int i = 0; i < n; i++) x[i] = (float)i;
    Run r(o);
    r.sf(2.f); r.bufF(x); uint64_t yb = r.bufF(y); r.si(n);
    r.go(o, "saxpy", 2, 40);
    auto out = r.getF(yb, n);
    bool ok = true;
    for (int i = 0; i < n; i++) ok &= out[i] == 2.f * i + 1.f;
    CHECK(ok);
    CHECK(r.m.stats().divergent > 0);  // bounds check splits the last warp
}

static void testLoop() {
    for (int opt = 0; opt <= 1; opt++) {
        Object o = build({"rowsum.kc"}, opt);
        const int rows = 40, cols = 7;
        std::vector<float> in(rows * cols), out(rows, 0.f);
        for (int i = 0; i < rows * cols; i++) in[i] = (float)(i % 5);
        Run r(o);
        r.bufF(in); uint64_t ob = r.bufF(out); r.si(rows); r.si(cols);
        r.go(o, "rowsum", 2, 32);
        auto res = r.getF(ob, rows);
        bool ok = true;
        for (int row = 0; row < rows; row++) {
            float s = 0; for (int j = 0; j < cols; j++) s += in[row * cols + j];
            ok &= res[row] == s;
        }
        CHECK(ok);
    }
}

static void testCollatz() {
    for (int opt = 0; opt <= 1; opt++) {
        Object o = build({"collatz.kc"}, opt);
        const int n = 100;
        Run r(o);
        uint64_t sb = r.bufI(std::vector<int32_t>(n, -1)); r.si(n);
        r.go(o, "collatz", 4, 32);
        auto res = r.getI(sb, n);
        bool ok = true;
        for (int i = 0; i < n; i++) {
            long x = i + 1; int c = 0;
            while (x != 1) { x = x % 2 == 0 ? x / 2 : 3 * x + 1; c++; }
            ok &= res[i] == c;
        }
        CHECK(ok);
        CHECK(r.m.stats().divergent > 0);
    }
}

static void testMultiModuleLink() {
    Object o = build({"mathlib.kc", "use_mathlib.kc"}, 1);
    CHECK(o.exec);
    CHECK(o.relocs.empty());
    const int n = 12;
    std::vector<float> in(n);
    for (int i = 0; i < n; i++) in[i] = (float)i;
    Run r(o);
    r.bufF(in); uint64_t ob = r.bufF(std::vector<float>(n, 0.f)); r.si(n);
    r.go(o, "apply", 1, 16);
    auto res = r.getF(ob, n);
    bool ok = true;
    int fact[6] = {1, 1, 2, 6, 24, 120};
    for (int i = 0; i < n; i++) ok &= res[i] == (float)i * i * i + (float)fact[i % 6];
    CHECK(ok);
    CHECK(r.m.stats().calls > 0);
    // module order must not matter
    Object o2 = build({"use_mathlib.kc", "mathlib.kc"}, 1);
    Run r2(o2);
    r2.bufF(in); uint64_t ob2 = r2.bufF(std::vector<float>(n, 0.f)); r2.si(n);
    r2.go(o2, "apply", 1, 16);
    CHECK(r2.getF(ob2, n) == res);
}

static void testOptimizer() {
    OptStats s0, s1;
    Object o0 = build({"fold_demo.kc"}, 0, &s0), o1 = build({"fold_demo.kc"}, 1, &s1);
    CHECK(o1.text.size() < o0.text.size());
    CHECK(s1.folded > 0); CHECK(s1.cse > 0); CHECK(s1.dce > 0); CHECK(s1.cfg > 0);
    const int n = 8;
    std::vector<float> res[2];
    for (int k = 0; k < 2; k++) {
        const Object& o = k ? o1 : o0;
        std::vector<float> x(n);
        for (int i = 0; i < n; i++) x[i] = (float)i;
        Run r(o);
        r.bufF(x); uint64_t yb = r.bufF(std::vector<float>(n, 0.f)); r.si(n);
        r.go(o, "demo", 1, 8);
        res[k] = r.getF(yb, n);
    }
    CHECK(res[0] == res[1]);
    CHECK(res[1][3] == 3.f * 3.f * 2.f + 6.f);
    CHECK(res[1][0] != 12345.f);  // the folded-away branch never ran
}

static void testObjectRoundTrip() {
    Object o = build({"mathlib.kc", "use_mathlib.kc"}, 1);
    Object p = deserialize(serialize(o));
    CHECK(p.exec == o.exec);
    CHECK(p.text.size() == o.text.size());
    CHECK(std::memcmp(p.text.data(), o.text.data(), o.text.size() * sizeof(Instr)) == 0);
    CHECK(p.syms.size() == o.syms.size());
    CHECK(p.syms[2].name == o.syms[2].name);
    CHECK(p.lines.size() == o.lines.size());
    CHECK(p.vars.size() == o.vars.size());
    // relocatable object keeps its relocation records
    Object rel = assemble(compileToPtx(readFile(ex("use_mathlib.kc")), "use_mathlib.kc", 1));
    CHECK(!rel.exec);
    CHECK(!rel.relocs.empty());
    Object rel2 = deserialize(serialize(rel));
    CHECK(rel2.relocs.size() == rel.relocs.size());
    bool undef = false;
    for (auto& s : rel2.syms) undef |= s.kind == SYM_UNDEF;
    CHECK(undef);
}

static bool throwsCompile(const std::function<void()>& f) {
    try { f(); } catch (CompileError&) { return true; }
    return false;
}

static void testErrors() {
    CHECK(throwsCompile([] { compileToPtx("kernel k( { }", "x", 1); }));
    CHECK(throwsCompile([] { compileToPtx("kernel k(int n) { y = 1; }", "x", 1); }));
    CHECK(throwsCompile([] { compileToPtx("kernel k(float* p) { p = 1; }", "x", 1); }));
    CHECK(throwsCompile([] { compileToPtx("kernel k(int n) { int a = nope(1); }", "x", 1); }));
    CHECK(throwsCompile([] { compileToPtx("kernel k(int n) { int a = 1; int a = 2; }", "x", 1); }));
    CHECK(throwsCompile([] { compileToPtx("device int f(int a) { return a; } kernel k() { int x = f(1, 2); }", "x", 1); }));
    // undefined symbol at link time
    CHECK(throwsCompile([] {
        auto o = assemble(compileToPtx(readFile(ex("use_mathlib.kc")), "u", 1));
        linkObjects({o});
    }));
    // duplicate definition at link time
    CHECK(throwsCompile([] {
        auto a = assemble(compileToPtx(readFile(ex("mathlib.kc")), "m", 1));
        linkObjects({a, a});
    }));
    // bad assembly
    CHECK(throwsCompile([] { assemble(".visible .entry k()\n{\n  bogus.s32 %r1;\n}\n"); }));
    // illegal memory access traps instead of crashing
    Object o = build({"vecadd.kc"}, 1);
    Machine m(o);
    uint64_t a = m.alloc(16);
    m.launch(o.findSym("vecadd"), 1, 64, {a, a, a, 4096});
    bool trapped = false;
    try { m.run(); } catch (SimError&) { trapped = true; }
    CHECK(trapped);
}

int main() {
    struct T { const char* name; void (*fn)(); } tests[] = {
        {"vecadd (-O0/-O1)", testVecAdd}, {"saxpy + divergence", testSaxpyDivergence}, {"loops / back-edge regalloc", testLoop},
        {"collatz divergence", testCollatz}, {"multi-module link + relocations", testMultiModuleLink},
        {"optimizer", testOptimizer}, {"object round trip", testObjectRoundTrip}, {"error handling", testErrors}};
    for (auto& t : tests) {
        int before = g_fail;
        try { t.fn(); }
        catch (std::exception& e) { g_fail++; std::cerr << "  EXCEPTION in " << t.name << ": " << e.what() << "\n"; }
        std::printf("[%s] %s\n", g_fail == before ? " ok " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
