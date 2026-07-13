# AMDV — AMD-V (SVM) enablement kext for macOS Big Sur

A macOS kernel extension that detects and enables **AMD-V / SVM** hardware
virtualization on AMD ("Hackintosh") systems running macOS Big Sur (11.x).

## What this is — and what it is not

macOS never shipped on AMD hardware, so its in-kernel hypervisor (behind
`Hypervisor.framework`) is **Intel VMX only** and closed-source. **No kext can
retarget Apple's hypervisor to AMD-V.** So this project does what third-party
hypervisor drivers (VirtualBox's `VBoxDrv`, VMware's `vmmon`) do instead: it
programs the AMD **SVM** hardware directly.

**Implemented and safe to run:**

- CPUID-based SVM capability detection (vendor, `0x80000001` ECX.SVM,
  `0x8000000A` feature flags), plus firmware `VM_CR.SVMDIS` / SVM-lock checks.
- Enabling `EFER.SVME` and installing a valid host state-save area
  (`VM_HSAVE_PA`).
- Allocating a page-aligned, physically-contiguous VMCB and setting a baseline
  control area (intercepts + ASID).

**Deliberately *not* done (skeleton only):**

- The `VMRUN` guest-entry loop. It is compiled out behind
  `AMDV_ENABLE_GUEST_LAUNCH` because launching a guest requires guest physical
  memory (nested page tables), a fully populated VMCB state-save area, and
  per-CPU thread pinning. Running `VMRUN` without those will fault. See
  `amdv_run_guest()` in [`src/AMDV.cpp`](src/AMDV.cpp) for the exact TODO list.
- Multi-CPU enablement. `SVME` is per-core; this driver enables it on the
  current processor only. A production build must broadcast via a CPU
  rendezvous. Each such site is flagged in the source.

This has **not** been tested on real AMD hardware — treat it as a correct-by-
construction foundation, not a shipping hypervisor.

## Layout

```
Info.plist          Bundle + IOKitPersonalities (matches on IOResources)
Makefile            Hand-built kext against a Big Sur KDK
src/SVM.h           SVM MSRs, CPUID leaves, VMCB layout, instruction wrappers
src/AMDV.hpp/.cpp   IOService that detects + enables SVM on start()
```

`src/SVM.h` is self-contained (only `stdint.h`) and its VMCB layout is checked
at compile time by a `_Static_assert` against the 4-KiB page size.

## Building

You need the **Kernel Debug Kit** matching your running build, from
<https://developer.apple.com/download/all/> ("Kernel Debug Kit 11.x").

```sh
make KDK=/Library/Developer/KDKs/KDK_11.7.10_20G1427.kdk
make sign        # ad-hoc codesign for local development
```

The Makefile uses the standard hand-built-kext flag set (`-mkernel
-fapple-kext -fno-exceptions -fno-rtti`, linked with `-kext`). An Xcode
"Generic Kernel Extension" target is an equally valid alternative.

## Loading on Big Sur (important caveats)

Big Sur tightened kext loading considerably:

1. **SIP must permit unsigned/third-party kexts.** From recoveryOS,
   `csrutil` must allow kext loading (and, on Apple-Silicon-era tooling,
   `csrutil authenticated-root disable` if you modify the boot kext
   collection). A properly notarized kext avoids most of this; an ad-hoc
   signed dev build does not.
2. **`kextload`/`kextutil` are deprecated.** Loading now goes through
   `kmutil`:

   ```sh
   sudo cp -R AMDV.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/AMDV.kext
   sudo kmutil load -p /Library/Extensions/AMDV.kext      # dev load
   # or rebuild the auxiliary/boot collection and reboot:
   sudo kmutil install --update-all
   ```
3. Watch the log:

   ```sh
   log stream --predicate 'senderImagePath CONTAINS "AMDV"' --style compact
   # or after the fact:
   log show --last 5m --predicate 'eventMessage CONTAINS "AMDV:"'
   ```

   On success you'll see the detected SVM revision, ASID count, feature
   flags, and the `VM_HSAVE_PA` / VMCB physical addresses.

On non-AMD or firmware-locked machines the kext loads, logs why SVM is
unavailable, and stays idle.

## Safety

Enabling `EFER.SVME` and executing SVM instructions is privileged, kernel-mode
CPU programming. A malformed VMCB or an errant `VMRUN` can panic or hang the
machine. Test in a disposable Big Sur install, keep a recovery path, and leave
`AMDV_ENABLE_GUEST_LAUNCH` at `0` until the guest-memory setup is real.

## License / status

Educational reference implementation, version 0.1.0. No warranty.
