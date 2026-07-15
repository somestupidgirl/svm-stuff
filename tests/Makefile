# Makefile - build the userspace SVM probe for the x86_64 Big Sur target.
#
# Cross-compiles from ANY host (including Apple Silicon): the deployment target
# is pinned to macOS 11 and the arch to x86_64, so the resulting binary can be
# copied straight to the AMD Big Sur machine and run there.
#
#   make -C tests            # -> tests/svmcheck (x86_64, macOS 11+)
#   make -C tests run        # run locally (needs an x86_64 host or Rosetta 2)
#   make -C tests verify     # print the binary's arch/deployment target
#   make -C tests clean

BIN     := svmcheck
SRC     := svmcheck.c

# x86_64 + Big Sur deployment target, regardless of build host.
TARGET  := x86_64-apple-macos11

CC      := $(shell xcrun -f clang)
SDK     := $(shell xcrun --sdk macosx --show-sdk-path)

CFLAGS  := -target $(TARGET) -isysroot $(SDK) -Wall -Wextra -O2

.PHONY: all run verify clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@
	@echo "Built $@ for $(TARGET)"

# Only runs where an x86_64 binary can execute (native Intel, or Apple Silicon
# with Rosetta 2 installed). CPUID reports the *host* CPU, so results are only
# meaningful on the actual AMD machine.
run: $(BIN)
	@if [ "$$(uname -m)" != "x86_64" ] && ! /usr/bin/pgrep -q oahd 2>/dev/null; then \
	  echo "note: non-x86_64 host and Rosetta 2 not detected; copy $(BIN) to the AMD box instead"; \
	fi
	@./$(BIN) || echo "(exit $$? - see svmcheck.c for exit-code meanings)"

verify: $(BIN)
	@file $(BIN)
	@lipo -archs $(BIN)
	@otool -l $(BIN) | awk '/LC_BUILD_VERSION/,/sdk/' | sed -n '1,6p'

clean:
	rm -f $(BIN)
