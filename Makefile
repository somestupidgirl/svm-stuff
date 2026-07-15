# Makefile - build AMDV.kext as a Lilu plugin.
#
# Cross-compiles for x86_64 from ANY host (including Apple Silicon) using the
# MacKernelSDK submodule instead of an Intel Kernel Debug Kit - that is the
# whole point of vendoring MacKernelSDK. No KDK, no Intel Mac required.
#
#   git submodule update --init --recursive
#   make            # -> build/AMDV.kext
#   make sign       # ad-hoc codesign (dev only)
#   make tests      # build the userspace probes in tests/
#   make clean      # remove build/ (does not touch tests/)
#
# Everything this Makefile produces lands in build/; nothing is written to the
# project root.
#
# Requirements: Command Line Tools (clang/ld/codesign) and the two submodules
# (Lilu, MacKernelSDK) checked out under this directory.

PRODUCT      := AMDV
BUILD        := build
BUNDLE       := $(BUILD)/$(PRODUCT).kext
EXEC         := $(BUILD)/$(PRODUCT)
PLIST        := src/Info.plist
BUNDLE_ID    := org.hackintosh.AMDV
MODULE_VER   := 0.1.0

# Target an x86_64 Big Sur kernel regardless of build host arch.
TARGET       := x86_64-apple-macos11

MACSDK       := MacKernelSDK
LILU         := Lilu/Lilu

CXX          := $(shell xcrun -f clang++)
CODESIGN     := $(shell xcrun -f codesign)

# Kernel C++ flags. -nostdinc + MacKernelSDK headers replaces the KDK; clang's
# own builtin headers (stdint.h, ...) are kept. PRODUCT_NAME / MODULE_VERSION
# are consumed by Lilu's plugin_start.cpp and our kern_start.cpp.
CXXFLAGS := -target $(TARGET) -std=gnu++17 \
            -mkernel -fapple-kext -fno-builtin -fno-common \
            -fno-exceptions -fno-rtti -fno-stack-protector \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -DLILU_KEXTPATCH_SUPPORT \
            -DPRODUCT_NAME=$(PRODUCT) -DMODULE_VERSION=$(MODULE_VER) \
            -DBUNDLE_ID=$(BUNDLE_ID) \
            -nostdinc -isystem $(MACSDK)/Headers \
            -I$(LILU) -Isrc \
            -Wall -Wno-unused-parameter -O2

# Kext link. -Xlinker -kext produces MH_KEXT_BUNDLE (filetype 11), which is
# what kmutil requires for x86_64. Do NOT add -r: that yields MH_OBJECT
# (filetype 1), which loads nowhere and is only correct for legacy i386 kexts.
# Lilu API symbols stay undefined here and are resolved at load time via the
# OSBundleLibraries dependency on as.vit9696.Lilu.
LDFLAGS  := -target $(TARGET) -nostdlib -Xlinker -kext \
            -L$(MACSDK)/Library/x86_64 -lkmod

# Our sources plus Lilu's plugin bootstrap (kmod entry + IOService glue).
SRCS := src/kern_start.cpp \
        src/kern_hv_amd.cpp \
        src/kern_svm.cpp \
        src/kern_vmcs_vmcb.cpp \
        src/kern_vmx_emu.cpp \
        $(LILU)/Library/plugin_start.cpp

# kmod_info.c defines _kmod_info/_realmain/_antimain, which libkmod's
# _start/_stop stubs reference. Xcode generates this; a hand-built kext must
# supply it or the kernel has no entry point.
CSRCS := src/kmod_info.c

ASRCS := src/svm_switch.S

# Flatten object paths into build/ so the submodule tree stays clean.
OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.cpp=.o))) \
        $(addprefix $(BUILD)/,$(notdir $(CSRCS:.c=.o))) \
        $(addprefix $(BUILD)/,$(notdir $(ASRCS:.S=.o)))

# `tests` MUST be phony: a tests/ directory exists, so without this make would
# consider the target already satisfied and do nothing.
.PHONY: all sign clean check-submodules tests

all: check-submodules $(BUNDLE)

# Delegate to tests/Makefile (builds the userspace x86_64 probes).
tests:
	@$(MAKE) -C tests

check-submodules:
	@if [ ! -d "$(MACSDK)/Headers" ] || [ ! -d "$(LILU)/Headers" ]; then \
	  echo "error: submodules missing. Run: git submodule update --init --recursive"; \
	  exit 1; \
	fi

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/plugin_start.o: $(LILU)/Library/plugin_start.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CXX) -x c $(filter-out -std=gnu++17 -fno-rtti -fcheck-new -fapple-kext,$(CXXFLAGS)) -c $< -o $@

$(BUILD)/%.o: src/%.S
	@mkdir -p $(BUILD)
	$(CXX) -target $(TARGET) -c $< -o $@

$(EXEC): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(BUNDLE): $(EXEC) $(PLIST)
	@rm -rf $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/MacOS
	@cp $(PLIST) $(BUNDLE)/Contents/Info.plist
	@cp $(EXEC) $(BUNDLE)/Contents/MacOS/$(PRODUCT)
	@# Both of these have silently produced an unloadable kext before, and the
	@# failure only shows up on real hardware via kmutil. Check them here.
	@ft=`otool -h $(BUNDLE)/Contents/MacOS/$(PRODUCT) | awk 'NR==4{print $$5}'`; \
	if [ "$$ft" != "11" ]; then \
	  echo "error: Mach-O filetype $$ft, expected 11 (MH_KEXT_BUNDLE)."; \
	  echo "       filetype 1 (MH_OBJECT) means -r crept back into LDFLAGS."; \
	  exit 1; \
	fi
	@kid=`strings $(BUNDLE)/Contents/MacOS/$(PRODUCT) | grep -x '$(BUNDLE_ID)' | head -1`; \
	if [ "$$kid" != "$(BUNDLE_ID)" ]; then \
	  echo "error: kmod_info.name is not '$(BUNDLE_ID)' (CFBundleIdentifier)."; \
	  echo "       the kernel matches these; a mismatch is rejected at load."; \
	  exit 1; \
	fi
	@echo "Built $(BUNDLE) (x86_64 MH_KEXT_BUNDLE, id $(BUNDLE_ID))"

sign: $(BUNDLE)
	$(CODESIGN) --force --sign - $(BUNDLE)
	@echo "Ad-hoc signed $(BUNDLE) (dev only)."

# Everything we produce is under build/, so removing it is sufficient.
# tests/ has its own clean (make -C tests clean).
clean:
	rm -rf $(BUILD)
