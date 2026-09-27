#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SC="${SIMPLEC:-}"
if [[ -z "$SC" || ! -x "$SC" ]]; then
  for cand in /tmp/simplec_cpp "$ROOT/build/simplec"; do
    if [[ -x "$cand" ]]; then SC="$cand"; break; fi
  done
fi
if [[ -z "$SC" || ! -x "$SC" ]]; then
  echo "no simplec binary; build first or set SIMPLEC" >&2
  exit 2
fi
echo "using $SC"

if [[ ! -f runtime/simple_rt.o ]]; then
  cc -O2 -Iruntime -fPIC -c runtime/simple_rt.c -o runtime/simple_rt.o
fi

pass=0
fail=0

echo "== pass tests =="
for src in tests/pass/*.simple; do
  base="${src%.simple}"
  name="$(basename "$base")"
  exp="${base}.out"
  out="/tmp/simple_test_$name"
  if ! "$SC" "$src" -o "$out" >/tmp/sc_compile.txt 2>&1; then
    echo "FAIL $name (compile)"
    cat /tmp/sc_compile.txt
    fail=$((fail+1))
    continue
  fi
  if ! "$out" > /tmp/sc_run.txt 2>&1; then
    echo "FAIL $name (runtime)"
    cat /tmp/sc_run.txt
    fail=$((fail+1))
    continue
  fi
  if [[ -f "$exp" ]] && ! diff -u "$exp" /tmp/sc_run.txt; then
    echo "FAIL $name (output)"
    fail=$((fail+1))
    continue
  fi
  echo "ok   $name"
  pass=$((pass+1))
done

echo "== fail tests (must not succeed silently) =="
for src in tests/fail/*.simple; do
  name="$(basename "${src%.simple}")"
  out="/tmp/simple_fail_$name"
  set +e
  "$SC" "$src" -o "$out" >/tmp/sc_compile.txt 2>&1
  c=$?
  set -e
  if [[ $c -ne 0 ]]; then
    echo "ok   $name (rejected at compile)"
    pass=$((pass+1))
    continue
  fi
  set +e
  "$out" >/tmp/sc_run.txt 2>&1
  r=$?
  set -e
  if [[ $r -ne 0 ]]; then
    echo "ok   $name (rejected at runtime)"
    pass=$((pass+1))
  else
    echo "FAIL $name (unexpected success)"
    cat /tmp/sc_run.txt
    fail=$((fail+1))
  fi
done

echo
echo "passed=$pass failed=$fail"
[[ $fail -eq 0 ]]
