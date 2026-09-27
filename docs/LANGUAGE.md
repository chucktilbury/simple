# The Simple programming language

Reference for the compiler in this repository (`simplec`, C++ frontend, LLVM IR backend, C runtime).  
Formal syntax: [`grammar.ebnf`](../grammar.ebnf).

Simple is a small object-oriented language. A program is a list of declarations plus one `start` block (the entry point). Memory is garbage-collected. The compiler produces a native executable (or `.ll` / `.o`).

---

## 1. Program structure

```simple
include "helpers.simple"
import "lib.simple" as L
import greet as G          # package name via simple.paths / sim

namespace Util { ... }

class Box { ... }

int add (int a, int b) { return (a + b) }

start {
    print add(1, 2)
}
```

- `start { ... }` is required for an executable.
- Only one useful `start` per compile; extra `start` blocks are not a hard error.
- Library files used with `include` / `import` should omit `start`.

### Comments

```
# to end of line
// to end of line
/* block comment */
```

---

## 2. Types

| Name | LLVM / runtime | Notes |
|------|----------------|--------|
| `int` / `integer` | i64 | default integer |
| `unsigned` | i64 | not fully distinct at runtime |
| `float` | double | arithmetic is still thin (prefer `int` in examples) |
| `bool` / `boolean` | i1 | `1`/`0` often printed |
| `string` | pointer | GC string |
| `list` | pointer | untyped elements |
| `dict` | pointer | string/ident keys in literals |
| `nothing` | void | functions with no value |
| *class name* | pointer to object | `Dog`, `Box_int`, … |
| `null` | null pointer | literal; `type(null)` is `"null"` |

`type(x)` returns a string. For objects it uses the **runtime** class name when set (`Dog`), otherwise the static name.

`print null` and `type(null)` print `null`. A class variable that holds `null` compares false with `is Class`. `print var` / `type(var)` after `var = null` may still use the **static** class name or a raw pointer print — prefer `print null` and `is` for tests.

---

## 3. Variables and assignment

```simple
int n
n = 3
int m = 4          # if you write type + name + '='
string s
s = "hi"
```

Class-typed locals:

```simple
Dog d
d = Dog.create("Rex")
```

`d = Dog.create(...)` **binds** the new object.  
`d2 = d` **copy-constructs or copy-assigns** (memberwise), except when the RHS is `Class.create(...)`.  
`d2 = copy d` always clones.

If the RHS class is a **derived** class of the LHS class (`Animal a` / `a = d` where `d` is `Dog`), Simple **stores the same pointer** (upcast). `a.speak()` then uses the object’s vptr, so a `virtual` method runs as `Dog.speak`. Same-class `d2 = d1` is still copy-assign. `copy d` still clones.

---

## 4. Expressions

### Operators (low → high)

`or` `||` → `and` `&&` → `== != equ neq` → comparisons → `+ -` → `* / %` → `^` (right-assoc)

`^` is **bitwise XOR**, not exponentiation.

Word forms: `lt gt lte gte equ neq and or not`.

Unary: `-`, `!`, `not`, `copy`.

### Other forms

```simple
type(x)
x is Dog
x as Dog              # null if not that class / subclass
(Dog) x               # same check for class types
(int) f
super Base.create()
super Base.method()
foo<int>(1)
Box<int>.create(7)
xs[i]
name.method(args)
"hello {} {who}"("world", who="A")
```

Safe downcast (`as` / `(Class)`) uses `s_is_classname`. Failure is **null**, not an exception.

---

## 5. Statements

```simple
print expr
print "x={}"(x)
return
return (expr)
raise (ErrorName)
break
continue
yield (expr)
exit (code)
inline { /* C */ }
inline (n, msg) { n = n + 1; }

if (cond) { ... }
else (cond) { ... }     # else-if
else if (cond) { ... }
else () { ... }
else { ... }

while (cond) { ... }
do { ... } while (cond)
for (n as int i) { ... }   # counted 0 .. n-1

try { ... }
except (Name) { ... }
except (Name as err) { ... }
finally { ... }
```

`except as err` binds a **local string** (exception name).

`yield` turns the function into a generator (state machine). Driving it with `for (gen() as x)` is incomplete; defining a yielding function compiles.

---

## 6. Functions

```simple
int add (int a, int b) {
    return (a + b)
}

nothing speak () {
    print "hi"
    return ()
}

nothing show <T> (T x) {     # explicit type args
    print "{}"(x)
    return ()
}

# use
show<int>(3)
```

Out-of-class method:

