# arturo-compiler: Specification

A compiler for Arturo, written in Arturo, for the whole language. The
compiler owns the structure of the language: scoping, control flow,
application, code generation. The donated native VM owns the primitives,
the runtime, and the machine code. The spec is written in the language's own
terms. Grammars are blocks, the AST is the lexed block, and the compiler is
the evaluator with its mode flag set to emit.

## 1. Goal

- Accept any Arturo source and produce a runnable artifact.
- The compiler is written in Arturo, in a core subset it can compile itself.
- The language's semantics are expressed once, in a single rule table. The
  interpreter and the compiler are two modes of that one expression.
- The runtime and machine code come from the donated VM, not from a second
  implementation of the language.

## 2. Non-goals (for now)

- A hand-written bytecode emitter or LLVM pass.
- Reimplementing the standard library in Arturo.
- Compiler performance beyond "fast enough to bootstrap".

## 3. Principles

1. **One expression.** Semantics are written once, in a rule table: one row
   per construct, carrying both its interpret behavior and its emit behavior.
   Adding a language feature is adding a row.
2. **Donation.** The kernel owns structure; primitives are donated to the
   host. Full-language output exists from the first compile.
3. **The host is the authority.** When the kernel and the host disagree, the
   host wins. The compiled output must match it. The kernel is an
   independent, structurally explicit model, kept in agreement by
   differential tests. The kernel is not the truth; it is a testable mirror
   of the truth.
4. **The invariant.** `eval(env, emit(n)) == eval(env, n)` for every node.
   Compiling is meaning-preserving by construction and by test.

## 4. Architecture

```
 source.art
    │ (1) stripComments           — carpintero grammar
    ▼
 clean text
    │ (2) to :block               — donated native lexer; its output is the tree
    ▼
 source block  (the AST)
    │ (3) lowering (front.art)    — carpintero tree walks; v0 = identity
    ▼
 core block
    │ (4) kernel, emit mode       — one dispatch, mode = :emit
    ▼
 IR   (self-contained data)
    │ (5) IR passes               — v0: none; folding arrives here
    ▼
 IR'
    │ (6) backend
    │     v0: kernel executes IR (compile-then-run)
    │     v1: ir2art printer → arturo -c / -b   (donated codegen)
    │     v2: cbackend → C, linked against runtime/runtime.c  (native)
    ▼
 artifact: result value / .bcode / native binary
```

### 4.1 The invariant

Let `eval` be the kernel evaluator and `emit` its emit mode (the same
function, different mode). Correctness is:

    for every node n:   eval(env, emit(n)) == eval(env, n)

"==" here is same value, compared by `codify` (canonical text), not by `=`.
Arturo 0.10.0's `:symbolliteral` is not equal to itself. This invariant is
what differential tests check. It holds universally (donation included)
because donation resolves to the host in both modes.

## 5. The kernel, the one-time expression

`src/kernel.art` is a data-driven evaluator over blocks. One function
dispatches every node; a rule table supplies the per-construct behavior.

```
evalNode: function [env node mode][      ; mode ∈ { :interpret, :emit }
    ; dispatch on shape:
    ;   dict with `op`   → IR node    (op → rule)
    ;   source element   → Arturo type (type → rule)
    rule: ruleFor node
    if :interpret = mode -> rule.eval  env node
    else                 -> rule.emit  env node
]
```

Each rule:

```
RULE = #[
    name:  'function
    match: [shape predicate on a node]  ; source form, or IR `op`
    eval:  [env node] -> value          ; interpret mode
    emit:  [env node] -> ir-node        ; emit mode
]
```

The interpreter and the compiler are the same dispatch with the mode flag
flipped. "The compiler falls out of the spec" means exactly this: no second
implementation, just a flag and one more field per rule.

### 5.1 What the kernel owns vs. donates

The criterion that draws the line and drives rule-set growth:

- A construct whose evaluation depends on the kernel's environment must
  become a kernel rule. Its meaning is structural: scoping, control flow,
  application.
- A construct whose meaning is independent of the kernel's environment is
  donated: a primitive applied to kernel-evaluated arguments.

Consequence: laziness can never be donated (lazy arms must not evaluate), so
`if`, loops, and function bodies are kernel rules from the start. `+`,
`print`, and most builtins are donated from the start. This is why the whole
language is free while the kernel stays small.

### 5.2 Value model (v0)

Kernel-defined: `:integer`, `:float`, `:string`, `:char`, `:block`,
`:dictionary`, `:word`, `:symbol`, `:label`, `:null`, and kernel `:function`
values (closures). All other types flow through donation.

### 5.3 Environment model

