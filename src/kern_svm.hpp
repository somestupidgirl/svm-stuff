//
//  kern_svm.hpp - AMD-V (SVM) hardware backend for the AMDV Lilu plugin.
//
//  Wraps the privileged SVM setup that a VMX->SVM shim would sit on top of:
//  capability detection, EFER.SVME enable, host save area and a VMCB. This
//  is the part that genuinely works; see kern_hv_amd.* for the (unsolved)
//  translation layer that would make Apple's Hypervisor.framework use it.
//

#ifndef kern_svm_hpp
#define kern_svm_hpp

#include <IOKit/IOBufferMemoryDescriptor.h>
#include "SVM.h"

class SvmBackend {
public:
	// Probe the current CPU. Fills the feature fields. Returns false when
	// SVM is absent or locked out by firmware.
	bool detect();

	// Allocate the host save area + VMCB and set EFER.SVME on the current
	// CPU. NOTE: SVME is per-core; a production build must broadcast this.
	bool enable();

	// Clear EFER.SVME and free everything.
	void disable();

	bool     enabled()     const { return fEnabled; }
	uint32_t svmRevision() const { return fSvmRevision; }
	uint32_t numAsids()    const { return fNumAsids; }
	uint32_t features()    const { return fSvmFeatures; }

	// The prepared VMCB and its physical address (valid after enable()).
	vmcb_t  *vmcb()        const { return fVmcb; }
	uint64_t vmcbPA()      const { return fVmcbPA; }

private:
	void *allocContiguousPage(IOBufferMemoryDescriptor **outDesc, uint64_t *outPhys);

	IOBufferMemoryDescriptor *fHsaveDesc = nullptr;
	IOBufferMemoryDescriptor *fVmcbDesc  = nullptr;
	uint64_t  fHsavePA   = 0;
	vmcb_t   *fVmcb      = nullptr;
	uint64_t  fVmcbPA    = 0;

	bool      fEnabled     = false;
	uint32_t  fSvmRevision = 0;
	uint32_t  fNumAsids    = 0;
	uint32_t  fSvmFeatures = 0;
};

#endif /* kern_svm_hpp */
