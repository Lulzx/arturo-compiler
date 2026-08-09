# arturo-compiler

A compiler for Arturo, written in Arturo, that compiles itself.

No two implementations. One rule table, one dispatch, one mode
flag. The interpreter and the compiler are the same function:

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

Semantics are written once — one row per construct, carrying both
its interpret behavior and its emit behavior. Adding a language
feature is adding a row. The donated VM owns the primitives; the
kernel owns structure: scoping, control flow, application.

## Run it

    arturo src/compiler.art <in.art> <out.art>   # emit -> fold -> print
    arturo -c <out.art> && arturo -x <out.art.bcode>

## Test it

    arturo src/tests.art       # differential: host vs kernel vs compiled
    arturo src/smoke.art       # interpret mode
    arturo src/bootstrap.art   # the compiler emits its own source to IR

The harness runs every corpus program three ways — on the real
`arturo`, through the kernel interpreter, and through the emitted
IR rendered back to source and recompiled by the host. All three
agree.

## Files

    src/front.art     strip, lex, lower — the grouping, pinned to ast.nim
    src/kernel.art    the dual evaluator, plus runIR
    src/ir.art        IR, the printer, constant folding
    src/backend.art   runIR; shell out to -c / -x
    src/tests.art     the differential harness
    corpus/           one program per rule, then compositions

Host quirk, stated plainly: Arturo 0.10.0's value-stack bug
contaminates deep recompiled walks. The corpus round-trip doesn't
care. Read `SPEC.md` for the full design.
