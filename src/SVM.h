/*
 * SVM.h - AMD-V (Secure Virtual Machine) hardware definitions.
 *
 * All constants, MSR numbers, VMCB field offsets and the low-level
 * instruction wrappers are taken from the "AMD64 Architecture Programmer's
 * Manual, Volume 2: System Programming" (publication 24593), chapter 15
 * ("Secure Virtual Machine"). Section references below point into that
 * manual so the encodings can be re-verified against the spec.
 */

#ifndef AMDV_SVM_H
#define AMDV_SVM_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* CPUID leaves (Vol.2 §15.4 "Enabling SVM")                          */
/* ------------------------------------------------------------------ */
#define CPUID_VENDOR              0x00000000u  /* EBX:EDX:ECX = vendor  */
#define CPUID_EXT_MAX             0x80000000u  /* EAX = max ext. leaf   */
#define CPUID_EXT_FEATURES        0x80000001u  /* ECX bit 2 = SVM       */
#define CPUID_SVM_FEATURES        0x8000000Au  /* SVM revision/features */

#define CPUID_ECX_SVM             (1u << 2)    /* 0x80000001 ECX.SVM    */

/* 0x8000000A EDX feature flags */
#define SVM_FEATURE_NP            (1u << 0)    /* Nested paging          */
#define SVM_FEATURE_LBRVIRT       (1u << 1)    /* LBR virtualization     */
#define SVM_FEATURE_SVML          (1u << 2)    /* SVM lock               */
#define SVM_FEATURE_NRIPS         (1u << 3)    /* NRIP save on #VMEXIT   */
#define SVM_FEATURE_TSCRATE       (1u << 4)    /* TSC rate MSR           */
#define SVM_FEATURE_VMCBCLEAN     (1u << 5)    /* VMCB clean bits        */
#define SVM_FEATURE_FLUSHBYASID   (1u << 6)    /* Flush-by-ASID          */
#define SVM_FEATURE_DECODEASSIST  (1u << 7)    /* Decode assists         */
#define SVM_FEATURE_PAUSEFILTER   (1u << 10)   /* Pause-intercept filter */

/* ------------------------------------------------------------------ */
/* Model Specific Registers (Vol.2 §15.4, §15.30)                     */
/* ------------------------------------------------------------------ */
#define MSR_EFER                  0xC0000080u
#define EFER_SVME                 (1ull << 12) /* SVM enable             */

#define MSR_VM_CR                 0xC0010114u
#define VM_CR_DPD                 (1u << 0)
#define VM_CR_R_INIT              (1u << 1)
#define VM_CR_DIS_A20M            (1u << 2)
#define VM_CR_LOCK                (1u << 3)
#define VM_CR_SVMDIS              (1u << 4)    /* SVM disabled by BIOS   */

#define MSR_VM_HSAVE_PA           0xC0010117u  /* Host state save area   */

/* ------------------------------------------------------------------ */
/* VMCB (Vol.2 §15.5, Appendix B "Layout of VMCB")                    */
/*                                                                    */
/* The VMCB is a single 4-KiB page split into a control area          */
/* (offsets 0x000-0x3FF) and a guest state-save area (offset 0x400+). */
/* Only the fields this driver touches are named; the rest is padded  */
/* so the struct is exactly 0x1000 bytes.                             */
/* ------------------------------------------------------------------ */

#define VMCB_SIZE                 0x1000u
#define VMCB_STATE_SAVE_OFFSET    0x400u

/* Control-area intercept bits we care about (Vol.2 §15.9-15.13). */
#define INTERCEPT_INTR            (1u << 0)    /* offset 0x00C, bit 0    */
#define INTERCEPT_CPUID           (1u << 18)   /* offset 0x00C, bit 18   */
#define INTERCEPT_HLT             (1u << 24)   /* offset 0x00C, bit 24   */
#define INTERCEPT2_VMRUN          (1u << 0)    /* offset 0x010, bit 0    */
#define INTERCEPT2_VMMCALL        (1u << 1)    /* offset 0x010, bit 1    */

