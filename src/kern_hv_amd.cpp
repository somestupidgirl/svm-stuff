//
//  kern_hv_amd.cpp - VMX gate hook + (stubbed) VMX->SVM shim.
//

#include "kern_hv_amd.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_mach.hpp>

HypervisorAMD hvAmd;

HypervisorAMD    *HypervisorAMD::callbackInst    = nullptr;
mach_vm_address_t HypervisorAMD::orgVmxIsAvailable = 0;

// Candidate XNU symbols that gate hypervisor availability on x86_64, most
// likely first. Verified on Big Sur 11.6.6 (20G624): _vmx_hv_support EXISTS,
// _vmx_is_available does NOT. The latter is kept as a fallback for other
// kernel versions. These are unexported internals and may differ or be
// inlined; solveSymbol returns 0 on a miss so we log and degrade rather than
// panic.
static const char *kVmxAvailSymbols[] = {
	"_vmx_hv_support",     // int vmx_hv_support(void)      - present on 11.6.6
	"_vmx_is_available",   // boolean_t vmx_is_available(void)
};

// The latched result of the boot-time check (int hv_support_available in
// osfmk/kern/hv_support.c). See setHvSupportAvailable() for why this, not the
// function route, is what actually moves kern.hv_support.
static const char *kHvSupportAvailSymbol = "_hv_support_available";

// ---------------------------------------------------------------------------
// The VMX capability gate.
//
// NOTE: routing this is very probably USELESS on its own. XNU latches the
// answer at boot:
//
//     int hv_support_available = 0;
//     void hv_support_init(void) { hv_support_available = vmx_hv_support(); }
//     int  hv_get_support(void)  { return hv_support_available; }
//
// hv_support_init() runs in kernel_bootstrap, long before any kext (even a
// boot-injected one) starts, so by the time this route is installed the 0 is
// already stored and vmx_hv_support() is never called again. The route is kept
// because it is harmless and correct-in-principle for any later caller, but
// setHvSupportAvailable() below is what actually changes kern.hv_support.
// ---------------------------------------------------------------------------
bool HypervisorAMD::wrapVmxIsAvailable(void)
{
	return true;
}

// ---------------------------------------------------------------------------
// Write the latched hv_support_available directly.
//
// This is a raw kernel data write, so it goes through Lilu's write-protection
// helper under the kernel write lock.
//
// DANGER: flipping this to 1 makes sysctl kern.hv_support report 1, which
// tells userspace that Hypervisor.framework is usable. It is NOT usable yet -
// any app that acts on it (Docker, VirtualBox, qemu -accel hvf, ...) will
// drive Apple's VMX path and #UD on AMD, and with no #UD handler installed
// that is a panic. This is a diagnostic for confirming the latch model, not a
// feature. Gated behind the -amdvgate boot argument.
// ---------------------------------------------------------------------------
bool HypervisorAMD::setHvSupportAvailable(KernelPatcher &patcher, int value)
{
	mach_vm_address_t addr = patcher.solveSymbol(KernelPatcher::KernelID, kHvSupportAvailSymbol);
	patcher.clearError();
	if (!addr) {
		SYSLOG("amdv", "%s not found; cannot set the hv_support latch", kHvSupportAvailSymbol);
		return false;
	}

	int before = *reinterpret_cast<volatile int *>(addr);

	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		SYSLOG("amdv", "failed to disable kernel write protection");
		return false;
	}
	*reinterpret_cast<volatile int *>(addr) = value;
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

	int after = *reinterpret_cast<volatile int *>(addr);
	SYSLOG("amdv", "%s @ 0x%llx: %d -> %d (wrote %d)",
		   kHvSupportAvailSymbol, addr, before, after, value);
	return after == value;
}

void HypervisorAMD::onPatcherLoad(KernelPatcher &patcher)
{
	// 1. Bring up the SVM hardware the shim would ultimately drive.
	if (!fSvm.detect()) {
		SYSLOG("amdv", "SVM not usable on this machine; leaving kernel unpatched");
		return;
	}
	if (!fSvm.enable()) {
		SYSLOG("amdv", "SVM enable failed; leaving kernel unpatched");
		return;
	}

	// 2. Route the VMX availability gate (whichever symbol resolves first).
	bool routed = false;
	for (size_t i = 0; i < arrsize(kVmxAvailSymbols); i++) {
		if (!patcher.solveSymbol(KernelPatcher::KernelID, kVmxAvailSymbols[i])) {
			patcher.clearError();
			continue;
		}
		patcher.clearError();

		KernelPatcher::RouteRequest request(kVmxAvailSymbols[i], wrapVmxIsAvailable, orgVmxIsAvailable);
		if (patcher.routeMultiple(KernelPatcher::KernelID, &request, 1)) {
			SYSLOG("amdv", "routed %s -> forced-available", kVmxAvailSymbols[i]);
			routed = true;
			break;
		}
		patcher.clearError();
	}

	if (!routed) {
		SYSLOG("amdv", "could not locate a VMX gate symbol; symbol names must be "
			   "updated for this kernel build.");
	}

	// 3. Report the latch, and optionally flip it.
	//
	// Routing the gate above cannot move kern.hv_support by itself, because
	// hv_support_init() already ran during kernel_bootstrap and stored the
	// answer. Read it back so the log states plainly whether the route had any
	// effect, then only write it when explicitly asked: reporting
	// kern.hv_support=1 while the #UD handler is still missing is a panic
	// waiting for the first app that believes it.
	if (checkKernelArgument("-amdvgate")) {
		SYSLOG("amdv", "-amdvgate: forcing the hv_support latch. Nothing can USE "
			   "Hypervisor.framework yet - expect a panic if anything tries.");
		setHvSupportAvailable(patcher, 1);
	} else {
		mach_vm_address_t addr = patcher.solveSymbol(KernelPatcher::KernelID, kHvSupportAvailSymbol);
		patcher.clearError();
		if (addr)
			SYSLOG("amdv", "%s = %d (boot-latched). Routing the gate does not change "
				   "it; boot with -amdvgate to force it.",
				   kHvSupportAvailSymbol, *reinterpret_cast<volatile int *>(addr));
		else
			SYSLOG("amdv", "%s not found; cannot report the hv_support latch",
				   kHvSupportAvailSymbol);
	}

	// 4. Bind the VMX->SVM emulator to the prepared VMCB. The instruction
	//    semantics (kern_vmx_emu.*) and the VMCS<->VMCB translation
	//    (kern_vmcs_vmcb.*) are implemented; what remains before guests run is
	//    (a) trapping #UD and routing it into fVmx.handleUD(), and
	//    (b) filling decodeVmx() + the EPT->NPT rebuild.
	fVmx.bind(fSvm.vmcb(), fSvm.vmcbPA(), fSvm.features());
	SYSLOG("amdv", "VMX->SVM emulator bound to VMCB; #UD interception + EPT/NPT "
		   "rebuild still required before Hypervisor.framework guests run. "
		   "Set AMDV_ENABLE_GUEST_LAUNCH=1 only on validated AMD hardware.");

	// TODO: install a #UD (vector 6) IDT hook that calls fVmx.handleUD(frame).
	// The IDT-entry rewrite is per-CPU and platform-specific; it is the last
	// piece of plumbing and is intentionally not performed automatically.
}

void HypervisorAMD::init()
{
	callbackInst = this;

	lilu.onPatcherLoad([](void *user, KernelPatcher &patcher) {
		static_cast<HypervisorAMD *>(user)->onPatcherLoad(patcher);
	}, this);
}

void HypervisorAMD::deinit()
{
	fSvm.disable();
}
