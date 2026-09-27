# Testing Simple

## Unit tests (no LLVM)

Lexer, parser, and semantic analysis only.

```bash
bash tests/run_unit.sh
```

Source: `tests/unit/test_compiler.cpp`  
Links: `Lexer.cpp`, `Parser.cpp`, `Semant.cpp`

Expect a summary like `unit tests passed=N failed=0`.  
Parse/semant messages on stderr are from **negative** cases (`print (`, `zzz`, `Nope`, `null` at module level).

## Functional tests (full compiler)

Each `tests/pass/*.simple` is compiled with `simplec` and run. Stdout must match the sibling `*.out` file.

Each `tests/fail/*.simple` must fail at compile time **or** at runtime (non-zero).

```bash
SIMPLEC=/path/to/simplec bash tests/run_tests.sh
```

The runner looks for `$SIMPLEC`, then `/tmp/simplec_cpp`. It will not use a non-executable `./simplec` blob.

Helpers:

- `tests/pass/mods/` — `import` fixture  
- `tests/pass/inc/` — `include` fixture  

## What is covered

| Area | Unit | Functional |
|------|------|------------|
| Keywords / tokens | yes | — |
| Templates `<T>` | parse + semant | `template_*` |
| `null` | token + `print null` | `nullval`, `null_print`, `null_is` |
| Classes / create / destroy | parse + semant | `objects`, `destroy`, `destroy2` |
| `is` / `as` | parse | `istype`, `asfail`, `is_inherit` |
| Exceptions | parse | `except`, `finally`, `twoexcept`, `nestedtry` |
| Modules | parse import/include | `import_mod`, `include_mod` |
| Out-of-class `Class.method` | parse | `adder_plus` (defines + constructs; call still thin) |

## Adding a test

**Pass**

1. `tests/pass/foo.simple`
2. `tests/pass/foo.out` — exact stdout (including newline)

**Fail**

1. `tests/fail/foo.simple` — must not produce a successful run

**Unit**

Add a `CHECK` / `CHECK_EQ` in `tests/unit/test_compiler.cpp` and run `bash tests/run_unit.sh`.
