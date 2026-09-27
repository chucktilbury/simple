# sim — Simple package manager

## Commands

```
sim init [name]
sim registry-init [dir]
sim publish <pkg-dir> [registry]
sim list
sim add name@version
sim install
sim build [entry] [outfile]
```

Environment:

- `SIMPLE_REGISTRY` — registry root (default `./registry` or `~/.simple/registry`)
- `SIMPLEC` — path to the compiler

## Package layout

```
greet/
  simple.toml
  src/greet.simple
```

```toml
name = "greet"
version = "1.0.0"
entry = "src/greet.simple"
```

Do not put `start { }` in a library package.

## App

```toml
name = "demo"
version = "0.1.0"
entry = "src/main.simple"

[depends]
greet = "1.0.0"
```

```simple
import greet as G

start {
    G.hello("world")
}
```

`sim add greet@1.0.0` copies the package into `vendor/` and writes `simple.paths`.  
`simplec` reads `simple.paths` and resolves `import greet`.

## Registry

A directory:

```
registry/index.txt          # name version relative-path
registry/greet/1.0.0/...
```

`sim publish ./pkgs/greet` installs a package into the registry.


## Linking model

`import` / `include` splice **source** into one LLVM module. `sim build` then:

1. Compiles `native/*.c` and `vendor/**/native/*.{c,cpp}` to `.o`
2. `simplec entry.simple -o app --extra-obj native/foo.o ...`
   (`simplec` links those with `simple_rt.o`)
