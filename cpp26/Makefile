# arturoc -- the optimizing native Arturo compiler in C++26.
#
#   make            build bin/arturoc and bin/arturo-stub
#   make test       build + run the differential test suite (needs the pinned
#                   `arturo` host on PATH or in ../arturo-bin)
#   make bench      compile-time and run-time comparison on tools/bench
#   make clean
ROOT    := $(abspath ..)
CXX     ?= clang++
CC      ?= cc
STD     := -std=c++2c
ifeq ($(shell uname -s),Darwin)
STD     := -std=c++2c
endif
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS   ?= -O2 -fvisibility=hidden
BIN      := bin
SRC      := src

.PHONY: all test bench clean

all: $(BIN)/arturoc $(BIN)/arturo-stub

$(BIN)/runtime.o: $(ROOT)/runtime/runtime.c $(ROOT)/runtime/runtime.h $(ROOT)/runtime/intrinsic_arity.inc
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -I$(ROOT)/runtime -c $(ROOT)/runtime/runtime.c -o $@

$(ROOT)/runtime/intrinsic_arity.inc: $(ROOT)/src/intrinsics.art $(ROOT)/tools/gen_intrinsic_arity.sh
	cd $(ROOT) && sh tools/gen_intrinsic_arity.sh

$(BIN)/ir.o: $(SRC)/ir.cpp $(SRC)/ir.hpp
	@mkdir -p $(BIN)
	$(CXX) $(STD) $(CXXFLAGS) -Isrc -c $(SRC)/ir.cpp -o $@

$(BIN)/arturoc: $(SRC)/main.cpp $(SRC)/ir.hpp $(SRC)/lexer.hpp $(SRC)/parser.hpp $(SRC)/emit.hpp $(SRC)/emit_c.hpp $(SRC)/direct_c.hpp $(SRC)/modules.hpp $(SRC)/optimizer.hpp $(SRC)/verify.hpp $(SRC)/tables.inc $(BIN)/ir.o
	$(CXX) $(STD) $(CXXFLAGS) -Isrc $(SRC)/main.cpp $(BIN)/ir.o -o $@

$(BIN)/arturo-stub: $(SRC)/stub.cpp $(SRC)/ir.hpp $(BIN)/ir.o $(BIN)/runtime.o
	$(CXX) $(STD) $(CXXFLAGS) -Isrc -I$(ROOT)/runtime $(SRC)/stub.cpp $(BIN)/ir.o $(BIN)/runtime.o -lm -o $@

src/tables.inc: tools/gen_tables.py $(ROOT)/src/intrinsics.art $(ROOT)/src/semantics.art $(ROOT)/src/front.art
	python3 tools/gen_tables.py

test: all
	bash tests/run_tests.sh

bench: all
	bash tests/bench.sh

clean:
	rm -f $(BIN)/arturoc $(BIN)/arturo-stub $(BIN)/*.o
