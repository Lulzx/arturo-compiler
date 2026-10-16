# Semantic contract: arturoc (C++26) vs. the original compiler

This document is the explicit contract between the C++26 compiler in this
directory and the original self-hosting implementation (Arturo source in
`../src`, C runtime in `../runtime`, specification in `../SPEC.md`). It
states what must agree, where agreement is proven, and where the C++
compiler deliberately does not claim coverage. Anything not listed as owned
below is outside the claim, exactly as in the original's own proof boundary.

## 1. Authority and pin

- The compatibility authority is the pinned host interpreter:
  **Arturo 0.10.0**, upstream revision
  `d8079c6bd4ed170bfd8c5b786a38fd52a9527e97` (the revision recorded in the
  generated `../src/intrinsics.art`).
- When arturoc and the host disagree, the host wins, unless the behavior is
  one of the documented host defects in §8. The original kernel is a
  structural model, not an oracle; arturoc follows the same rule.
- All generated tables in `src/tables.inc` (395 intrinsic arities, aliases,
  infix flags, mutation flags, the pure-intrinsic allowlist, the unsupported
  list, and the shadowable list) are produced by `tools/gen_tables.py` from
  the original's own `src/intrinsics.art`, `src/semantics.art`, and
  `src/front.art`. They are never hand-edited; an Arturo bump re-runs the
  generator, exactly as the original re-runs its extractor.

## 2. The invariant

For every program in the owned subset and every node `n`:

```
eval(env, emit(n)) == eval(env, n)
```

with equality scoped as in the original: stdout, stderr, process exit
status, and filesystem effects compared byte for byte against the host.
Compiling must be meaning-preserving by construction (one emit rule per
construct, mirroring the kernel rule table) and by differential test (§9).

## 3. Pipeline correspondence

| Stage | Original | arturoc (C++26) |
| --- | --- | --- |
| Lexing | donated host lexer (`to :block`), token kinds are Arturo types | `src/lexer.hpp`: lossless tokens with the same kinds (word, label, literal, symbol literal, type, attribute, path, block, inline, quantity, version, color, regex, verbatim/curly/safe strings) |
| Import resolution | `src/modules.art`, compile-time source inclusion; `import.lean` → dictionary, `import.only` → projected names; cycles rejected | `src/modules.hpp`: same line-based resolver, same lowerings, same cycle/missing-file errors |
| Grouping | `src/front.art` `lowerWalk`: left-to-right, arity frames, single-precedence right-associative infix | `src/parser.hpp`: the same stack machine over the generated arity table, including dynamic user-function arities, shadowing renames, `->`, `=>`, alias registration, attributes, and path get/set frames |
| Semantics | `src/kernel.art`: one rule per construct with `eval`/`emit`/`run` columns | `src/emit.hpp`: one emit rule per construct producing the compact IR; semantics live in the shared runtime (below) |
| Semantic IR | Arturo dictionaries `#[op: ...]` | `src/ir.hpp`: flat pools — interned strings, deduplicated constants, nodes as (1-byte opcode, pool indices); binary form `AIR1` (LEB128 varints) in `src/ir.cpp` |
| Optimization | `src/ir.art` `fold`: pure-intrinsic constant folding only | `src/optimizer.hpp`: constant folding (same allowlist, scalar subset), branch folding, dead pure statements, unreachable-code removal, sequence flattening, constant propagation across call-free stretches, reachability compaction. No pass reorders, removes, or executes an effect |
| Verification | differential corpus | `src/verify.hpp`: structural IR verification (pool ranges, per-op shapes) after emission and after optimization, plus the differential suites in `tests/` |
| Execution semantics | `runtime/runtime.c` `runNode`/`runSeq` over builder-built IR | the same `runtime.c`, unmodified except for three additive ABI wrappers (§6). The fast and C backends execute IR through it; the direct backend crosses into it only through its public ABI |

## 4. Grouping contract (must not be "fixed")

