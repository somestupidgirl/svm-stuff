/*
 * AMDV.cpp - AMD-V (SVM) enablement driver for macOS Big Sur (11.x).
 *
 * SCOPE / HONESTY NOTE
 * --------------------
 * This kext programs AMD's SVM hardware directly, the same way third-party
 * hypervisor drivers (VirtualBox's VBoxDrv, VMware's vmmon) do. It does NOT
 * and cannot retarget Apple's Hypervisor.framework, whose in-kernel VMX
 * logic is closed and Intel-only. What is implemented and safe to run:
 *
 *   - full CPUID-based SVM capability detection,
 *   - EFER.SVME enable with a valid VM_HSAVE_PA host save area,
 *   - VMCB allocation and baseline control-area setup.
 *
 * The actual VMRUN guest-entry loop is left as a clearly marked skeleton:
 * it needs a guest address space (nested page tables) and per-CPU pinning
 * that a production hypervisor must provide. It is compiled out behind
 * AMDV_ENABLE_GUEST_LAUNCH so a load of this kext never blindly executes
 * VMRUN on hardware it was not verified against.
 *
 * Enablement runs on the current processor only. A real hypervisor must
 * broadcast enable/disable to every CPU (SVME is per-core) via a rendezvous;
 * that is noted at each call site rather than faked here.
 */

#include "AMDV.hpp"

#include <i386/proc_reg.h>   /* rdmsr64 / wrmsr64                         */
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <string.h>

#define super IOService
OSDefineMetaClassAndStructors(AMDV, IOService)

#define LOG(fmt, ...) IOLog("AMDV: " fmt "\n", ##__VA_ARGS__)

/* Toggle to compile the (unverified) guest-entry path. Off by default. */
#ifndef AMDV_ENABLE_GUEST_LAUNCH
#define AMDV_ENABLE_GUEST_LAUNCH 0
#endif

