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

	// IP discovery only exists in a full flash dump; a bare legacy VBIOS
	// (typical expansion ROM contents) won't have it. Non-fatal.
	if (ipDiscovery.init(vbiosData, vbiosSize)) {
		FBLOG("discovery: binary at +0x%zx, %u IPs", ipDiscovery.binaryOffset(),
		      ipDiscovery.ipCount());
		IpDiscovery::IpEntry gc, dmu;
		if (ipDiscovery.findIp(IpDiscovery::HwGc, 0, gc)) {
			char ver[16];
			snprintf(ver, sizeof(ver), "%u.%u.%u", gc.major, gc.minor, gc.revision);
			setProperty("Discovery,GCVersion", ver);
		}
		if (ipDiscovery.findIp(IpDiscovery::HwDmu, 0, dmu)) {
			char ver[16];
			snprintf(ver, sizeof(ver), "%u.%u.%u", dmu.major, dmu.minor, dmu.revision);
			setProperty("Discovery,DCNVersion", ver);
		}
	} else {
		FBLOG("discovery: not present in image (inject the full 2MiB flash dump to enable)");
	}
	return true;
}

// ---------------------------------------------------------------------------
// Register MMIO (BAR5)
// ---------------------------------------------------------------------------

bool RX9070XTFB::mapRegisters() {
	// On AMD dGPUs since Bonaire the register aperture is BAR5 (BAR0/1 is the
	// VRAM aperture, BAR2/3 doorbells) — amdgpu_device.c does the same.
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!bar) {
		FBLOG("mmio: BAR5 not present/assigned");
		return false;
	}

	rmmioMap = bar->map();
	if (!rmmioMap) {
		FBLOG("mmio: failed to map BAR5");
		return false;
	}

	rmmio = reinterpret_cast<volatile uint32_t *>(rmmioMap->getVirtualAddress());
	rmmioSize = rmmioMap->getLength();
	FBLOG("mmio: BAR5 mapped, %zu KiB", rmmioSize / 1024);
	return true;
}

void RX9070XTFB::unmapRegisters() {
	rmmio = nullptr;
	rmmioSize = 0;
	if (rmmioMap) {
		rmmioMap->release();
		rmmioMap = nullptr;
	}
}

uint32_t RX9070XTFB::regRead32(uint32_t byteOffset) const {
	if (!rmmio || byteOffset + 4 > rmmioSize)
		return 0xFFFFFFFF;
	return rmmio[byteOffset / 4];
}

uint32_t RX9070XTFB::regReadDmu(uint8_t baseIdx, uint32_t dwordOffset) const {
	uint32_t byteOffset;
	if (!ipDiscovery.isValid() ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwDmu, 0, baseIdx, dwordOffset, byteOffset))
		return 0xFFFFFFFF;
	return regRead32(byteOffset);
}

