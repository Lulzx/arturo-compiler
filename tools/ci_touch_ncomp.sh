#!/bin/bash
# After actions/cache restores the bootstrapped compiler, its files carry
# whatever mtimes the extraction gave them. Re-stamp them in dependency order so
# make sees every product as newer than its inputs and does not rebuild.
set -euo pipefail
cd "$(dirname "$0")/.."
for f in runtime/intrinsic_arity.inc runtime/runtime.o runtime/runtime.a tmp/ncomp_src.art tmp/ncomp; do
    [ -e "$f" ] || { echo "cache incomplete: missing $f" >&2; exit 0; }
    touch "$f"; sleep 1
done
ls -la tmp/ncomp runtime/runtime.a
