# M1: The application model (pinned)

Pinned empirically (this corpus, run under `arturo 0.10.0`, one probe per
process) and by reading the authoritative front-end `src/vm/ast.nim`
(`processBlock`) and builtin registrations. Everything here is the contract
the kernel's grouping/lowering must reproduce.

## The grouping algorithm

A block is compiled left→right into a tree:

1. **A word naming a known function creates a Call node with that function's
   arity and the build descends into it.** Subsequent values fill it.
2. **A value fills the deepest open Call node; when a Call node has exactly
   `arity` children it pops back to its parent.** This single rule produces
   all application nesting:
   - `f a b` (f arity 2) → one call `f(a,b)`.
   - `print add 3 4` → `print(add(3,4))`: `print`'s one child is the whole
     `add 3 4` call.
3. **Infix symbols bind an operand.** When the element after a value is an
   infix *symbol*, the value becomes its first operand and the build descends.
   Since completion pops at exactly `arity`, **infix chains are
   right-associative and the rightmost infix operator binds tightest**: there
   is a single `InfixPrecedence`; there is no numeric precedence table.

   Verified on the host:
   - `2 * 3 + 4` → 14 = `*(2, +(3,4))`
   - `2 + 3 * 4` → 14 = `+(2, *(3,4))`
   - `10 - 3 - 2` → 9 = `-(10, -(3,2))`
   - `2 ^ 2 ^ 3` → 256 = `^(2, ^(2,3))`
   - `1 < 2 < 3` → false = `<(1, <(2,3))`
   - `print 10 - add 1 2 - 3` → 10 = `-(10, add(1, -(2,3)))`

4. **Arity sources.** Initially `TmpArities` = every function in the global
   symbol table (builtins carry declared arities; `function`/`$` = 2). As a
   block is processed, `x: function [params][body]` records
   `arity(x)` = number of non-`:type` elements in `params`, so user functions
   are known for the rest of that block (same-block forward references work).
   A word that is *not* a function is a variable load, never a call.

5. **Incomplete calls error at eval.** A Call node that reaches end-of-block
   with fewer children than its arity fails with "Not enough parameters".
   Verified: `print add 3` → add required 2.
   Extra values after a completed call are **dropped** (left unclaimed).
   Verified: `print add 3 4 5` → 7, `print "a" "b"` → `a`, `if ... [a] [b]`'s
   third block is dropped.

6. **Labels.** `x:` creates a 1-arity Store node; the following expression is
   its value.

7. **Evaluation order.** Tree children evaluate left→right, then the call
   applies; nested calls run innermost-rightmost. Verified: in
   `f p 1 p 2`, first `p` printed before second `p` before `f`.

8. **`true` / `false`** are compile-time constants.

## Special forms (builtin signatures + observed behavior)

| Form | Arity | Behavior |
| --- | --- | --- |
| `function` (alias `$`) | 2 | (params, body). `function [x] -> expr` works. |
| `if` | 2 | (condition, action), lazy, **no else block** (third block dropped). |
| `do` | 1 | evaluate block in place; block value = last value. |
| `map` `select` `loop` | 3 | (collection, params, action); params ∈ {Literal, Block, Null}. |
| `and?` `or?` | 2 | prefix words, **eager**. Lazy infix forms are `∧` `∨`. |
| `and` `or` | 2 | bitwise, integer operands. |
| `print` | 1 | returns Nothing; cannot chain (`print print 5` errors). |
| `size` | 1 | — |
| `+ - * / % ^ //`, comparisons, `++` | 2 | infix symbols. |

Canonical higher-order forms (all verified):
- `map [1 2 3] [x][x * 2]` → 2 4 6
- `map [1 2 3] 'x [x * 2]` → 2 4 6
- `map [1 2 3] => [& * 2]` → 2 4 6
- `select [1 2 3 4] [x][x > 2]` → 3 4
- `loop 1..3 [x][print x]` → 1 2 3
- **`map [1 2 3] function [x][x*2]` is an error**: that's 2 args, map needs 3.

`=>` expansion: `=> [& * 2]` becomes the two elements `_0` and `[_0 * 2]`
(replacing `&` with `_0`, `_1`, …). It feeds the (collection, params, action)
combinators directly. `&` is special only inside `=>`; it is not auto-injected
by `loop`/`map`.

Parens `( ... )` are inline sub-blocks (explicit grouping). `1..5` is one
Range literal. `->` after params is a single-expression arrow body.

## Result protocol

