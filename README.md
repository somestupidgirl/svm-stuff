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

This plugin is honest about that boundary:

| Piece | Status |
|-------|--------|
| Lilu integration, cross-compile, load path | ✅ done, builds clean |
| SVM detection + `EFER.SVME` + host save area + VMCB (`kern_svm.*`) | ✅ real |
| Hooking the kernel VMX gate so `kern.hv_support` reports available | ✅ implemented (necessary, **not** sufficient) |
| **VMX→SVM guest-run translation** (`vmxToSvmVcpuRun`) | ❌ **stubbed** — compiled out, specified in comments |

So after loading, `sysctl kern.hv_support` can be made to read `1`, but a real
guest launched through `Hypervisor.framework` will still fault, because the
VMCS↔VMCB translation in [`src/kern_hv_amd.cpp`](src/kern_hv_amd.cpp) (the
`#if 0` block) is not implemented. It is left explicit rather than faked. This
has **not** been tested on AMD hardware — treat it as a correct-by-construction
scaffold, not a working hypervisor shim.

If you only need the SVM hardware brought up (the part that genuinely works),
that lives in `kern_svm.*` and runs independently of the gate hook.

## Layout

```
Info.plist            Lilu-plugin bundle (depends on as.vit9696.Lilu)
Makefile              Cross-compiles x86_64 via MacKernelSDK (no KDK/Intel Mac)
src/SVM.h             SVM MSRs, CPUID leaves, VMCB layout, instruction wrappers
src/kern_svm.*        SvmBackend: detect + enable SVM, allocate VMCB
src/kern_hv_amd.*     VMX gate hook + the (stubbed) VMX→SVM shim spec
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
