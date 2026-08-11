# arturo-compiler

A compiler for Arturo, written in Arturo, that compiles itself — to a
native binary that reproduces the interpreter's output byte-for-byte.
Proven, not promised.

## The whole trick is one table

One row per construct, three columns.

```arturo
defRule "while" #[
    eval: function [env argv][ evalWhile env argv\0 argv\1 ]
    emit: function [env argv][ #[op: "while" ...] ]
    run:  function [env argv][ ... ]
]
```

`eval` is what the construct means interpreted from source, `emit` is
the IR it produces, `run` is what that IR does when executed. The
dispatchers know no construct by name — they look up the row and pick
a column. Adding a language feature is adding a row. That's it.

Arity isn't in the row. It's generated into `INTRINSICS` from
Arturo's own source, one copy, nothing to drift.

`call` is a row too. A kernel closure won't hand its internals to the
host, so the kernel applies it itself — which is exactly how the table
is executed.

## The invariant

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

Every construct is held to it across four engines — host, kernel,
compiled, and in-kernel IR. `corpus/` is one program per rule, and
each runs through all four and must agree.

## Self-hosting

The compiler emits itself to C, links it, and the result —
`tmp/ncomp`, a standalone native CLI — compiles every corpus program
to a native binary. Its output is byte-identical to the host
interpreter. 27/27.

```
bash tools/selfhost_test.sh     # pass=27 fail=0
```

## Build, run, test

```
arturo tools/cbnative.art                 # build the native compiler -> tmp/ncomp
./tmp/ncomp  in.art  out                  # compile a program to a native binary
./out                                     # run it
arturo src/compiler.art in.art out.art    # emit -> fold -> render
```

```
arturo src/tests.art            # host vs kernel vs compiled vs runIR
bash tools/selfhost_test.sh     # native == host, 27/27
```

## Files

    src/intrinsics.art   GENERATED — every builtin's signature, read off Arturo's source
    src/front.art        lex + lower
    src/kernel.art       the rule table, its dispatchers, runIR
    src/ir.art           IR + constant folding
    src/backend.art      runIR; shells out to -c / -x
    src/cbackend.art     C emitter
    src/compiler.art     entry point
    tools/extract_intrinsics.art  the generator
    tools/cbnative.art            build the native compiler
    tools/selfhost_test.sh        the proof
    corpus/                      one program per rule
    SPEC.md                      the full design

Built against Arturo 0.10.0, pinned to the revision in
`corpus/m1/RESULTS.md`. Read `SPEC.md` for the design; the compiler
reproduces itself, and that's the whole claim.
