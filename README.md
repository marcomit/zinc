# Zinc

Zinc is a statically-typed, compiled systems programming language with a focus on clarity, performance, and expressiveness.
It compiles to native binaries via LLVM and supports generics, receiver functions (methods), a hygienic macro system, and C interoperability through foreign declarations.

---

## Requirements

- `clang`
- LLVM (with `llvm-config` on `PATH`)

---

## Usage

```sh
make
```
This produces a `zinc` binary in the current directory.

```
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

**Example:**

```sh
zinc hello.zn -o hello
./hello
```

```sh
zinc hello.zn --emit-llvm -o hello.ll
```

---

## Language Overview

### Types

| Zinc type | Description |
|---|---|
| `u0` | No value (function return) |
| `u1` | Boolean |
| `char` | Character |
| `i8`, `i16`, `i32`, `i64` | Signed integers |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers |
| `f32`, `f64` | Floating-point |
| `*T` | Pointer to `T` |
| `[T]` | Array of `T` |
| `(T, U, ...)` | Tuple |

---

### Variables

Explicit type:

```zinc
i32 x = 42
f64 pi = 3.14159
```

Inferred type (`:=`):

```zinc
x := 42
name := "zinc"
```

---

### Functions

```zinc
i32 add(i32 a, i32 b) {
    return a + b
}
```

---

### Structs

```zinc
struct Point {
    f32 x
    f32 y
}
```

Struct literal:

```zinc
Point p = Point { x: 1.0, y: 2.0 }
```

---

### Receiver Functions (Methods)

A receiver function takes an explicit receiver after the parameter list. The syntax is:

```
ReturnType functionName(params) ReceiverType self { ... }
```

```zinc
f32 length(f32 scale) Vec self {
    return scale * self.x
}

// Call:
v.length(2.0)
```

Receiver with pointer:

```zinc
void push(i32 item) *Vec self {
    self.data[self.len] = item
    self.len = self.len + 1
}
```

Static-style receiver (no instance arguments):

```zinc
u0 Point:print() {
    printf("Point\n")
}

// Call:
Point:print()
```

---

### Generics

Generic structs:

```zinc
struct Vec[T] {
    *T data
    u64 len
    u64 cap
}

struct Pair[K, V] {
    K key
    V value
}
```

Generic functions:

```zinc
T max[T](T a, T b) {
    if a > b {
        return a
    }
    return b
}

Pair[K, V] makePair[K, V](K key, V value) {
    Pair[K, V] p
    p.key = key
    p.value = value
    return p
}
```

Generic receiver functions:

```zinc
void push[T](T item) *Vec[T] self {
    self.data[self.len] = item
    self.len = self.len + 1
}
```

Usage:

```zinc
Vec[i32] numbers
numbers.push(10)
numbers.push(20)

result := max(a, b)
```

---

### Control Flow

**If / else:**

```zinc
if x > 0 {
    return x
} else {
    return -x
}
```

**While:**

```zinc
while i < 10 {
    i = i + 1
}
```

**For:**

```zinc
for i := 0; i < n; i = i + 1 {
    // ...
}
```

**Break / continue / return / defer:**

```zinc
while true {
    if done { break }
    defer cleanup()
    // ...
}
```

**Goto and labels:**

```zinc
start:
    if x < 0 { goto end }
    x = x - 1
    goto start
end:
```

---

### Pointers and Arrays

```zinc
i32 x = 5
i32 y = 10
swap(&x, &y)      // pass by pointer

*i32 ptr = &x
i32 val = *ptr    // dereference

[i32] arr = [1, 2, 3, 4, 5]
i32 first = arr[0]
```

---

### Tuples

```zinc
tuple := (1, 2, 3, 4, 5)
```

---

### Type Aliases

```zinc
type PairList[K, V] Vec[Pair[K, V]]
type NodePtr[T]     *Node[T]
```

---

### Visibility

Zinc support only two types of visibility:
**Public**: The declaration is public everywhere in every other file.
**Private**: Visible only in the current file.

All file-level declaration supports the keyword `pub` before the declaration. 

```zinc
pub struct Point { ... }      // explicit pub annotation also supported

pub void Push(i32 item) ...   // public method
void internal() ...           // private
```

---

### Modules

Import another Zinc file with `use`:

```zinc
use "path/to/Module"
```

All public symbols from the imported file become available in the current scope.

---

### Foreign Functions (C FFI)

Declare external C functions with `foreign`:

```zinc
foreign void printf(*char, ...)
foreign void putchar(u8)
```

These can then be called like normal Zinc functions.

---

### Macros

Zinc macros use pattern matching to expand to arbitrary code at compile time. Patterns use:

- `$name` — captures an expression or statement block
- `@name` — captures an identifier (introduces a new variable)
- Literal keywords for fixed tokens

```zinc
macro unless $cond $body -> {
    if !($cond) {
        $body
    }
}
```

```zinc
macro for @item from $start to $end $body -> {
    @item := $start
    while @item < $end {
        $body
        @item = @item + 1
    }
}
```

Usage:

```zinc
unless is_valid {
    return
}

for i from 0 to 10 {
    print(i)
}
```

Macros can use `goto` and labels internally, making them capable of expressing complex control structures:

```zinc
macro while $cond $body {
    entry:
    if !($cond) goto end_while
    $body
    goto entry
    end_while:
}
```

---

## Examples

### Hello World

```zinc
foreign u0 printf(*char)

u32 main() {
    printf("Hello, World!\n")
    return 0
}
```

### Struct with Method

```zinc
foreign u0 printf(*char)

struct Point {
    f32 x
    f32 y
}

u0 Point:print() {
    printf("Point\n")
}

u32 main() {
    Point p = Point { x: 10, y: 10 }
    Point:print()
    return 0
}
```

### Generic Stack

```zinc
struct Vec[T] {
    *T  data
    u64 len
    u64 cap
}

void push[T](T item) *Vec[T] self {
    self.data[self.len] = item
    self.len = self.len + 1
}

void main() {
    Vec[i32] numbers
    numbers.len = 0
    numbers.cap = 0
    numbers.push(10)
    numbers.push(20)
    numbers.push(30)
}
```

### Iterator with Receiver Functions

```zinc
struct range {
    i64 start
    i64 end
    i64 current
}

i64 next() *range self {
    self.current = self.current + 1
    return self.current
}

bool hasNext() *range self {
    return self.current < self.end - 1
}

range Range(i64 start, i64 end) {
    return range { start: start, end: end, current: 0 }
}
```

---

## Compiler Architecture

| Phase | File | Responsibility |
|---|---|---|
| Lexical | `zlex.c` | Tokenize source text; keyword hashing |
| Syntax | `zparse.c` | Build AST from token stream |
| Semantic | `zsem.c` | Type checking, scope and symbol resolution |
| Generate | `zgen.c` | Emit LLVM IR; compile to native binary |

Supporting components:

- **`zmod.c`** — handles `use` imports, multi-file compilation, debug printing
- **`zmacro.c`** — expands macro invocations before/during parsing using pattern matching
- **`zmem.c`** — arena allocator; scoped cleanup with `startScope()` / `endScope()`
- **`zvec.h`** — header-only generic dynamic array used throughout the compiler
- **`zhset.h`** — header-only string hashset for duplicate detection in symbol tables

---

## License

BSD 3-Clause License. See [LICENSE](LICENSE).
