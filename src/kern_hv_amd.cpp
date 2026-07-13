//
//  kern_hv_amd.cpp - VMX gate hook + (stubbed) VMX->SVM shim.
//

#include "kern_hv_amd.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

HypervisorAMD hvAmd;

HypervisorAMD    *HypervisorAMD::callbackInst    = nullptr;
mach_vm_address_t HypervisorAMD::orgVmxIsAvailable = 0;

// Candidate XNU symbols that gate hypervisor availability on x86_64. These
// names come from open-source XNU (osfmk/i386/vmx/vmx_cpu.c, osfmk/kern/
// hv_support.c) but MUST be verified against the exact Big Sur build you run
// - they are unexported/internal and can differ or be inlined. solveSymbol
// returns 0 when a name is absent, so a miss logs and degrades rather than
// panicking.
static const char *kVmxAvailSymbols[] = {
	"_vmx_is_available",   // boolean_t vmx_is_available(void)
	"_vmx_hv_support",     // int vmx_hv_support(void)
};

// ---------------------------------------------------------------------------
// The VMX capability gate.
//
// Forcing this true makes kern.hv_support report 1 and lets Hypervisor.
// framework proceed past its "is virtualization available" check. This alone
// does NOT make virtualization work on AMD - the subsequent vmlaunch will
// #UD. It is the first, easy half of the hook, provided so the wiring is
// demonstrable end to end.
// ---------------------------------------------------------------------------
bool HypervisorAMD::wrapVmxIsAvailable(void)
{
	return true;
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
		SYSLOG("amdv", "could not locate a VMX gate symbol; kern.hv_support unchanged. "
			   "Symbol names must be updated for this kernel build.");
	}

	// 3. Bind the VMX->SVM emulator to the prepared VMCB. The instruction
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
