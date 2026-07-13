//
//  kern_vmcs_vmcb.cpp - VMCS<->VMCB translation + SVM/VMX exit mapping.
//

#include "kern_vmcs_vmcb.hpp"
#include <Headers/kern_util.hpp>

// ---------------------------------------------------------------------------
// ShadowVMCS storage
// ---------------------------------------------------------------------------
uint64_t ShadowVMCS::read(uint32_t enc) const
{
	for (size_t i = 0; i < count; i++)
		if (fields[i].enc == enc)
			return fields[i].val;
	return 0;
}

bool ShadowVMCS::write(uint32_t enc, uint64_t val)
{
	for (size_t i = 0; i < count; i++) {
		if (fields[i].enc == enc) {
			fields[i].val = val;
			return true;
		}
	}
	if (count >= MaxFields) {
		SYSLOG("amdv", "shadow VMCS full; dropped write to field 0x%04X", enc);
		return false;
	}
	fields[count].enc = enc;
	fields[count].val = val;
	count++;
	return true;
}

// ---------------------------------------------------------------------------
// Segment access-rights conversion
//
//   VMX AR : type[3:0] S[4] DPL[6:5] P[7] AVL[12] L[13] D/B[14] G[15] Unusable[16]
//   SVM att: type[3:0] S[4] DPL[6:5] P[7] AVL[8]  L[9]  D/B[10] G[11]
//
// The upper nibble of attributes sits 4 bits higher in VMX, so it shifts by 4.
// VMX's "unusable" has no SVM bit; SVM represents an unusable segment by a
// cleared Present bit, so the conversions round-trip that way.
// ---------------------------------------------------------------------------
uint16_t vmxArToSvmAttrib(uint32_t vmxAr)
{
	if (vmxAr & (1u << 16))          // unusable
		return 0;                    // Present=0, everything else 0
	return static_cast<uint16_t>((vmxAr & 0xFF) | ((vmxAr >> 4) & 0xF00));
}

uint32_t svmAttribToVmxAr(uint16_t svmAttrib)
{
	if ((svmAttrib & 0x80) == 0)     // Present clear
		return (1u << 16);           // VMX unusable
	return (static_cast<uint32_t>(svmAttrib & 0xFF)) |
	       ((static_cast<uint32_t>(svmAttrib & 0xF00)) << 4);
}

// ---------------------------------------------------------------------------
// VMCS -> VMCB
// ---------------------------------------------------------------------------
namespace {

// One segment's four VMCS encodings, in {selector, limit, base, ar} order.
struct SegMap {
	uint32_t vmcbOff;
	uint32_t selEnc, limitEnc, baseEnc, arEnc;
};

const SegMap kSegs[] = {
	{ VMCB_SEG_ES,   VMCS_GUEST_ES_SELECTOR,   VMCS_GUEST_ES_LIMIT,   VMCS_GUEST_ES_BASE,   VMCS_GUEST_ES_AR   },
	{ VMCB_SEG_CS,   VMCS_GUEST_CS_SELECTOR,   VMCS_GUEST_CS_LIMIT,   VMCS_GUEST_CS_BASE,   VMCS_GUEST_CS_AR   },
	{ VMCB_SEG_SS,   VMCS_GUEST_SS_SELECTOR,   VMCS_GUEST_SS_LIMIT,   VMCS_GUEST_SS_BASE,   VMCS_GUEST_SS_AR   },
	{ VMCB_SEG_DS,   VMCS_GUEST_DS_SELECTOR,   VMCS_GUEST_DS_LIMIT,   VMCS_GUEST_DS_BASE,   VMCS_GUEST_DS_AR   },
	{ VMCB_SEG_FS,   VMCS_GUEST_FS_SELECTOR,   VMCS_GUEST_FS_LIMIT,   VMCS_GUEST_FS_BASE,   VMCS_GUEST_FS_AR   },
	{ VMCB_SEG_GS,   VMCS_GUEST_GS_SELECTOR,   VMCS_GUEST_GS_LIMIT,   VMCS_GUEST_GS_BASE,   VMCS_GUEST_GS_AR   },
	{ VMCB_SEG_LDTR, VMCS_GUEST_LDTR_SELECTOR, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_BASE, VMCS_GUEST_LDTR_AR },
	{ VMCB_SEG_TR,   VMCS_GUEST_TR_SELECTOR,   VMCS_GUEST_TR_LIMIT,   VMCS_GUEST_TR_BASE,   VMCS_GUEST_TR_AR   },
};

} // namespace