void RX9070XTFB::dumpDCN() {
	if (!ipDiscovery.isValid()) {
		FBLOG("dcn: no discovery bases, skipping register dump");
		return;
	}

	// DCN 4.1.0 (Navi 48) output-pipe registers, pipe/instance 0.
	// {name, base_idx, dword offset} — offsets from Linux dcn_4_1_0_offset.h.
	// This is the console pipe the firmware lit up; if these read as identity
	// we will widen to pipes 1-3.
	// All pipe color blocks read identity, so the (G,B,R) rotation is not in
	// the DCN color pipe. Prime suspect now: the DP stream encoder emitting
	// YCbCr while the sink assumes RGB. Dump each DP encoder's pixel format
	// (DP_PIXEL_ENCODING: 0=RGB, 1=YCbCr422, 2=YCbCr444) and MSA colorimetry,
	// plus each HUBP surface config to identify the active pipe.
	struct DpEnc { uint32_t pixFmt, colorimetry; };
	static const DpEnc dp[4] = {
		{ 0x211f, 0x2120 }, { 0x2243, 0x2244 }, { 0x2367, 0x2368 }, { 0x248b, 0x248c },
	};
	static const uint32_t hubpCfg[4] = { 0x05e5, 0x06c1, 0x079d, 0x0879 };
	static const uint32_t digFeCntl[4] = { 0x2093, 0x21b7 /*+0x124*/, 0x22db, 0x23ff };

	FBLOG("dcn: register dump (encoders, read-only) ---");
	FBLOG("dcn:   OTG0_OTG_CONTROL = 0x%08x", regReadDmu(2, 0x1b43));
	for (int i = 0; i < 4; i++)
		FBLOG("dcn:   [%d] HUBP_surf=0x%08x DP_PIXEL_FORMAT=0x%08x DP_MSA_COLORIMETRY=0x%08x DIG_FE_CNTL=0x%08x",
		      i, regReadDmu(2, hubpCfg[i]), regReadDmu(2, dp[i].pixFmt),
		      regReadDmu(2, dp[i].colorimetry), regReadDmu(2, digFeCntl[i]));
	// MSA reads consistent (RGB 10bpc, MISC0=0x40 in COLORIMETRY[31:24]).
	// Remaining GPU-side suspect: secondary data packets (VSC/GSP) that DP1.3+
	// sinks may honor over the MSA. Dump the SDP enables and stream control.
	FBLOG("dcn:   DP0_DP_MSA_MISC = 0x%08x (MISC1..4)", regReadDmu(2, 0x2124));
	FBLOG("dcn:   DP0_DP_VID_STREAM_CNTL = 0x%08x", regReadDmu(2, 0x2122));
	FBLOG("dcn:   DP0_DP_SEC_CNTL  = 0x%08x (GSP/VSC/ASP enables)", regReadDmu(2, 0x2141));
	FBLOG("dcn:   DP0_DP_SEC_CNTL1 = 0x%08x", regReadDmu(2, 0x2142));
	FBLOG("dcn:   DP0_DP_SEC_CNTL2 = 0x%08x", regReadDmu(2, 0x2169));
	FBLOG("dcn:   DP0_DP_SEC_CNTL7 = 0x%08x", regReadDmu(2, 0x216e));
	FBLOG("dcn:   DP0_DP_MSA_VBID_MISC = 0x%08x", regReadDmu(2, 0x2170));
	FBLOG("dcn: --- end register dump ---");
}

bool RX9070XTFB::regWriteDmu(uint8_t baseIdx, uint32_t dwordOffset, uint32_t value) {
	uint32_t byteOffset;
	if (!ipDiscovery.isValid() ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwDmu, 0, baseIdx, dwordOffset, byteOffset))
		return false;
	if (!rmmio || byteOffset + 4 > rmmioSize)
		return false;
	rmmio[byteOffset / 4] = value;
	return true;
}

void RX9070XTFB::tryForce8bpc() {
	uint32_t v = 0;
	if (!PE_parse_boot_argn("rx9070xt-8bpc", &v, sizeof(v)) || v == 0)
		return;

	// DP0 stream encoder (the active one on this machine).
	constexpr uint32_t kDpPixelFormat   = 0x211f; // base 2
	constexpr uint32_t kDpMsaColorimetry= 0x2120; // base 2
	constexpr uint32_t kDepthMask       = 0x00000700; // UNCOMPRESSED_COMPONENT_DEPTH
	constexpr uint32_t kDepth8bpc       = 1u << 8;
	constexpr uint32_t kMisc0Mask       = 0xFF000000; // MISC0 in [31:24]
	constexpr uint32_t kMisc0Rgb8bpc    = 0x20u << 24; // bits[7:5]=001 -> 8 bpc, RGB

	uint32_t pf  = regReadDmu(2, kDpPixelFormat);
	uint32_t col = regReadDmu(2, kDpMsaColorimetry);
	if (pf == 0xFFFFFFFF || col == 0xFFFFFFFF) {
		FBLOG("8bpc: register read failed, aborting");
		return;
	}

	uint32_t pfNew  = (pf  & ~kDepthMask) | kDepth8bpc;
	uint32_t colNew = (col & ~kMisc0Mask) | kMisc0Rgb8bpc;
	FBLOG("8bpc: DP_PIXEL_FORMAT 0x%08x -> 0x%08x, MSA_COLORIMETRY 0x%08x -> 0x%08x",
	      pf, pfNew, col, colNew);
	regWriteDmu(2, kDpPixelFormat, pfNew);
	regWriteDmu(2, kDpMsaColorimetry, colNew);
	FBLOG("8bpc: readback DP_PIXEL_FORMAT=0x%08x MSA_COLORIMETRY=0x%08x",
	      regReadDmu(2, kDpPixelFormat), regReadDmu(2, kDpMsaColorimetry));
}

