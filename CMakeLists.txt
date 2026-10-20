cmake_minimum_required(VERSION 3.14)
project(MiniPTX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(miniptx_core STATIC
  src/object.cpp src/lexer.cpp src/parser.cpp src/irgen.cpp src/opt.cpp src/codegen.cpp
  src/compiler.cpp src/assembler.cpp src/linker.cpp src/simulator.cpp src/debugger.cpp)
target_include_directories(miniptx_core PUBLIC src)

add_executable(miniptx src/main.cpp)
target_link_libraries(miniptx PRIVATE miniptx_core)

enable_testing()
add_executable(miniptx_tests tests/test_main.cpp)
target_link_libraries(miniptx_tests PRIVATE miniptx_core)
target_compile_definitions(miniptx_tests PRIVATE MINIPTX_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
add_test(NAME miniptx_tests COMMAND miniptx_tests)