/* ------------------------------------------------------------------ */
/* Detection                                                          */
/* ------------------------------------------------------------------ */
bool AMDV::detectSVM(void)
{
    uint32_t a, b, c, d;

    /* Vendor string must be "AuthenticAMD" (EBX="Auth" EDX="enti" ECX="cAMD"). */
    amdv_cpuid(CPUID_VENDOR, 0, &a, &b, &c, &d);
    if (b != 0x68747541u || d != 0x69746e65u || c != 0x444d4163u) {
        LOG("not an AMD processor (vendor %.4s%.4s%.4s) - SVM unsupported",
            (char *)&b, (char *)&d, (char *)&c);
        return false;
    }

    /* Extended leaf 0x8000000A must exist. */
    amdv_cpuid(CPUID_EXT_MAX, 0, &a, &b, &c, &d);
    if (a < CPUID_SVM_FEATURES) {
        LOG("extended CPUID leaf 0x%08X unavailable - no SVM", CPUID_SVM_FEATURES);
        return false;
    }

    /* 0x80000001 ECX bit 2 = SVM support. */
    amdv_cpuid(CPUID_EXT_FEATURES, 0, &a, &b, &c, &d);
    if (!(c & CPUID_ECX_SVM)) {
        LOG("CPU does not advertise SVM (0x80000001 ECX.2 clear)");
        return false;
    }

    /* SVM feature identification. */
    amdv_cpuid(CPUID_SVM_FEATURES, 0, &a, &b, &c, &d);
    fSvmRevision = a & 0xFF;
    fNumAsids    = b;
    fSvmFeatures = d;

    LOG("SVM present: rev %u, %u ASIDs, features 0x%08X%s%s%s",
        fSvmRevision, fNumAsids, fSvmFeatures,
        (d & SVM_FEATURE_NP)    ? " NP"    : "",
        (d & SVM_FEATURE_NRIPS) ? " NRIPS" : "",
        (d & SVM_FEATURE_VMCBCLEAN) ? " VMCBCLEAN" : "");

    /*
     * VM_CR.SVMDIS + SVM lock: if firmware disabled SVM and locked it
     * (CPUID 0x8000000A EDX.SVML set), SVME cannot be turned on. When SVML
     * is clear, software is permitted to clear SVMDIS itself, but changing
     * a firmware policy is out of scope for this driver.
     */
    uint64_t vmcr = rdmsr64(MSR_VM_CR);
    if (vmcr & VM_CR_SVMDIS) {
        if (fSvmFeatures & SVM_FEATURE_SVML) {
            LOG("SVM disabled and locked by firmware (VM_CR.SVMDIS + SVML) - "
                "enable SVM/AMD-V in BIOS setup");
            return false;
        }
        LOG("VM_CR.SVMDIS set but not locked; firmware disabled SVM "
            "(enable it in BIOS rather than overriding here)");
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Contiguous page allocation                                         */
/* ------------------------------------------------------------------ */
void *AMDV::allocContiguousPage(IOBufferMemoryDescriptor **outDesc,
                                uint64_t *outPhys)
{
    /* Page-aligned, physically contiguous, 32-bit-safe mask is unnecessary
     * (VMCB/HSAVE take full 64-bit physical addresses), but the buffer must
     * be a single contiguous page so getPhysicalSegment returns one run. */
    IOBufferMemoryDescriptor *desc =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            VMCB_SIZE,
            0xFFFFFFFFFFFFF000ull /* 4-KiB alignment */);
    if (!desc)
        return nullptr;

    if (desc->prepare() != kIOReturnSuccess) {
        desc->release();
        return nullptr;
    }

    IOByteCount seg = 0;
    IOPhysicalAddress64 pa = desc->getPhysicalSegment(0, &seg, 0);
    void *va = desc->getBytesNoCopy();
    if (!pa || !va || seg < VMCB_SIZE) {
        desc->complete();
        desc->release();
        return nullptr;
    }

    memset(va, 0, VMCB_SIZE);
    *outDesc = desc;
    *outPhys = (uint64_t)pa;
    return va;
}

/* ------------------------------------------------------------------ */
/* Enable / disable                                                   */
/* ------------------------------------------------------------------ */
bool AMDV::enableSVM(void)
{
    /* Host state-save area: hardware writes host state here on VMRUN and
     * restores it on #VMEXIT. Must outlive every VMRUN. */
    fHsaveVA = allocContiguousPage(&fHsaveDesc, &fHsavePA);
    if (!fHsaveVA) {
        LOG("failed to allocate host save area");
        return false;
    }

    /* Set EFER.SVME. NOTE: per-CPU. A production build must run this on
     * every logical processor via mp_rendezvous; here it applies to the
     * current CPU only. */
    uint64_t efer = rdmsr64(MSR_EFER);
    wrmsr64(MSR_EFER, efer | EFER_SVME);

    if (!(rdmsr64(MSR_EFER) & EFER_SVME)) {
        LOG("EFER.SVME did not stick - cannot enable SVM");
        return false;
    }

    wrmsr64(MSR_VM_HSAVE_PA, fHsavePA);
    LOG("SVME enabled; VM_HSAVE_PA = 0x%016llx", fHsavePA);

    /* Prepare a baseline VMCB. */
    fVmcb = (vmcb_t *)allocContiguousPage(&fVmcbDesc, &fVmcbPA);
    if (!fVmcb) {
        LOG("failed to allocate VMCB");
        disableSVM();
        return false;
    }

    /* Minimal control setup: intercept the instructions a monitor must see.
     * VMRUN must always be intercepted per the spec, and CPUID/HLT/VMMCALL
     * give the guest a way to trap back out. ASID must be non-zero. */
    fVmcb->intercept_1 = INTERCEPT_CPUID | INTERCEPT_HLT;
    fVmcb->intercept_2 = INTERCEPT2_VMRUN | INTERCEPT2_VMMCALL;
    fVmcb->guest_asid  = 1;

    LOG("VMCB prepared at PA 0x%016llx", fVmcbPA);
    fSvmEnabled = true;
    return true;
}

void AMDV::disableSVM(void)
{
    if (fSvmEnabled) {
        uint64_t efer = rdmsr64(MSR_EFER);
        wrmsr64(MSR_EFER, efer & ~EFER_SVME);
        wrmsr64(MSR_VM_HSAVE_PA, 0);
        fSvmEnabled = false;
        LOG("SVME cleared");
    }

    if (fVmcbDesc)  { fVmcbDesc->complete();  fVmcbDesc->release();  fVmcbDesc = nullptr; }
    if (fHsaveDesc) { fHsaveDesc->complete(); fHsaveDesc->release(); fHsaveDesc = nullptr; }
    fVmcb = nullptr;  fVmcbPA = 0;
    fHsaveVA = nullptr; fHsavePA = 0;
}

#if AMDV_ENABLE_GUEST_LAUNCH
/*
 * Skeleton guest-entry loop. Intentionally not wired into start(). To make
 * this real you must, before the first VMRUN:
 *   1. Pin the calling thread to the CPU where SVME was enabled.
 *   2. Populate the VMCB state-save area (CR0/CR3/CR4, EFER, RIP, RSP,
 *      segment descriptors, RFLAGS) for the guest's initial context.
 *   3. Provide guest physical memory, either via nested paging
 *      (set fVmcb->np_enable |= 1 and fVmcb->nested_cr3) or shadow paging.
 *   4. VMSAVE host segment state that VMRUN does not itself preserve.
 * Only then is executing VMRUN safe.
 */
static uint64_t amdv_run_guest(vmcb_t *vmcb, uint64_t vmcb_pa)
{
    amdv_clgi();
    amdv_vmload(vmcb_pa);   /* load guest FS/GS/TR/LDTR/SYSENTER/etc.     */
    amdv_vmrun(vmcb_pa);    /* enter guest; returns on #VMEXIT            */
    amdv_vmsave(vmcb_pa);   /* save guest segment state back to VMCB      */
    amdv_stgi();
    return vmcb->exit_code;
}
#endif /* AMDV_ENABLE_GUEST_LAUNCH */

/* ------------------------------------------------------------------ */
/* IOService lifecycle                                                */
/* ------------------------------------------------------------------ */
bool AMDV::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    LOG("starting on macOS (AMD-V / SVM enablement)");

    if (!detectSVM()) {
        LOG("SVM unavailable on this machine; driver will stay loaded but idle");
        /* Return true so the kext stays resident and its log is visible;
         * it simply does nothing further on non-SVM hardware. */
        registerService();
        return true;
    }

    if (!enableSVM()) {
        LOG("SVM enablement failed");
        return true;
    }

    LOG("AMD-V ready (current CPU). Guest launch is gated behind "
        "AMDV_ENABLE_GUEST_LAUNCH and requires guest-memory setup.");
    registerService();
    return true;
}

void AMDV::stop(IOService *provider)
{
    LOG("stopping");
    disableSVM();
    super::stop(provider);
}
