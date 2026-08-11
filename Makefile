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
SOURCES  = src/intrinsics.art src/semantics.art src/front.art src/kernel.art \
           src/ir.art src/backend.art src/cbackend.art runtime/runtime.c

.PHONY: ncomp test verify compat coverage check clean

# The native compiler: emitted by its own C backend, linked against runtime.c.
ncomp: $(NCCOMP)

$(NCCOMP): tools/cbnative.art $(SOURCES) $(RUNTIME_A)
	$(ARTURO) tools/cbnative.art

$(RUNTIME_O): runtime/runtime.c runtime/runtime.h Makefile
	$(CC) $(CFLAGS) -c -Iruntime runtime/runtime.c -o $(RUNTIME_O)

$(RUNTIME_A): $(RUNTIME_O)
	$(AR) rcs $(RUNTIME_A) $(RUNTIME_O)

# 4-way differential: host vs kernel vs compiled vs runIR, every corpus program.
test:
	$(ARTURO) --no-color src/tests.art

# Self-hosting proof: native compiler output == host interpreter, byte for byte.
verify: ncomp
	bash tools/selfhost_test.sh

# Deliberate improvements over pinned host defects have their own expected
# outputs; they do not belong in the host-parity corpus above.
compat: ncomp
	bash tools/compat_test.sh

# Report the native runtime's declared-intrinsic coverage. This is a scope
# metric, not a correctness metric; `test`/`verify` prove implemented behavior.
coverage:
	bash tools/language_coverage.sh

check: test verify compat

clean:
	rm -f $(NCCOMP) tmp/ncomp_src.art native_compiler
	rm -f src/*.bcode runtime/*.o runtime/*.a