void translateVmcsToVmcb(const ShadowVMCS &vmcs, vmcb_t *vmcb)
{
	// --- Segments (ES..TR) ---
	for (auto &s : kSegs) {
		vmcb_seg_t *seg = vmcb_seg(vmcb, s.vmcbOff);
		seg->selector = static_cast<uint16_t>(vmcs.read(s.selEnc));
		seg->limit    = static_cast<uint32_t>(vmcs.read(s.limitEnc));
		seg->base     = vmcs.read(s.baseEnc);
		seg->attrib   = vmxArToSvmAttrib(static_cast<uint32_t>(vmcs.read(s.arEnc)));
	}
	// GDTR/IDTR have only base+limit (no selector/attrib).
	vmcb_seg(vmcb, VMCB_SEG_GDTR)->base  = vmcs.read(VMCS_GUEST_GDTR_BASE);
	vmcb_seg(vmcb, VMCB_SEG_GDTR)->limit = static_cast<uint32_t>(vmcs.read(VMCS_GUEST_GDTR_LIMIT));
	vmcb_seg(vmcb, VMCB_SEG_IDTR)->base  = vmcs.read(VMCS_GUEST_IDTR_BASE);
	vmcb_seg(vmcb, VMCB_SEG_IDTR)->limit = static_cast<uint32_t>(vmcs.read(VMCS_GUEST_IDTR_LIMIT));

	// --- Control / general registers ---
	vmcb_set64(vmcb, VMCB_CR0,    vmcs.read(VMCS_GUEST_CR0));
	vmcb_set64(vmcb, VMCB_CR3,    vmcs.read(VMCS_GUEST_CR3));
	vmcb_set64(vmcb, VMCB_CR4,    vmcs.read(VMCS_GUEST_CR4));
	vmcb_set64(vmcb, VMCB_CR2,    vmcs.read(VMCS_GUEST_CR2));
	vmcb_set64(vmcb, VMCB_RIP,    vmcs.read(VMCS_GUEST_RIP));
	vmcb_set64(vmcb, VMCB_RSP,    vmcs.read(VMCS_GUEST_RSP));
	vmcb_set64(vmcb, VMCB_RFLAGS, vmcs.read(VMCS_GUEST_RFLAGS));
	vmcb_set64(vmcb, VMCB_GPAT,   vmcs.read(VMCS_GUEST_IA32_PAT));

	// Guest EFER. SVM REQUIRES EFER.SVME be set in the guest EFER image or
	// VMRUN fails with VMEXIT_INVALID, even though the guest itself must not
	// observe it. AMD handles the masking; we just satisfy the check.
	vmcb_set64(vmcb, VMCB_EFER, vmcs.read(VMCS_GUEST_IA32_EFER) | EFER_SVME);

	// --- Intercepts derived from VMX execution controls ---
	uint32_t pin = static_cast<uint32_t>(vmcs.read(VMCS_PIN_EXEC_CONTROL));
	uint32_t cpu = static_cast<uint32_t>(vmcs.read(VMCS_CPU_EXEC_CONTROL));
	uint32_t sec = (cpu & CPU_CTL_SECONDARY_CTLS)
		? static_cast<uint32_t>(vmcs.read(VMCS_SECONDARY_EXEC_CTL)) : 0;

	uint32_t intercept1 = 0;
	if (pin & PIN_CTL_EXT_INTR_EXITING) intercept1 |= INTERCEPT_INTR;
	if (cpu & CPU_CTL_HLT_EXITING)      intercept1 |= INTERCEPT_HLT;
	vmcb->intercept_1 = intercept1;

	// VMRUN intercept is mandatory in every VMCB.
	vmcb->intercept_2 = INTERCEPT2_VMRUN;

	// Exception bitmap layout is identical (bit n == vector n) in both models.
	vmcb->intercept_exceptions = static_cast<uint32_t>(vmcs.read(VMCS_EXCEPTION_BITMAP));

	// --- Nested paging from EPT ---
	// NOTE: an EPT pointer is NOT a usable N_CR3. EPT and NPT are both 4-level
	// but use different PTE bit semantics (EPT read/write/execute + memory type
	// vs. standard x86 present/rw/us/nx). A correct build must WALK the EPT and
	// build a parallel NPT; here we only wire the enable + a placeholder so the
	// data path is visible. Left as the largest single TODO in this file.
	if (sec & SEC_CTL_ENABLE_EPT) {
		vmcb->np_enable  = 1;
		vmcb->nested_cr3 = vmcs.read(VMCS_EPT_POINTER) & ~0xFFFull; // TODO: rebuild as NPT
	} else {
		vmcb->np_enable  = 0;
		vmcb->nested_cr3 = 0;
	}

	// ASID must be non-zero. Reuse VPID when present, else a fixed guest ASID.
	uint32_t vpid = (sec & SEC_CTL_ENABLE_VPID)
		? static_cast<uint32_t>(vmcs.read(VMCS_VIRTUAL_PROCESSOR_ID)) : 0;
	vmcb->guest_asid = vpid ? vpid : 1;

	// TODO: MSR/IO intercepts. SVM MSRPM/IOPM have a different layout from VMX
	// MSR/IO bitmaps and need translation into separate SVM permission pages;
	// until then MSR/IO exiting requested via CPU controls is not honoured.
	(void)0;
}

