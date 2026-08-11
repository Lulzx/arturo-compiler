# arturo-compiler

A compiler for Arturo, written in Arturo, that compiles itself to a native
binary. On the owned, corpus-pinned subset it reproduces the interpreter's
output byte for byte, except for explicitly documented host defects.

## One table

One row per construct, three columns.

```arturo
defRule "while" #[
    eval: function [env argv][ evalWhile env argv\0 argv\1 ]
    emit: function [env argv][ #[op: "while" ...] ]
    run:  function [env argv][ ... ]
]
```

`eval` is what the construct means interpreted from source, `emit` is the IR
it produces, `run` is what that IR does when executed. The dispatchers know
no construct by name; they look up the row and pick a column. Adding a
language feature is adding a row.

Arity is not in the row. It is generated into `INTRINSICS` from Arturo's own
source, one copy, nothing to drift.

`call` is a row too. A kernel closure will not hand its internals to the
host, so the kernel applies it itself, which is how the table is executed.

## The invariant

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

Every construct is held to it across four engines: host, kernel, compiled,
and in-kernel IR. `corpus/` is one program per rule, and each runs through
all four and must agree.

## Self-hosting

The compiler emits itself to C, links it, and the result, `tmp/ncomp`, is a
standalone native CLI. It compiles corpus programs to native binaries whose
output is byte-identical to the host interpreter. `make verify` proves it
every time it runs.

Three things "byte-identical" does not claim. It means the printed output
(stdout and stderr) of the native binary matches the host interpreter's
character for character. It does not compare state that never prints. And it
is proven on the corpus subset and on the compiler's own source, not on the
whole language; constructs the compiler does not yet own (see the backlog in
`SPEC.md`) are outside the proof until a corpus program pins them. Finally,
documented host defects may be corrected rather than reproduced; those cases
are compatibility exceptions and are not part of the byte-identical claim.

### Known host compatibility exception

Arturo 0.10.0 mishandles string append when the right operand is an integer:
`print "n=" ++ 13` raises a misleading `print` arity error, while assigning
the same append before printing may silently produce no output. The native C
runtime deliberately uses the useful semantics and prints `n=13`, consistent
with its existing string conversion for integer, logical, and character
operands. String-to-string append remains byte-identical. This exception was
found by randomized differential testing and is intentionally not added to
the parity corpus, because its expected result differs from the pinned host.

## Build, run, test

    make ncomp              # build the native compiler -> tmp/ncomp
    ./tmp/ncomp in.art out  # compile a program to a native binary
    ./out                   # run it
    make test               # 4-way differential, every corpus program
    make verify             # native == host, byte for byte
    make compat             # expected output for documented host fixes

Or by hand:

    arturo tools/cbnative.art            # build the native compiler
    arturo src/compiler.art in.art out   # emit -> fold -> render

## Files

    src/intrinsics.art   GENERATED: builtin arities, read off Arturo's source
    src/front.art        lex + lower
    src/kernel.art       the rule table, its dispatchers, runIR
    src/ir.art           IR + constant folding
    src/backend.art      runIR; shells out to -c / -x
    src/cbackend.art     C emitter
    src/compiler.art     entry point
    tools/extract_intrinsics.art  the generator
    tools/cbnative.art            build the native compiler
    tools/selfhost_test.sh        the self-hosting proof
    corpus/                       one program per rule
    compat/                       deliberate host-defect fixes
    SPEC.md                       the full design
    CONTRIBUTING.md               how to add a construct

Built against Arturo 0.10.0, pinned to the revision in `corpus/m1/RESULTS.md`.
Read `SPEC.md` for the precise proof boundary and design.
