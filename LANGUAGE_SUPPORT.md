# Language support and production-readiness gate

The compiler is self-hosting and differential tests prove its current corpus,
but the standalone C target does **not** yet implement the entire Arturo
language. `make coverage` reports the mechanical intrinsic boundary; `make
check` proves behavior for the checked-in corpus, compatibility cases,
deterministically generated valid programs, and representative invalid cases.

## Current target boundary

| Area | Status | Required production gate |
| --- | --- | --- |
| Literals, grouping, calls, closures, paths, dictionaries | Corpus-proven | Keep host/kernel/IR/native differential tests green |
| `if`, `do`, `return`, `while`, `until`, `loop`, ranges | Corpus-proven | Add nested control-flow and failure cases |
| Common arithmetic, collections, strings, predicates | Corpus-proven portable surface | Keep edge and negative parity coverage growing |
| Modules/imports | Corpus-proven | Keep recursive, selective, lean, local-package, and package-entry resolution tests green |
| Objects/classes/methods | Corpus-proven | Constructors, bound methods, inheritance, `super`, reflection, string rendering, and generated ordering methods are covered |
| Exceptions | Partial (`try`, `throw`, `throws?`, `error?`, `ensure`, `panic`) | Arturo-level stack frames and exact upstream diagnostic formatting remain |
| Required portable Arturo value kinds | Corpus-proven | Rational, complex, quantity/unit, date, color, regex, binary, version, error-kind, and symbol-literal values are native; external handles such as stores remain explicitly unavailable |
| I/O, networking, databases, GUI | Filesystem/process basics supported; 43 declarations intentionally unavailable | Keep compile-time rejection policy machine-checked; only add capabilities with portable implementations and effect tests |
| Diagnostics/tooling | Partial | Native failures have filenames, stable stderr, and nonzero exits; source lines and Arturo-level stack frames remain |

## Route to the whole language

1. **Make scope executable.** Keep `src/intrinsics.art` generated from the
   pinned Arturo revision and make `make coverage` trend to 100%. Maintain an
   explicit allowlist for intentionally unavailable platform capabilities.
   The current boundary is classified declaration-by-declaration in
   `config/intrinsic-policy.tsv`; `make coverage` fails if that policy and the
   generated declaration/runtime tables drift apart.
2. **Prefer runtime donation over reimplementation.** For full compatibility,
   link/embed Arturo's authoritative VM/runtime and lower this compiler's IR to
   that ABI. The hand-written C runtime is useful for a compact core target,
   but reproducing 400+ intrinsics and all value kinds independently creates a
   permanent semantic-drift risk.
3. **Own structural semantics.** Add compiler rules and IR nodes for modules,
   objects/method dispatch, assignment/rebinding, structured exceptions, and
   interpolation. Each starts with a host-pinned corpus case.
4. **Expand differential testing.** Keep importing Arturo's upstream language
   suite and broadening deterministic randomized and negative tests. Compare
   stdout, stderr, exit status, and filesystem effects—not stdout alone.
5. **Harden the product.** Add source locations and diagnostics, sanitizers and
   leak checks for the C runtime, deterministic/reproducible builds, CI across
   supported operating systems and architectures, versioned artifacts, and a
   documented compatibility policy.

The current mechanical boundary is 352/395 declarations: no required
declarations remain and 43 platform/runtime-internal declarations are intentionally
unavailable. The local corpus has 111 four-way differential and self-hosted
native cases, plus thirty-two unmodified tests from the pinned upstream suite. `make unsupported`
proves unavailable capabilities fail during compilation without producing an
executable.

`make sanitize` is the ASan/UBSan gate across all 111 local and thirty-two
vendored-upstream native cases. Runtime allocations have an explicit
process-lifetime arena ownership boundary and are reclaimed at exit. Linux
therefore enables LeakSanitizer as a hard gate for every sanitizer case;
Apple Clang runs ASan/UBSan without leak detection because it does not provide
LeakSanitizer on macOS.

`make random` currently exercises 25 reproducible seeds across host, kernel,
compiled bytecode, runIR, and the standalone native executable. `make negative`
checks eleven representative invalid programs: host and native must both fail
without a signal and must agree on output emitted before the error. Exact native
stderr remains enforced separately by `make diagnostics`. Generated collection
indices stay inside the currently supported bounds; out-of-range `slice`
semantics remain a known compatibility gap to close with a fixed regression.

The release gate for a “full Arturo compiler” is: the pinned upstream suite and
the local suite pass against the host for every supported target, no
unclassified intrinsic or syntax construct remains, failures return stable
nonzero statuses, and supported platforms build reproducibly in CI.