A block's value is its **last evaluated value** (verified: `do [1 2 3]` → 3;
`do [[1 2] [3 4]]` → the block `[3 4]`). A statement returning Nothing leaves
nothing usable; values that are never claimed are dropped at block end.

## 0.10.0 bugs to design around

1. **Value-stack contamination.** An unused value left inside a callee is
   later popped as an argument by the enclosing call. Corrupted results in
   probes whose function bodies leave trailing values (e.g. `function [x][
   print "p:" x  x]` returns garbage; the trailing `x` leaks). Workaround:
   `return`, or no trailing values. This contaminated several multi-statement
   probes (app15/app16/d02); single-statement isolation is authoritative.
   The kernel implements the *intended* model; differential tests must avoid
   the buggy pattern or pin it as a known divergence.
2. **Silent exit 1** with no message on some errors (`x: print 7` then
   `print x`).
3. **`to :dictionary` hands out copies.** `get d k` on a dictionary built
   by `to :dictionary <block>` returns a *copy* of the value; on a `#[...]`
   literal it returns the reference. So `d\a\b: v`, which lowers to
   `set (get d 'a) 'b v`, writes into a copy and the update is lost. The
   kernel builds dictionaries as `#[]` seeded with `set` instead.
4. **`-c` drops mutation through a path reference.** `append 'c\stack 4`
   and `pop 'c\stack` work when the source is interpreted but write
   nothing once compiled to bytecode; `append 'b 4` through a plain
   literal reference is fine either way. Binding the path to a local
   first is enough, because the local aliases the same container, so the
   printer emits `do [_ref1: c\stack append '_ref1 4]`.
5. **Binding an in-place `append`'s result kills the process.**
   `v: append 'b 4` exits silently, source or bytecode, even wrapped in
   `do [...]`. `v: pop 'b` is fine. This is why the rewrite for 4 must
   not bind the call's result: it leaves the block's value as the
   call's, so `pop` still returns the element it removed, and a source
   that reads an in-place `append` dies exactly as the host does.
6. **`-c` dies on a store bound to a negative constant.** `x: sub 4 10`
   compiles to a program that stops there; `x: sub 10 4` is fine, and so
   is `x: neg 6`. It is the negative result, not the call. Negative
   literals are therefore emitted as `neg n`.
7. **`-c` mis-compiles operator-like dictionary keys.** `#["+": "add"]`
   is fine interpreted, but raises a `let` arity error as bytecode.
8. **An inline group that returns a block, written straight into an
   `@[...]` literal, is spliced.** The intermediate value is pushed as an
   extra element of the result block, so `@[a (f b)]` where `f b` returns a
   block can come back with three elements instead of two. Every `emit`
   column in the compiler binds its arguments to names first and builds
   `@[...]` from plain variables.
9. **The host `call` builtin applied to a kernel closure corrupts the
   value stack when the closure's body recurses.** `call r\run @[env args]`
   works when the run column is shallow (the `makeDict` and `call` rows),
   but when the construct's body runs, via `runSeq` → `runCall` → `applyVal`,
   `applyVal`'s `argVals` binding is lost and the host raises a name error.
   Constant folding also poisoned the stack: a fold's `runIR` left it dirty
   and the *next* nested call under a construct dropped its arguments. The
   kernel therefore calls the construct run helpers as plain globals, and
   constant folding hands a pure call straight to the host (`delegate`)
   instead of through `runIR`. corpus/06_if, 07_do, 13_while, 15_parens
   pin the corrected behaviour on all four engines.

## Consequences for the compiler

- **Lowering owns grouping**: reproduce the left→right build (arity table +
  single-precedence right-assoc infix + completion-pop) to produce the
  kernel's explicit `call` trees. The kernel never re-derives grouping.
- **A compile-time arity table is mandatory**, covering builtins and user
  functions (params-block count), including same-block forward references.
- **Infix** is a lowering concern; the emitted tree must match host behavior
  exactly for differential tests, including the non-standard precedence.
- **`=>` expansion** is a lowering pass (`&` → `_0...`, split params/action).
- **`if` is lazy, arity 2, no else.** The spec's `if` rule is updated.
- **Nothing-returning calls** (e.g. `print`) must be modeled or
  `print print 5` diverges.
- **Output hygiene.** The emitted program names builtins as prefix words
  (`a * b` → `mul a b`, `d\k` → `get d 'k`), so a source that rebinds a
  builtin word to a plain value would capture the compiler's own calls.
  The binding is renamed instead (`_shadow_mul`), which leaves the word
  meaning the builtin exactly as it did in the source. A `function`
  binding is left alone, because it does not shadow on the host either.
