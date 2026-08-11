# arturo-compiler — build, test, verify.
# Requires `arturo` on PATH (see README for the pinned revision).

ARTURO  ?= arturo
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -Oz -flto -fvisibility=hidden
NCCOMP   = tmp/ncomp
RUNTIME_O = runtime/runtime.o
RUNTIME_A = runtime/runtime.a
SOURCES  = src/intrinsics.art src/semantics.art src/front.art src/kernel.art \
           src/ir.art src/backend.art src/cbackend.art runtime/runtime.c

.PHONY: ncomp test verify check clean

# The native compiler: emitted by its own C backend, linked against runtime.c.
ncomp: $(NCCOMP)

$(NCCOMP): tools/cbnative.art $(SOURCES) $(RUNTIME_A)
	$(ARTURO) tools/cbnative.art

$(RUNTIME_O): runtime/runtime.c runtime/runtime.h
	$(CC) $(CFLAGS) -c -Iruntime runtime/runtime.c -o $(RUNTIME_O)

$(RUNTIME_A): $(RUNTIME_O)
	$(AR) rcs $(RUNTIME_A) $(RUNTIME_O)

# 4-way differential: host vs kernel vs compiled vs runIR, every corpus program.
test:
	$(ARTURO) --no-color src/tests.art

# Self-hosting proof: native compiler output == host interpreter, byte for byte.
verify: ncomp
	bash tools/selfhost_test.sh

check: test verify

clean:
	rm -f $(NCCOMP) tmp/ncomp_src.art native_compiler
	rm -f src/*.bcode runtime/*.o runtime/*.a