Grouping is the host's, including its surprises, as pinned by the original
M1 experiments (`../corpus/m1/RESULTS.md`):

- A word's declared arity opens a call frame; the frame pops at exactly
  `arity` arguments. Arity comes from the generated intrinsic table plus
  user functions registered at their definitions (including same-block
  forward references through later definitions in the same sequence).
- Infix symbols take the preceding value as first operand with a single
  precedence: chains are right-associative, the rightmost operator binds
  tightest (`2 * 3 + 4` is `*(2, +(3,4))`; `10 - 3 - 2` is `-(10, -(3,2))`).
- `if` is arity 2 with no `else`. `and?`/`or?` are eager; lazy logic is
  `∧`/`∨`. `=> [& ...]` expands to parameters `x1, x2, ...` plus an action
  block (two values). Nothing-returning calls cannot be chained.
- An infix symbol always denotes its builtin even when the word of that
  name is rebound as a variable; a rebound builtin *word* is renamed to
  `_shadow_<name>` for both its binding and its loads, while a binding of
  a builtin word to a function keeps the word callable (the original's
  shadowable list).

## 5. IR contract

One IR node per kernel construct; the op set and runtime meaning are the
kernel's (`const`, `load`, `intrinsic`, `define`, `let`, `call`, `function`,
`method`, `params`, `block`, `if`, `unless`, `switch`, `when`, `using`,
`try`, `throws?`, `ensure`, `do`, `return`, `while`, `until`, `loop`,
`higher`, `range`, `dictionary`, `passthrough`, `break`, `continue`,
`constructor`, `is`, plus the internal `__seq`). Differences from the
original representation are representational only:

- Nodes are pool indices, not dictionaries; there are no per-node hash
  lookups or string comparisons after interning.
- `passthrough` remains the only seam where a source token survives, and it
  is used for the same purpose as the original: literal references handed
  to mutating intrinsics and sortable field keys.
- The `higher` op is used only for a literal block action with literal
  parameters (the kernel's `emitHigher` rule); a stored action stays a
  plain intrinsic call carrying a block value.
- Value kinds map one-to-one onto the runtime's `VKind` set (including
  rational/complex via runtime calls, quantity/unit, date/color/regex/
  binary/version/error-kind values, big integers, and token kinds).
  Token-kind preservation matches the finished native compiler's
  `PRESERVE_TOKEN_KINDS` behavior.
- AIR1 emission is deterministic: the same source produces the same bytes.

## 6. Backend contract (three tiers, one semantics)

| Tier | Flag | What executes | Guarantee |
| --- | --- | --- | --- |
| 0 (default, fast) | `--backend=fast` | the prebuilt `arturo-stub` (runtime + AIR loader) with the program's AIR1 payload appended; no C compiler runs at program-compile time | execution is the runtime's own `runNode`, hence the original native semantics by construction |
| 1 (C) | `--backend=c` | C generated from the optimized IR on the runtime builder API, compiled with `cc -O2` and linked against the runtime | the same IR the stub runs, rebuilt as C data |
| 2 (direct) | `--backend=direct` | C that performs the computation: C control flow for `if`/`while`/`return`, frame bindings via `rt_define_local`/`rt_let`, direct `rt_builtin` calls, checked inline integer `add`/`sub`/`mul` with generic fallback on overflow or non-integers, direct C calls to binding-stable user functions (defined exactly once at top level, never rebound, with an `env_bound` guard so an unexecuted definition still fails exactly like the original), and per-expression fallback to `runNode` for anything unproven (attributes, mutating references, action loops, exotic nodes) | every lowered form is guarded; a failed guard takes the generic path with identical effects and evaluation order |

The runtime ABI used by generated code is the public surface in
`../runtime/runtime.h`: values, environments, `rt_builtin`, `runNode`, and
the three additive wrappers `rt_apply`, `rt_define_local`, `rt_let`
(semantics: apply a function value; bind in the current frame only;
rebind the nearest binding — the runtime's own internal operations,
exported unchanged). Generated code never depends on runtime internals.

