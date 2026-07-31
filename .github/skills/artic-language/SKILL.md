---
name: artic-language
description: "Use when writing, reviewing or debugging Artic or Impala source code (.art / .impala files) for AnyDSL — includes syntax for fn/struct/enum/mod/implicit declarations, the unusual for-loop generator idiom, partial-evaluation annotations (@, $, ?), struct literals using '=' instead of ':', and how to validate a file with the artic compiler. Also use when authoring .art test fixtures for the artic language server."
---

# Writing Artic source code

Artic is AnyDSL's successor to Impala. The syntax is Rust-like but **not Rust** — the
differences below are where mistakes actually happen.

## Always validate

Never assume a snippet compiles. Check it:

```powershell
artic-lsp/buildGcc/bin/artic.exe --no-color path/to/file.art
```

Exit code 0 means it compiled. Pass every file of a project in one invocation —
declarations are visible across files given to the same command, and order does not matter.
Build the compiler with `cmake --build artic-lsp/buildGcc --parallel` (the default target
includes it).

## The five things that trip people up

| Trap | Wrong | Right |
| ---- | ----- | ----- |
| Struct literals use `=` | `S { x: 1 }` | `S { x = 1 }` |
| Struct *types* still use `:` | `struct S { x = i32 }` | `struct S { x: i32 }` |
| Tuples are not callable | `t(1)` | `t.1` |
| Generics use brackets | `S<T>` | `S[T]` |
| `for` needs a generator fn | `for i in 0..10` | `for i in range(0, 10)` |

## Declarations

```rust
fn add(a: i32, b: i32) -> i32 { a + b }   // block body
fn mul(a: i32, b: i32) = a * b;           // expression body, needs the ';'
fn inferred() = 1;                        // return type deduced

struct Vec2 { x: f32, y: f32 }            // record form
struct Pair(i32, i64);                    // tuple-like form
struct Unit;                              // empty form

enum Option[T] { None, Some(T) }
enum Shape { Dot, Line(i32), Box { w: i32, h: i32 } }

type Real = f32;                          // type alias

static counter = 0;                       // immutable global
static mut total = 0;                     // mutable global

mod geometry { fn area() = 1; }           // modules are order-independent
use geometry as geo;                      // optional rename with 'as'
```

Return types are deduced only when the function is **neither recursive nor uses `return`**.
Recursive functions must be annotated:

```rust
fn fact(n: i32) -> i32 = if n <= 1 { 1 } else { n * fact(n - 1) };
```

Generic parameters go in brackets and are explicit at the use site when inference cannot
resolve them:

```rust
fn select[T](c: bool, a: T, b: T) -> T = if c { a } else { b };
fn chosen() = select[i32](true, 0, 1);
```

`static` initializers must be compile-time constants, so a call like the above cannot
appear at `static` scope.

## Types

Primitives: `bool`, `i8` `i16` `i32` `i64`, `u8` `u16` `u32` `u64`, `f16` `f32` `f64`.

```rust
[i32 * 4]         // fixed-size array
&[i32]            // slice / unsized array reference
&i32              // immutable reference
&mut i32          // mutable reference
(i32, f32)        // tuple
fn(i32) -> bool   // function type
simd[i32 * 4]     // vector type
&addrspace(1)i32  // reference in an explicit address space
```

Literals take their type from context; a bare integer defaults to `i32`.

```rust
let a: u8 = 1;    // u8
let b = 1;        // i32
let c: f32 = 1;   // f32
let s = "abcd";   // &[u8], also coercible to [u8 * 5] (trailing NUL)
```

Annotate any expression with `:` — `let x: i32 = (1: i32): i32;`

## Expressions and patterns

```rust
let p = Vec2 { x = 1.0, y = 2.0 };
let q = p.{ y = 3.0 };            // structure update, q.x stays 1.0
let n = p.x;                      // field projection
let t = (1, 2); let first = t.0;  // tuple projection by constant

let arr = [1, 2, 3];
let zeros = [0; 4];               // [0, 0, 0, 0]
let elem = arr(1);                // arrays are indexed with ()

let sign = if n > 0.0 { 1 } else { -1 };

match shape {
    Shape::Dot => 0,
    Shape::Line(len) => len,
    Shape::Box { w = w_, ... } => w_,   // '...' ignores remaining fields
}

if let Option[i32]::Some(v) = maybe { use_it(v) }
while let Option[i32]::Some(v) = next() { use_it(v) }
```

Mutability is opt-in with `mut`, on locals, parameters, statics and references:

```rust
fn bump(mut x: i32) -> i32 { x += 1; x }
let mut buf = [1, 2];
buf(1) = 0;
let r = &mut buf;
```

Casts use `as`: `a as i64`. Operators follow C/Rust precedence, including `+= -= *= /= %=
&= |= ^= <<= >>=` and `++ --`.

## The for-loop idiom

This is the biggest departure from Rust. `for` does not iterate a range — it takes a
**generator**: a function whose first parameter is the loop body.

```rust
fn @range(body: fn(i32) -> ()) -> fn(i32, i32) -> () {
    fn @(?beg & ?end) loop(beg: i32, end: i32) -> () {
        if beg < end {
            @body(beg);
            loop(beg + 1, end)
        }
    }
    loop
}

fn sum_to(n: i32) -> i32 {
    let mut acc = 0;
    for i in range(0, n) { acc += i; }
    acc
}
```

`for i in range(0, n) { .. }` desugars to `range(|i| { .. })(0, n)`.
So a generator must return a function accepting the arguments written in the call.
`break()` and `continue()` are called like functions.

Because `for` depends on a generator, **a file using `for` must define or import one** —
this is the most common cause of "cannot find `range`" in fixtures.

## Attributes

```rust
#[export] fn visible() -> i32 { 1 }
#[import(cc = "C", name = "puts")] fn puts(_: &[u8]) -> i32;
#[import(cc = "builtin")] fn sizeof[T]() -> i64;
```

`#[export]` only works for first-order functions (no function-typed parameters).
`#[import]` declarations have no body and end with `;`.

## Implicits

Ad-hoc polymorphism resolved by **type**, not by name. An `implicit` parameter may be
omitted at the call site and is then summoned from the enclosing scopes.

```rust
struct Emergency { number: i32 }

fn call_help(implicit e: Emergency) = e.number;

mod eu {
    implicit super::Emergency = super::Emergency { number = 112 };
    #[export] fn dial() = super::call_help();
}
```

Implicit parameters must come last. `implicit = value;` lets the type be inferred, but
prefer the explicit `implicit T = value;` form.

## Partial evaluation annotations

AnyDSL-specific, used to steer Thorin's partial evaluator:

- `fn @f(..)` — always inline / evaluate at compile time.
- `@f(x)` — force inlining at this call site.
- `fn @(cond) f(..)` — inline only when `cond` holds (a *filter*).
- `?x` — true when `x` is a compile-time constant.
- `$x` — hide `x` from the partial evaluator.

## Reference material

Authoritative examples live in the submodule and are the best source of truth:

- `artic/test/simple/*.art` — one feature per file (`for.art`, `match1.art`, `mut.art`,
  `implicit1.art`, `structs1.art`, `enums1.art`, `types.art`, `ops.art`, ...).
- `artic/test/failure/*.art` — constructs that must be rejected.
- `artic/README.md` — the "Syntax" section lists every deviation from Impala.
- `artic/doc/implicits.md`, `artic/doc/pattern_matching.md`.

Do **not** copy code out of other AnyDSL repositories (for example stincilla) into this
repository — read them for reference only and write fresh code.
