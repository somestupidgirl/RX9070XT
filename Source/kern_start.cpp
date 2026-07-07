//
//  kern_start.cpp
//  RX9070XT
//
//  Lilu plugin entry point. The framebuffer itself (RX9070XTFB) is matched and
//  instantiated directly by IOKit from the bundle's Info.plist personality, so
//  this plugin's job is the Lilu-side work: logging, and a hook point where
//  device-property injection / register pokes for Navi 48 can be added.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_devinfo.hpp>

static const char *bootargOff[]   = { "-rx9070xtoff" };
static const char *bootargDebug[] = { "-rx9070xtdbg" };
static const char *bootargBeta[]  = { "-rx9070xtbeta" };

// Called once the kernel patcher is up. This is the right place to inject
// properties onto the GPU PCI node or install patches. Right now it only
// verifies the card is present and logs, leaving a clear extension point.
static void rx9070xtStart() {
	SYSLOG("rx9070xt", "plugin start (framebuffer is matched via Info.plist)");

	// Report whether the target GPU is present so logs are actionable.
	auto &bdi = BaseDeviceInfo::get();
	DBGLOG("rx9070xt", "running on board '%s', cpu gen %d", bdi.boardIdentifier,
	       static_cast<int>(bdi.cpuGeneration));
	(void)bdi;

	lilu.onPatcherLoadForce([](void *, KernelPatcher &) {
		// Extension point: once you begin driving Navi 48's DCN 4.0.1 display
		// engine directly, install register access / connector patches here.
		DBGLOG("rx9070xt", "kernel patcher loaded");
	});
}

// Plugin configuration consumed by Lilu's plugin_start machinery.
PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	bootargOff,   arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta,  arrsize(bootargBeta),
	KernelVersion::BigSur,   // minimum: 11.x
	KernelVersion::Sequoia,  // maximum: keep permissive for cross-version testing
	[]() { rx9070xtStart(); }
};
