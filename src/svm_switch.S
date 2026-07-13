/*
 * svm_switch.S - AMD-V guest world-switch.
 *
 *   void svmWorldSwitch(uint64_t vmcb_pa /rdi/, GuestGprs *g /rsi/);
 *
 * Loads the guest GPRs from *g, enters the guest with VMLOAD/VMRUN/VMSAVE,
 * then writes the guest GPRs back to *g. RAX and RSP are carried in the VMCB,
 * not here. Written as a standalone function (no inline-asm register pressure)
 * that preserves the SysV callee-saved registers per the ABI.
 *
 * GuestGprs field order (must match kern_vmx_emu.hpp):
 *   0x00 rbx  0x08 rcx  0x10 rdx  0x18 rbp  0x20 rsi  0x28 rdi
 *   0x30 r8   0x38 r9   0x40 r10  0x48 r11  0x50 r12  0x58 r13
 *   0x60 r14  0x68 r15
 *
 * UNVERIFIED: executes real VMRUN; not yet run on AMD hardware.
 */

	.text
	.intel_syntax noprefix
	.globl _svmWorldSwitch
	.p2align 4
_svmWorldSwitch:
	/* Preserve callee-saved registers (guest will clobber them). */
	push    rbp
	push    rbx
	push    r12
	push    r13
	push    r14
	push    r15

	/* Stash the two arguments; the guest clobbers rdi/rsi too. */
	push    rdi                 /* [rsp+8] = vmcb_pa            */
	push    rsi                 /* [rsp+0] = GuestGprs *g       */

	/* Load guest GPRs from *g (rsi still holds g here). */
	mov     rax, rsi
	mov     rbx, [rax+0x00]
	mov     rcx, [rax+0x08]
	mov     rdx, [rax+0x10]
	mov     rbp, [rax+0x18]
	mov     rsi, [rax+0x20]
	mov     rdi, [rax+0x28]
	mov     r8,  [rax+0x30]
	mov     r9,  [rax+0x38]
	mov     r10, [rax+0x40]
	mov     r11, [rax+0x48]
	mov     r12, [rax+0x50]
	mov     r13, [rax+0x58]
	mov     r14, [rax+0x60]
	mov     r15, [rax+0x68]

	/* rax = vmcb_pa for the SVM instructions (implicit rAX operand). */
	mov     rax, [rsp+8]
	clgi
	vmload  rax
	vmrun   rax
	vmsave  rax
	stgi

	/* Guest rbx..r15 are live; recover g and write them back. */
	mov     rax, [rsp+0]
	mov     [rax+0x00], rbx
	mov     [rax+0x08], rcx
	mov     [rax+0x10], rdx
	mov     [rax+0x18], rbp
	mov     [rax+0x20], rsi
	mov     [rax+0x28], rdi
	mov     [rax+0x30], r8
	mov     [rax+0x38], r9
	mov     [rax+0x40], r10
	mov     [rax+0x48], r11
	mov     [rax+0x50], r12
	mov     [rax+0x58], r13
	mov     [rax+0x60], r14
	mov     [rax+0x68], r15

	add     rsp, 16             /* discard stashed args         */
	pop     r15
	pop     r14
	pop     r13
	pop     r12
	pop     rbx
	pop     rbp
	ret
