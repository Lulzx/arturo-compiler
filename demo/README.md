# demo — a real ecosystem program, compiled natively

`getbuiltins.art` is an **unmodified** snapshot of the Arturo project's own
`tools/getbuiltins.art` (revision
`d8079c6bd4ed170bfd8c5b786a38fd52a9527e97`). It walks the `symbols`
reflection table and lists the builtin names by category:

- `1` — names ending in `?` (predicates)
- `2` — the builtin function names
- `3` — non-function constants

It is the program used to exercise the self-hosted native compiler beyond
the parity corpus: it depends on `symbols`, `var`, `function?`, `suffix?`,
`chop`, `sort`, block set-difference (`--`), `filter` with a closure, and
`join.with` inside a `case` arm — several of which were missing or broken
until the fixes in commit `b176aba`.

## Build and run

    make ncomp                          # self-hosted native compiler
    ./tmp/ncomp demo/getbuiltins.art demo/getbuiltins
    ./demo/getbuiltins 2                # the builtin function list

## Expected output

The native binary lists the builtin names joined by `|`, e.g.:

    abs|absolute|accept|acos|acosh|acsec|acsech|actan|actanh|add|after|...

The list is byte-compatible with the pinned host for the portable declared
surface. `symbols` intentionally reports the standalone runtime's declared
intrinsics, so a handful of VM-internal names that exist in the host's full
symbol table (unit/quantity predicates such as `acceleration?`, and UI or
stream names such as `webview`/`dialog`) are absent from the native output;
calling those names is already rejected at compile time per
`config/intrinsic-policy.tsv`.
