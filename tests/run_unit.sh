#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
g++ -std=c++17 -g -O0 -Iinclude -Iruntime \
  -o /tmp/simple_unit \
  tests/unit/test_compiler.cpp src/Lexer.cpp src/Parser.cpp src/Semant.cpp
/tmp/simple_unit
