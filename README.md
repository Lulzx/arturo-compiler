# arturo-compiler

A compiler for Arturo, written in Arturo, that compiles itself.

One semantic model, expressed along three paths: interpreting
source, emitting IR, and executing that IR. They are held to each
other by construction —

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

— and by a harness that runs every corpus program all three ways
plus on the real `arturo`, and compares. What the model is *not*,
yet, is one literal rule table: `evalCall`, `emitCall` and
`runNode` are three dispatchers over the same constructs, and
collapsing them into one row per construct is the next milestone.
Anything short of that and this paragraph would be describing a
program that does not exist.

The donated VM owns the primitives; the kernel owns structure:
scoping, control flow, application.

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

## Run it

    arturo src/compiler.art <in.art> <out.art>   # emit -> fold -> print
    arturo -c <out.art> && arturo -x <out.art.bcode>

## Test it

    arturo src/tests.art       # differential: host vs kernel vs compiled
    arturo src/smoke.art       # interpret mode
    arturo src/bootstrap.art   # the compiler compiles itself

The harness runs every corpus program three ways — on the real
`arturo`, through the kernel interpreter, and through the emitted
IR rendered back to source and recompiled by the host. All three
agree.

The bootstrap goes one further. Stage 1 (the compiler in `src/`)
emits stage 2, a self-contained compiler that runs on the donated
VM with no access to `src/`. Stage 2 then renders every corpus
program exactly as stage 1 did, and re-emits the compiler itself:
stage 3 is byte-identical to stage 2. That fixpoint is the whole
claim — the compiler reproduces itself.

## Files

    src/intrinsics.art  GENERATED — every builtin's declared signature
    src/semantics.art   what Arturo does not declare and we decide
    tools/extract_intrinsics.art  the generator
    src/front.art     strip, lex, lower — the grouping, pinned to ast.nim
    src/kernel.art    the dual evaluator, plus runIR
    src/ir.art        IR, the printer, constant folding
    src/backend.art   runIR; shell out to -c / -x
    src/tests.art     the differential harness
    corpus/           one program per rule, then compositions

Every construct the compiler emits survives `arturo -c`, which
takes the bootstrap one step further: stage 2 compiled to host
bytecode and run by `-x` re-emits the compiler byte-identically
too, with no source-level interpretation anywhere in the loop.

Getting there meant designing around seven bugs in the host —
`-c` dropping mutation made through a path reference, dying on a
store bound to a negative constant, and mis-compiling
operator-like dictionary keys; `to :dictionary` handing out
copies of its values; and binding an in-place `append`'s result
killing the process outright. Each one is pinned with its probe
in `corpus/m1/RESULTS.md`, alongside what the compiler does
instead.

Read `SPEC.md` for the full design.
