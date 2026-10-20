#!/usr/bin/env bash
# One-command build without CMake/make:  ./build.sh
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -g -Wall -Wextra src/*.cpp -o miniptx
g++ -std=c++17 -O1 -Isrc -DMINIPTX_EXAMPLES_DIR='"examples"' $(ls src/*.cpp | grep -v main.cpp) tests/test_main.cpp -o miniptx_tests
echo "built ./miniptx and ./miniptx_tests"
