/*
 * svmcheck.c - dump AMD-V / SVM capability straight from CPUID.
 *
 * An independent, kext-free check of what the AMDV plugin's
 * SvmBackend::detect() sees. Run it on the target AMD Big Sur machine; the NP
 * and NRIPS flags in particular decide which paths the VMX->SVM shim needs.
 *
 * IMPORTANT: VM_CR.SVMDIS - the "AMD-V disabled in BIOS" bit - lives in an MSR
 * and is NOT readable from userspace. A clean "SVM supported: yes" here does
 * not prove firmware has SVM enabled; only the kext log can confirm that.
 *
 * Build (cross-compiles for x86_64 from any host, incl. Apple Silicon):
 *     make -C tests          # or: make tests
 *
 * Exit codes (so scripts can branch):
 *     0  AMD with SVM present
 *     1  not an AMD processor
 *     2  AMD, but SVM not advertised
 *     3  CPUID leaf 0x8000000A unavailable
 */

#include <cpuid.h>
#include <stdio.h>
#include <string.h>

#define EX_OK       0
#define EX_NOT_AMD  1
#define EX_NO_SVM   2
#define EX_NO_LEAF  3

/* CPUID 0x8000000A EDX feature bits (AMD64 APM Vol.2 §15.4, table 15-1). */
struct svm_feature {
	unsigned    bit;
	const char *name;
	const char *note;
};

static const struct svm_feature kFeatures[] = {
	{  0, "NP (nested paging)",     "required to map guest memory" },
	{  1, "LbrVirt",                "" },
	{  2, "SVML (SVM lock)",        "if set with VM_CR.SVMDIS, BIOS locked SVM off" },
	{  3, "NRIPS",                  "if 0, RIP advance needs an instruction decoder" },
	{  4, "TscRateMsr",             "" },
	{  5, "VMCB clean bits",        "" },
	{  6, "FlushByAsid",            "" },
	{  7, "Decode assists",         "" },
	{ 10, "Pause filter",           "" },
	{ 12, "Pause filter threshold", "" },
	{ 13, "AVIC",                   "" },
	{ 15, "V_VMSAVE_VMLOAD",        "" },
	{ 16, "vGIF",                   "" },
};

int main(void)
{
	unsigned a, b, c, d;
	char vendor[13] = {0};

	/* Vendor gate: everything below is AMD-specific. Without this check the
	 * 0x8000000A leaf returns whatever happens to be in the registers, which
	 * prints as a confident but meaningless result on non-AMD CPUs. */
	if (!__get_cpuid(0, &a, &b, &c, &d)) {
		printf("CPUID leaf 0 unavailable; cannot identify the CPU.\n");
		return EX_NOT_AMD;
	}
	memcpy(vendor + 0, &b, 4);
	memcpy(vendor + 4, &d, 4);
	memcpy(vendor + 8, &c, 4);
	printf("vendor            : %s\n", vendor);

	if (strcmp(vendor, "AuthenticAMD") != 0) {
		printf("=> not an AMD processor; SVM checks are not applicable.\n");
		return EX_NOT_AMD;
	}

	/* Leaf 0x8000000A must exist before its contents mean anything. */
	if (!__get_cpuid(0x80000000, &a, &b, &c, &d)) {
		printf("=> extended CPUID leaves unavailable.\n");
		return EX_NO_LEAF;
	}
	printf("max extended leaf : 0x%08x\n", a);
	if (a < 0x8000000A) {
		printf("=> CPUID leaf 0x8000000A unavailable; no SVM information.\n");
		return EX_NO_LEAF;
	}

	__get_cpuid(0x80000001, &a, &b, &c, &d);
	int svm = (c & (1u << 2)) != 0;
	printf("SVM supported     : %s  (0x80000001 ECX bit 2)\n", svm ? "yes" : "NO");
	if (!svm) {
		printf("=> CPU does not advertise SVM.\n");
		return EX_NO_SVM;
	}

	__get_cpuid(0x8000000A, &a, &b, &c, &d);
	unsigned rev   = a & 0xff;
	unsigned asids = b;
	unsigned edx   = d;

	printf("SVM revision      : %u\n", rev);
	printf("usable ASIDs      : %u%s\n", asids,
	       asids > 1 ? "" : "   <- need > 1 to run a guest");
	printf("feature edx       : 0x%08x\n", edx);

	for (unsigned i = 0; i < sizeof(kFeatures) / sizeof(kFeatures[0]); i++) {
		int on = (edx >> kFeatures[i].bit) & 1;
		printf("  [%c] %-24s", on ? 'x' : ' ', kFeatures[i].name);
		if (kFeatures[i].note[0])
			printf("   %s", kFeatures[i].note);
		printf("\n");
	}

	/* Compact one-liner: the line worth pasting into an issue/report. */
	printf("\nsummary: rev=%u ASIDs=%u edx=0x%08x NP=%d NRIPS=%d\n",
	       rev, asids, edx, !!(edx & (1u << 0)), !!(edx & (1u << 3)));
	printf("note: VM_CR.SVMDIS is MSR-only; load the kext to confirm firmware\n"
	       "      has not disabled SVM.\n");
	return EX_OK;
}
