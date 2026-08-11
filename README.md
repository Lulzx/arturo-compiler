# arturo-compiler

A compiler for Arturo, written in Arturo, that compiles itself.

One rule table, one row per construct, three columns:

```arturo
defRule "while" #[
    eval: function [env argv][ evalWhile env argv\0 argv\1 ]
    emit: function [env argv][ #[op: "while" ...] ]
    run:  function [env argv][ ... ]
]
```

`eval` is what the construct means interpreted from source, `emit`
is the IR it produces, `run` is what that IR means when executed.
`evalCall`, `emitCall` and `runCall` no longer know any construct
by name — each looks the row up and picks its column. Adding a
language feature is adding a row, and getting one path wrong is a
visibly missing field rather than a branch nobody added to the
third `if "while" = name` chain.

Arity is deliberately not in the row. It is declared upstream and
generated into `INTRINSICS`; a second copy is a second thing to
drift. A row says what a construct *means*.

`call` is a row too — the builtin that takes a function value and
a block of arguments and applies them. It cannot stay delegated:
the function value is usually a kernel closure, and the delegate
refuses to hand kernel-internal values to the host, so the kernel
applies it itself. That is also how the table is *executed*: the
dispatchers reach their rows through `call r\eval @[env argv]`,
pulling a closure out of a dictionary, and until this row existed
that was a call the compiler could compile but the kernel could
not run. `corpus/27_call.art` pins it across all four engines.

The three columns are held to each other by construction —

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

— and by a harness that runs every corpus program four ways
(`host` on the real `arturo`, `kernel` interpret, `compiled` via `ir2art`+host, and `runIR` in-kernel) and compares.

All three columns take the same `[env argv]`. A construct's `emit`
column lowers its arguments to a flat `args` list inside one
op-tagged dictionary; its `run` column consumes exactly that list,
so `run` and `eval` are the same construct in the same shape, one
in IR and one in source. The op name is the construct name, which
is what lets `runNode` reach the rows: it has no per-op chains, it
dispatches straight to the run columns (each of which is a shared
helper, `runIf`/`runWhile`/…, so nothing is duplicated). Two host
quirks shaped that dispatch. An inline group that returns a block,
written into an `@[...]` literal, is mis-grouped — the intermediate
is spliced in as an extra element — so every `emit` column binds
its arguments to names first. And the host `call` builtin applied
to a kernel closure one level up from `runSeq` corrupts the value
stack, so a deeper `runCall` loses its `argVals`; `runNode` calls
the helpers as plain globals instead. Constant folding hands a pure
call straight to the host (`delegate`) rather than through `runIR`:
the kernel's interpreter frames during a fold leave the stack dirty
and the next nested call under a construct drops its arguments.

The donated VM owns the primitives; the kernel owns structure:
scoping, control flow, application.

## load, intrinsic, call

`load "mul"` and the Arturo primitive MUL used to be the same IR
node, which meant every question about a callee had to be asked as
a string — is `"mul"` in `PURE_WORDS`, does `"append"` write
through its first argument — and a word that merely spelled a
builtin got the same answers as the builtin. They are separate
nodes now:

```
load       resolve this name in the environment
intrinsic  this Arturo primitive, statically identified
call       invoke a callee, whichever kind it is
```

`10 - 3` lowers to `call(intrinsic("sub"), ...)`; a bare word
lowers to `intrinsic` or `load` by the host's own rule, the one
`loadRule` already implemented — a builtin word reaches the
builtin, and only a user *value* binding takes the word away, at
which point hygiene has already renamed that binding aside.

Constant folding now folds `intrinsic` nodes only, and asks
`pureIntrinsic?` rather than matching a name against a string
table. A call through a name is whatever the name is bound to when
the call runs, and folding it would mean running the primitive in
place of the user's function.

## What the compiler knows about Arturo

`src/intrinsics.art` is generated from Arturo's own source, by
`tools/extract_intrinsics.art`, and pinned to an upstream
revision. Every builtin's arity, infix glyph, and per-argument
reference and mutation behaviour is read off the `builtin "..."`
declarations in `src/library/*.nim` rather than mirrored by hand:

    arturo tools/extract_intrinsics.art [path-to-arturo-checkout]

