# arturo-compiler — build, test, verify.
# Requires `arturo` on PATH (see README for the pinned revision).

ARTURO  ?= arturo
CC      ?= cc
AR      ?= ar
# Optimize for compiler turnaround. `-Oz -flto` spent over six minutes in the
# bootstrap link on macOS, while `-O1` builds the same compiler in seconds and
# keeps its corpus compile throughput effectively unchanged.
CFLAGS  ?= -O1 -fvisibility=hidden
NCCOMP   = tmp/ncomp
RUNTIME_O = runtime/runtime.o
RUNTIME_A = runtime/runtime.a
SOURCES  = src/intrinsics.art src/modules.art src/semantics.art src/front.art src/kernel.art \
           src/ir.art src/backend.art src/cbackend.art runtime/runtime.c

.PHONY: ncomp test verify upstream compat diagnostics unsupported coverage random negative sanitize check clean

# The native compiler: emitted by its own C backend, linked against runtime.c.
ncomp: $(NCCOMP)

$(NCCOMP): tools/cbnative.art $(SOURCES) $(RUNTIME_A)
	$(ARTURO) tools/cbnative.art

runtime/intrinsic_arity.inc: src/intrinsics.art tools/gen_intrinsic_arity.sh
	sh tools/gen_intrinsic_arity.sh

$(RUNTIME_O): runtime/runtime.c runtime/runtime.h runtime/intrinsic_arity.inc Makefile
	$(CC) $(CFLAGS) -c -Iruntime runtime/runtime.c -o $(RUNTIME_O)

$(RUNTIME_A): $(RUNTIME_O)
	$(AR) rcs $(RUNTIME_A) $(RUNTIME_O)

# 4-way differential: host vs kernel vs compiled vs runIR, every corpus program.
test:
	$(ARTURO) --no-color src/tests.art

# Self-hosting proof: native compiler output == host interpreter, byte for byte.
verify: ncomp
	bash tools/selfhost_test.sh

# Unmodified programs imported from the pinned Arturo upstream suite.
upstream: ncomp
	bash tools/upstream_test.sh

diagnostics: ncomp
	bash tools/diagnostic_test.sh

# Deliberate improvements over pinned host defects have their own expected
# outputs; they do not belong in the host-parity corpus above.
compat: ncomp
	bash tools/compat_test.sh

# Intentionally unavailable capabilities must be rejected by the compiler,
# before any output executable is produced.
unsupported: ncomp
	bash tools/unsupported_test.sh

# Report the native runtime's declared-intrinsic coverage. This is a scope
# metric, not a correctness metric; `test`/`verify` prove implemented behavior.
coverage:
	bash tools/language_coverage.sh

# Deterministic generated valid programs stress combinations and boundaries
# beyond the fixed corpus while remaining reproducible by seed.
random: ncomp
	bash tools/random_differential_test.sh

negative: ncomp
	bash tools/negative_differential_test.sh

sanitize: ncomp
	bash tools/sanitize_test.sh

check: coverage test verify upstream compat diagnostics unsupported random negative

clean:
	rm -f $(NCCOMP) tmp/ncomp_src.art native_compiler
	rm -f src/*.bcode runtime/*.o runtime/*.a runtime/intrinsic_arity.inc