/* Well-known VMEXIT codes (Vol.2 Appendix C "SVM Intercept Codes"). */
#define VMEXIT_EXCP_BASE          0x040ull   /* 0x40+vector for #exceptions */
#define VMEXIT_EXCP_LAST          0x05Full
#define VMEXIT_INTR               0x060ull   /* physical INTR               */
#define VMEXIT_NMI                0x061ull
#define VMEXIT_CPUID              0x072ull
#define VMEXIT_HLT                0x078ull
#define VMEXIT_IOIO               0x07Bull
#define VMEXIT_MSR                0x07Cull   /* EXITINFO1 bit0: 1=wrmsr     */
#define VMEXIT_VMMCALL            0x081ull
#define VMEXIT_NPF                0x400ull   /* nested page fault           */
#define VMEXIT_INVALID            (-1ll)

/*
 * Packed VMCB overlay. The named leading fields match the control area;
 * everything else is reached through the raw byte view when needed. The
 * struct is deliberately sized to the full page so a single allocation
 * doubles as the hardware VMCB.
 */
typedef struct __attribute__((packed)) {
    uint32_t intercept_cr;          /* 0x000 CR read/write intercepts  */
    uint32_t intercept_dr;          /* 0x004 DR read/write intercepts  */
    uint32_t intercept_exceptions;  /* 0x008 exception intercepts      */
    uint32_t intercept_1;           /* 0x00C INTR/CPUID/HLT/... group  */
    uint32_t intercept_2;           /* 0x010 VMRUN/VMMCALL/... group   */
    uint8_t  reserved_1[0x028];     /* 0x014 .. 0x03B                  */
    uint16_t pause_filter_thresh;   /* 0x03C                           */
    uint16_t pause_filter_count;    /* 0x03E                           */
    uint64_t iopm_base_pa;          /* 0x040 I/O permission map        */
    uint64_t msrpm_base_pa;         /* 0x048 MSR permission map        */
    uint64_t tsc_offset;            /* 0x050                           */
    uint32_t guest_asid;            /* 0x058 address space id (!= 0)   */
    uint8_t  tlb_control;           /* 0x05C                           */
    uint8_t  reserved_2[3];         /* 0x05D                           */
    uint64_t v_intr;                /* 0x060 virtual interrupt state   */
    uint64_t interrupt_shadow;      /* 0x068                           */
    uint64_t exit_code;             /* 0x070 EXITCODE                  */
    uint64_t exit_info_1;           /* 0x078 EXITINFO1                 */
    uint64_t exit_info_2;           /* 0x080 EXITINFO2                 */
    uint64_t exit_int_info;         /* 0x088 EXITINTINFO               */
    uint64_t np_enable;             /* 0x090 nested paging control     */
    uint8_t  reserved_3[0x18];      /* 0x098 .. 0x0AF                  */
    uint64_t nested_cr3;            /* 0x0B0 nested page table CR3     */
    /* The remainder of the page (control tail + state-save area) is   */
    /* accessed via the byte view; pad out to a full 4-KiB page.       */
    uint8_t  raw_tail[VMCB_SIZE - 0xB8];
} vmcb_t;

_Static_assert(sizeof(vmcb_t) == VMCB_SIZE, "VMCB must be exactly one 4-KiB page");

/* ------------------------------------------------------------------ */
/* Low-level instruction wrappers                                     */
/* ------------------------------------------------------------------ */

