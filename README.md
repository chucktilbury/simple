# Simple language compiler (C++ / LLVM)

Object-oriented language **Simple**: recursive-descent frontend, LLVM IR backend, C runtime with mark-and-sweep GC.

| Doc | What |
|-----|------|
| [docs/LANGUAGE.md](docs/LANGUAGE.md) | Language reference |
| [grammar.ebnf](grammar.ebnf) | Syntax as implemented |
| [docs/sim.md](docs/sim.md) | Package manager `sim` |
| [docs/TESTING.md](docs/TESTING.md) | Unit + functional tests |

## Highlights

- Classes: `create` / `destroy`, fields, methods, `virtual`, `super`
- Multiple inheritance, `public` / `private` / `protected` (default **private**)
- Templates with **explicit** args: `class Box<T>`, `Box<int>`, `show<int>(x)`
- `null` literal; safe `as` / `(Class)` downcasts
- `copy`, `is`, `type()`
- `try` / `except as` / `finally` / `raise`
- `include` / `import` (source merge) + `sim` packages
- `inline { C }` and `inline (vars) { C }`
- `print` with `{}` / `{name}` formats
- Lists, dicts, generators (`yield`)
- DWARF via `-g` (off by default)
- CLI: `-o`, `--emit-llvm`, `-c`, `-v`, `--dump-tokens`, `--dump-ast`

## Build

Needs LLVM 18+ (22 if you have it) and a C++17 compiler.

```bash
# CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Or directly
g++ -std=c++17 -O2 -Iinclude -Iruntime $(llvm-config --cxxflags) \
    -o simplec src/main.cpp src/Lexer.cpp src/Parser.cpp \
       src/Semant.cpp src/Codegen.cpp src/ModuleLoader.cpp \
    $(llvm-config --ldflags --libs)
cc -O2 -Iruntime -fPIC -c runtime/simple_rt.c -o runtime/simple_rt.o
```

## Usage

```bash
./simplec examples/hello.simple -o hello
./hello

./simplec --emit-llvm x.simple -o x.ll
./simplec -c x.simple -o x.o
./simplec x.simple -o x --extra-obj extra.o
./simplec -g x.simple -o x          # DWARF for gdb
./simplec -v --dump-ast x.simple
```

`--emit-obj` is still accepted as an alias of `-c`.

## Tests

```bash
bash tests/run_unit.sh                         # lexer / parser / semant
SIMPLEC=./simplec bash tests/run_tests.sh      # compile + run .simple
```

## Pipeline

```
.simple → Lexer → Parser → include/import → templates
        → Semant (types, visibility) → LLVM IR → .ll | .o | exe + simple_rt.o
```

## Layout

```
include/     Token, Lexer, Ast, Parser, Semant, Codegen, ModuleLoader
src/
runtime/     simple_rt.c / simple_rt.h
examples/
tests/       pass/, fail/, unit/, run_tests.sh, run_unit.sh
docs/
pkgs/        sample sim package (greet)
sim          package manager
grammar.ebnf
```

## License

Free for any use.
