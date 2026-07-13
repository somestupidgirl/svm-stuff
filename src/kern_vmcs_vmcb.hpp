//
//  kern_vmcs_vmcb.hpp - translate a shadow VMCS into an SVM VMCB and back.
//
//  This is the intellectual core of "VMX on SVM": mapping the Intel VMCS
//  Apple populated onto the fields AMD's VMRUN consumes, and mapping the SVM
//  #VMEXIT back onto the VMX exit information Apple's code will read. The
//  mapping is concrete and reviewable; it is NOT yet validated on hardware,
//  and two mappings are fundamentally lossy (see notes in the .cpp):
//    * EPT page tables are not NPT page tables (different PTE formats).
//    * VMX MSR/IO bitmaps are not SVM MSRPM/IOPM (different layouts).
//

#ifndef kern_vmcs_vmcb_hpp
#define kern_vmcs_vmcb_hpp

#include "SVM.h"
#include "kern_vmx.hpp"

// Fill a VMCB from the shadow VMCS ahead of VMRUN.
void translateVmcsToVmcb(const ShadowVMCS &vmcs, vmcb_t *vmcb);

// Write guest state + exit information from the VMCB back into the shadow
// VMCS after VMRUN, so Apple's subsequent vmreads observe the exit.
void translateVmcbToVmcs(vmcb_t *vmcb, ShadowVMCS &vmcs, uint32_t svmFeatures);

// Map an SVM EXITCODE (+ EXITINFO1 for MSR direction) to a VMX basic exit
// reason. Returns EXIT_UNKNOWN for codes with no clean VMX analogue.
uint32_t svmExitToVmxReason(uint64_t svmExitCode, uint64_t exitInfo1);

// Segment access-rights format conversions (VMX 32-bit AR <-> SVM 12-bit).
uint16_t vmxArToSvmAttrib(uint32_t vmxAr);
uint32_t svmAttribToVmxAr(uint16_t svmAttrib);

#endif /* kern_vmcs_vmcb_hpp */
