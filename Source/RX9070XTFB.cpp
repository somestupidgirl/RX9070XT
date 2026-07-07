//
//  RX9070XTFB.cpp
//  RX9070XT
//

#include "RX9070XTFB.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>

#define FBLOG(fmt, ...)  IOLog("RX9070XTFB: " fmt "\n", ## __VA_ARGS__)

OSDefineMetaClassAndStructors(RX9070XTFB, IOFramebuffer)

// ---------------------------------------------------------------------------
// Console framebuffer discovery
// ---------------------------------------------------------------------------

bool RX9070XTFB::captureConsoleInfo() {
	IOPlatformExpert *platform = getPlatform();
	if (!platform) {
		FBLOG("no platform expert");
		return false;
	}

	PE_Video video {};
	IOReturn ret = platform->getConsoleInfo(&video);
	if (ret != kIOReturnSuccess) {
		FBLOG("getConsoleInfo failed 0x%x", ret);
		return false;
	}

	fbPhysBase = static_cast<IOPhysicalAddress64>(video.v_baseAddr);
	fbWidth    = static_cast<uint32_t>(video.v_width);
	fbHeight   = static_cast<uint32_t>(video.v_height);
	fbRowBytes = static_cast<uint32_t>(video.v_rowBytes);
	fbDepth    = static_cast<uint32_t>(video.v_depth);

	// v_length may be 0, in which case it is rowBytes * height.
	fbLength = video.v_length ? static_cast<uint64_t>(video.v_length)
	                          : static_cast<uint64_t>(fbRowBytes) * fbHeight;

	if (fbPhysBase == 0 || fbWidth == 0 || fbHeight == 0 || fbRowBytes == 0) {
		FBLOG("console framebuffer looks invalid: base=0x%llx %ux%u stride=%u depth=%u",
		      fbPhysBase, fbWidth, fbHeight, fbRowBytes, fbDepth);
		return false;
	}

	// We only support a 32bpp linear framebuffer.
	if (fbDepth != 32 && fbDepth != 30 && fbDepth != 24) {
		FBLOG("unsupported console depth %u", fbDepth);
		return false;
	}
	fbDepth = 32;

	FBLOG("console framebuffer: base=0x%llx %ux%u stride=%u depth=%u len=%llu",
	      fbPhysBase, fbWidth, fbHeight, fbRowBytes, fbDepth, fbLength);
	return true;
}

// ---------------------------------------------------------------------------
// IOService
// ---------------------------------------------------------------------------

IOService *RX9070XTFB::probe(IOService *provider, SInt32 *score) {
	if (!super::probe(provider, score))
		return nullptr;

	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) {
		FBLOG("provider is not an IOPCIDevice");
		return nullptr;
	}

	uint32_t vendorDevice = pci->configRead32(kIOPCIConfigVendorID);
	FBLOG("probe on PCI device 0x%08x", vendorDevice);

	// Vendor 0x1002 (AMD), device 0x7550 (Navi 48 / RX 9070 XT).
	if (vendorDevice != 0x75501002) {
		FBLOG("device id mismatch, not matching");
		return nullptr;
	}

	if (score) *score += 20000;
	return this;
}

bool RX9070XTFB::start(IOService *provider) {
	pciDevice = OSDynamicCast(IOPCIDevice, provider);
	if (!pciDevice) {
		FBLOG("start: no PCI provider");
		return false;
	}

	// Grab the bootloader-provided framebuffer before anything else touches it.
	if (!captureConsoleInfo()) {
		FBLOG("start: could not capture console framebuffer, aborting");
		return false;
	}

	// Enable memory space so the aperture is reachable; do NOT enable bus
	// mastering — we never issue DMA.
	pciDevice->setMemoryEnable(true);

	if (!super::start(provider)) {
		FBLOG("start: super::start failed");
		return false;
	}

	FBLOG("started");
	return true;
}

void RX9070XTFB::stop(IOService *provider) {
	FBLOG("stop");
	super::stop(provider);
}

// ---------------------------------------------------------------------------
// IOFramebuffer — bring-up
// ---------------------------------------------------------------------------

IOReturn RX9070XTFB::enableController() {
	// The controller is already "enabled": the firmware programmed the display
	// pipe and scanout address. We just re-validate the console geometry.
	if (fbPhysBase == 0 && !captureConsoleInfo())
		return kIOReturnNoResources;

	FBLOG("enableController: adopting firmware scanout %ux%u", fbWidth, fbHeight);
	return kIOReturnSuccess;
}

bool RX9070XTFB::isConsoleDevice() {
	return true;
}

// ---------------------------------------------------------------------------
// IOFramebuffer — memory
// ---------------------------------------------------------------------------