```simple
class Greeter {
    public
    int hello ()          # declare in the class
}
int Greeter.hello () {    # define later
    print name
    return (0)
}
```

An out-of-class body is an **error** if that method/constructor/destructor was not declared in the class:

```
[semantic] error: 'Adder.plus' is not declared in class 'Adder'
```

---

## 7. Classes

```simple
class Animal {
    public
    string kind

    public
    create () {
        kind = "animal"
    }

    public
    destroy {
        print "gone"
    }

    public
    virtual nothing speak () {
        print "..."
        return ()
    }
}

class Dog (public Animal) {
    public
    string name

    public
    create (string n) {
        name = n
    }

    public
    virtual nothing speak () {
        print "{} woofs"(name)
        return ()
    }
}
```

### Rules

- Visibility keywords are **section markers**. Default in a class is **private**.
- `public` / `private` / `protected` stay in effect until the next marker.
- Compile-time checks plus **runtime** `s_access_or_die` on field/method use.
- Multiple inheritance: `class Duck (public Flyer, public Swimmer)`.
- Default 0-arg base `create` is called from a derived `create`.
- `super Base.m(...)` is only for an **explicit** base call.
- `virtual` methods without a body are abstract (stub that prints if called).
- `destroy` emits `Class_destroy(self)`. **GC does not call it.** Call `obj.destroy()` yourself.
- Templates: `class Box <T>` then `Box<int>` → monomorphized class `Box_int`. Args are **required**.

### Object operations

```simple
d = Dog.create("Rex")
print d.name
d.speak()
d.name = "Max"
print (d is Animal)
e = d as Dog
f = copy d
d.destroy()
```

---

## 8. Modules

| Form | Meaning |
|------|---------|
| `include "file.simple"` | Parse and splice declarations into this module |
| `import "file.simple"` | Same splice |
| `import "file.simple" as A` | Splice under `namespace A` |
| `import greet as G` | Resolve `greet` via `simple.paths` (see `sim`) |

Paths are relative to the file that contains the directive. Cycles are errors.

This is **source merge**, not linking a second `.o`. One `simplec` invocation → one executable.

---

## 9. Templates

```simple
class Pair <A, B> {
    public
    A first
    public
    B second
    public
    create (A x, B y) {
        first = x
        second = y
    }
}

Pair<int, string> p
p = Pair<int, string>.create(1, "one")
```

No inference, no specialization, no constraints. Errors appear on the instantiated copy.

---

## 10. Inline C

```simple
inline {
    printf("hi\n");
}

int n
n = 1
inline (n) {
    n = n + 1;
}
```

The brace body is raw C, compiled with `cc` and linked. Named imports become pointers/`#define` wrappers so C can read and write Simple locals.

---

## 11. Runtime

`runtime/simple_rt.c` provides:

- Mark-and-sweep GC (`s_gc_alloc`)
- Strings, format helpers, `print`
- Lists and dicts
- Exceptions (`setjmp` / `longjmp`)
- Class name registry (`is` / `as`)
- Visibility tables
- Generators (`SGen`)

The driver links this automatically when producing an executable.

---

## 12. Compiler CLI

```
simplec [options] file.simple
  -o FILE             output (default a.out)
  --emit-llvm         write .ll
  -c                  write .o   (--emit-obj still works)
  -g, --debug-info    DWARF (off by default)
  --no-debug-info
  -v, --verbose
  --dump-tokens
  --dump-ast
  --no-semant
  --extra-obj FILE   extra .o to link (repeatable)
  -h
```

```bash
simplec examples/hello.simple -o hello
simplec -g examples/hello.simple -o hello   # gdb
simplec --emit-llvm x.simple -o x.ll
```

Implementation language: C++. IR: LLVM. Link: system `cc` + `simple_rt.o`.

---

## 13. Package manager (`sim`)

See [`sim.md`](sim.md).

```bash
./sim init
./sim registry-init
./sim publish pkgs/greet
./sim add greet@1.0.0
./sim build
```

`import greet as G` works after `simple.paths` is written.

---

## 14. Tests

See [TESTING.md](TESTING.md).

```bash
bash tests/run_unit.sh
SIMPLEC=./simplec bash tests/run_tests.sh
```

---

## 15. What is intentionally thin or missing

- No interfaces, static fields, or operator overloading

---

## 16. Quick example

```simple
class Counter {
    public
    int n
    public
    create () {
        n = 0
    }
    public
    nothing inc () {
        n = n + 1
        return ()
    }
}

start {
    Counter c
    c = Counter.create()
    c.inc()
    print "n={} type={}"(c.n, type(c))
}
```
