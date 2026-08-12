# Pinned upstream compatibility tests

These files are unmodified snapshots from Arturo revision
`d8079c6bd4ed170bfd8c5b786a38fd52a9527e97`. Their original paths are
the matching paths below `tests/` in the Arturo repository.

`tools/upstream_test.sh` compiles each snapshot with the self-hosted compiler
and compares stdout, stderr, exit status, and filesystem effects against the
pinned Arturo executable in isolated directories.

Only tests whose language surface is part of the standalone product are
vendored here. Tests that exercise declarations classified as intentionally
unavailable in `config/intrinsic-policy.tsv` remain outside this gate.

The vendored set is the 23 original `tests/unittests/*.art` files plus
`strings.art` (multiline `««…»»` strings, `{…}`/`{:…:}`/`{!…}` blocks, and
`---` dash strings). The full upstream suite also contains files the
standalone product does not reproduce and that are therefore excluded:

- `lib.numbers.art`, `lib.iterators.art` fail on the pinned host itself
  (`sign` and a `collect.after` assertion are absent/broken at revision
  `d8079c6`), so no engine can pass them.
- `evaluator.art` prints `to :bytecode` opcode bytes that are internal to
  the donated VM; the native runtime does not reimplement the bytecode
  compiler and documents the fixed `[32 189 223]` stub instead.
- `sorting.art` requires locale-aware Unicode collation (`LANG`-dependent
  `sort`), which is outside the portable standalone surface.
- `lib.files.art` / `lib.comparison.art` use declarations classified as
  intentionally unavailable (`zip`, `volume`, `config`, `permissions`,
  `store`).
