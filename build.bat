@echo off
REM Windows (MinGW-w64 g++ on PATH, e.g. via MSYS2). Run from this folder.
g++ -std=c++17 -O2 -g -Wall -Wextra src\lexer.cpp src\parser.cpp src\irgen.cpp src\opt.cpp src\codegen.cpp src\compiler.cpp src\object.cpp src\assembler.cpp src\linker.cpp src\simulator.cpp src\debugger.cpp src\main.cpp -o miniptx.exe
if errorlevel 1 exit /b 1
g++ -std=c++17 -O1 -Isrc -DMINIPTX_EXAMPLES_DIR=\"examples\" src\lexer.cpp src\parser.cpp src\irgen.cpp src\opt.cpp src\codegen.cpp src\compiler.cpp src\object.cpp src\assembler.cpp src\linker.cpp src\simulator.cpp src\debugger.cpp tests\test_main.cpp -o miniptx_tests.exe
echo built miniptx.exe and miniptx_tests.exe
