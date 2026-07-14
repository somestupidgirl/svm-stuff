//
//  kern_vmx.hpp - Intel VMX model definitions + a software shadow VMCS.
//
//  Apple's Hypervisor.framework backend programs a hardware VMCS with vmwrite
//  and enters the guest with vmlaunch/vmresume. On AMD those instructions
//  #UD, so we cannot let the CPU maintain a VMCS. Instead we shadow it: every
//  vmwrite the kernel issues is captured into ShadowVMCS (a flat encoding->
//  value store), and at vmlaunch/vmresume time the shadow is translated into
//  an SVM VMCB (see kern_vmcs_vmcb.*).
//
//  Field encodings and exit reasons are from the Intel SDM Vol.3C, Appendix B
//  ("Field Encoding in VMCS") and Chapter 27 ("VM Exit Reasons"). Only the
//  subset the translation consumes is enumerated; ShadowVMCS still stores any
//  unrecognised encoding losslessly.
//

#ifndef kern_vmx_hpp
#define kern_vmx_hpp

#include <stdint.h>
#include <stddef.h>

// ---- VMCS field encodings (SDM Vol.3C App. B) -----------------------------
enum VmcsField : uint32_t {
	// 16-bit control
	VMCS_VIRTUAL_PROCESSOR_ID = 0x0000,

	// 16-bit guest state
	VMCS_GUEST_ES_SELECTOR   = 0x0800,
	VMCS_GUEST_CS_SELECTOR   = 0x0802,
	VMCS_GUEST_SS_SELECTOR   = 0x0804,
	VMCS_GUEST_DS_SELECTOR   = 0x0806,
	VMCS_GUEST_FS_SELECTOR   = 0x0808,
	VMCS_GUEST_GS_SELECTOR   = 0x080A,
	VMCS_GUEST_LDTR_SELECTOR = 0x080C,
	VMCS_GUEST_TR_SELECTOR   = 0x080E,

	// 64-bit control
	VMCS_IO_BITMAP_A         = 0x2000,
	VMCS_MSR_BITMAP          = 0x2004,
	VMCS_LINK_POINTER        = 0x2800,
	VMCS_EPT_POINTER         = 0x201A,

	// 64-bit guest
	VMCS_GUEST_IA32_PAT      = 0x2804,
	VMCS_GUEST_IA32_EFER     = 0x2806,

	// 32-bit control
	VMCS_PIN_EXEC_CONTROL    = 0x4000,
	VMCS_CPU_EXEC_CONTROL    = 0x4002,
	VMCS_EXCEPTION_BITMAP    = 0x4004,
	VMCS_VM_EXIT_CONTROLS    = 0x400C,
	VMCS_VM_ENTRY_CONTROLS   = 0x4012,
	VMCS_VM_ENTRY_INTR_INFO  = 0x4016,
	VMCS_VM_ENTRY_EXCEPTION_ERROR = 0x4018,
	VMCS_VM_ENTRY_INSTR_LEN  = 0x401A,
	VMCS_SECONDARY_EXEC_CTL  = 0x401E,

	// 32-bit read-only exit information
	VMCS_VM_EXIT_REASON      = 0x4402,
	VMCS_VM_EXIT_INTR_INFO   = 0x4404,
	VMCS_VM_EXIT_INTR_ERROR  = 0x4406,
	VMCS_IDT_VECTORING_INFO  = 0x4408,
	VMCS_VM_EXIT_INSTR_LEN   = 0x440C,

	// 32-bit guest state
	VMCS_GUEST_ES_LIMIT      = 0x4800,
	VMCS_GUEST_CS_LIMIT      = 0x4802,
	VMCS_GUEST_SS_LIMIT      = 0x4804,
	VMCS_GUEST_DS_LIMIT      = 0x4806,
	VMCS_GUEST_FS_LIMIT      = 0x4808,
	VMCS_GUEST_GS_LIMIT      = 0x480A,
	VMCS_GUEST_LDTR_LIMIT    = 0x480C,
	VMCS_GUEST_TR_LIMIT      = 0x480E,
	VMCS_GUEST_GDTR_LIMIT    = 0x4810,
	VMCS_GUEST_IDTR_LIMIT    = 0x4812,
	VMCS_GUEST_ES_AR         = 0x4814,
	VMCS_GUEST_CS_AR         = 0x4816,
	VMCS_GUEST_SS_AR         = 0x4818,
	VMCS_GUEST_DS_AR         = 0x481A,
	VMCS_GUEST_FS_AR         = 0x481C,
	VMCS_GUEST_GS_AR         = 0x481E,
	VMCS_GUEST_LDTR_AR       = 0x4820,
	VMCS_GUEST_TR_AR         = 0x4822,
	VMCS_GUEST_INTERRUPTIBILITY = 0x4824,

	// natural-width guest state
	VMCS_GUEST_CR0           = 0x6800,
	VMCS_GUEST_CR3           = 0x6802,
	VMCS_GUEST_CR4           = 0x6804,
	VMCS_GUEST_ES_BASE       = 0x6806,
	VMCS_GUEST_CS_BASE       = 0x6808,
	VMCS_GUEST_SS_BASE       = 0x680A,
	VMCS_GUEST_DS_BASE       = 0x680C,
	VMCS_GUEST_FS_BASE       = 0x680E,
	VMCS_GUEST_GS_BASE       = 0x6810,
	VMCS_GUEST_LDTR_BASE     = 0x6812,
	VMCS_GUEST_TR_BASE       = 0x6814,
	VMCS_GUEST_GDTR_BASE     = 0x6816,
	VMCS_GUEST_IDTR_BASE     = 0x6818,
	VMCS_GUEST_RSP           = 0x681C,
	VMCS_GUEST_RIP           = 0x681E,
	VMCS_GUEST_RFLAGS        = 0x6820,
	VMCS_GUEST_CR2           = 0x6822, // not a real VMCS field; see note in .cpp
};