Binding classification at each call site is explicit: a callee is a known
intrinsic (generated table, shadowable names checked against the current
environment first), a binding-stable user function (direct call), or a
dynamic binding (generic apply). No name is ever replaced by an intrinsic
merely because it looks familiar.

Effects are classified from the generated metadata: pure (folder-eligible,
`../src/semantics.art`), mutating-through-reference (upstream `PathLiteral`
declarations), and everything else (conservatively effectful, including all
I/O). The optimizer folds only the first class, only on constants, and
never when the runtime could fail in a way the folder does not model
exactly (division by zero, overflow beyond `int64`, exotic kinds).

## 7. Errors

- Compile-time rejection, with a stable diagnostic and no output
  executable, for the original's intentionally unavailable capabilities
  (the 16 declarations classified `unsupported` in
  `../config/intrinsic-policy.tsv`: sockets, servers, databases, GUI,
  `eval`, and friends). arturoc's diagnostic is
  `unsupported standalone capability: <name>`.
- Invalid programs (unbound names, arity failures, runtime type errors)
  fail with a nonzero exit status and no signal in both engines. Exact
  host diagnostic *formatting* (fancy panels, source spans, Arturo-level
  stack frames) is a tooling surface the original also lists as partial;
  it is not part of this contract. arturoc runtime errors do identify the
  failing statement as `file:line` on all three backends (per-node source
  lines in AIR format v2; v1 payloads remain readable with no line).

## 8. Documented host-defect exception (tested, not excluded)

Arturo 0.10.0 mishandles string append with an integer right operand
(`print "n=" ++ 13` raises a misleading arity error; the assignment form
may silently print nothing). Like the original native runtime, arturoc
uses the useful semantics (`n=13`) for integer, logical, and character
operands. This is a divergence from the *host interpreter* only: it is
byte-identical to the original native compiler, and it is verified by the
original's own golden outputs in `../compat/` (all compat cases, including
`01_string_integer_append`, run in `tests/run_tests.sh`). The residual
boundary is exactly the original's: the native compilers and the host
disagree here, on purpose.

## 9. Proof boundary (what is verified, and how)

Verified by `tests/run_tests.sh` against the pinned host (stdout, stderr,
exit status byte for byte):

- all **112** local corpus programs (`tests/owned-corpus.txt`) on the fast
  backend, and with `-O0` vs `-O1` optimizer equivalence on the same set;
- all **32** pinned upstream programs (`tests/owned-upstream.txt`) on the
  fast backend;
- all of the original's `../compat/` cases against their golden outputs
  (the tested host-defect exceptions of §8);
- the C and direct backends on a representative subset
  (`tests/backend-subset.txt`);
- compile-time rejection of unsupported capabilities, nonzero failure of
  invalid programs, and AIR determinism;
- regression programs pinning past optimizer and constant-pool bugs
  (float constants closer than 1e-6, integer comparisons above 2^53,
  constant propagation across rebinding `if`/`try`/`do` bodies) against
  the host and across `-O0`/`-O1` and the C/direct backends;
- rejection of malformed AIR payloads by the stub (unknown opcode,
  out-of-range pool indices, misshapen nodes) with a diagnostic and no
  signal, plus the CLI contract, C-backend shell quoting and intermediate
  cleanup, and nesting-depth limits (pathological nesting is a compile
  error, not a crash).

The suite runs in CI (the `cpp26` job in `../.github/workflows/ci.yml`,
alongside the original's parity and sanitize jobs).

Not claimed: the original's self-hosting proof (arturoc is written in
C++26 and does not compile itself), and bit-identical behavior for
programs outside the owned lists (for example file-system, network, and
process behavior beyond the corpus fixtures). Constructs outside the
owned subset either compile and pass, or fail with a compile-time
diagnostic; they must never silently produce a wrong-result executable —
any such case found is a contract bug.
