cmake_minimum_required(VERSION 3.16)
project(SimpleCompiler LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Locate llvm-config (prefer 22 → 18)
# ---------------------------------------------------------------------------
set(_LLVM_CONFIG_CANDIDATES
  llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18
  llvm-config
)
set(LLVM_CONFIG "")
foreach(c ${_LLVM_CONFIG_CANDIDATES})
  find_program(_cand ${c})
  if(_cand)
    set(LLVM_CONFIG ${_cand})
    break()
  endif()
endforeach()

if(NOT LLVM_CONFIG)
  message(FATAL_ERROR "llvm-config not found. Install llvm-dev (e.g. llvm-22-dev).")
endif()
message(STATUS "Using LLVM_CONFIG: ${LLVM_CONFIG}")

execute_process(COMMAND ${LLVM_CONFIG} --version
  OUTPUT_VARIABLE LLVM_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG} --cxxflags
  OUTPUT_VARIABLE LLVM_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG} --ldflags
  OUTPUT_VARIABLE LLVM_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG} --system-libs
  OUTPUT_VARIABLE LLVM_SYSTEM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
# Prefer the single shared lib when available
execute_process(COMMAND ${LLVM_CONFIG} --libs
  OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG} --includedir
  OUTPUT_VARIABLE LLVM_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG} --libdir
  OUTPUT_VARIABLE LLVM_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "LLVM version: ${LLVM_VERSION}")
message(STATUS "LLVM include: ${LLVM_INCLUDE_DIR}")

# Turn flag strings into CMake lists
separate_arguments(LLVM_CXXFLAGS_LIST NATIVE_COMMAND ${LLVM_CXXFLAGS})
separate_arguments(LLVM_LDFLAGS_LIST NATIVE_COMMAND ${LLVM_LDFLAGS})
separate_arguments(LLVM_LIBS_LIST NATIVE_COMMAND ${LLVM_LIBS})
separate_arguments(LLVM_SYSTEM_LIBS_LIST NATIVE_COMMAND ${LLVM_SYSTEM_LIBS})

# ---------------------------------------------------------------------------
# Runtime (static + .o for the driver)
# ---------------------------------------------------------------------------
add_library(simple_rt STATIC runtime/simple_rt.c)
target_include_directories(simple_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_compile_options(simple_rt PRIVATE -fPIC -g -O2)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/runtime)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/runtime/simple_rt.o
  COMMAND ${CMAKE_C_COMPILER} -O2 -fPIC -g -c
          ${CMAKE_CURRENT_SOURCE_DIR}/runtime/simple_rt.c
          -o ${CMAKE_BINARY_DIR}/runtime/simple_rt.o
          -I${CMAKE_CURRENT_SOURCE_DIR}/runtime
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/simple_rt.c
          ${CMAKE_CURRENT_SOURCE_DIR}/runtime/simple_rt.h
  COMMENT "Compiling runtime/simple_rt.o"
  VERBATIM
)
add_custom_target(simple_rt_obj ALL DEPENDS ${CMAKE_BINARY_DIR}/runtime/simple_rt.o)

# ---------------------------------------------------------------------------
# simplec compiler
# ---------------------------------------------------------------------------
add_executable(simplec
  src/main.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/Semant.cpp
  src/Codegen.cpp
  src/ModuleLoader.cpp
)

target_include_directories(simplec PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  ${LLVM_INCLUDE_DIR}
)

target_compile_options(simplec PRIVATE
  -Wall -Wextra
  -Wno-deprecated-declarations
  -g
  ${LLVM_CXXFLAGS_LIST}
)

# Drop conflicting flags llvm-config may inject if already set
# (CMake adds -std=c++17 itself)
target_link_directories(simplec PRIVATE ${LLVM_LIBDIR})
target_link_options(simplec PRIVATE ${LLVM_LDFLAGS_LIST})
target_link_libraries(simplec PRIVATE ${LLVM_LIBS_LIST} ${LLVM_SYSTEM_LIBS_LIST})

add_dependencies(simplec simple_rt_obj)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS simplec RUNTIME DESTINATION bin)
install(TARGETS simple_rt ARCHIVE DESTINATION lib)
install(FILES runtime/simple_rt.h DESTINATION include)
install(FILES ${CMAKE_BINARY_DIR}/runtime/simple_rt.o DESTINATION lib OPTIONAL)
install(DIRECTORY examples/ DESTINATION share/simple/examples)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME help COMMAND $<TARGET_FILE:simplec> --help)
add_test(NAME emit_llvm
  COMMAND $<TARGET_FILE:simplec> --emit-llvm
          ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.simple
          -o ${CMAKE_BINARY_DIR}/hello.ll
)
