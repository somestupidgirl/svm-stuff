/*
 * AMDV.hpp - IOService that detects and enables AMD-V (SVM) on load.
 *
 * The driver matches on IOResources so it starts once, early, without any
 * hardware personality. On start() it probes the CPU, and if SVM is
 * available and not locked out by firmware it enables SVME on the current
 * processor, installs a host state-save area and prepares a VMCB. stop()
 * tears the state back down and clears SVME.
 */

#ifndef AMDV_HPP
#define AMDV_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include "SVM.h"

class AMDV : public IOService {
    OSDeclareDefaultStructors(AMDV)

public:
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;

private:
    /* Detection. Fills the feature fields; returns false if this CPU
     * cannot host SVM (not AMD, no SVM bit, or disabled+locked by BIOS). */
    bool detectSVM(void);

    /* Allocate a page-aligned, physically-contiguous buffer and report its
     * physical address. Returns the kernel virtual pointer or nullptr. */
    void *allocContiguousPage(IOBufferMemoryDescriptor **outDesc,
                              uint64_t *outPhys);

    bool enableSVM(void);   /* set EFER.SVME + program VM_HSAVE_PA        */
    void disableSVM(void);  /* clear EFER.SVME                            */

    /* Backing allocations. */
    IOBufferMemoryDescriptor *fHsaveDesc  = nullptr;
    IOBufferMemoryDescriptor *fVmcbDesc   = nullptr;
    void     *fHsaveVA   = nullptr;
    uint64_t  fHsavePA   = 0;
    vmcb_t   *fVmcb      = nullptr;
    uint64_t  fVmcbPA    = 0;

    /* Cached detection results. */
    bool      fSvmEnabled = false;
    uint32_t  fSvmRevision = 0;
    uint32_t  fNumAsids    = 0;
    uint32_t  fSvmFeatures = 0;  /* CPUID 0x8000000A EDX */
};

#endif /* AMDV_HPP */
