//
//  kern_vmx_emu.hpp - emulate the VMX instruction set on top of SVM.
//
//  Because VMX instructions #UD on AMD, the plan is to trap #UD, decode the
//  faulting VMX instruction, and emulate it here against a ShadowVMCS. The
//  instruction *semantics* below are implemented; the trap-frame decoding that
//  feeds them (decodeVmx) is scaffolded, and the actual guest world-switch is
//  gated behind AMDV_ENABLE_GUEST_LAUNCH because it cannot be validated
//  without AMD hardware.
//

#ifndef kern_vmx_emu_hpp
#define kern_vmx_emu_hpp

#include "kern_vmx.hpp"
#include "kern_vmcs_vmcb.hpp"
#include "kern_svm.hpp"

// Guest general-purpose registers not held in the VMCB. RAX and RSP live in
// the VMCB state-save area; everything else is swapped in software around
// VMRUN. Field order MUST match the offsets in svm_switch.inc / the asm.
struct GuestGprs {
	uint64_t rbx, rcx, rdx, rbp, rsi, rdi;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

// A decoded VMX instruction, produced by decodeVmx() from a #UD trap frame.
struct VmxDecoded {
	VmxOpcode op       {VMXOP_NONE};
	uint32_t  field    {0};        // vmread/vmwrite: VMCS field encoding
	uint64_t  srcValue {0};        // vmwrite source operand value
	uint64_t *dstReg   {nullptr};  // vmread destination register (if reg form)
	uint8_t   length   {0};        // bytes to advance RIP past the instruction
};

class VmxEmulator {
public:
	// vmcb / vmcbPA come from the SVM backend; svmFeatures gates NRIPS use.
	void bind(vmcb_t *vmcb, uint64_t vmcbPA, uint32_t svmFeatures) {
		fVmcb = vmcb; fVmcbPA = vmcbPA; fSvmFeatures = svmFeatures;
	}

	// --- VMX instruction semantics (operands already resolved) ---
	void     vmxon()                              { fVmxRoot = true; }
	void     vmxoff()                             { fVmxRoot = false; }
	void     vmptrld()                            { fVmcs.setLaunched(false); }
	void     vmclear()                            { fVmcs.setLaunched(false); }
	bool     vmwrite(uint32_t f, uint64_t v)      { return fVmcs.write(f, v); }
	uint64_t vmread(uint32_t f) const             { return fVmcs.read(f); }

	// VMLAUNCH/VMRESUME: translate, enter the guest, translate the exit back.
	// Returns the VMX basic exit reason now readable from the shadow VMCS.
	uint32_t vmlaunchResume();

	// #UD entry point. Returns true if the fault was a VMX instruction we
	// consumed (caller advances RIP by decoded.length); false to re-raise.
	bool handleUD(void *trapFrame);

	ShadowVMCS &vmcs()      { return fVmcs; }
	GuestGprs  &gprs()      { return fGprs; }
	uint64_t   &guestRax()  { return fGuestRax; }

private:
	// Scaffolded: decode a VMX instruction from a saved-state trap frame.
	bool decodeVmx(void *trapFrame, VmxDecoded &out);

	ShadowVMCS fVmcs;
	GuestGprs  fGprs {};
	uint64_t   fGuestRax {0};

	vmcb_t    *fVmcb {nullptr};
	uint64_t   fVmcbPA {0};
	uint32_t   fSvmFeatures {0};
	bool       fVmxRoot {false};
};

#endif /* kern_vmx_emu_hpp */