/* CPUID with an explicit sub-leaf in ECX. */
static inline void amdv_cpuid(uint32_t leaf, uint32_t subleaf,
                              uint32_t *a, uint32_t *b,
                              uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

/*
 * SVM instructions (Vol.2 §15.5-15.6). VMRUN / VMLOAD / VMSAVE take the
 * physical address of the VMCB in rAX implicitly. clang understands the
 * mnemonics directly on any x86_64 target, so no raw .byte encodings are
 * needed. STGI/CLGI toggle the global interrupt flag.
 */
static inline void amdv_vmrun(uint64_t vmcb_pa)
{
    __asm__ volatile ("vmrun" :: "a"(vmcb_pa) : "memory");
}

static inline void amdv_vmload(uint64_t vmcb_pa)
{
    __asm__ volatile ("vmload" :: "a"(vmcb_pa) : "memory");
}

static inline void amdv_vmsave(uint64_t vmcb_pa)
{
    __asm__ volatile ("vmsave" :: "a"(vmcb_pa) : "memory");
}

static inline void amdv_stgi(void) { __asm__ volatile ("stgi" ::: "memory"); }
static inline void amdv_clgi(void) { __asm__ volatile ("clgi" ::: "memory"); }

/* ------------------------------------------------------------------ */
/* VMCB state-save area (Vol.2 Appendix B, offsets from VMCB base).    */
/*                                                                    */
/* The save area begins at 0x400. Rather than model the whole page as */
/* a fragile packed struct, named absolute offsets plus small typed   */
/* accessors are used; each offset is verifiable against the manual.  */
/* ------------------------------------------------------------------ */

/* Each segment slot is {selector:u16, attrib:u16, limit:u32, base:u64}. */
#define VMCB_SEG_ES    0x400u
#define VMCB_SEG_CS    0x410u
#define VMCB_SEG_SS    0x420u
#define VMCB_SEG_DS    0x430u
#define VMCB_SEG_FS    0x440u
#define VMCB_SEG_GS    0x450u
#define VMCB_SEG_GDTR  0x460u
#define VMCB_SEG_LDTR  0x470u
#define VMCB_SEG_IDTR  0x480u
#define VMCB_SEG_TR    0x490u

#define VMCB_CPL       0x4CBu   /* u8  */
#define VMCB_EFER      0x4D0u   /* u64 (guest EFER; SVME must be set)   */
#define VMCB_CR4       0x548u
#define VMCB_CR3       0x550u
#define VMCB_CR0       0x558u
#define VMCB_DR7       0x560u
#define VMCB_DR6       0x568u
#define VMCB_RFLAGS    0x570u
#define VMCB_RIP       0x578u
#define VMCB_RSP       0x5D8u
#define VMCB_RAX       0x5F8u
#define VMCB_STAR      0x600u
#define VMCB_LSTAR     0x608u
#define VMCB_CSTAR     0x610u
#define VMCB_SFMASK    0x618u
#define VMCB_KGSBASE   0x620u
#define VMCB_SYSENTCS  0x628u
#define VMCB_SYSENTESP 0x630u
#define VMCB_SYSENTEIP 0x638u
#define VMCB_CR2       0x640u
#define VMCB_GPAT      0x668u   /* nested-paging guest PAT              */

/* Control-area fields past nested_cr3 that the shim reads. */
#define VMCB_CTL_NRIP  0x0C8u   /* next-sequential RIP on #VMEXIT       */

typedef struct __attribute__((packed)) {
    uint16_t selector;
    uint16_t attrib;   /* SVM 12-bit packed attributes (see below)     */
    uint32_t limit;
    uint64_t base;
} vmcb_seg_t;

_Static_assert(sizeof(vmcb_seg_t) == 16, "VMCB segment slot is 16 bytes");

static inline uint64_t vmcb_get64(const vmcb_t *v, uint32_t off)
{
    return *(const volatile uint64_t *)((const uint8_t *)v + off);
}
static inline void vmcb_set64(vmcb_t *v, uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)((uint8_t *)v + off) = val;
}
static inline uint8_t vmcb_get8(const vmcb_t *v, uint32_t off)
{
    return *((const uint8_t *)v + off);
}
static inline void vmcb_set8(vmcb_t *v, uint32_t off, uint8_t val)
{
    *((uint8_t *)v + off) = val;
}
static inline vmcb_seg_t *vmcb_seg(vmcb_t *v, uint32_t off)
{
    return (vmcb_seg_t *)((uint8_t *)v + off);
}

#endif /* AMDV_SVM_H */
