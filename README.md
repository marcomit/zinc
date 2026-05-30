# Zinc

A statically-typed, compiled systems programming language. Compiles to native binaries via LLVM.

## Requirements

- `clang`
- LLVM (with `llvm-config` on `PATH`)

## Build

```sh
make
```

This produces a `zinc` binary in the current directory.

## Usage

```sh
zinc <file> [options]
```

| Option | Description |
|---|---|
| `--emit-llvm` | Output LLVM IR (`.ll`) instead of a native binary |
| `-o <file>`, `--output <file>` | Specify the output file name |
| `-d`, `--debug` | Enable debug output |
| `--unused-variable` | Suppress unused variable warnings |
| `--unused-function` | Suppress unused function warnings |
| `--unused-struct` | Suppress unused struct warnings |

Example:

```sh
zinc hello.zn -o hello
./hello
```

## Tests

```sh
./run_tests.sh
```

## Documentation

See `docs/` for the language reference and compiler internals, and `examples/` for sample programs.

## License

BSD 3-Clause License. See [LICENSE](LICENSE).