This replaced two hand-written tables, and the replacement
immediately disagreed with them in three places — `chunk` was
recorded at arity 2 when it takes three, ten infix aliases were
missing, and the in-place-mutation set had been worked out from
host crashes when `PathLiteral` in an argument's type set had
been declaring it all along. Those are silent errors: the program
still compiles, it just groups differently than the host does.

Bootstrapping against the full table then broke the fixpoint
twice, on two bugs the small table had been hiding. A nested
`#[...]` value was emitted as `do [#[...]]`, and the `do` there is
an ordinary word lookup — so a sibling key named `do`, which the
generated table has, captured it and every later value in the
literal was built from the wrong thing. And a word of arity zero
opened a call frame that nothing ever completed, which left the
frame parked on the stack; `->` ends its body when that stack
empties, so an arrow ran on and swallowed the rest of its block.
`corpus/25_nested_dicts.art` and `corpus/26_nullary_arrow.art`
hold both.

`src/semantics.art` is the other half — the few judgements Arturo
does not declare and the compiler has to make itself. It is meant
to stay small. Purity is deliberately not generated: Arturo does
not state it, so generating it would mean dressing guesses up as
extracted facts.

## Prerequisites

`arturo` 0.10.0 (the revision pinned in `corpus/m1/RESULTS.md`). To regenerate `src/intrinsics.art` from an Arturo checkout, pass its path to the generator — see above.

## Run it

    arturo src/compiler.art <in.art> <out.art>          # emit -> fold -> print
    arturo src/compiler.art <in.art> <out.art> -b       # also emit native binary via cbackend (--native)
    arturo -c <out.art> && arturo -x <out.art.bcode>    # or host bytecode
    ./native_compiler                                    # prebuilt binary at repo root (gitignored)

## Test it

    arturo src/tests.art       # differential: host vs kernel vs compiled vs runIR
    arturo src/smoke.art       # interpret mode
    arturo src/bootstrap.art   # the compiler compiles itself

The harness runs every corpus program four ways — `host` (real `arturo`), `kernel` (interpret), `compiled` (emit -> fold -> `ir2art` -> host), and `runIR` (emit -> fold -> in-kernel `runIR`). All four agree. Each file runs in its own process via `src/one_test.art` to isolate host value-stack contamination.

The bootstrap goes one further. Stage 1 (the compiler in `src/`)
emits stage 2, a self-contained compiler that runs on the donated
VM with no access to `src/`. Stage 2 then renders every corpus
program exactly as stage 1 did, and re-emits the compiler itself:
stage 3 is byte-identical to stage 2. Stage 2 compiled to host
bytecode (`arturo -c`) and run by `-x` re-emits the compiler
byte-identically too — the donation end-to-end, with no
source-level interpretation left in the loop. That fixpoint is the
whole claim — the compiler reproduces itself.

## Files

    src/intrinsics.art  GENERATED — every builtin's declared signature
    src/semantics.art   what Arturo does not declare and we decide
    tools/extract_intrinsics.art  the generator
    src/front.art       strip, lex, lower — grouping, pinned to ast.nim
    src/kernel.art      the rule table and its three dispatchers, plus runIR
    src/ir.art          IR, the printer, constant folding
    src/backend.art     runIR; shell out to -c / -x
    src/cbackend.art    C emitter for -b / --native
    src/mknative.art    link helper for native binary
    src/tests.art / one_test.art / inv.art / ir_runner.art / kernel_runner.art / smoke.art / bootstrap.art
                        harnesses (differential, invariant, runners)
    src/compiler.art    entry point (emit -> fold -> print, with -b)
    corpus/             one program per rule, then compositions (27_call.art pins `call`)
    corpus/m1/          host probes and RESULTS.md
    runtime/            donated VM objects (*.o/*.a, gitignored)
    tmp/                scratch (*.bcode etc., gitignored)
    native_compiler     prebuilt native binary (gitignored)

Getting there meant designing around seven bugs in the host —
`-c` dropping mutation made through a path reference, dying on a
store bound to a negative constant, and mis-compiling
operator-like dictionary keys; `to :dictionary` handing out
copies of its values; and binding an in-place `append`'s result
killing the process outright. Each one is pinned with its probe
in `corpus/m1/RESULTS.md`, alongside what the compiler does
instead.

Read `SPEC.md` for the full design.