- Frame = `#[bindings: #[] parent: frame-or-null]`.
- `define(env, name, value)` inserts into the current frame.
- `lookup(env, name)` walks the parent chain. Names are stored and compared
  as strings, not symbols. 0.10.0 symbols are not equal to themselves.
- Lookup distinguishes a bound `null` from an unbound name (check the frame;
  do not rely on returning `null`).
- A kernel function value is a closure: params, body IR, defining env.
  Application creates a child frame binding params to evaluated args
  (call-by-value), then evaluates the body.
- How many trailing expressions a word consumes (Arturo's application
  semantics) is pinned by host-parity tests before the application rule is
  fixed (see open questions).

### 5.4 v0 rule set

| Construct | Source form | eval | emit |
| --- | --- | --- | --- |
| literal | `:integer` `:float` `:string` `:char` | the value | `#[op:'const value:v]` |
| word | `:word` | `lookup(env, name)` | `#[op:'load name:w]` |
| label | `x:` + following expr | define/assign `x` | `#[op:'define name:x expr:<emit expr>]` |
| function | `function [params][body]` | closure | `#[op:'func params:.. body:<emit body>]` |
| if | `if cond [yes]` | lazy, arity 2, no else | `#[op:'if cond:.. yes:..]` |
| do | `do [..]` | eval block in env | `#[op:'do body:..]` |
| block expr | `:block` in expression position | a block value | `#[op:'block items:..]` |
| application | a resolved tree: operator + its arguments (grouping from §5.5) | eval operator; runtime dispatch: kernel closure → apply in-kernel; host function → delegate with evaluated args | `#[op:'call fn:.. args:..]` |
| passthrough | any subtree matching no rule | host `do` on preserved source | `#[op:'passthrough src:..]` |

### 5.5 Application, grouping, and the seam

One application node, runtime dispatch. `call` evaluates its operator and
lets the value decide: a kernel closure → apply in-kernel (child frame); a
host function → delegate to the host with the evaluated args; anything else
→ error. Donation is therefore a runtime behavior of the call rule, not a
distinct node. This is what keeps shadowing correct: a locally defined `+`
is a kernel closure and wins over the host builtin. Delegation constructs a
call block from a word value (`to :word`) plus the evaluated arg values and
`do`s it; it is env-independent by construction.

The grouping problem is owned by lowering, now pinned. M1 (`corpus/m1/`)
fixed the model by experiment and by reading the authoritative front-end
(`src/vm/ast.nim`): left→right build, a word's declared arity opens a Call
node, a Call node pops to its parent once it has exactly `arity` children,
and infix symbols bind an operand with a single precedence (so chains are
right-associative and the rightmost infix operator binds tightest; `2 * 3 +
4` is `*(2, +(3,4))`). Lowering reproduces this exactly and emits explicit
`call` trees using a compile-time arity table (builtins + user functions
from their params blocks, including same-block forward references). The
kernel never sees an ungrouped sequence; it only applies resolved `call`
trees. Notable pinned facts: `if` is arity 2 with no else; `and?`/`or?` are
eager, lazy logic is `∧`/`∨`; `=> [& ...]` expands to `_0` + `[_0 ...]` (two
args, not one); Nothing-returning calls cannot be chained. Full write-up:
`corpus/m1/RESULTS.md`.

The one structural seam is `passthrough`. A whole subtree the kernel cannot
yet express, such as a lazy construct, or syntax lowering has not desugared.
Interpret mode `do`s the preserved source verbatim, valid only when the
subtree's free variables resolve at host top level. Any subtree that
references kernel-bound names must become a kernel rule or a lowering target.
That is the pressure that grows the rule set. Emit mode keeps the source for
the printer.

Laziness can never be delegated at runtime either (delegation evaluates its
arguments eagerly), so lazy constructs are kernel rules or `passthrough`.

## 6. The IR

The emitted artifact: a block of tagged dictionaries. IR is closed and
self-contained, with no references to source, except `passthrough`, which is
explicitly the seam where source survives. IR is the stable contract: every
future backend and pass consumes IR, never source.

```
#[op:'const       value:v]
#[op:'load        name:w]
#[op:'define      name:w expr:ir]
#[op:'func        params:[..] body:ir]
#[op:'if          cond:ir yes:ir no:ir]
#[op:'do          body:ir]
#[op:'block       items:[ir..]]
#[op:'call        fn:ir args:[ir..]]   ; runtime dispatch: closure | host fn
#[op:'passthrough src:<source element>]
```

### 6.1 Worked example

`add: function [a b][a + b]` + `print add 3 4`, lexed as:

```
[add: function [a b][a + b] print add 3 4]
```

Emitted IR (v0: function and application are kernel-owned; `+` and `print`
are host functions resolved at runtime):

