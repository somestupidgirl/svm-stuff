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

	// 3. The real work would go here: route the VMX vcpu-run path to
	//    vmxToSvmVcpuRun(). It is deliberately NOT installed - see below.
	SYSLOG("amdv", "NOTE: VMX->SVM guest-run translation is not implemented; "
		   "Hypervisor.framework guests will still fault on AMD. See kern_hv_amd.cpp.");
}

#if 0
// ===========================================================================
// UNIMPLEMENTED CORE - kept as a specification, compiled out.
//
// To make Hypervisor.framework actually run on AMD you would route the
// kernel's per-vCPU VMX run function to something like this and translate the
// VMCS Apple populated (via its vmwrite calls) into a VMCB, run it, and map
// the SVM #VMEXIT back onto the VMX exit reason Apple's code expects. Every
// line below is a hard sub-project:
//
//   1. Intercept vmwrite/vmread (or shadow the VMCS region) so Apple's guest
//      state lands somewhere we can read - the CPU won't maintain a VMCS.
//   2. Map VMCS fields -> VMCB control/state-save fields (imperfect: e.g.
//      VMX "VM-exit controls" have no 1:1 SVM analogue).
//   3. Build nested page tables (NPT) mirroring Apple's EPT.
//   4. amdv_vmload/vmrun/vmsave with per-CPU pinning.
//   5. Translate SVM EXITCODE -> VMX basic exit reason + qualification.
//
// This is why no shipping product does this; it is left explicit rather than
// faked.
// ===========================================================================
static uint64_t vmxToSvmVcpuRun(void * /*apple_vcpu*/)
{
	PANIC("amdv", "vmxToSvmVcpuRun is unimplemented");
	return 0;
}
#endif

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
