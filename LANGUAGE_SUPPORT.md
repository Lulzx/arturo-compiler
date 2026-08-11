# Language support and production-readiness gate

The compiler is self-hosting and differential tests prove its current corpus,
but the standalone C target does **not** yet implement the entire Arturo
language. `make coverage` reports the mechanical intrinsic boundary; `make
check` proves behavior only for the checked-in corpus and compatibility cases.

## Current target boundary

| Area | Status | Required production gate |
| --- | --- | --- |
| Literals, grouping, calls, closures, paths, dictionaries | Corpus-proven | Keep host/kernel/IR/native differential tests green |
| `if`, `do`, `return`, `while`, `until`, `loop`, ranges | Corpus-proven | Add nested control-flow and failure cases |
| Common arithmetic, collections, strings, predicates | Partial | Every declared runtime builtin needs positive, edge, and error parity tests |
| Modules/imports | Unsupported by standalone target | Resolve/import dependencies at compile time and test multi-file packages |
| Objects/classes/methods | Unsupported | Add value/runtime representation, dispatch, mutation, and inheritance tests |
| Exceptions | Partial (`try` only) | Structured error values, stack traces, `try`/`except` parity, nonzero exits |
| Remaining Arturo value kinds | Partial | Represent and test quantities, dates, colors, regexes, binaries, stores, etc. |
| I/O, networking, databases, GUI | Mostly unsupported | Either link the authoritative Arturo runtime or implement capability modules |
| Diagnostics/tooling | Not production-grade | Source spans, filenames, actionable errors, stable exit codes, debug metadata |

## Route to the whole language

1. **Make scope executable.** Keep `src/intrinsics.art` generated from the
   pinned Arturo revision and make `make coverage` trend to 100%. Maintain an
   explicit allowlist for intentionally unavailable platform capabilities.
2. **Prefer runtime donation over reimplementation.** For full compatibility,
   link/embed Arturo's authoritative VM/runtime and lower this compiler's IR to
   that ABI. The hand-written C runtime is useful for a compact core target,
   but reproducing 400+ intrinsics and all value kinds independently creates a
   permanent semantic-drift risk.
3. **Own structural semantics.** Add compiler rules and IR nodes for modules,
   objects/method dispatch, assignment/rebinding, structured exceptions, and
   interpolation. Each starts with a host-pinned corpus case.
4. **Expand differential testing.** Import Arturo's upstream language test
   suite, then add randomized and negative tests. Compare stdout, stderr, exit
   status, and filesystem effects—not stdout alone.
5. **Harden the product.** Add source locations and diagnostics, sanitizers and
   leak checks for the C runtime, deterministic/reproducible builds, CI across
   supported operating systems and architectures, versioned artifacts, and a
   documented compatibility policy.

The release gate for a “full Arturo compiler” is: the pinned upstream suite and
the local suite pass against the host for every supported target, no
unclassified intrinsic or syntax construct remains, failures return stable
nonzero statuses, and supported platforms build reproducibly in CI.
