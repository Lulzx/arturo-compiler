#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sed -n 's/^    "\([^"]*\)": #\[arity: \(-\{0,1\}[0-9][0-9]*\).*/    {"\1", \2},/p' \
    src/intrinsics.art > runtime/intrinsic_arity.inc