```
[
  #[op:'define name:add expr:#[op:'func params:[a b] body:[
      #[op:'call fn:#[op:'load name:+] args:[
          #[op:'load name:a] #[op:'load name:b]]]]]]
  #[op:'call fn:#[op:'load name:print] args:[
      #[op:'call fn:#[op:'load name:add] args:[
          #[op:'const value:3] #[op:'const value:4]]]]]
]
```

The inner `call` loads `+`, a host function, so the call rule delegates to
the host with the evaluated args `a`, `b`. The middle `call` applies the
kernel closure `add` in-kernel (child frame → 7). The outer `call` loads
`print`, a host function, and delegates with the value 7. Grouping (`print`
takes one argument, which is the whole `add 3 4` application) was resolved
by lowering (§5.5), not by the kernel.

## 7. Front-end

- `stripComments`, the existing carpintero grammar (validated over Arturo's
  own corpus).
- `to :block`, the donated native lexer. The tree is the block. Node types
  are Arturo value types; there is no separate AST.
- Lowering, carpintero tree walks (`into` recursion) that normalize dialects
  into the core the kernel understands: `method`/`$`-sigil/arrow forms → the
  `function` rule; later, parens, `=>`, paths, string interpolation.
- Lowering resolves application grouping (§5.5): from the lexed sequence it
  builds explicit `call` trees, using an arity table for host builtins as an
  interim mechanism. The kernel and the printer never reason about ungrouped
  sequences.
- v0: grouping resolution is real; dialect desugaring stays identity.

## 8. Backend, the donation, phased

| Phase | Backend | Output | Status |
| --- | --- | --- | --- |
| v0 | kernel executes IR (compile-then-run) | result value | shipped |
| v1 | printer (`ir2art`) → `arturo -c` / `-b` | `.bcode` / standalone executable | shipped |
| v2 | cbackend emits C, linked against `runtime/runtime.c` | native binary | shipped, self-hosting |

v1's printer is mandatory, not optional. `-c` and `-b` consume `.art` source,
so the IR must be rendered back to core-Arturo text before the host compiles
it. `ir2art` inverts `emit` for the kernel-owned nodes and prints
`passthrough` as its preserved source. Because grouping lives in the `call`
trees, the printer renders explicit structure and parenthesizes whenever the
IR's nesting would be ambiguous to the host's own parser; its correctness is
verified by compiling its output and diffing behavior in the differential
harness. The v1 output runs on the donated VM, so a compiled executable is
bytecode plus the VM shell. v2 changes only how the artifact is produced,
never what executes it.

v2 is the shipped native backend. `src/cbackend.art` lowers the IR to C and
links it against `runtime/runtime.c`, a hand-written C runtime that mirrors
the host's semantics. The C runtime is not a second implementation of the
language; it implements the same donated primitives and the same value model
so that a natively compiled program observes the same semantics as the
interpreted one. Self-hosting is the proof: `tmp/ncomp` is a native compiler
built from the compiler's own C output, and it compiles corpus programs whose
output matches the host byte for byte (`make verify`).

## 9. IR passes

The IR is where the compiler computes; v0 has no passes, and the first to
arrive is constant folding: a subtree whose leaves are all `const` and whose
`call`s are all to known-pure host words is evaluated at compile time and
replaced by a single `const`. Purity is a word list, not a property of the
IR. Only words known to be pure (arithmetic, comparison, string ops) may be
folded. `print`, `input`, `random`, and file/io words are explicitly
excluded, or folding would execute side effects during compilation. Because
the IR is data, evaluating at compile time is just running the kernel on the
subtree. Passes are pure IR→IR functions, which keeps them testable in
isolation.

## 10. Testing contract, differential

The triangle:

    host(P)     — run P on the real `arturo`            (the authority)
    kernel(P)   — run P through kernel interpret mode   (the model)
    compiled(P) — emit(P) → [fold] → run IR / print+host (the product)

Assertions:
- `compiled(P) == kernel(P)`, the invariant, always.
- `kernel(P) == host(P)`, the model matches the authority. In v0, this is
  checked for corpus programs; as the kernel grows, it is checked for
  everything.

A fourth engine, runIR, evaluates the emitted IR in-kernel. The native
backend adds a fifth check: a corpus program compiled by `tmp/ncomp` must
produce the same output as the host (`make verify`).

Result protocol: a program's result is the value of its final expression
(Arturo parity to be pinned). Comparisons use `codify` (canonical text), not
`=`, because of the `:symbolliteral` equality bug.

Procedure for adding a construct: (1) write the corpus program; (2) pin the
expected behavior from the host; (3) add the kernel rule + emit rule;
(4) the three-way check now passes. Semantics are pinned by test, not
asserted in prose.

Corpus: one program per rule, then compositions, then the kernel's own
source, then the compiler's own source (the self-host check).

