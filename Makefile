# Makefile - build AMDV.kext against a Big Sur Kernel Development Kit.
#
# Requirements:
#   * Command Line Tools / Xcode (for clang, ld, codesign).
#   * A Kernel Debug Kit matching your running build, from
#     https://developer.apple.com/download/all/  ("Kernel Debug Kit 11.x").
#     Point KDK at it, e.g.:
#       make KDK=/Library/Developer/KDKs/KDK_11.7.10_20G1427.kdk
#
# Targets:
#   make            - compile and assemble AMDV.kext
#   make sign       - ad-hoc codesign the bundle (dev only; see README)
#   make clean

BUNDLE      := AMDV.kext
EXEC        := AMDV
BUNDLE_ID   := org.hackintosh.AMDV

KDK         ?= $(lastword $(wildcard /Library/Developer/KDKs/*.kdk))
SDK         := $(shell xcrun --sdk macosx --show-sdk-path)
CXX         := $(shell xcrun -f clang++)
CODESIGN    := $(shell xcrun -f codesign)

KHDRS       := $(KDK)/System/Library/Frameworks/Kernel.framework/Headers
KPRIV       := $(KDK)/System/Library/Frameworks/Kernel.framework/PrivateHeaders

# Standard flags for a hand-built C++ kernel extension.
CXXFLAGS := -arch x86_64 -std=gnu++17 \
            -fno-builtin -fno-common -mno-red-zone -mkernel -fapple-kext \
            -fno-exceptions -fno-rtti -fcheck-new -msoft-float \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -nostdinc -isysroot $(SDK) \
            -I$(KHDRS) -I$(KPRIV) -Isrc \
            -Wall -Wno-unused-parameter -O2

LDFLAGS  := -arch x86_64 -static -nostdlib -r -Xlinker -kext -lkmod -lcc_kext

SRCS     := src/AMDV.cpp
OBJS     := $(SRCS:.cpp=.o)

.PHONY: all sign clean check-kdk

all: check-kdk $(BUNDLE)

check-kdk:
	@if [ -z "$(KDK)" ] || [ ! -d "$(KHDRS)" ]; then \
	  echo "error: no KDK found. Install a Big Sur Kernel Debug Kit and pass KDK=/path/to/KDK_11.x.kdk"; \
	  exit 1; \
	fi
	@echo "Using KDK: $(KDK)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(EXEC): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(BUNDLE): $(EXEC) Info.plist
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS
	@cp Info.plist $(BUNDLE)/Contents/Info.plist
	@cp $(EXEC) $(BUNDLE)/Contents/MacOS/$(EXEC)
	@echo "Built $(BUNDLE)"

sign: $(BUNDLE)
	$(CODESIGN) --force --sign - $(BUNDLE)
	@echo "Ad-hoc signed $(BUNDLE) (dev only)."

clean:
	rm -rf $(OBJS) $(EXEC) $(BUNDLE)
