# AMDV — an AMD-V (SVM) Lilu plugin for macOS Big Sur

A [Lilu](https://github.com/acidanthera/Lilu) plugin that brings up AMD **SVM
(AMD-V)** hardware and hooks the XNU kernel's VMX availability gate, as a
foundation for making virtualization work on AMD ("Hackintosh") systems
running macOS Big Sur (11.x). It cross-compiles for x86_64 from **any** host,
including Apple Silicon, via the vendored **MacKernelSDK**.

## Read this first — what actually works

Apple's `Hypervisor.framework` is backed by **VMX (Intel VT-x)** code compiled
into the closed XNU kernel: VMCS setup through `vmwrite`/`vmread`, guest entry
through `vmlaunch`/`vmresume`. Those instructions **`#UD` (fault) on AMD.**
"Making the framework SVM-compatible" is therefore **not a one-symbol hook** —
it requires emulating the entire VMX/VMCS model on top of SVM/VMCB. That
translation layer does not exist in the wild and is a research-scale effort.

This plugin is honest about that boundary. Progress is now split across the
translation core (built) and the interception plumbing (still required):

| Piece | Status |
|-------|--------|
| Lilu integration, cross-compile, load path | ✅ done, builds clean |
| SVM detection + `EFER.SVME` + host save area + VMCB (`kern_svm.*`) | ✅ real |
| Hooking the kernel VMX gate so `kern.hv_support` reports available | ✅ implemented (necessary, **not** sufficient) |
| Shadow VMCS + **VMCS↔VMCB field translation** (`kern_vmcs_vmcb.*`) | ✅ implemented, **untested on HW** |
| Segment AR↔attrib conversion, SVM-exit→VMX-reason map | ✅ implemented |
| VMX instruction **semantics** (vmread/write/ptrld/launch…) (`kern_vmx_emu.*`) | ✅ implemented |
| Guest world-switch asm (`svm_switch.S`) | ⚠️ written, **gated off** (`AMDV_ENABLE_GUEST_LAUNCH`), unvalidated |
| `#UD` trap → `decodeVmx()` (instruction decode from trap frame) | ❌ **scaffolded** — returns false |
| **EPT→NPT** page-table rebuild | ❌ **not done** (pointer wired with a TODO) |
| VM-exit qualification / intr-info back-translation | ❌ partial |

What this means in practice: the pipeline that turns Apple's `vmwrite`s into a
VMCB, runs it, and maps the exit back is **written and reviewable**, but two
things stop a guest from actually running: (1) nothing yet routes the `#UD`
that Apple's VMX instructions raise into `VmxEmulator::handleUD`, and the
decode that would feed it is stubbed; (2) an EPT pointer is not a valid AMD
nested-page-table root, so guest memory would not be mapped. Both are called
out at their sites. **None of this is tested on AMD hardware.**

The SVM hardware bring-up (`kern_svm.*`) still works independently of all this.

### Where the hard parts are (start here to continue)

- `src/kern_vmcs_vmcb.cpp` — `translateVmcsToVmcb`: the `SEC_CTL_ENABLE_EPT`
  branch needs an EPT→NPT walk; MSR/IO bitmaps need MSRPM/IOPM translation.
- `src/kern_vmx_emu.cpp` — `decodeVmx` (use Lilu's bundled `hde64`), and the
  RIP-advance / VMX-flag write-back in `handleUD`.
- `src/kern_hv_amd.cpp` — install the per-CPU `#UD` (vector 6) IDT hook that
  calls `handleUD`.
- `src/svm_switch.S` — validate the world-switch on real hardware before
  setting `AMDV_ENABLE_GUEST_LAUNCH=1`.

## Layout

```
src/Info.plist        Lilu-plugin bundle (depends on as.vit9696.Lilu)
Makefile              Cross-compiles x86_64 via MacKernelSDK (no KDK/Intel Mac)
src/SVM.h             SVM MSRs, CPUID leaves, VMCB layout + state-save accessors
src/kern_svm.*        SvmBackend: detect + enable SVM, allocate VMCB
src/kern_vmx.hpp      VMX field encodings, exit reasons, ShadowVMCS
src/kern_vmcs_vmcb.*  VMCS↔VMCB translation + SVM/VMX exit mapping
src/kern_vmx_emu.*    VMX instruction emulation + vmlaunch/vmresume run loop
src/svm_switch.S      Guest world-switch (VMLOAD/VMRUN/VMSAVE), gated off
src/kern_hv_amd.*     VMX gate hook; binds the emulator to the VMCB
src/kern_start.cpp    PluginConfiguration + init entry point
Lilu/                 submodule — plugin API + bootstrap (plugin_start.cpp)
MacKernelSDK/         submodule — kernel headers + libkmod for cross-compile
```

## Building

```sh
git submodule update --init --recursive
make                 # -> AMDV.kext (x86_64), works on Apple Silicon too
make sign            # ad-hoc codesign for local development
```

The Makefile compiles with `-target x86_64-apple-macos11`, `-nostdinc
-isystem MacKernelSDK/Headers`, and links the relocatable kext against
`MacKernelSDK/Library/x86_64/libkmod.a`. Lilu API symbols stay undefined at
link time and are resolved on load via the `OSBundleLibraries` dependency on
`as.vit9696.Lilu` — verified with `nm -u`.

## Loading on Big Sur

Requires **Lilu.kext** to be present and loaded, plus the usual Big Sur kext
prerequisites:

1. SIP must permit third-party kexts (`csrutil`), and modifying the boot kext
   collection needs `csrutil authenticated-root disable`.
2. Use `kmutil` (not the deprecated `kextload`):

   ```sh
   sudo cp -R Lilu.kext AMDV.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/Lilu.kext /Library/Extensions/AMDV.kext
   sudo kmutil load -p /Library/Extensions/Lilu.kext
   sudo kmutil load -p /Library/Extensions/AMDV.kext
   ```
   On a Hackintosh these normally go in the bootloader (OpenCore) kext list
   instead, with Lilu ordered before AMDV.
3. Boot args: `-amdvdbg` (verbose), `-amdvoff` (disable), `-amdvbeta` (allow on
   untested kernels). Watch the log:

   ```sh
   log stream --predicate 'eventMessage CONTAINS "AMDV"' --style compact
   ```

## Safety

Setting `EFER.SVME` and executing SVM instructions is privileged kernel CPU
programming; a bad VMCB or an errant `VMRUN` can panic the machine. The
guest-run path is intentionally left disabled. Test only in a disposable Big
Sur install with a recovery path.

## Status

Educational reference / research scaffold, v0.1.0. No warranty. Lilu and
MacKernelSDK are © Acidanthera under their own licenses (see submodules).
