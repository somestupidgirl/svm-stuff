//
//  kern_trap.hpp - a view of XNU's x86_saved_state64 for the #UD handler.
//
//  x86_saved_state64 is XNU-private and not shipped in MacKernelSDK, so this
//  mirrors its well-known field order. It is used ONLY by the #UD path, which
//  is not yet reachable (the IDT hook is a TODO), so nothing executes against
//  it today.
//
//  >>> VERIFY THIS LAYOUT against osfmk/mach/i386/thread_status.h for your
//  >>> exact Big Sur build before installing the #UD hook. A wrong offset here
//  >>> reads the wrong register and will misbehave or panic.
//

#ifndef kern_trap_hpp
#define kern_trap_hpp

#include <stdint.h>

// x86_64_intr_stack_frame (hardware + trap metadata pushed on entry).
struct amdv_intr_stack_frame {
	uint16_t trapno;
	uint16_t cpuflag;
	uint32_t _pad;
	uint64_t trapfn;
	uint64_t err;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};

// Mirrors struct x86_saved_state64.
struct amdv_saved_state64 {
	uint64_t rdi, rsi, rdx, r10, r8, r9;
	uint64_t cr2;
	uint64_t r15, r14, r13, r12, r11, rbp, rbx, rcx, rax;
	uint32_t gs, fs;
	uint32_t _isf_pad;
	amdv_intr_stack_frame isf;
};

// Map an x86 GPR encoding (0=rax,1=rcx,...,4=rsp,5=rbp,6=rsi,7=rdi,8..15=r8..r15)
// to the corresponding slot in the saved state. RSP lives in the trap frame.
static inline uint64_t *amdv_gpr(amdv_saved_state64 *s, uint8_t idx)
{
	switch (idx & 0xF) {
	case 0:  return &s->rax;
	case 1:  return &s->rcx;
	case 2:  return &s->rdx;
	case 3:  return &s->rbx;
	case 4:  return &s->isf.rsp;
	case 5:  return &s->rbp;
	case 6:  return &s->rsi;
	case 7:  return &s->rdi;
	case 8:  return &s->r8;
	case 9:  return &s->r9;
	case 10: return &s->r10;
	case 11: return &s->r11;
	case 12: return &s->r12;
	case 13: return &s->r13;
	case 14: return &s->r14;
	default: return &s->r15;
	}
}

#endif /* kern_trap_hpp */
