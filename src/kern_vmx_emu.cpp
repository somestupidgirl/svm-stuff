//
//  kern_vmx_emu.cpp - VMX instruction emulation + guest world-switch.
//

#include "kern_vmx_emu.hpp"
#include <Headers/kern_util.hpp>

#ifndef AMDV_ENABLE_GUEST_LAUNCH
#define AMDV_ENABLE_GUEST_LAUNCH 0
#endif

// Guest world-switch, implemented in svm_switch.S (VMLOAD/VMRUN/VMSAVE around
// a GPR swap). Always assembled so the asm is build-verified; only *called*
// when AMDV_ENABLE_GUEST_LAUNCH is set, since it executes real VMRUN and is
// unvalidated on hardware.
extern "C" void svmWorldSwitch(uint64_t vmcbPA, GuestGprs *g);

// ---------------------------------------------------------------------------
// VMLAUNCH / VMRESUME
// ---------------------------------------------------------------------------
uint32_t VmxEmulator::vmlaunchResume()
{
	if (!fVmcb) {
		SYSLOG("amdv", "vmlaunchResume with no VMCB bound");
		return EXIT_UNKNOWN;
	}

	// VMCS -> VMCB, then carry guest RAX (VMCB-resident) into the VMCB.
	translateVmcsToVmcb(fVmcs, fVmcb);
	vmcb_set64(fVmcb, VMCB_RAX, fGuestRax);

#if AMDV_ENABLE_GUEST_LAUNCH
	svmWorldSwitch(fVmcbPA, &fGprs);
#else
	// Not entering the guest: mark the exit invalid so callers see that the
	// world-switch is disabled rather than acting on stale VMCB state.
	fVmcb->exit_code = static_cast<uint64_t>(VMEXIT_INVALID);
	DBGLOG("amdv", "vmlaunchResume: world-switch compiled out (AMDV_ENABLE_GUEST_LAUNCH=0)");
#endif

	// Guest RAX back out of the VMCB, then VMCB -> VMCS for Apple's vmreads.
	fGuestRax = vmcb_get64(fVmcb, VMCB_RAX);
	translateVmcbToVmcs(fVmcb, fVmcs, fSvmFeatures);
	fVmcs.setLaunched(true);

	return static_cast<uint32_t>(fVmcs.read(VMCS_VM_EXIT_REASON));
}

// ---------------------------------------------------------------------------
// #UD dispatch
// ---------------------------------------------------------------------------
bool VmxEmulator::handleUD(void *trapFrame)
{
	VmxDecoded d;
	if (!decodeVmx(trapFrame, d) || d.op == VMXOP_NONE)
		return false;   // not a VMX instruction; let the kernel re-raise #UD

	switch (d.op) {
	case VMXOP_VMXON:    vmxon();                  break;
	case VMXOP_VMXOFF:   vmxoff();                 break;
	case VMXOP_VMPTRLD:  vmptrld();                break;
	case VMXOP_VMCLEAR:  vmclear();                break;
	case VMXOP_VMPTRST:  /* store current VMCS ptr; harmless no-op here */ break;
	case VMXOP_VMWRITE:  vmwrite(d.field, d.srcValue); break;
	case VMXOP_VMREAD:
		if (d.dstReg) *d.dstReg = vmread(d.field);
		break;
	case VMXOP_VMLAUNCH:
	case VMXOP_VMRESUME:
		vmlaunchResume();
		break;
	case VMXOP_INVEPT:
	case VMXOP_INVVPID:
		// TLB-management hints; SVM flushes via VMCB TLB_CONTROL/ASID instead.
		break;
	default:
		return false;
	}

	// TODO: advance the trap frame's RIP by d.length and set the VMX success
	// flags (CF=ZF=0) in the guest/host RFLAGS image so Apple's code sees the
	// instruction as having completed successfully. Requires the concrete
	// x86_saved_state64 layout - wired once decodeVmx is real.
	return true;
}

// ---------------------------------------------------------------------------
// decodeVmx - SCAFFOLD.
//
// Turning a #UD trap frame into a VmxDecoded requires:
//   1. Reading the faulting instruction bytes at the trap RIP.
//   2. Decoding prefixes/REX/opcode to classify the VMX opcode (Lilu bundles
//      hde64 - Headers/hde64.h - which gives length + operand fields).
//   3. Resolving the r/m and reg operands against the saved GPRs to obtain the
//      VMCS field encoding (reg operand) and the source/destination (r/m).
// None of that can be exercised without the real x86_saved_state64 and AMD
// hardware, so this returns false for now (every #UD is re-raised).
// ---------------------------------------------------------------------------
bool VmxEmulator::decodeVmx(void *trapFrame, VmxDecoded &out)
{
	(void)trapFrame;
	(void)out;
	return false;
}
