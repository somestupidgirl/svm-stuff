//
//  kern_hv_amd.hpp - the VMX -> SVM translation attempt.
//
//  READ THIS BEFORE BELIEVING THE FILENAME.
//  ----------------------------------------
//  Apple's Hypervisor.framework is backed by VMX (Intel VT-x) code compiled
//  into the closed XNU kernel: VMCS setup via vmwrite/vmread, guest entry via
//  vmlaunch/vmresume. Those instructions #UD on AMD. "Making it SVM-
//  compatible" therefore is NOT a one-symbol hook - it requires emulating the
//  entire VMX instruction/VMCS model on top of SVM/VMCB. That translation
//  layer does not exist in the wild and is a research-scale effort; this file
//  provides the Lilu plumbing to reach the hook points and an HONEST,
//  clearly-stubbed shim, not a working translation.
//
//  What is real here:
//    * Locating and routing the kernel's VMX capability gate so that
//      `sysctl kern.hv_support` reports available (necessary, not sufficient).
//    * The SVM hardware backend (kern_svm.*) the shim would drive.
//  What is stubbed and will NOT run a guest:
//    * vmxToSvmVcpuRun() - the VMCS<->VMCB translation and VMRUN loop.
//

#ifndef kern_hv_amd_hpp
#define kern_hv_amd_hpp

#include <Headers/kern_patcher.hpp>
#include "kern_svm.hpp"
#include "kern_vmx_emu.hpp"

class HypervisorAMD {
public:
	void init();
	void deinit();

private:
	void onPatcherLoad(KernelPatcher &patcher);

	// Trampoline to the original kernel VMX availability check (unused once
	// routed, kept for symmetry / potential fallback).
	static bool wrapVmxIsAvailable(void);

	// Write the boot-latched hv_support_available. Returns true if the value
	// stuck. Gated behind -amdvgate: see the DANGER note in the .cpp.
	static bool setHvSupportAvailable(KernelPatcher &patcher, int value);

	SvmBackend   fSvm;
	VmxEmulator  fVmx;

	static HypervisorAMD *callbackInst;
	static mach_vm_address_t orgVmxIsAvailable;
};

extern HypervisorAMD hvAmd;

#endif /* kern_hv_amd_hpp */