void RX9070XTFB::probeMemSize() {
	// RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE (NBIF 6.3.1 seg 2, dword 0x00c3):
	// VRAM size in MiB — amdgpu's nbif_v6_3_1_get_memsize. The byte offset
	// below was derived from this card's IP discovery table (base 0xd20)
	// and is used as the fallback when discovery isn't available at runtime.
	uint32_t off = 0x378c;
	uint32_t discOff;
	if (ipDiscovery.isValid() &&
	    ipDiscovery.regByteOffset(IpDiscovery::HwNbif, 0, 2, 0x00c3, discOff)) {
		if (discOff != off)
			FBLOG("mmio: discovery moved RCC_CONFIG_MEMSIZE to 0x%x", discOff);
		off = discOff;
	}

	uint32_t memsizeMB = regRead32(off);
	if (memsizeMB == 0 || memsizeMB == 0xFFFFFFFF) {
		FBLOG("mmio: RCC_CONFIG_MEMSIZE read failed (0x%08x) — MMIO not usable", memsizeMB);
		return;
	}

	FBLOG("mmio: VRAM size %u MiB (RCC_CONFIG_MEMSIZE @ 0x%x)", memsizeMB, off);
	setProperty("VRAM,TotalMB", static_cast<uint64_t>(memsizeMB), 32);
	// Independent confirmation that register MMIO works — the gate for all
	// future DCN (AUX/EDID, mode setting) work. The register reports usable
	// VRAM (nominal size minus firmware reservations: 16304 on this 16 GiB
	// card), so accept any plausible value rather than an exact match.
	setProperty("MMIO,Verified", memsizeMB >= 1024 && memsizeMB <= 65536);
}

// ---------------------------------------------------------------------------
// IOService
// ---------------------------------------------------------------------------

IOService *RX9070XTFB::probe(IOService *provider, SInt32 *score) {
	// Kill switch: boot-arg "rx9070xt-off=1" (no leading dash) disables the
	// driver without removing it, so a bad build can be recovered from the
	// OpenCore boot-args instead of Safe Mode / filesystem surgery.
	uint32_t off = 0;
	if (PE_parse_boot_argn("rx9070xt-off", &off, sizeof(off)) && off != 0) {
		FBLOG("disabled by rx9070xt-off boot-arg");
		return nullptr;
	}

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

	uint32_t cmap = 0;
	if (PE_parse_boot_argn("rx9070xt-cmap", &cmap, sizeof(cmap)) && cmap <= 5) {
		channelMap = cmap;
		FBLOG("start: using channel map %u", channelMap);
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

	// Best effort: prove register MMIO works with one safe read (VRAM size),
	// then dump the DCN output-colour registers (read-only) for diagnosis.
	if (mapRegisters()) {
		probeMemSize();
		dumpDCN();
		tryForce8bpc();
	}

	if (!super::start(provider)) {
		FBLOG("start: super::start failed");
		return false;
	}

	FBLOG("started");
	return true;
}

void RX9070XTFB::stop(IOService *provider) {
	FBLOG("stop");
	unmapRegisters();
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

	// Which memory byte (0=LSB..2) each colour component occupies, per the
	// active channel map. Index [channelMap] -> {byteForR, byteForG, byteForB}.
	static const uint8_t kMaps[6][3] = {
		{2, 1, 0},  // 0: standard ARGB (RGB in bytes 2,1,0)
		{1, 0, 2},  // 1: cancels observed (G,B,R) rotation
		{0, 1, 2},  // 2: BGR swap
		{2, 0, 1},  // 3
		{1, 2, 0},  // 4
		{0, 2, 1},  // 5
	};
	uint32_t idx = channelMap <= 5 ? channelMap : 0;
	uint8_t byteR = kMaps[idx][0], byteG = kMaps[idx][1], byteB = kMaps[idx][2];

	pixelInfo->componentMasks[0] = 0xFFu << (byteR * 8); // R
	pixelInfo->componentMasks[1] = 0xFFu << (byteG * 8); // G
	pixelInfo->componentMasks[2] = 0xFFu << (byteB * 8); // B

	// Build a matching IOPixelEncoding string (MSB byte first). Some
	// CoreGraphics paths key off this rather than the masks, so keep both
	// consistent. Byte 3 is unused (X).
	char letters[4] = { '-', '-', '-', '-' };  // index = byte number (0..3)
	letters[byteR] = 'R';
	letters[byteG] = 'G';
	letters[byteB] = 'B';
	char *p = pixelInfo->pixelFormat;
	for (int b = 3; b >= 0; b--)
		for (int i = 0; i < 8; i++)
			*p++ = letters[b];
	*p = '\0';

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
