/*
 * kmod_info.c - the kext's kmod_info structure and start/stop bindings.
 *
 * Xcode's "Generic Kernel Extension" product type synthesises this file from
 * the MODULE_NAME / MODULE_VERSION build settings. This project links by hand
 * from a Makefile, so it must be provided explicitly - without it libkmod's
 * _start/_stop stubs have no _kmod_info, _realmain or _antimain to bind to and
 * the kernel has no entry point for the kext.
 *
 * Lilu's plugin_start.cpp supplies the actual entry points, named via ADDPR():
 * with PRODUCT_NAME=AMDV they are AMDV_kern_start / AMDV_kern_stop.
 */

#include <mach/mach_types.h>
#include <mach/kmod.h>
#include <libkern/libkern.h>

#ifndef PRODUCT_NAME
#error "PRODUCT_NAME must be defined by the build system"
#endif
#ifndef BUNDLE_ID
#error "BUNDLE_ID must be defined by the build system"
#endif
#ifndef MODULE_VERSION
#error "MODULE_VERSION must be defined by the build system"
#endif

#define Stringify(a)  #a
#define xStringify(a) Stringify(a)

/* Token-paste PRODUCT_NAME onto the Lilu ADDPR() entry-point names. */
#define ADDPR_CAT(a, b) a ## _ ## b
#define ADDPR_EVAL(a, b) ADDPR_CAT(a, b)
#define ADDPR_SYM(sym) ADDPR_EVAL(PRODUCT_NAME, sym)

extern kern_return_t ADDPR_SYM(kern_start)(kmod_info_t *ki, void *data);
extern kern_return_t ADDPR_SYM(kern_stop)(kmod_info_t *ki, void *data);

/* libkmod's thin entry stubs (c_start.o / c_stop.o). Naming them here is what
 * pulls those objects out of libkmod.a, which also brings in
 * OSKextGetCurrentIdentifier/VersionString/LoadTag. They trampoline through
 * _realmain / _antimain below. This mirrors exactly what Xcode's generated
 * kmod info file does; don't shortcut kmod_info straight to the real entry
 * points or c_start.o is never linked.
 *
 * Note: C++ static constructors are NOT run here. cplus_start.c is
 * #if __i386__ || __ppc__, so on x86_64 the kernel itself runs them via
 * OSRuntimeInitializeCPP when loading an MH_KEXT_BUNDLE.
 */
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

/*
 * Defines _kmod_info. The bundle id MUST match CFBundleIdentifier in
 * src/Info.plist, or the kernel rejects the kext.
 *
 * KMOD_EXPLICIT_DECL stringifies its first argument with '#', and '#'
 * suppresses macro expansion of that argument - passing BUNDLE_ID straight in
 * bakes the literal text "BUNDLE_ID" into kmod_info.name. Routing through one
 * extra macro layer forces the argument to expand first. (The version
 * argument is NOT stringified by the macro, so it is pre-stringified here.)
 */
#define KMOD_DECL_EXPANDED(name, version, start, stop) \
	KMOD_EXPLICIT_DECL(name, version, start, stop)

KMOD_DECL_EXPANDED(BUNDLE_ID, xStringify(MODULE_VERSION), _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = ADDPR_SYM(kern_start);
__private_extern__ kmod_stop_func_t  *_antimain = ADDPR_SYM(kern_stop);
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
