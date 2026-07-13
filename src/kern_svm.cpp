//
//  kern_svm.cpp - AMD-V (SVM) hardware backend.
//

#include "kern_svm.hpp"

#include <Headers/kern_util.hpp>
#include <i386/proc_reg.h>   // rdmsr64 / wrmsr64
#include <string.h>

bool SvmBackend::detect()
{
	uint32_t a, b, c, d;

	// Vendor must be "AuthenticAMD".
	amdv_cpuid(CPUID_VENDOR, 0, &a, &b, &c, &d);
	if (b != 0x68747541u || d != 0x69746e65u || c != 0x444d4163u) {
		DBGLOG("amdv", "not an AMD processor; SVM unsupported");
		return false;
	}

	amdv_cpuid(CPUID_EXT_MAX, 0, &a, &b, &c, &d);
	if (a < CPUID_SVM_FEATURES) {
		DBGLOG("amdv", "extended CPUID leaf 0x%08X unavailable", CPUID_SVM_FEATURES);
		return false;
	}

	amdv_cpuid(CPUID_EXT_FEATURES, 0, &a, &b, &c, &d);
	if (!(c & CPUID_ECX_SVM)) {
		DBGLOG("amdv", "CPU does not advertise SVM");
		return false;
	}

	amdv_cpuid(CPUID_SVM_FEATURES, 0, &a, &b, &c, &d);
	fSvmRevision = a & 0xFF;
	fNumAsids    = b;
	fSvmFeatures = d;

	uint64_t vmcr = rdmsr64(MSR_VM_CR);
	if (vmcr & VM_CR_SVMDIS) {
		DBGLOG("amdv", "SVM disabled by firmware (VM_CR.SVMDIS%s) - enable AMD-V in BIOS",
			   (fSvmFeatures & SVM_FEATURE_SVML) ? "+locked" : "");
		return false;
	}

	SYSLOG("amdv", "SVM present: rev %u, %u ASIDs, features 0x%08X (NP=%d NRIPS=%d)",
		   fSvmRevision, fNumAsids, fSvmFeatures,
		   !!(fSvmFeatures & SVM_FEATURE_NP), !!(fSvmFeatures & SVM_FEATURE_NRIPS));
	return true;
}

void *SvmBackend::allocContiguousPage(IOBufferMemoryDescriptor **outDesc, uint64_t *outPhys)
{
	IOBufferMemoryDescriptor *desc =
		IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
			kernel_task,
			kIODirectionInOut | kIOMemoryPhysicallyContiguous,
			VMCB_SIZE,
			0xFFFFFFFFFFFFF000ull /* 4-KiB alignment */);
	if (!desc)
		return nullptr;

	if (desc->prepare() != kIOReturnSuccess) {
		desc->release();
		return nullptr;
	}

	IOByteCount seg = 0;
	IOPhysicalAddress64 pa = desc->getPhysicalSegment(0, &seg, 0);
	void *va = desc->getBytesNoCopy();
	if (!pa || !va || seg < VMCB_SIZE) {
		desc->complete();
		desc->release();
		return nullptr;
	}

	memset(va, 0, VMCB_SIZE);
	*outDesc = desc;
	*outPhys = static_cast<uint64_t>(pa);
	return va;
}

bool SvmBackend::enable()
{
	uint64_t hsavePhys = 0;
	if (!allocContiguousPage(&fHsaveDesc, &hsavePhys)) {
		SYSLOG("amdv", "failed to allocate host save area");
		return false;
	}
	fHsavePA = hsavePhys;

	// Per-CPU: sets SVME on the current processor only.
	uint64_t efer = rdmsr64(MSR_EFER);
	wrmsr64(MSR_EFER, efer | EFER_SVME);
	if (!(rdmsr64(MSR_EFER) & EFER_SVME)) {
		SYSLOG("amdv", "EFER.SVME did not stick");
		return false;
	}
	wrmsr64(MSR_VM_HSAVE_PA, fHsavePA);

	fVmcb = static_cast<vmcb_t *>(allocContiguousPage(&fVmcbDesc, &fVmcbPA));
	if (!fVmcb) {
		SYSLOG("amdv", "failed to allocate VMCB");
		disable();
		return false;
	}

	fVmcb->intercept_1 = INTERCEPT_CPUID | INTERCEPT_HLT;
	fVmcb->intercept_2 = INTERCEPT2_VMRUN | INTERCEPT2_VMMCALL;
	fVmcb->guest_asid  = 1;

	SYSLOG("amdv", "SVME enabled; VM_HSAVE_PA=0x%016llx VMCB=0x%016llx", fHsavePA, fVmcbPA);
	fEnabled = true;
	return true;
}

void SvmBackend::disable()
{
	if (fEnabled) {
		uint64_t efer = rdmsr64(MSR_EFER);
		wrmsr64(MSR_EFER, efer & ~EFER_SVME);
		wrmsr64(MSR_VM_HSAVE_PA, 0);
		fEnabled = false;
	}
	if (fVmcbDesc)  { fVmcbDesc->complete();  fVmcbDesc->release();  fVmcbDesc = nullptr; }
	if (fHsaveDesc) { fHsaveDesc->complete(); fHsaveDesc->release(); fHsaveDesc = nullptr; }
	fVmcb = nullptr; fVmcbPA = 0; fHsavePA = 0;
}