IODeviceMemory *RX9070XTFB::getApertureRange(IOPixelAperture aperture) {
	if (aperture != kIOFBSystemAperture)
		return nullptr;
	if (fbPhysBase == 0 || fbLength == 0)
		return nullptr;

	// A fresh instance (retain) per contract with the caller.
	return IODeviceMemory::withRange(fbPhysBase, fbLength);
}

IODeviceMemory *RX9070XTFB::getVRAMRange() {
	// We expose only the scanout region as "VRAM"; we do not manage the card's
	// full VRAM because we have no memory controller driver.
	if (fbPhysBase == 0 || fbLength == 0)
		return nullptr;
	return IODeviceMemory::withRange(fbPhysBase, fbLength);
}

// ---------------------------------------------------------------------------
// IOFramebuffer — pixel / mode description
// ---------------------------------------------------------------------------

const char *RX9070XTFB::getPixelFormats() {
	// NUL-separated, double-NUL terminated list. IO32BitDirectPixels is a
	// string literal macro from IOGraphicsTypes.h.
	static const char formats[] = IO32BitDirectPixels "\0";
	return formats;
}

IOItemCount RX9070XTFB::getDisplayModeCount() {
	return 1;
}

IOReturn RX9070XTFB::getDisplayModes(IODisplayModeID *allDisplayModes) {
	if (!allDisplayModes)
		return kIOReturnBadArgument;
	allDisplayModes[0] = kRX9070XTDisplayModeID;
	return kIOReturnSuccess;
}

IOReturn RX9070XTFB::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                  IODisplayModeInformation *info) {
	if (displayMode != kRX9070XTDisplayModeID || !info)
		return kIOReturnBadArgument;

	bzero(info, sizeof(*info));
	info->nominalWidth  = fbWidth;
	info->nominalHeight = fbHeight;
	info->refreshRate   = 60 << 16;   // 60.0 Hz in 16.16 fixed point
	info->maxDepthIndex = 0;
	info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
	return kIOReturnSuccess;
}

UInt64 RX9070XTFB::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
	// Obsolete: must return 0.
	return 0;
}

IOReturn RX9070XTFB::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                         IOPixelAperture aperture,
                                         IOPixelInformation *pixelInfo) {
	if (displayMode != kRX9070XTDisplayModeID || depth != 0 ||
	    aperture != kIOFBSystemAperture || !pixelInfo)
		return kIOReturnBadArgument;

	bzero(pixelInfo, sizeof(*pixelInfo));
	pixelInfo->bytesPerRow      = fbRowBytes;
	pixelInfo->bitsPerPixel     = 32;
	pixelInfo->pixelType        = kIORGBDirectPixels;
	pixelInfo->componentCount   = 3;
	pixelInfo->bitsPerComponent = 8;
	// ARGB8888 / XRGB8888 little-endian: R,G,B masks.
	pixelInfo->componentMasks[0] = 0x00FF0000; // R
	pixelInfo->componentMasks[1] = 0x0000FF00; // G
	pixelInfo->componentMasks[2] = 0x000000FF; // B
	strlcpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
	pixelInfo->activeWidth  = fbWidth;
	pixelInfo->activeHeight = fbHeight;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// IOFramebuffer — current mode
// ---------------------------------------------------------------------------

IOReturn RX9070XTFB::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
	if (displayMode) *displayMode = kRX9070XTDisplayModeID;
	if (depth)       *depth       = 0;
	return kIOReturnSuccess;
}

IOReturn RX9070XTFB::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
	// Only one mode/depth exists; accept it, reject anything else.
	if (displayMode != kRX9070XTDisplayModeID || depth != 0)
		return kIOReturnUnsupported;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// IOFramebuffer — attributes / connection
// ---------------------------------------------------------------------------

IOReturn RX9070XTFB::getAttribute(IOSelect attribute, uintptr_t *value) {
	switch (attribute) {
		case kIOHardwareCursorAttribute:
			// No hardware cursor: force IOGraphics to use a software cursor.
			if (value) *value = 0;
			return kIOReturnSuccess;
		default:
			return super::getAttribute(attribute, value);
	}
}

IOItemCount RX9070XTFB::getConnectionCount() {
	return 1;
}

IOReturn RX9070XTFB::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t *value) {
	switch (attribute) {
		case kConnectionEnable:
			if (value) *value = 1;
			return kIOReturnSuccess;
		case kConnectionCheckEnable:
			if (value) *value = 1;
			return kIOReturnSuccess;
		case kConnectionFlags:
			if (value) *value = 0;
			return kIOReturnSuccess;
		default:
			return super::getAttributeForConnection(connectIndex, attribute, value);
	}
}

IOReturn RX9070XTFB::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t value) {
	switch (attribute) {
		case kConnectionPower:
			// We cannot change power; pretend success so the OS proceeds.
			return kIOReturnSuccess;
		default:
			return super::setAttributeForConnection(connectIndex, attribute, value);
	}
}
