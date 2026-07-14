//
//  kern_vmx_emu.cpp - VMX instruction emulation + guest world-switch.
//

#include "kern_vmx_emu.hpp"
#include "kern_trap.hpp"
#include <Headers/kern_util.hpp>
#include <Headers/hde64.h>

// VMsucceed: CF=PF=AF=ZF=SF=OF all clear (SDM 30.2). Used after emulation.
static constexpr uint64_t RFLAGS_STATUS_MASK = 0x8D5; // CF|PF|AF|ZF|SF|OF

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

	// Advance past the emulated instruction and report VMsucceed (all status
	// flags clear) so Apple's code proceeds as if the VMX op completed.
	auto *st = static_cast<amdv_saved_state64 *>(trapFrame);
	st->isf.rip    += d.length;
	st->isf.rflags &= ~RFLAGS_STATUS_MASK;
	return true;
}

// ---------------------------------------------------------------------------
// decodeVmx - classify the faulting instruction at the trap RIP and, for
// VMREAD/VMWRITE, resolve the register operands.
//
// The faulting VMX instruction lives in kernel memory (Apple's Hypervisor
// backend), so the bytes at the trap RIP are host-accessible. HDE (bundled by
// Lilu) gives the length, prefixes and ModRM decomposition; the ModRM.reg
// operand names the register holding the VMCS field encoding and ModRM.r/m the
// data register (SDM VMREAD/VMWRITE). Memory-operand forms are not handled yet.
// ---------------------------------------------------------------------------
bool VmxEmulator::decodeVmx(void *trapFrame, VmxDecoded &out)
{
	auto *st = static_cast<amdv_saved_state64 *>(trapFrame);

	hde64s hs;
	unsigned len = hde64_disasm(reinterpret_cast<const void *>(st->isf.rip), &hs);
	if (len == 0 || (hs.flags & F_ERROR))
		return false;
	out.length = static_cast<uint8_t>(len);

	// All VMX instructions are two-byte opcodes led by 0x0F.
	if (hs.opcode != 0x0F)
		return false;

	switch (hs.opcode2) {
	case 0x78: out.op = VMXOP_VMREAD;  break;
	case 0x79: out.op = VMXOP_VMWRITE; break;
	case 0x01: // group: VMLAUNCH/VMRESUME/VMXOFF share 0F 01 /modrm
		switch (hs.modrm) {
		case 0xC2: out.op = VMXOP_VMLAUNCH; break;
		case 0xC3: out.op = VMXOP_VMRESUME; break;
		case 0xC4: out.op = VMXOP_VMXOFF;   break;
		default:   return false;            // e.g. VMCALL (C1) - not ours
		}
		break;
	case 0xC7: // VMPTRLD / VMPTRST / VMCLEAR / VMXON by prefix + ModRM.reg
		if (hs.modrm_reg == 6)
			out.op = (hs.p_rep == 0xF3) ? VMXOP_VMXON
			       : (hs.p_66  == 0x66) ? VMXOP_VMCLEAR
			       :                      VMXOP_VMPTRLD;
		else if (hs.modrm_reg == 7)
			out.op = VMXOP_VMPTRST;
		else
			return false;
		break;
	case 0x38: // INVEPT (66 0F 38 80) / INVVPID (66 0F 38 81)
		if (hs.p_66 == 0x66 && hs.modrm == 0x80) out.op = VMXOP_INVEPT;
		else if (hs.p_66 == 0x66 && hs.modrm == 0x81) out.op = VMXOP_INVVPID;
		else return false;
		break;
	default:
		return false;
	}

	// VMREAD/VMWRITE carry register operands we must resolve now.
	if (out.op == VMXOP_VMREAD || out.op == VMXOP_VMWRITE) {
		if (hs.modrm_mod != 3) {
			// Memory operand: needs full ModRM/SIB/disp address computation.
			DBGLOG("amdv", "VMREAD/WRITE memory operand not yet decoded");
			return false;
		}
		uint8_t regIdx = static_cast<uint8_t>(hs.modrm_reg | (hs.rex_r ? 8 : 0));
		uint8_t rmIdx  = static_cast<uint8_t>(hs.modrm_rm  | (hs.rex_b ? 8 : 0));
		out.field = static_cast<uint32_t>(*amdv_gpr(st, regIdx));
		if (out.op == VMXOP_VMWRITE)
			out.srcValue = *amdv_gpr(st, rmIdx);
		else
			out.dstReg = amdv_gpr(st, rmIdx);
	}
	return true;
}