// ---- Primary processor-based VM-execution controls (SDM 24.6.2) -----------
enum VmxCpuExecCtl : uint32_t {
	CPU_CTL_HLT_EXITING      = 1u << 7,
	CPU_CTL_CR3_LOAD_EXITING = 1u << 15,
	CPU_CTL_CR3_STORE_EXITING= 1u << 16,
	CPU_CTL_USE_IO_BITMAPS   = 1u << 25,
	CPU_CTL_USE_MSR_BITMAP   = 1u << 28,
	CPU_CTL_SECONDARY_CTLS   = 1u << 31,
};

// Pin-based controls (SDM 24.6.1)
enum VmxPinExecCtl : uint32_t {
	PIN_CTL_EXT_INTR_EXITING = 1u << 0,
	PIN_CTL_NMI_EXITING      = 1u << 3,
};

// Secondary processor-based controls (SDM 24.6.2)
enum VmxSecondaryExecCtl : uint32_t {
	SEC_CTL_ENABLE_EPT       = 1u << 1,
	SEC_CTL_ENABLE_VPID      = 1u << 5,
	SEC_CTL_UNRESTRICTED     = 1u << 7,
};

// ---- VMX VM-entry interruption-information field (SDM 24.8.3) --------------
// Layout matches SVM EVENTINJ except for the TYPE encoding; bit 31 = valid,
// bit 11 = deliver-error-code.
#define VMX_ENTRY_INTR_VALID   (1u << 31)
#define VMX_ENTRY_INTR_DELIVER_EC (1u << 11)
static inline uint8_t  vmxEntryVector(uint32_t f) { return static_cast<uint8_t>(f & 0xFF); }
static inline uint32_t vmxEntryType(uint32_t f)   { return (f >> 8) & 0x7; }

enum VmxEntryIntrType : uint32_t {
	VMX_INTR_TYPE_EXT      = 0,
	VMX_INTR_TYPE_NMI      = 2,
	VMX_INTR_TYPE_HW_EXCEP = 3,
	VMX_INTR_TYPE_SW_INT   = 4,
	VMX_INTR_TYPE_PRIV_SW  = 5,
	VMX_INTR_TYPE_SW_EXCEP = 6,
};

// ---- VMX basic exit reasons (SDM Vol.3C Appendix C) -----------------------
enum VmxExitReason : uint32_t {
	EXIT_EXCEPTION_NMI       = 0,
	EXIT_EXTERNAL_INTERRUPT  = 1,
	EXIT_TRIPLE_FAULT        = 2,
	EXIT_CPUID               = 10,
	EXIT_HLT                 = 12,
	EXIT_VMCALL              = 18,
	EXIT_CR_ACCESS           = 28,
	EXIT_IO_INSTRUCTION      = 30,
	EXIT_RDMSR               = 31,
	EXIT_WRMSR               = 32,
	EXIT_EPT_VIOLATION       = 48,
	EXIT_EPT_MISCONFIG       = 49,
	EXIT_UNKNOWN             = 0xFFFFFFFF,
};

// ---- VMX instruction opcodes (for the #UD decoder, SDM Vol.2) -------------
// Distinguished after stripping legacy/REX prefixes.
enum VmxOpcode {
	VMXOP_NONE = 0,
	VMXOP_VMREAD,     // 0F 78
	VMXOP_VMWRITE,    // 0F 79
	VMXOP_VMPTRLD,    // 0F C7 /6 (no prefix)
	VMXOP_VMPTRST,    // 0F C7 /7
	VMXOP_VMCLEAR,    // 66 0F C7 /6
	VMXOP_VMXON,      // F3 0F C7 /6
	VMXOP_VMLAUNCH,   // 0F 01 C2
	VMXOP_VMRESUME,   // 0F 01 C3
	VMXOP_VMXOFF,     // 0F 01 C4
	VMXOP_INVEPT,     // 66 0F 38 80
	VMXOP_INVVPID,    // 66 0F 38 81
};

// ---------------------------------------------------------------------------
// ShadowVMCS: a lossless encoding->value store standing in for the hardware
// VMCS the CPU refuses to maintain. Fixed-capacity to stay allocation-free in
// the fault path. A production build would size this to the full VMCS field
// set (~150 fields); the cap here comfortably covers what Apple programs.
// ---------------------------------------------------------------------------
class ShadowVMCS {
public:
	static constexpr size_t MaxFields = 256;

	void     reset()                       { count = 0; launched = false; }
	uint64_t read(uint32_t enc) const;
	bool     write(uint32_t enc, uint64_t val);   // false if store is full

	bool     isLaunched() const            { return launched; }
	void     setLaunched(bool v)           { launched = v; }

private:
	struct Field { uint32_t enc; uint64_t val; };
	Field  fields[MaxFields] {};
	size_t count {0};
	bool   launched {false};
};

#endif /* kern_vmx_hpp */
