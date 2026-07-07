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

	// Publish what we adopted so `ioreg -lw0` on the target shows the
	// geometry without needing the kernel log.
	setProperty("Console,BaseAddress", fbPhysBase, 64);
	setProperty("Console,Width", static_cast<uint64_t>(fbWidth), 32);
	setProperty("Console,Height", static_cast<uint64_t>(fbHeight), 32);
	setProperty("Console,RowBytes", static_cast<uint64_t>(fbRowBytes), 32);
	setProperty("Console,Depth", static_cast<uint64_t>(fbDepth), 32);
	setProperty("Console,Length", fbLength, 64);
	return true;
}

// ---------------------------------------------------------------------------
// VBIOS acquisition and parsing
// ---------------------------------------------------------------------------

// Cap on how much ROM we are willing to copy. The full Navi 48 flash is 2 MiB;
// the legacy image within it is ~58 KiB.
static constexpr size_t kMaxVBIOSSize = 2 * 1024 * 1024;

bool RX9070XTFB::copyVBIOSFromProperty() {
	// OpenCore DeviceProperties (or WhateverGreen) can inject the VBIOS as
	// ATY,bin_image on the GPU's PCI node. Preferred: no hardware access.
	auto *rom = OSDynamicCast(OSData, pciDevice->getProperty("ATY,bin_image"));
	if (!rom || rom->getLength() < 512 || rom->getLength() > kMaxVBIOSSize)
		return false;

	vbiosSize = rom->getLength();
	vbiosData = static_cast<uint8_t *>(IOMalloc(vbiosSize));
	if (!vbiosData) {
		vbiosSize = 0;
		return false;
	}
	memcpy(vbiosData, rom->getBytesNoCopy(), vbiosSize);
	FBLOG("VBIOS: %zu bytes from ATY,bin_image", vbiosSize);
	return true;
}

bool RX9070XTFB::copyVBIOSFromExpansionROM() {
	// Size the expansion ROM BAR, then map and copy it with decode enabled.
	uint32_t saved = pciDevice->configRead32(kIOPCIConfigExpansionROMBase);
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, 0xFFFFF800);
	uint32_t sizing = pciDevice->configRead32(kIOPCIConfigExpansionROMBase);
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, saved);

	uint32_t addr = saved & 0xFFFFF800;
	size_t romLen = sizing ? static_cast<size_t>(~(sizing & 0xFFFFF800)) + 1 : 0;
	if (!addr || !romLen || romLen > kMaxVBIOSSize) {
		FBLOG("VBIOS: expansion ROM unavailable (bar=0x%08x size=%zu)", saved, romLen);
		return false;
	}

	auto *desc = IOMemoryDescriptor::withPhysicalAddress(addr, romLen, kIODirectionIn);
	if (!desc)
		return false;

	bool ok = false;
	// Enable ROM decode only for the duration of the copy.
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, addr | 1);
	auto *map = desc->map();
	if (map) {
		vbiosData = static_cast<uint8_t *>(IOMalloc(romLen));
		if (vbiosData) {
			memcpy(vbiosData, reinterpret_cast<const void *>(map->getVirtualAddress()), romLen);
			vbiosSize = romLen;
			ok = true;
		}
		map->release();
	}
	pciDevice->configWrite32(kIOPCIConfigExpansionROMBase, saved);
	desc->release();

	if (ok)
		FBLOG("VBIOS: %zu bytes from expansion ROM at 0x%08x", vbiosSize, addr);
	return ok;
}

void RX9070XTFB::freeVBIOS() {
	if (vbiosData) {
		IOFree(vbiosData, vbiosSize);
		vbiosData = nullptr;
		vbiosSize = 0;
	}
}

void RX9070XTFB::publishVBIOSInfo() {
	char name[64];
	if (atomBios.configName(name, sizeof(name)))
		setProperty("AtomBIOS,ImageName", name);

	AtomBios::FirmwareInfo3 fw;
	if (atomBios.getFirmwareInfo(fw)) {
		setProperty("AtomBIOS,FirmwareRevision", fw.firmwareRevision, 32);
		setProperty("AtomBIOS,FirmwareCapability", fw.firmwareCapability, 32);
	}

	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = atomBios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	if (n) {
		// e.g. "DisplayPort/ddc0/hpd1,DisplayPort/ddc1/hpd2,..."
		char list[192] {};
		for (size_t i = 0; i < n; i++) {
			const char *name =
			    AtomBios::connectorName(AtomBios::connectorType(paths[i].connectorObjId));

			AtomBios::PathRecords rec {};
			atomBios.getPathRecords(paths[i], rec);

			char entry[48];
			snprintf(entry, sizeof(entry), "%s%s/ddc%u/hpd%u",
			         i ? "," : "", name, rec.ddcLine, rec.hpdPin);
			strlcat(list, entry, sizeof(list));

			FBLOG("connector %zu: %s objid=0x%04x encoder=0x%04x ddc-line=%u hpd-pin=%u", i,
			      name, paths[i].connectorObjId, paths[i].encoderObjId, rec.ddcLine, rec.hpdPin);
		}
		setProperty("AtomBIOS,Connectors", list);
		setProperty("AtomBIOS,ConnectorCount", static_cast<uint64_t>(n), 32);
	}
}

bool RX9070XTFB::loadVBIOS() {
	if (!copyVBIOSFromProperty() && !copyVBIOSFromExpansionROM()) {
		FBLOG("VBIOS: no image available (inject ATY,bin_image via DeviceProperties)");
		return false;
	}

	if (!atomBios.init(vbiosData, vbiosSize)) {
		FBLOG("VBIOS: image failed AtomBIOS validation");
		freeVBIOS();
		return false;
	}

	FBLOG("VBIOS: valid AtomBIOS image at +0x%zx, %zu bytes",
	      atomBios.imageOffset(), atomBios.imageLength());
	publishVBIOSInfo();
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

	// Best effort: connector layout and firmware info for later native
	// mode-setting work. The framebuffer itself does not depend on this.
	loadVBIOS();

	if (!super::start(provider)) {
		FBLOG("start: super::start failed");
		return false;
	}

	FBLOG("started");
	return true;
}

void RX9070XTFB::stop(IOService *provider) {
	FBLOG("stop");
	freeVBIOS();
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
