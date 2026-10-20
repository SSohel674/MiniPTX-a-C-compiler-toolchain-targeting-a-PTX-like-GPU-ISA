# MiniPTX — a PTX-style GPU compiler toolchain in C++17

A from-scratch toolchain that mirrors the pieces of NVIDIA's PTX stack: a **compiler front end**, an
**optimizer**, a **PTX-like virtual ISA code generator with register allocation**, an **assembler** producing a
binary **object format** (sections, symbol table, relocations, debug info), a **linker/loader**, a **SIMT GPU
simulator** (warps, divergence), and a **source-level debugger** (a mini cuda-gdb). No external dependencies.

```
 .kc source ──lex/parse──▶ AST ──irgen──▶ IR ──optimize──▶ IR ──codegen/regalloc──▶ .ptx text
 .ptx ──assemble──▶ .mpo (relocatable object) ──link──▶ .mpx (executable) ──load──▶ SIMT simulator ◀── debugger
```

## Quick start

Requirements: a C++17 compiler (g++/clang++; on Windows use MSYS2/MinGW-w64). CMake is optional.

```bash
./build.sh                     # or: make   |   cmake -S . -B build && cmake --build build
./miniptx_tests                # 44 checks
./run_demo.sh                  # guided tour of every stage
```

Windows: run `build.bat` from a MinGW shell (or use the CMake route below).

### VS Code
1. Install the **C/C++** extension (`ms-vscode.cpptools`) and open this folder (`File > Open Folder`).
2. `Ctrl+Shift+B` builds `miniptx` (task *build (g++)*). `Terminal > Run Task > run tests` runs the tests.
3. `F5` has launch configs for debugging the compiler itself, the simulator, and the unit tests.
4. For the CMake route install *CMake Tools*, pick a kit, and build; binaries land in `build/`.

## The kernel language (`.kc`)
C-like, one thread per kernel invocation, 1-D grids. Types `int`, `float`, `int*`, `float*` (pointers only as
parameters, indexed with `a[i]`). Built-ins: `threadIdx`, `blockIdx`, `blockDim`, `gridDim`. Statements: declarations,
assignment (`= += -= *= /= ++ --`), `if/else`, `while`, `for`, `return`. `device` functions (max 4 params,
recursion allowed) are callable from kernels; `extern device ...;` declares a function defined in another module.

```c
kernel saxpy(float alpha, float* x, float* y, int n) {
    int i = blockIdx * blockDim + threadIdx;
    if (i < n) { y[i] = alpha * x[i] + y[i]; }
}
```

## Commands
```
miniptx compile <f.kc> [-o f.ptx] [-O0|-O1] [--emit-ir] [--stats]
miniptx asm     <f.ptx> [-o f.mpo]
miniptx link    <a.mpo> [b.mpo ...] -o prog.mpx
miniptx build   <a.kc> [b.kc ...] -o prog.mpx [-O0|-O1] [--keep]     # compile + asm + link in one step
miniptx objdump <f.mpo|f.mpx>                                        # sections, symbols, relocations, disassembly
miniptx run     <prog.mpx> [--kernel k] --grid G --block B --arg ... [--print N] [--stats] [--debug]
```
`--arg` (kernel parameter order): `i32:5`, `f32:2.5`, `buf:f32:COUNT:iota|ones|zeros|rand`, `buf:i32:COUNT:iota`, `buf:f32:1,2,3`.

```bash
./miniptx build examples/vecadd.kc -o vecadd.mpx
./miniptx run vecadd.mpx --grid 3 --block 50 --arg buf:f32:130:iota --arg buf:f32:130:ones --arg buf:f32:130:zeros --arg i32:130 --stats
./miniptx build examples/mathlib.kc examples/use_mathlib.kc -o apply.mpx      # two modules, linked
```

## Debugger
Compile with `-O0` so every source variable keeps its own register (at `-O1` copy propagation removes some of them,
exactly like real debuggers on optimized code), then add `--debug`:
```bash
./miniptx build examples/saxpy.kc -O0 -o saxpy.mpx
./miniptx run saxpy.mpx --grid 1 --block 40 --arg f32:2 --arg buf:f32:40:iota --arg buf:f32:40:ones --arg i32:36 --debug
(mdb) break 6        (mdb) run        (mdb) locals       (mdb) thread 3     (mdb) print i
(mdb) step / next / stepi             (mdb) bt / list / disas / mask / regs / mem <addr> [n] / help
```
Breakpoints are resolved through `.debug_line`; variables through `.debug_vars` (register + live pc range).

## Architecture (src/)
| File | Stage |
|---|---|
| `lexer`, `parser`, `ast.h` | Front end: tokens → recursive-descent / precedence-climbing parser → AST (`for` is desugared to `while`) |
| `irgen` | Type checking (int/float promotion, pointers, calls) and lowering to a 3-address IR over virtual registers |
| `opt` | Passes (see below), iterated to a fixed point |
| `codegen` | Live-interval analysis (extended across loop back-edges), **linear-scan register allocation** per register class, PTX-style text emission with `.loc`/`.var` debug directives |
| `assembler` | Parses the text, encodes 64-byte fixed-width instructions, builds symbol table, **relocations** and line/variable tables |
| `object` | Binary container (`MPTO` relocatable / `MPTX` executable): `.text .symtab .rela.text .debug_line .debug_file .debug_vars .strtab`; disassembler |
| `linker` | Lays out `.text`, resolves undefined symbols across modules (errors on undefined / duplicate), applies `R_TEXT_REL` and `R_SYM_ABS` relocations, rebases debug info |
| `simulator` | SIMT machine: warps of 32 lock-step threads, **min-PC reconvergence** for divergence, per-thread call stacks, bounds-checked device memory, statistics (SIMD efficiency) |
| `debugger` | Breakpoints, source/instruction stepping, variable & register inspection, backtrace, warp mask |

### Optimizer passes (`--stats` shows counts)
1. Block-local constant propagation, copy propagation, constant folding, algebraic simplification
2. Block-local common-subexpression elimination (value numbering; loads are CSE'd until the next store/call)
3. Unreachable-block, redundant-branch and unused-label elimination (CFG)
4. Global dead-code elimination using backward **liveness dataflow** over basic blocks

`miniptx compile examples/fold_demo.kc --stats --emit-ir` (41 → 16 IR instructions) shows all four at work.

### Virtual ISA (subset of PTX concepts)
Register classes `%r` (s32), `%f` (f32), `%p` (pred), `%rd` (u64 addresses). Instructions: `mov add sub mul div rem neg
and or not setp cvt mul.wide ld.param ld.global st.global bra call ret`, any of them predicable with `@%p` / `@!%p`.

## Known limitations / ideas to extend (great interview talking points)
* The IR uses mutable virtual registers (not SSA). Next step: SSA construction (dominators + phi insertion) and global CSE/GVN.
* Register allocation is linear scan with unlimited physical registers per class (no spilling yet).
* No shared memory / `__syncthreads`, no 2-D/3-D grids, no atomics, no short-circuit `&&`/`||`.
* Simulator is functional (not cycle-accurate). Compare against `nvcc -ptx` output for the same kernel to study real PTX.

## Layout
```
src/       compiler, assembler, linker, simulator, debugger, CLI
examples/  *.kc kernels (vecadd, saxpy, rowsum, collatz, fold_demo, mathlib + use_mathlib)
tests/     test_main.cpp (self-contained runner; also registered with CTest)
.vscode/   tasks, launch configs, IntelliSense settings
```