// ---------------------------------------------------------------------------
// VMCB -> VMCS (post-exit)
// ---------------------------------------------------------------------------
void translateVmcbToVmcs(vmcb_t *vmcb, ShadowVMCS &vmcs, uint32_t svmFeatures)
{
	// Guest register state observed by Apple's post-exit vmreads.
	vmcs.write(VMCS_GUEST_CR0,    vmcb_get64(vmcb, VMCB_CR0));
	vmcs.write(VMCS_GUEST_CR3,    vmcb_get64(vmcb, VMCB_CR3));
	vmcs.write(VMCS_GUEST_CR4,    vmcb_get64(vmcb, VMCB_CR4));
	vmcs.write(VMCS_GUEST_CR2,    vmcb_get64(vmcb, VMCB_CR2));
	vmcs.write(VMCS_GUEST_RIP,    vmcb_get64(vmcb, VMCB_RIP));
	vmcs.write(VMCS_GUEST_RSP,    vmcb_get64(vmcb, VMCB_RSP));
	vmcs.write(VMCS_GUEST_RFLAGS, vmcb_get64(vmcb, VMCB_RFLAGS));
	vmcs.write(VMCS_GUEST_IA32_EFER, vmcb_get64(vmcb, VMCB_EFER) & ~EFER_SVME);

	for (auto &s : kSegs) {
		vmcb_seg_t *seg = vmcb_seg(vmcb, s.vmcbOff);
		vmcs.write(s.selEnc,   seg->selector);
		vmcs.write(s.limitEnc, seg->limit);
		vmcs.write(s.baseEnc,  seg->base);
		vmcs.write(s.arEnc,    svmAttribToVmxAr(seg->attrib));
	}

	// Exit reason.
	uint64_t code = vmcb->exit_code;
	uint32_t reason = svmExitToVmxReason(code, vmcb->exit_info_1);
	vmcs.write(VMCS_VM_EXIT_REASON, reason);

	// Instruction length: with NRIPS the CPU records the next RIP, so the
	// executed instruction length is nRIP - RIP. Without NRIPS this is a hard
	// gap (Apple expects a length for several exits) that decode-assist or an
	// instruction decoder must fill - noted, not solved.
	if (svmFeatures & SVM_FEATURE_NRIPS) {
		uint64_t nrip = vmcb_get64(vmcb, VMCB_CTL_NRIP);
		uint64_t rip  = vmcb_get64(vmcb, VMCB_RIP);
		if (nrip > rip)
			vmcs.write(VMCS_VM_EXIT_INSTR_LEN, nrip - rip);
	}

	// TODO: EXIT_QUALIFICATION, VM_EXIT_INTR_INFO, IDT_VECTORING_INFO. These
	// derive from EXITINFO1/EXITINFO2/EXITINTINFO but the encodings differ per
	// exit type (e.g. NPF fault code vs. EPT qualification bits) and must be
	// filled case by case.
}

// ---------------------------------------------------------------------------
// SVM exit code -> VMX basic exit reason
// ---------------------------------------------------------------------------
uint32_t svmExitToVmxReason(uint64_t svmExitCode, uint64_t exitInfo1)
{
	switch (svmExitCode) {
	case VMEXIT_CPUID:   return EXIT_CPUID;
	case VMEXIT_HLT:     return EXIT_HLT;
	case VMEXIT_VMMCALL: return EXIT_VMCALL;
	case VMEXIT_IOIO:    return EXIT_IO_INSTRUCTION;
	case VMEXIT_MSR:     return (exitInfo1 & 1) ? EXIT_WRMSR : EXIT_RDMSR;
	case VMEXIT_INTR:    return EXIT_EXTERNAL_INTERRUPT;
	case VMEXIT_NMI:     return EXIT_EXCEPTION_NMI;
	case VMEXIT_NPF:     return EXIT_EPT_VIOLATION;
	default:
		if (svmExitCode >= VMEXIT_EXCP_BASE && svmExitCode <= VMEXIT_EXCP_LAST)
			return EXIT_EXCEPTION_NMI;
		return EXIT_UNKNOWN;
	}
}
