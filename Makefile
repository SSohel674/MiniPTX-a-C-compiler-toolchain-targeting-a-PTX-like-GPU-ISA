# Plain-make alternative to CMake (Linux / macOS / MSYS2 / MinGW)
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra
SRC := $(filter-out src/main.cpp,$(wildcard src/*.cpp))

all: miniptx miniptx_tests

miniptx: $(wildcard src/*.cpp) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) src/*.cpp -o $@

miniptx_tests: $(SRC) tests/test_main.cpp $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -Isrc -DMINIPTX_EXAMPLES_DIR='"examples"' $(SRC) tests/test_main.cpp -o $@

test: miniptx_tests
	./miniptx_tests

demo: miniptx
	./run_demo.sh

clean:
	rm -f miniptx miniptx_tests *.mpx *.mpo examples/*.ptx examples/*.mpo
.PHONY: all test demo clean
