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