## 11. Bootstrap

1. Write the compiler (front-end + kernel + printer + backend) in core-Arturo.
   Subset discipline: the compiler uses only constructs it owns, except
   through the seams, which stay honest because the seams delegate to the
   authority.
2. Stage 1: `arturo compiler.art <in> <out>`, slow, correct.
3. Stage 2: the compiler compiles its own source → IR → render → native
   binary via the C backend.
4. Stage 3: the native compiler recompiles itself; differential testing
   against the host runs forever.

## 12. Error model

Kernel errors are values, not host panics. An evaluation that fails produces
a `#[error: message]` value (or throws a kernel marker) so the kernel never
depends on host panic behavior. Arturo 0.10.0 panics unwind incorrectly
through `try` (a known bug). `try`/`except` in source is donated until the
kernel owns an error model; program-level `try` around kernel-owned
constructs is a v1 concern.

## 13. Language surface backlog

| Construct | v0 | Path to ownership |
| --- | --- | --- |
| `method`, `$`, arrow bodies, `=>` | delegated/passthrough | lowering → `function` |
| arithmetic/comparison/io builtins | delegated | stay delegated (env-independent) |
| `set`, `assign` | delegated | kernel rule after host-parity pin |
| paths `a/b`, symbols `'x` | delegated/passthrough | kernel rules (note `=` bug) |
| parens `( .. )` | passthrough | lowering |
| string interpolation | passthrough | lowering |
| loops (`loop`, `while`, `until`, ranges) | passthrough | kernel rules (lazy) |
| `import` / modules | passthrough | later, after the driver |
| objects / classes | passthrough | later |
| `try` / `except` | passthrough | after §12 error model |

Lazy constructs always take the kernel-rule path, never delegation.

## 14. File layout

```
arturo-compiler/
  SPEC.md
  src/
    front.art     ; stripComments + to :block + lowering (carpintero)
    kernel.art    ; rule table + evalNode (the spec executable)
    ir.art        ; IR node constructors + printers (ir2art)
    backend.art   ; v0: runIR ; v1: shell out to -c / -b
    cbackend.art  ; v2: emit IR as C, link against runtime.c
    tests.art     ; differential harness + corpus driver
  runtime/
    runtime.c     ; the C runtime the native backend links against
    runtime.h     ; Value/Block/Env/Dict/IR, the shared value model
  corpus/         ; one program per rule, then compositions
  tools/          ; build (cbnative), generator (extract_intrinsics)
```

## 15. Decisions & open questions

Decisions:
- One dispatch, one mode flag; rules carry eval+emit. The duality is literal.
- Donation runs in both modes; the host is the authority; the kernel is a
  mirror, kept honest by tests.
- Laziness cannot be delegated at runtime; lazy constructs are kernel rules
  or passthrough.
- One application node, `call`. Host-function delegation is a runtime
  property of the call rule, so shadowing stays correct. `passthrough` is the
  only structural seam.
- Lowering resolves application grouping into explicit `call` trees; the
  kernel and printer never see ungrouped sequences.
- `-c`/`-b` require the `ir2art` printer; IR stays the only artifact contract.
- The native backend is C emitted to IR and linked against `runtime/runtime.c`
  (v2). Its semantics must match the host; the self-hosting proof holds it to
  that.

Open, pinned by host-parity tests:
- `set` vs `label` assignment (redefine vs rebind nearest); when labels in
  function bodies bind.
- Result protocol: whether a program's result is the final expression value.
  (M1 confirms `do` returns the last value; the program-level protocol is
  still to be pinned for top-level scripts.)

## 16. Build order

- **M1, pin the application model.** DONE. `corpus/m1/` + `RESULTS.md`.
  The grouping algorithm, arity sources, infix precedence, `=>`/`$`/`if`/
  `and?`/`or?`/`∧`/`∨`, argument order, and the result protocol are pinned,
  along with the 0.10.0 value-stack bug to design around.
- **M2, kernel interpret skeleton.** DONE. Env, then rules for
  literal/word/label/function/if/do/block and the runtime dispatch in `call`.
  The compile-time arity table (from M1) is built here and owned by lowering.
  Corpus parity on kernel-owned constructs.
- **M3, emit mode.** DONE. IR nodes; the invariant `compiled(P) == kernel(P)`.
- **M4, printer + `-c`/`-b` backend.** DONE. `ir2art`, then full-language
  output; the three-way differential against the host.
- **M5, constant folding.** DONE. Pure words only.
- **M6, bootstrap.** DONE. The compiler compiles itself, then the C backend
  emits a native compiler (`tmp/ncomp`) that reproduces host output byte for
  byte. That is the self-hosting claim, held by `make verify`.
