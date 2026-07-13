//
//  kern_start.cpp - AMDV Lilu plugin entry point.
//
//  PRODUCT_NAME and MODULE_VERSION are provided by the build system (-D).
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include "kern_hv_amd.hpp"

static const char *bootargOff[]   { "-amdvoff"   };  // disable the plugin
static const char *bootargDebug[] { "-amdvdbg"   };  // verbose logging
static const char *bootargBeta[]  { "-amdvbeta"  };  // allow on untested kernels

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
	bootargOff,   arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta,  arrsize(bootargBeta),
	KernelVersion::BigSur,   // min: this plugin targets Big Sur specifically
	KernelVersion::BigSur,   // max
	[]() {
		hvAmd.init();
	}
};
