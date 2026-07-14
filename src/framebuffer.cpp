//
//  framebuffer.cpp
//  RDNA4FB
//

#include "framebuffer.hpp"
#include "edid.hpp"
#include "dmub.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/IOPM.h>

#define FBLOG(fmt, ...)  IOLog("RDNA4FB: " fmt "\n", ## __VA_ARGS__)

OSDefineMetaClassAndStructors(RDNA4FB, IOFramebuffer)

// ---------------------------------------------------------------------------
// Console framebuffer discovery
// ---------------------------------------------------------------------------

bool RDNA4FB::captureConsoleInfo() {
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

	// v_baseAddr carries flag bits in its low bits (observed 0x840000001 on
	// this machine); the scanout base itself is page-aligned. Apple's own
	// framebuffer code masks these off — failing to do so hands WindowServer
	// an aperture shifted one byte from the real scanout, which recolours
	// every pixel (B'=prev alpha, G'=B, R'=G).
	fbPhysBase = static_cast<IOPhysicalAddress64>(video.v_baseAddr) & ~0xFFFULL;
	if (video.v_baseAddr & 0xFFF)
		FBLOG("console base 0x%lx carries flag bits, using 0x%llx",
		      video.v_baseAddr, fbPhysBase);
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

bool RDNA4FB::copyVBIOSFromProperty() {
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

bool RDNA4FB::copyVBIOSFromExpansionROM() {
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

void RDNA4FB::freeVBIOS() {
	if (vbiosData) {
		IOFree(vbiosData, vbiosSize);
		vbiosData = nullptr;
		vbiosSize = 0;
	}
}

void RDNA4FB::publishVBIOSInfo() {
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
			const char *conn =
			    AtomBios::connectorName(AtomBios::connectorType(paths[i].connectorObjId));

			AtomBios::PathRecords rec {};
			atomBios.getPathRecords(paths[i], rec);

			char entry[48];
			snprintf(entry, sizeof(entry), "%s%s/ddc%u/hpd%u",
			         i ? "," : "", conn, rec.ddcLine, rec.hpdPin);
			strlcat(list, entry, sizeof(list));

			FBLOG("connector %zu: %s objid=0x%04x encoder=0x%04x ddc-line=%u hpd-pin=%u", i,
			      conn, paths[i].connectorObjId, paths[i].encoderObjId, rec.ddcLine, rec.hpdPin);
		}
		setProperty("AtomBIOS,Connectors", list);
		setProperty("AtomBIOS,ConnectorCount", static_cast<uint64_t>(n), 32);
	}
}

bool RDNA4FB::loadVBIOS() {
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
		setProperty("Discovery,Source", "VBIOS image");
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

	// Command-function inventory for the mode-setting plan: amdgpu drives the
	// PHY/PLL through these bytecode routines rather than raw registers.
	static const struct { uint8_t idx; const char *name; } cmds[] = {
		{ AtomBios::CmdSetPixelClock,          "setpixelclock" },
		{ AtomBios::CmdDig1TransmitterControl, "dig1transmittercontrol" },
		{ AtomBios::CmdDigEncoderControl,      "digxencodercontrol" },
		{ AtomBios::CmdEnableCrtc,             "enablecrtc" },
		{ AtomBios::CmdSetCrtcUsingDtdTiming,  "setcrtc_usingdtdtiming" },
		{ AtomBios::CmdEnableDispPowerGating,  "enabledisppowergating" },
	};
	for (auto &c : cmds) {
		AtomBios::CmdTableInfo info;
		if (atomBios.getCommandTable(c.idx, info))
			FBLOG("cmd: %s v%u.%u %u bytes at +0x%zx", c.name,
			      info.formatRev, info.contentRev, info.size,
			      info.offset - atomBios.imageOffset());
		else
			FBLOG("cmd: %s absent", c.name);
	}
	return true;
}

bool RDNA4FB::loadOnDieDiscovery() {
	// The PSP keeps a copy of the IP discovery binary in a reserved region
	// at the top of VRAM (upstream: DISCOVERY_TMR_OFFSET = 64 KiB below the
	// end, DISCOVERY_TMR_SIZE = 10 KiB) — present on every powered-on card,
	// no ROM dump needed. It sits far outside the CPU aperture, so read it
	// through MM_INDEX/MM_DATA. Technique per lemonade-sdk/mac-amdgpu
	// (MIT) amdgpu_discovery.cpp and upstream amdgpu_discovery.c.
	constexpr uint32_t kTmrSize   = 10 << 10;
	constexpr uint32_t kTmrOffset = 64 << 10;
	constexpr uint32_t kMemsizeFallback = 0x378c; // NBIF RCC_CONFIG_MEMSIZE

	if (!rmmio)
		return false;
	uint32_t vramMB = regRead32(kMemsizeFallback);
	if (vramMB == 0 || vramMB == 0xFFFFFFFF) {
		FBLOG("discovery: on-die: VRAM size unreadable (0x%08x)", vramMB);
		return false;
	}

	onDieDisc = static_cast<uint8_t *>(IOMalloc(kTmrSize));
	if (!onDieDisc)
		return false;

	uint64_t pos = (static_cast<uint64_t>(vramMB) << 20) - kTmrOffset;
	uint32_t *dw = reinterpret_cast<uint32_t *>(onDieDisc);
	for (uint32_t i = 0; i < kTmrSize / 4; i++)
		dw[i] = vramRead32(pos + 4ULL * i);

	FBLOG("discovery: on-die TMR at vram+0x%llx, first dwords %08x %08x",
	      pos, dw[0], dw[1]);
	if (!ipDiscovery.init(onDieDisc, kTmrSize)) {
		FBLOG("discovery: on-die TMR did not validate");
		IOFree(onDieDisc, kTmrSize);
		onDieDisc = nullptr;
		return false;
	}

	FBLOG("discovery: on-die binary valid, %u IPs (no ROM injection needed)",
	      ipDiscovery.ipCount());
	IpDiscovery::IpEntry gc, dmu;
	char ver[16];
	if (ipDiscovery.findIp(IpDiscovery::HwGc, 0, gc)) {
		snprintf(ver, sizeof(ver), "%u.%u.%u", gc.major, gc.minor, gc.revision);
		setProperty("Discovery,GCVersion", ver);
	}
	if (ipDiscovery.findIp(IpDiscovery::HwDmu, 0, dmu)) {
		snprintf(ver, sizeof(ver), "%u.%u.%u", dmu.major, dmu.minor, dmu.revision);
		setProperty("Discovery,DCNVersion", ver);
	}
	setProperty("Discovery,Source", "on-die TMR");
	return true;
}

// ---------------------------------------------------------------------------
// Register MMIO (BAR5)
// ---------------------------------------------------------------------------

bool RDNA4FB::mapRegisters() {
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

void RDNA4FB::unmapRegisters() {
	rmmio = nullptr;
	rmmioSize = 0;
	if (rmmioMap) {
		rmmioMap->release();
		rmmioMap = nullptr;
	}
}

uint32_t RDNA4FB::regRead32(uint32_t byteOffset) const {
	if (!rmmio || byteOffset + 4 > rmmioSize)
		return 0xFFFFFFFF;
	return rmmio[byteOffset / 4];
}

uint32_t RDNA4FB::regReadDmu(uint8_t baseIdx, uint32_t dwordOffset) const {
	uint32_t byteOffset;
	if (!ipDiscovery.isValid() ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwDmu, 0, baseIdx, dwordOffset, byteOffset))
		return 0xFFFFFFFF;
	return regRead32(byteOffset);
}

void RDNA4FB::dumpDCN() {
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

	// MPC MCM blocks (shaper -> 3DLUT -> 1DLUT), the post-CSC colour stages
	// not covered by earlier dumps. An enabled 3DLUT with unloaded RAM would
	// produce exactly the observed spatially-perfect arbitrary recolouring.
	static const uint32_t mcm[4][3] = {  // {SHAPER_CONTROL, 3DLUT_MODE, 1DLUT_CONTROL}, base 3
		{ 0x0453, 0x048a, 0x0493 },
		{ 0x0503, 0x053a, 0x0543 },
		{ 0x05b3, 0x05ea, 0x05f3 },
		{ 0x0663, 0x069a, 0x06a3 },
	};
	for (int i = 0; i < 4; i++)
		FBLOG("dcn:   MCM%d shaper=0x%08x 3dlut_mode=0x%08x 1dlut=0x%08x",
		      i, regReadDmu(3, mcm[i][0]), regReadDmu(3, mcm[i][1]),
		      regReadDmu(3, mcm[i][2]));
	FBLOG("dcn: --- end register dump ---");

	// Boot-arg "rdna4-lutbypass=1": force all MCM stages to bypass.
	// Bypass is the hardware's pass-through state, so this cannot make the
	// image worse than a wrong LUT; a reboot restores firmware state.
	uint32_t fix = 0;
	if (PE_parse_boot_argn("rdna4-lutbypass", &fix, sizeof(fix)) && fix != 0) {
		for (int i = 0; i < 4; i++) {
			regWriteDmu(3, mcm[i][0], 0);  // shaper: bypass
			regWriteDmu(3, mcm[i][1], 0);  // 3DLUT: bypass
			regWriteDmu(3, mcm[i][2], 0);  // 1DLUT: bypass
		}
		FBLOG("dcn: MCM shaper/3DLUT/1DLUT forced to bypass on all pipes");
	}
}

// ---------------------------------------------------------------------------
// DP AUX software engine (EDID / DDC over the AUX channel)
// ---------------------------------------------------------------------------

namespace {
// All AUX registers live in the DMU IP at base_idx 2. Engine `n` sits at
// kAuxBase0 + n*kAuxStride; the members below are dword offsets within one
// engine's block (regDP_AUXn_* from dcn_4_1_0_offset.h).
constexpr uint32_t kAuxBase0      = 0x16b2;  // regDP_AUX0_AUX_CONTROL
constexpr uint32_t kAuxStride     = 0x1c;    // dwords between DP_AUXn blocks
constexpr uint32_t kAuxControl    = 0x0;
constexpr uint32_t kAuxSwControl  = 0x1;
constexpr uint32_t kAuxArbControl = 0x2;
constexpr uint32_t kAuxIntControl = 0x3;
constexpr uint32_t kAuxSwStatus   = 0x4;
constexpr uint32_t kAuxSwData     = 0x6;

// Field masks/shifts (dcn_3_2_0_sh_mask.h; identical layout on dcn_4_1_0).
constexpr uint32_t kAuxEn                = 1u << 0;   // AUX_CONTROL.AUX_EN
constexpr uint32_t kAuxSwGo              = 1u << 0;   // AUX_SW_CONTROL.AUX_SW_GO
constexpr uint32_t kAuxWrBytesShift      = 16;        // AUX_SW_WR_BYTES [20:16]
constexpr uint32_t kAuxWrBytesMask       = 0x1fu << 16;
constexpr uint32_t kAuxRwStatShift       = 2;         // AUX_REG_RW_CNTL_STATUS [3:2]
constexpr uint32_t kAuxRwStatMask        = 0x3u << 2;
constexpr uint32_t kAuxUseReq            = 1u << 16;  // AUX_SW_USE_AUX_REG_REQ
constexpr uint32_t kAuxDoneUsingReg      = 1u << 17;  // AUX_SW_DONE_USING_AUX_REG
constexpr uint32_t kAuxDoneAck           = 1u << 1;   // INTERRUPT_CONTROL.AUX_SW_DONE_ACK
constexpr uint32_t kAuxSwDone            = 1u << 0;   // AUX_SW_STATUS.AUX_SW_DONE
constexpr uint32_t kAuxRxTimeoutState    = 0x7u << 4; // [6:4]
constexpr uint32_t kAuxRxTimeout         = 1u << 7;
constexpr uint32_t kAuxHpdDiscon         = 1u << 9;
constexpr uint32_t kAuxReplyCountShift   = 24;        // AUX_SW_REPLY_BYTE_COUNT [28:24]
constexpr uint32_t kAuxReplyCountMask    = 0x1fu << 24;
constexpr uint32_t kAuxDataRw            = 1u << 0;   // AUX_SW_DATA.AUX_SW_DATA_RW (1=read)
constexpr uint32_t kAuxDataShift         = 8;         // AUX_SW_DATA.AUX_SW_DATA [15:8]
constexpr uint32_t kAuxDataMask          = 0xffu << 8;
constexpr uint32_t kAuxAutoincDisable    = 1u << 31;  // AUX_SW_AUTOINCREMENT_DISABLE

// enum aux_transaction_action — already aligned into the command high nibble.
constexpr uint8_t kActI2CWriteMot = 0x40;
constexpr uint8_t kActI2CReadMot  = 0x50;
constexpr uint8_t kActI2CRead     = 0x10;
constexpr uint8_t kActDpWrite     = 0x80;  // native AUX (DPCD) write

// AUX_REG_RW_CNTL_STATUS grant codes.
constexpr uint32_t kSwCanAccess   = 1;
constexpr uint32_t kDmcuCanAccess = 2;

// AUX reply nibble (first reply byte >> 4).
constexpr int kReplyAck      = 0x0;  // AUX ACK + I2C ACK
constexpr int kReplyAuxDefer = 0x2;
constexpr int kReplyI2CDefer = 0x8;

constexpr uint8_t kDdcSlave  = 0x50; // VESA DDC/EDID I2C address
constexpr uint8_t kAuxRetry  = 7;    // per-transaction defer retries
} // namespace

uint32_t RDNA4FB::auxDword(uint8_t inst, uint32_t reg) const {
	return kAuxBase0 + static_cast<uint32_t>(inst) * kAuxStride + reg;
}

int RDNA4FB::auxTransaction(uint8_t inst, uint8_t action, uint32_t address,
                               const uint8_t *data, uint8_t len,
                               uint8_t *reply, uint8_t replyCap,
                               uint8_t *replyBytes) {
	if (replyBytes) *replyBytes = 0;
	if (!ipDiscovery.isValid() || !rmmio)
		return -1;

	const uint32_t rCtl = auxDword(inst, kAuxControl);
	const uint32_t rArb = auxDword(inst, kAuxArbControl);
	const uint32_t rInt = auxDword(inst, kAuxIntControl);
	const uint32_t rSwc = auxDword(inst, kAuxSwControl);
	const uint32_t rSts = auxDword(inst, kAuxSwStatus);
	const uint32_t rDat = auxDword(inst, kAuxSwData);

	// Release helper: hand the engine back (mirrors dce_aux release_engine).
	auto release = [&]() {
		regWriteDmu(2, rArb,
		            (regReadDmu(2, rArb) & ~kAuxUseReq) | kAuxDoneUsingReg);
	};

	// --- acquire software access (dce_aux acquire_engine) ---
	uint32_t arb = regReadDmu(2, rArb);
	if (arb == 0xFFFFFFFF)
		return -1;
	if (((arb & kAuxRwStatMask) >> kAuxRwStatShift) == kDmcuCanAccess)
		return -1;  // the firmware microcontroller owns this engine
	uint32_t ctl = regReadDmu(2, rCtl);
	if (!(ctl & kAuxEn))                       // GOP normally leaves AUX enabled
		regWriteDmu(2, rCtl, ctl | kAuxEn);
	regWriteDmu(2, rArb, regReadDmu(2, rArb) | kAuxUseReq);
	arb = regReadDmu(2, rArb);
	if (((arb & kAuxRwStatMask) >> kAuxRwStatShift) != kSwCanAccess) {
		release();
		return -1;
	}

	// --- clear any stale completion, wait for the engine to be idle ---
	regWriteDmu(2, rInt, kAuxDoneAck);
	for (int i = 0; i < 100 && (regReadDmu(2, rSts) & kAuxSwDone); i++)
		IODelay(10);

	// --- build the request buffer (submit_channel_request) ---
	// header = 3 bytes (action+addr[19:0]); a data phase adds a length byte,
	// and writes append the payload. AUX_SW_WR_BYTES counts them all.
	// Command nibble bit 0 (byte bit 0x10) is the read flag: write actions
	// (I2C_WRITE 0x00, I2C_WRITE_MOT 0x40, DP_WRITE 0x80) clear it; reads
	// (I2C_READ 0x10, I2C_READ_MOT 0x50, DP_READ 0x90) set it.
	const bool isWrite = (action & 0x10) == 0;
	uint32_t length = len ? 4u : 3u;
	if (isWrite) length += len;
	regWriteDmu(2, rSwc,
	            (regReadDmu(2, rSwc) & ~kAuxWrBytesMask) |
	            ((length << kAuxWrBytesShift) & kAuxWrBytesMask));

	// FIFO writes: INDEX=0, RW=0. The first byte latches index 0 with
	// autoincrement disabled; clearing it for the rest advances the pointer.
	uint32_t v = 0;
	auto push = [&](bool first, uint8_t b) {
		v &= ~(kAuxDataMask | kAuxAutoincDisable | kAuxDataRw);
		v |= (static_cast<uint32_t>(b) << kAuxDataShift) & kAuxDataMask;
		if (first) v |= kAuxAutoincDisable;
		regWriteDmu(2, rDat, v);
	};
	push(true,  static_cast<uint8_t>(action | ((address >> 16) & 0x0f)));
	push(false, static_cast<uint8_t>((address >> 8) & 0xff));
	push(false, static_cast<uint8_t>(address & 0xff));
	if (len) push(false, static_cast<uint8_t>(len - 1));
	if (isWrite)
		for (uint8_t i = 0; i < len; i++)
			push(false, data ? data[i] : 0);

	// --- go ---
	regWriteDmu(2, rSwc, regReadDmu(2, rSwc) | kAuxSwGo);

	// --- wait for completion (get_channel_status) ---
	uint32_t sts = 0;
	bool done = false;
	for (int i = 0; i < 2000; i++) {   // ~20 ms ceiling; typically microseconds
		sts = regReadDmu(2, rSts);
		if (sts & kAuxSwDone) { done = true; break; }
		IODelay(10);
	}
	if (!done || (sts & kAuxHpdDiscon) ||
	    (sts & kAuxRxTimeout) || (sts & kAuxRxTimeoutState)) {
		release();
		return -1;
	}

	uint32_t nbytes = (sts & kAuxReplyCountMask) >> kAuxReplyCountShift;

	// --- read the reply (read_channel_reply) ---
	int replyCode = -1;
	if (nbytes >= 1) {
		// point the read cursor at byte 0; reads auto-advance from there
		regWriteDmu(2, rDat, kAuxAutoincDisable | kAuxDataRw);  // INDEX=0, RW=1
		uint32_t hdr = (regReadDmu(2, rDat) & kAuxDataMask) >> kAuxDataShift;
		replyCode = (hdr >> 4) & 0x0f;           // first byte is the reply header
		uint32_t dataBytes = nbytes - 1;
		for (uint32_t i = 0; i < dataBytes; i++) {
			uint32_t d = (regReadDmu(2, rDat) & kAuxDataMask) >> kAuxDataShift;
			if (reply && i < replyCap) reply[i] = static_cast<uint8_t>(d);
		}
		if (replyBytes)
			*replyBytes = static_cast<uint8_t>(dataBytes < replyCap ? dataBytes
			                                                        : replyCap);
	}

	release();
	return replyCode;
}

bool RDNA4FB::readEDID(uint8_t inst, uint8_t *edid, size_t count,
                          uint8_t start) {
	uint8_t rb = 0;

	// Point the sink's read pointer at `start` with an I2C write. MOT keeps
	// the I2C START asserted across the reads that follow.
	bool ok = false;
	for (uint8_t t = 0; t < kAuxRetry; t++) {
		int rc = auxTransaction(inst, kActI2CWriteMot, kDdcSlave, &start, 1,
		                        nullptr, 0, &rb);
		if (rc == kReplyAck) { ok = true; break; }
		if (rc < 0) return false;
		if (rc == kReplyAuxDefer || rc == kReplyI2CDefer) { IODelay(500); continue; }
		return false;  // NACK
	}
	if (!ok) return false;

	// Sequential 16-byte I2C reads (the AUX data FIFO limit); drop MOT on the
	// final chunk to issue the I2C STOP.
	for (size_t pos = 0; pos < count; ) {
		uint8_t chunk = (count - pos) > 16 ? 16 : static_cast<uint8_t>(count - pos);
		bool last = (pos + chunk) >= count;
		uint8_t act = last ? kActI2CRead : kActI2CReadMot;
		bool got = false;
		for (uint8_t t = 0; t < kAuxRetry; t++) {
			int rc = auxTransaction(inst, act, kDdcSlave, nullptr, chunk,
			                        edid + pos, chunk, &rb);
			if (rc == kReplyAck) { got = true; break; }
			if (rc < 0) return false;
			if (rc == kReplyAuxDefer || rc == kReplyI2CDefer) { IODelay(500); continue; }
			return false;
		}
		if (!got || rb == 0) return false;
		pos += rb;  // advance by what the sink actually returned
	}
	return true;
}

void RDNA4FB::probeEDID() {
	// Default-on since the AUX path was verified on hardware (2026-07-11);
	// "rdna4-noedid=1" opts out if a sink misbehaves.
	uint32_t noedid = 0;
	if (PE_parse_boot_argn("rdna4-noedid", &noedid, sizeof(noedid)) && noedid != 0) {
		FBLOG("edid: probing disabled by rdna4-noedid");
		return;
	}
	if (!ipDiscovery.isValid() || !rmmio) {
		FBLOG("edid: MMIO/discovery unavailable, skipping");
		return;
	}

	// Hot-plug detect pin states (DC_GPIO_HPD_Y, hpd1-4 at bits 0/8/16/24):
	// distinguishes "no monitor asserting presence" from engine failures.
	uint32_t hpdY = regReadDmu(2, 0x28f7);
	FBLOG("edid: HPD_Y=0x%08x (hpd1=%u hpd2=%u hpd3=%u hpd4=%u)", hpdY,
	      (hpdY >> 0) & 1, (hpdY >> 8) & 1, (hpdY >> 16) & 1, (hpdY >> 24) & 1);

	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = atomBios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	bool any = false;
	for (size_t i = 0; i < n; i++) {
		AtomBios::ConnectorType ct =
		    AtomBios::connectorType(paths[i].connectorObjId);
		// DisplayPort/USB-C sinks carry DDC on the AUX channel; HDMI/DVI
		// EDID travels over the DC_I2C hardware engine.
		bool viaAux = (ct == AtomBios::ConnectorDP || ct == AtomBios::ConnectorUSBC);
		bool viaI2c = (ct == AtomBios::ConnectorHDMIA || ct == AtomBios::ConnectorDVID);
		if (!viaAux && !viaI2c)
			continue;

		AtomBios::PathRecords rec {};
		atomBios.getPathRecords(paths[i], rec);
		uint8_t inst = rec.ddcLine;  // AUX engine / DDC line index
		const char *bus = viaAux ? "AUX" : "DDC";

		uint8_t edid[128] {};
		bool got = viaAux ? readEDID(inst, edid, sizeof(edid), 0)
		                  : readEDIDI2C(inst, edid, sizeof(edid), 0);
		if (!got) {
			FBLOG("edid: connector %zu (%s%u): no reply", i, bus, inst);
			continue;
		}

		static const uint8_t sig[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
		if (memcmp(edid, sig, sizeof(sig)) != 0) {
			FBLOG("edid: connector %zu (%s%u): data but bad header "
			      "%02x %02x %02x %02x", i, bus, inst, edid[0], edid[1], edid[2], edid[3]);
			continue;
		}
		any = true;

		// Manufacturer id: bytes 8-9 big-endian, three 5-bit letters (1=A).
		uint16_t m = static_cast<uint16_t>((edid[8] << 8) | edid[9]);
		char mfg[4] = {
			static_cast<char>('@' + ((m >> 10) & 0x1f)),
			static_cast<char>('@' + ((m >> 5) & 0x1f)),
			static_cast<char>('@' + (m & 0x1f)), 0 };
		uint16_t product = static_cast<uint16_t>(edid[10] | (edid[11] << 8));
		FBLOG("edid: connector %zu (%s%u): %s product 0x%04x EDID %u.%u",
		      i, bus, inst, mfg, product, edid[18], edid[19]);

		// Preferred timing, fully decoded: these are the raster numbers an
		// OTG would be programmed with to drive this sink.
		Edid::DetailedTiming t {};
		if (Edid::parseDetailedTiming(edid + 54, t)) {
			FBLOG("edid: connector %zu (%s%u): preferred %ux%u@%u.%03uHz "
			      "pclk=%ukHz h(blank=%u fp=%u sw=%u %c) v(blank=%u fp=%u sw=%u %c)%s",
			      i, bus, inst, t.hActive, t.vActive,
			      t.refreshMilliHz() / 1000, t.refreshMilliHz() % 1000,
			      t.pixelClockKHz,
			      t.hBlank, t.hSyncOffset, t.hSyncWidth, t.hSyncPositive ? '+' : '-',
			      t.vBlank, t.vSyncOffset, t.vSyncWidth, t.vSyncPositive ? '+' : '-',
			      t.interlaced ? " interlaced" : "");
		}

		char key[40];
		snprintf(key, sizeof(key), "EDID,%s%u", bus, inst);
		setProperty(key, edid, sizeof(edid));
		snprintf(key, sizeof(key), "EDID,%s%u-Vendor", bus, inst);
		setProperty(key, mfg);

		// Cache the first sink's EDID for hasDDCConnect()/getDDCBlock(). Only
		// the boot display is scanned out, and that is AUX0/DP0 on this board.
		if (edidLen == 0) {
			memcpy(edidData, edid, 128);
			edidLen = 128;
			if (viaAux) {   // DPCD power writes only exist on AUX sinks
				sinkAuxInst = inst;
				sinkAuxValid = true;
			}
			// One CTA extension block is the norm on EDID 1.4 sinks; fetch it
			// so the OS sees the full timing/audio capabilities.
			bool ext = edid[126] > 0 &&
			           (viaAux ? readEDID(inst, edidData + 128, 128, 128)
			                   : readEDIDI2C(inst, edidData + 128, 128, 128));
			if (ext) {
				edidLen = 256;
				FBLOG("edid: connector %zu (%s%u): read extension block "
				      "(tag 0x%02x)", i, bus, inst, edidData[128]);
			}
		}
	}
	if (!any)
		FBLOG("edid: no sink EDID read on any connector");
}

namespace {
// DC_I2C hardware engine (dcn_4_1_0_offset.h, all base_idx 2). One shared
// engine; per-DDC-line SETUP/SPEED registers plus a DDC_SELECT mux.
constexpr uint32_t kI2cControl     = 0x1e98;
constexpr uint32_t kI2cArbitration = 0x1e99;
constexpr uint32_t kI2cSwStatus    = 0x1e9b;
constexpr uint32_t kI2cSpeedBase   = 0x1ea2;  // + 2*line
constexpr uint32_t kI2cSetupBase   = 0x1ea3;  // + 2*line
constexpr uint32_t kI2cTxn0        = 0x1eae;  // txn N at +N
constexpr uint32_t kI2cDataReg     = 0x1eb2;

// DC_I2C_CONTROL fields.
constexpr uint32_t kI2cGo             = 1u << 0;
constexpr uint32_t kI2cSoftReset      = 1u << 1;
constexpr uint32_t kI2cSwStatusReset  = 1u << 3;
constexpr uint32_t kI2cDdcSelectShift = 8;       // [10:8]
constexpr uint32_t kI2cTxnCountShift  = 20;      // [21:20]
// DC_I2C_ARBITRATION fields.
constexpr uint32_t kI2cRwStatusShift  = 2;       // [3:2]: 0 idle, 1 SW, 2 HW
constexpr uint32_t kI2cRwStatusMask   = 0x3u << 2;
constexpr uint32_t kI2cNoQueuedSwGo   = 1u << 4;
constexpr uint32_t kI2cSwUseReq       = 1u << 20;
constexpr uint32_t kI2cSwDoneUsing    = 1u << 21;
// DC_I2C_SW_STATUS fields.
constexpr uint32_t kI2cSwDone         = 1u << 2;
constexpr uint32_t kI2cSwAborted      = 1u << 4;
constexpr uint32_t kI2cSwTimeout      = 1u << 5;
constexpr uint32_t kI2cSwOverflow     = 1u << 7;
constexpr uint32_t kI2cStoppedOnNack  = 1u << 8;
// DC_I2C_DDCx_SETUP fields (DCN values: TIME_LIMIT 3, 9-bit send-reset).
constexpr uint32_t kI2cSetupClkEn     = 1u << 3;   // dcn4.1: engine clock gate
constexpr uint32_t kI2cSetupEnable    = 1u << 6;
constexpr uint32_t kI2cSetupValue     = kI2cSetupEnable | kI2cSetupClkEn |
                                        (1u << 2) | (3u << 24);
// DIO memory low-power control: I2C engine RAM must be out of light sleep
// before the engine responds (DIO_MEM_PWR_CTRL/STATUS bit 0).
constexpr uint32_t kDioMemPwrStatus   = 0x1edd;
constexpr uint32_t kDioMemPwrCtrl     = 0x1ede;
// DC_I2C_DDCx_SPEED fields.
constexpr uint32_t kI2cSpeed100kHz    = 100;     // kHz, amdgpu's DCN default
// DC_I2C_TRANSACTIONx fields.
constexpr uint32_t kTxnRead           = 1u << 0;
constexpr uint32_t kTxnStopOnNack     = 1u << 8;
constexpr uint32_t kTxnStart          = 1u << 12;
constexpr uint32_t kTxnStop           = 1u << 13;
constexpr uint32_t kTxnCountShift     = 16;      // [25:16]
// DC_I2C_DATA fields.
constexpr uint32_t kI2cDataRead       = 1u << 0;
constexpr uint32_t kI2cDataShiftI2C   = 8;       // [15:8]
constexpr uint32_t kI2cIndexShift     = 16;      // [25:16]
constexpr uint32_t kI2cIndexWrite     = 1u << 31;

// MICROSECOND_TIME_BASE_DIV (DMU base_idx 1): reference for SCL prescale.
constexpr uint32_t kMicrosecondTimeBaseDiv = 0x007b;
} // namespace

bool RDNA4FB::readEDIDI2C(uint8_t line, uint8_t *edid, size_t count,
                             uint8_t start) {
	if (!ipDiscovery.isValid() || !rmmio || count == 0 || count > 256)
		return false;

	const uint32_t rSetup = kI2cSetupBase + 2u * line;
	const uint32_t rSpeed = kI2cSpeedBase + 2u * line;

	// --- wake the engine BEFORE acquiring (dce_i2c_hw setup_engine order).
	// A never-used engine sits in soft reset with its registers write-blocked
	// (arbitration reads 0 and ignores requests — observed on hardware), and
	// its RAM may be in light sleep.
	regWriteDmu(2, kI2cControl, 0);                 // deassert DC_I2C_SOFT_RESET
	uint32_t memPwr = regReadDmu(2, kDioMemPwrCtrl);
	if (memPwr != 0xFFFFFFFF && (memPwr & 1)) {
		regWriteDmu(2, kDioMemPwrCtrl, memPwr & ~1u);  // unforce light sleep
		for (int i = 0; i < 10 && (regReadDmu(2, kDioMemPwrStatus) & 1); i++)
			IODelay(1);
	}
	regWriteDmu(2, rSetup, regReadDmu(2, rSetup) | kI2cSetupClkEn);

	// --- acquire (dce_i2c_hw acquire_engine) ---
	uint32_t arb = regReadDmu(2, kI2cArbitration);
	if (arb == 0xFFFFFFFF)
		return false;
	uint32_t owner = (arb & kI2cRwStatusMask) >> kI2cRwStatusShift;
	if (owner == 2) {        // hardware/DMCU owns the engine
		FBLOG("i2c: line %u: engine owned by HW/DMCU (arb=0x%08x)", line, arb);
		return false;
	}
	if (owner != 1) {
		regWriteDmu(2, kI2cArbitration, arb | kI2cSwUseReq);
		arb = regReadDmu(2, kI2cArbitration);
		if (((arb & kI2cRwStatusMask) >> kI2cRwStatusShift) != 1) {
			FBLOG("i2c: line %u: SW acquire not granted (arb=0x%08x)", line, arb);
			return false;
		}
	}
	auto release = [&]() {
		// Soft reset is safe while SW owns the engine; DONE_USING clears
		// the SW request and hands the engine back.
		regWriteDmu(2, kI2cControl, kI2cSoftReset | kI2cSwStatusReset);
		regWriteDmu(2, rSetup, 0);
		regWriteDmu(2, kI2cArbitration,
		            regReadDmu(2, kI2cArbitration) | kI2cSwDoneUsing);
	};

	// --- engine setup (setup_engine) ---
	regWriteDmu(2, rSetup, kI2cSetupValue);
	regWriteDmu(2, kI2cControl,
	            kI2cSwStatusReset | (static_cast<uint32_t>(line) << kI2cDdcSelectShift));
	regWriteDmu(2, kI2cArbitration,
	            (regReadDmu(2, kI2cArbitration) | kI2cSwUseReq) & ~kI2cNoQueuedSwGo);

	// SCL speed: prescale from the microsecond time base (set_speed). If the
	// time base is unprogrammed, keep whatever speed the firmware left.
	uint32_t mtb = regReadDmu(1, kMicrosecondTimeBaseDiv);
	uint32_t refBase = mtb & 0x7f, xtalDiv = (mtb >> 8) & 0x7f;
	if (mtb != 0xFFFFFFFF && refBase != 0) {
		if (xtalDiv == 0)
			xtalDiv = 2;
		uint32_t prescale = (refBase * 1000 / xtalDiv) / kI2cSpeed100kHz;
		regWriteDmu(2, rSpeed, (prescale << 16) | (2u << 8) | 2u);
	}

	// --- queue both transactions, then a single GO (process_transaction /
	// execute_transaction): [START 0xA0 <start>] [START 0xA1 read*count STOP]
	regWriteDmu(2, kI2cTxn0,
	            kTxnStopOnNack | kTxnStart | (1u << kTxnCountShift));
	regWriteDmu(2, kI2cTxn0 + 1,
	            kTxnStopOnNack | kTxnStart | kTxnRead | kTxnStop |
	            (static_cast<uint32_t>(count) << kTxnCountShift));
	// Data FIFO: address bytes carry the R/W bit in bit 0.
	regWriteDmu(2, kI2cDataReg, kI2cIndexWrite | (0xA0u << kI2cDataShiftI2C));
	regWriteDmu(2, kI2cDataReg, static_cast<uint32_t>(start) << kI2cDataShiftI2C);
	regWriteDmu(2, kI2cDataReg, 0xA1u << kI2cDataShiftI2C);

	uint32_t ctl = (static_cast<uint32_t>(line) << kI2cDdcSelectShift) |
	               (1u << kI2cTxnCountShift);  // TRANSACTION_COUNT = 2-1
	regWriteDmu(2, kI2cControl, ctl);
	regWriteDmu(2, kI2cControl, ctl | kI2cGo);

	// --- poll for completion. 129 bytes at 100 kHz is ~12 ms; allow 100 ms.
	uint32_t sts = 0;
	bool done = false;
	for (int i = 0; i < 10000; i++) {
		sts = regReadDmu(2, kI2cSwStatus);
		if (sts & (kI2cStoppedOnNack | kI2cSwTimeout | kI2cSwAborted | kI2cSwOverflow))
			break;
		if (sts & kI2cSwDone) { done = true; break; }
		IODelay(10);
	}
	if (!done || (sts & (kI2cStoppedOnNack | kI2cSwTimeout | kI2cSwAborted | kI2cSwOverflow))) {
		FBLOG("i2c: line %u: transaction failed (sw_status=0x%08x%s)", line, sts,
		      (sts & kI2cStoppedOnNack) ? ", NACK" : !done ? ", poll timeout" : "");
		release();
		return false;
	}

	// --- read the reply FIFO (process_channel_reply): the read data starts
	// after the 3 bytes we wrote (0xA0, start, 0xA1).
	regWriteDmu(2, kI2cDataReg,
	            kI2cIndexWrite | kI2cDataRead | (3u << kI2cIndexShift));
	for (size_t i = 0; i < count; i++)
		edid[i] = static_cast<uint8_t>(
		    (regReadDmu(2, kI2cDataReg) >> kI2cDataShiftI2C) & 0xff);

	release();
	return true;
}

void RDNA4FB::dumpModeState() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-modedump", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio)
		return;

	// dcn_4_1_0_offset.h. OTG stride 0x80, DIG stride 0x124, HUBPREQ stride
	// 0xdc, all base_idx 2; DCCG per-OTG pixel-rate regs are base_idx 1.
	FBLOG("mode: --- mode-setting register survey (read-only) ---");
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t o = i * 0x80;
		FBLOG("mode: OTG%u ctl=0x%08x master_en=0x%08x h_total=0x%08x "
		      "h_blank=0x%08x h_sync=0x%08x v_total=0x%08x v_blank=0x%08x "
		      "v_sync=0x%08x", i,
		      regReadDmu(2, 0x1b43 + o), regReadDmu(2, 0x1b5d + o),
		      regReadDmu(2, 0x1b2a + o), regReadDmu(2, 0x1b2b + o),
		      regReadDmu(2, 0x1b2c + o), regReadDmu(2, 0x1b2f + o),
		      regReadDmu(2, 0x1b38 + o), regReadDmu(2, 0x1b39 + o));
	}
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t d = i * 0x124;
		FBLOG("mode: DIG%u fe_cntl=0x%08x fe_clk=0x%08x fe_en=0x%08x "
		      "hdmi_ctl=0x%08x be_cntl=0x%08x be_clk=0x%08x be_en=0x%08x "
		      "tmds=0x%08x", i,
		      regReadDmu(2, 0x2093 + d), regReadDmu(2, 0x2094 + d),
		      regReadDmu(2, 0x2095 + d), regReadDmu(2, 0x209e + d),
		      regReadDmu(2, 0x20bc + d), regReadDmu(2, 0x20bb + d),
		      regReadDmu(2, 0x20bd + d), regReadDmu(2, 0x20e4 + d));
	}
	FBLOG("mode: DCCG dp_dto phase/modulo = [0x%08x/0x%08x 0x%08x/0x%08x "
	      "0x%08x/0x%08x 0x%08x/0x%08x]",
	      regReadDmu(1, 0x0081), regReadDmu(1, 0x0082),
	      regReadDmu(1, 0x0085), regReadDmu(1, 0x0086),
	      regReadDmu(1, 0x0089), regReadDmu(1, 0x008a),
	      regReadDmu(1, 0x008d), regReadDmu(1, 0x008e));
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t h = i * 0xdc;
		FBLOG("mode: HUBP%u pitch=0x%08x addr=0x%08x addr_hi=0x%08x", i,
		      regReadDmu(2, 0x0607 + h), regReadDmu(2, 0x060a + h),
		      regReadDmu(2, 0x060b + h));
	}
	FBLOG("mode: DCCG otg_pixel_rate=[0x%08x 0x%08x 0x%08x 0x%08x]",
	      regReadDmu(1, 0x0080), regReadDmu(1, 0x0084),
	      regReadDmu(1, 0x0088), regReadDmu(1, 0x008c));
	FBLOG("mode: DCCG dtbclk_p=0x%08x symclk32_se=0x%08x dpstreamclk=0x%08x "
	      "phyasymclk=0x%08x gate_disable=0x%08x",
	      regReadDmu(1, 0x0068), regReadDmu(1, 0x0065),
	      regReadDmu(1, 0x004a), regReadDmu(1, 0x0052),
	      regReadDmu(1, 0x0074));
	FBLOG("mode: hdmicharclk=[0x%08x 0x%08x 0x%08x 0x%08x]",
	      regReadDmu(2, 0x004a), regReadDmu(2, 0x004b),
	      regReadDmu(2, 0x004c), regReadDmu(2, 0x004d));
	// DMUB display microcontroller: the VBIOS ships no display command
	// tables (verified via make test), so PHY/PLL work on DCN4 goes through
	// DMUB mailbox commands — IF a firmware is loaded and running. Nonzero
	// CNTL enable / inbox base+pointers / scratch means the GOP left DMUB
	// alive and route (a) is viable; all-zero means route (b) (register-level
	// PHY bring-up from lit-pipe diffs).
	FBLOG("mode: DMCUB cntl=0x%08x cntl2=0x%08x inbox1 base=0x%08x size=0x%08x "
	      "wptr=0x%08x rptr=0x%08x scratch0=0x%08x",
	      regReadDmu(2, 0x01f6), regReadDmu(2, 0x0200),
	      regReadDmu(2, 0x01d4), regReadDmu(2, 0x01d5),
	      regReadDmu(2, 0x01d6), regReadDmu(2, 0x01d7),
	      regReadDmu(2, 0x01e3));
	FBLOG("mode: --- end survey ---");
}

// ---------------------------------------------------------------------------
// DMUB mailbox (display firmware)
// ---------------------------------------------------------------------------

// Indirect VRAM access through MM_INDEX/MM_DATA (BIF_BX_PF0, BAR5 bytes
// 0x0 / 0x4 / 0x18) — amdgpu_device_mm_access. Reaches any VRAM byte
// through the register BAR; the way to touch the DMUB ring in top-of-VRAM
// firmware memory that the 256 MiB CPU aperture cannot map.
uint32_t RDNA4FB::vramRead32(uint64_t pos) {
	rmmio[0x18 / 4] = static_cast<uint32_t>(pos >> 31);
	rmmio[0]        = (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u;
	return rmmio[1];
}

void RDNA4FB::vramWrite32(uint64_t pos, uint32_t value) {
	rmmio[0x18 / 4] = static_cast<uint32_t>(pos >> 31);
	rmmio[0]        = (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u;
	rmmio[1]        = value;
}

void RDNA4FB::dmubPing() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-dmubping", &on, sizeof(on)) || on == 0)
		return;
	if (!ipDiscovery.isValid() || !rmmio) {
		FBLOG("dmub: MMIO unavailable, skipping ping");
		return;
	}

	// The GOP firmware demonstrably services the inbox1 RING (it consumed
	// 0x4c0 bytes of GOP commands) but ignored the register inbox0 on
	// hardware. The ring lives in top-of-VRAM firmware memory (REGION4);
	// reach it via MM_INDEX/MM_DATA indirect VRAM access.
	constexpr uint32_t kInbox1Size = 0x01d5;
	constexpr uint32_t kInbox1Wptr = 0x01d6, kInbox1Rptr = 0x01d7;

	uint32_t inboxSize = regReadDmu(2, kInbox1Size);
	uint32_t wptr = regReadDmu(2, kInbox1Wptr);
	uint32_t rptr = regReadDmu(2, kInbox1Rptr);
	if (inboxSize == 0 || inboxSize > 0x100000 || wptr != rptr ||
	    (wptr % Dmub::kCmdSize) != 0) {
		FBLOG("dmub: inbox not idle/sane (size=0x%x wptr=0x%x rptr=0x%x)",
		      inboxSize, wptr, rptr);
		return;
	}

	// Ring VRAM position: REGION4 (mailbox) MC address minus the VRAM MC
	// base from DCN_VM_FB_LOCATION_BASE (16 MiB units).
	uint64_t region4Mc = regReadDmu(2, 0x0196) |
	    (static_cast<uint64_t>(regReadDmu(2, 0x0197)) << 32);
	uint64_t vramMcBase =
	    static_cast<uint64_t>(regReadDmu(2, 0x0475) & 0xffffff) << 24;
	if (region4Mc <= vramMcBase) {
		FBLOG("dmub: region4 mc 0x%llx below vram base 0x%llx", region4Mc, vramMcBase);
		return;
	}
	uint64_t ringPos = (region4Mc - vramMcBase) + wptr;
	FBLOG("dmub: inbox1 ring at vram+0x%llx (region4 mc 0x%llx), wptr=0x%x",
	      region4Mc - vramMcBase, region4Mc, wptr);

	// Sanity: the entry at rptr-64 was written by the GOP; read it back as
	// proof the MM window reads the same memory the firmware reads.
	if (wptr >= Dmub::kCmdSize)
		FBLOG("dmub: previous GOP command header via MM: 0x%08x",
		      vramRead32(ringPos - Dmub::kCmdSize));

	// Compose QUERY_FEATURE_CAPS at wptr and submit.
	vramWrite32(ringPos, Dmub::headerWord(Dmub::CmdQueryFeatureCaps, 0,
	                                      Dmub::kCmdSize - 4));
	for (uint32_t i = 1; i < Dmub::kCmdSize / 4; i++)
		vramWrite32(ringPos + 4 * i, 0);
	uint32_t rb0 = vramRead32(ringPos);
	if (rb0 != Dmub::headerWord(Dmub::CmdQueryFeatureCaps, 0, Dmub::kCmdSize - 4)) {
		FBLOG("dmub: ring write readback mismatch (0x%08x), aborting", rb0);
		return;
	}

	uint32_t newWptr = (wptr + Dmub::kCmdSize) % inboxSize;
	regWriteDmu(2, kInbox1Wptr, newWptr);

	uint32_t nrptr = rptr;
	for (int i = 0; i < 20000; i++) {
		nrptr = regReadDmu(2, kInbox1Rptr);
		if (nrptr == newWptr)
			break;
		IODelay(10);
	}
	if (nrptr == newWptr)
		FBLOG("dmub: PING OK — rptr advanced 0x%x -> 0x%x; reply: %08x %08x %08x",
		      rptr, nrptr, vramRead32(ringPos), vramRead32(ringPos + 4),
		      vramRead32(ringPos + 8));
	else
		FBLOG("dmub: ping NOT consumed (wptr=0x%x rptr stuck at 0x%x)",
		      newWptr, nrptr);
}

// rdna4-dmubcursor=1: the flanking move for the invisible-cursor saga —
// hand the firmware our cursor register images via DMUB_CMD__UPDATE_CURSOR_
// INFO (two chained ring entries, dc_send_update_cursor_info_to_dmu) and let
// IT program the hardware. A white 64x64 square appearing at (100,100)
// means AMD's own code can light the cursor plane where nine rounds of
// direct programming could not; nothing appearing is decisive the other way.
// Note: the DMUB position layout is x[15:0], y[31:16] — the OPPOSITE of the
// dcn_4_1_0_sh_mask claim; whichever the FW writes is the silicon truth.
void RDNA4FB::dmubCursorTest() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("rdna4-dmubcursor", &on, sizeof(on)) || on == 0)
		return;
	if (!hwCursorReady || !cursorVram) {
		FBLOG("dmub: cursor test needs rdna4-hwcursor=1 (sprite slot)");
		return;
	}

	// Opaque white 64x64 sprite in the (128-pixel-pitch) slot.
	for (uint32_t i = 0; i < 128 * 128; i++)
		cursorVram[i] = 0;
	for (uint32_t r = 0; r < 64; r++)
		for (uint32_t c = 0; c < 64; c++)
			cursorVram[r * 128 + c] = 0xFFFFFFFFu;

	constexpr uint32_t kInbox1Size = 0x01d5;
	constexpr uint32_t kInbox1Wptr = 0x01d6, kInbox1Rptr = 0x01d7;
	uint32_t inboxSize = regReadDmu(2, kInbox1Size);
	uint32_t wptr = regReadDmu(2, kInbox1Wptr);
	uint32_t rptr = regReadDmu(2, kInbox1Rptr);
	if (inboxSize == 0 || inboxSize > 0x100000 || wptr != rptr) {
		FBLOG("dmub: inbox busy, skipping cursor test");
		return;
	}
	uint64_t region4Mc = regReadDmu(2, 0x0196) |
	    (static_cast<uint64_t>(regReadDmu(2, 0x0197)) << 32);
	uint64_t vramMcBase =
	    static_cast<uint64_t>(regReadDmu(2, 0x0475) & 0xffffff) << 24;
	uint64_t ringBase = region4Mc - vramMcBase;

	// HUBP cursor control image in the DMUB union layout (enable, mode[10:8],
	// pitch[17:16], lines_per_chunk[28:24]; REQ_MODE is not ours to set here).
	const uint32_t hubpCtl = 1u | (hwCursorMode << 8) |
	                         (1u << 16) /*pitch 128px*/ | (3u << 24);
	const uint32_t dppCtl  = 1u | (hwCursorMode << 4);

	uint32_t cmd0[16] = {
		Dmub::headerWord(Dmub::CmdUpdateCursorInfo, 0, 52, false, /*multi=*/true),
		100, 100, 64, 64,          // cursor_rect x,y,w,h
		0,                         // debug flags
		0x00020001,                // enable=1, pipe 0, VERSION_2 (external
		                           // monitor support — v0 makes the FW park
		                           // the cursor, observed on hardware), panel 0
		hubpCtl,
		100u | (100u << 16),       // position: x [15:0], y [31:16] per DMUB
		0,                         // hot spot
		0,                         // dst offset
		dppCtl,
		0,                         // position pipe_idx + padding
		0,                         // otg_inst + padding
		0, 0,
	};
	uint32_t cmd1[16] = {
		Dmub::headerWord(Dmub::CmdUpdateCursorInfo, 0, 24),
		static_cast<uint32_t>(cursorMcAddr >> 32) & 0xffff,   // SURFACE_ADDR_HIGH
		static_cast<uint32_t>(cursorMcAddr),                  // SURFACE_ADDR
		hubpCtl,
		64u | (64u << 16),         // size: width, height
		3u << 8,                   // settings: chunk_hdl_adjust
		dppCtl,
		0, 0, 0, 0, 0, 0, 0, 0, 0,
	};

	for (uint32_t i = 0; i < 16; i++)
		vramWrite32(ringBase + wptr + 4 * i, cmd0[i]);
	uint32_t slot1 = (wptr + Dmub::kCmdSize) % inboxSize;
	for (uint32_t i = 0; i < 16; i++)
		vramWrite32(ringBase + slot1 + 4 * i, cmd1[i]);

	uint32_t newWptr = (wptr + 2 * Dmub::kCmdSize) % inboxSize;
	regWriteDmu(2, kInbox1Wptr, newWptr);

	uint32_t nrptr = rptr;
	for (int i = 0; i < 20000; i++) {
		nrptr = regReadDmu(2, kInbox1Rptr);
		if (nrptr == newWptr)
			break;
		IODelay(10);
	}
	FBLOG("dmub: cursor-info %s (rptr 0x%x -> 0x%x); look for a white 64x64 "
	      "square at (100,100); hubp ctl rb=0x%08x pos rb=0x%08x cm rb=0x%08x",
	      nrptr == newWptr ? "CONSUMED" : "NOT consumed", rptr, nrptr,
	      regReadDmu(2, 0x0679), regReadDmu(2, 0x067d),
	      regReadDmu(2, 0x0cf1));
}

void RDNA4FB::setDisplayPower(bool on) {
	if (!displaySleepEnabled || on == displayPowerOn)
		return;
	if (!ipDiscovery.isValid() || !rmmio)
		return;

	// DP0 stream encoder — the boot pipe (dcn_4_1_0_offset.h, base_idx 2).
	constexpr uint32_t kDpVidStreamCntl = 0x2122;
	constexpr uint32_t kVidStreamEnable = 1u << 0;
	// DPCD SET_POWER (native AUX address 0x600): D0 = 1, D3/sleep = 2.
	constexpr uint32_t kDpcdSetPower = 0x600;

	auto sinkPower = [&](uint8_t state) -> bool {
		if (!sinkAuxValid)
			return false;
		// A sink coming out of D3 may need a moment before it ACKs AUX
		// (DP spec allows up to 1 ms; be generous).
		for (int t = 0; t < 10; t++) {
			uint8_t rb = 0;
			int rc = auxTransaction(sinkAuxInst, kActDpWrite, kDpcdSetPower,
			                        &state, 1, nullptr, 0, &rb);
			if (rc == kReplyAck)
				return true;
			IOSleep(1);
		}
		return false;
	};

	uint32_t v = regReadDmu(2, kDpVidStreamCntl);
	if (v == 0xFFFFFFFF) {
		FBLOG("power: stream register unreadable, leaving display alone");
		return;
	}

	if (on) {
		// Wake the sink first so it sees video the moment the stream returns.
		bool acked = sinkPower(0x1);
		regWriteDmu(2, kDpVidStreamCntl, v | kVidStreamEnable);
		FBLOG("power: display on (sink D0 %s, stream 0x%08x -> 0x%08x)",
		      acked ? "acked" : "no ack", v, regReadDmu(2, kDpVidStreamCntl));
	} else {
		// Blank the stream, then let the sink drop to D3. The timing
		// generator keeps running; only the video stream enable is touched.
		regWriteDmu(2, kDpVidStreamCntl, v & ~kVidStreamEnable);
		bool acked = sinkPower(0x2);
		FBLOG("power: display off (stream 0x%08x -> 0x%08x, sink D3 %s)",
		      v, regReadDmu(2, kDpVidStreamCntl), acked ? "acked" : "no ack");
	}
	displayPowerOn = on;
}

// ---------------------------------------------------------------------------
// Emulated VBL interrupt
// ---------------------------------------------------------------------------

void RDNA4FB::fireVBL(IOTimerEventSource *sender) {
	if (vblProc && vblEnabled)
		vblProc(vblTarget, vblRef);
	if (sender && vblEnabled)
		sender->setTimeoutUS(vblPeriodUS);
}

IOReturn RDNA4FB::registerForInterruptType(IOSelect interruptType,
                                              IOFBInterruptProc proc,
                                              OSObject *target, void *ref,
                                              void **interruptRef) {
	if (interruptType != kIOFBVBLInterruptType || !vblRequested || !proc)
		return super::registerForInterruptType(interruptType, proc, target,
		                                       ref, interruptRef);

	// Refine the tick period from the sink's EDID (59.996 Hz on the boot
	// display reads as 16668 us; default stays 60 Hz).
	Edid::DetailedTiming t {};
	if (edidLen && Edid::preferredTiming(edidData, edidLen, t) &&
	    t.refreshMilliHz() >= 30000 && t.refreshMilliHz() <= 240000)
		vblPeriodUS = 1000000000u / t.refreshMilliHz();

	vblProc = proc;
	vblTarget = target;
	vblRef = ref;

	if (!vblTimer) {
		vblTimer = IOTimerEventSource::timerEventSource(this,
		    OSMemberFunctionCast(IOTimerEventSource::Action, this,
		                         &RDNA4FB::fireVBL));
		if (!vblTimer || !getWorkLoop() ||
		    getWorkLoop()->addEventSource(vblTimer) != kIOReturnSuccess) {
			if (vblTimer) {
				vblTimer->release();
				vblTimer = nullptr;
			}
			vblProc = nullptr;
			return kIOReturnUnsupported;
		}
	}

	vblEnabled = true;
	vblTimer->setTimeoutUS(vblPeriodUS);
	FBLOG("vbl: emulated VBL armed, period %u us", vblPeriodUS);
	if (interruptRef)
		*interruptRef = &vblProc;   // opaque token identifying our VBL slot
	return kIOReturnSuccess;
}

IOReturn RDNA4FB::unregisterInterrupt(void *interruptRef) {
	if (interruptRef != &vblProc)
		return super::unregisterInterrupt(interruptRef);
	vblEnabled = false;
	if (vblTimer)
		vblTimer->cancelTimeout();
	vblProc = nullptr;
	return kIOReturnSuccess;
}

IOReturn RDNA4FB::setInterruptState(void *interruptRef, UInt32 state) {
	if (interruptRef != &vblProc)
		return super::setInterruptState(interruptRef, state);
	// IOFramebuffer throttles VBL when idle (vblThrottle) and re-enables it
	// on demand; honoring the state keeps the timer from ticking pointlessly.
	bool enable = (state == kEnabledInterruptState);
	if (enable && !vblEnabled && vblTimer)
		vblTimer->setTimeoutUS(vblPeriodUS);
	if (!enable && vblTimer)
		vblTimer->cancelTimeout();
	vblEnabled = enable;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Hardware cursor (DCN cursor plane, pipe 0)
// ---------------------------------------------------------------------------

namespace {
// dcn_4_1_0_offset.h, all base_idx 2. HUBP-side CURSOR0_0 block plus the
// DPP-side CM_CUR0 enable.
constexpr uint32_t kCurControl  = 0x0679;
constexpr uint32_t kCurAddr     = 0x067a;
constexpr uint32_t kCurAddrHigh = 0x067b;
constexpr uint32_t kCurSize     = 0x067c;
constexpr uint32_t kCurPosition = 0x067d;
constexpr uint32_t kCurHotSpot  = 0x067e;
constexpr uint32_t kCurDstOffset = 0x0680;
constexpr uint32_t kCmCur0Control = 0x0cf1;
// DCN 4.01 cursor FP pipeline (new on this generation): cursor pixels are
// multiplied by an FP16 scale before blending. The GOP never uses a cursor
// and leaves scale at 0.0 — every register and the sprite data verify
// perfectly while the cursor renders as nothing. 0x3c00 is FP16 1.0, the
// default from dcn10_set_cursor_sdr_white_level; bias 0; matrix bypass (0).
constexpr uint32_t kCmCur0FpScaleBiasGY = 0x0cf4;  // SCALE [15:0], BIAS [31:16]
constexpr uint32_t kCmCur0FpScaleBiasRB = 0x0cf5;
constexpr uint32_t kCmCur0MatrixMode    = 0x0cf6;
constexpr uint32_t kCurFpScaleOne       = 0x3c00;  // FP16 1.0
// Pipe update latching. Cursor/CM registers are double-buffered: writes go
// to a pending copy that latches into live hardware at the frame boundary —
// but only while OTG_MASTER_UPDATE_LOCK is released. The GOP programs its
// pipe under the lock and has no reason to ever release it, which freezes
// every later pipe update in pending space (reads return the pending values,
// so all writes "verify" while the hardware never changes). LOCK bit 0 is
// the request; UPDATE_LOCK_STATUS bit 8 and OTG_UPDATE_PENDING bit 0 of
// DOUBLE_BUFFER_CONTROL are true status bits.
constexpr uint32_t kOtgMasterUpdateLock  = 0x1b89;
constexpr uint32_t kOtgDoubleBufferCtl   = 0x1b5c;
constexpr uint32_t kDppTopControl        = 0x0cc5;
// Global sync: pipe updates latch on the VUPDATE pulse, which is a
// PROGRAMMABLE event (offset/width lines, positioned via VSTARTUP). amdgpu
// programs it at every modeset; a GOP that never updates its pipe again has
// no reason to program a pulse at all — width 0 means the latch event never
// fires and every double-buffered write stays pending forever. This also
// retroactively explains the earlier 8bpc/MCM writes that read back changed
// but never altered the picture. GLOBAL_SYNC_STATUS bit 8
// (VUPDATE_EVENT_OCCURRED) is the ground truth.
constexpr uint32_t kOtgVStartupParam     = 0x1b85;
constexpr uint32_t kOtgVUpdateParam      = 0x1b86;
constexpr uint32_t kOtgVReadyParam       = 0x1b87;
constexpr uint32_t kOtgGlobalSyncStatus  = 0x1b88;
// HUBPREQ0_CURSOR_SETTINGS — cursor fetch scheduling. amdgpu always programs
// CHUNK_HDL_ADJUST=3 ([9:8]); without it and a correct LINES_PER_CHUNK the
// cursor request pipeline can fetch nothing (sprite armed but invisible).
constexpr uint32_t kCurSettings = 0x0653;
constexpr uint32_t kCurChunkHdlAdjust = 3u << 8;

// Field layout (dcn_4_1_0_sh_mask.h): CURSOR_CONTROL enable bit0, REQ_MODE
// bit2, MODE [9:8], PITCH [17:16], LINES_PER_CHUNK [25:24]; SIZE height
// [15:0] width [31:16]; POSITION y [14:0], x starts at bit 15 (NOT 16 on
// this generation).
// CURSOR_REQ_MODE=1 (fetch during display prefetch) is MANDATORY on DCN4x:
// per dcn401_hubp.c, mode 0 (legacy fetch-just-in-time) "is no longer
// supported" — with it the cursor simply never fetches (hardware-confirmed:
// three rounds of perfect registers + verified sprite data, no pixels).
constexpr uint32_t kCurReqMode    = 1u << 2;
constexpr uint32_t kCurModeShift  = 8;
constexpr uint32_t kCurPitchShift = 16;
constexpr uint32_t kCurLpcShift   = 24;
constexpr uint32_t kCurXShift     = 15;
// CM_CUR0_CURSOR0_CONTROL: CUR0_ENABLE bit0, CUR0_MODE [6:4], plus two bits
// captured from a WORKING amdgpu cursor on this exact GPU (register diff,
// Debian, 2026-07-12: working value 0x000000a5 vs our 0x21): bit 7
// (CUR0_PIXEL_ALPHA_MOD_EN) and bit 2 (CUR0_PIX_INV_MODE per the sh_mask).
// Neither is set by dpp401_set_cursor_attributes — amdgpu programs them in
// a layer we had not ported. Mirror the proven-working value.
constexpr uint32_t kCur0ModeShift   = 4;
constexpr uint32_t kCur0WorkingBits = (1u << 7) | (1u << 2);

// hubp1_get_lines_per_chunk for color cursors: enum {1,2,4,8,16} lines
// encodes as 0..4.
static uint32_t cursorLinesPerChunk(uint32_t width) {
	if (width <= 32)  return 4;  // 16 lines
	if (width <= 64)  return 3;  // 8 lines
	if (width <= 128) return 2;  // 4 lines
	return 1;                    // 2 lines
}

// HUBPREQ0 scanout address registers — anchor for the sprite's GPU address.
constexpr uint32_t kHubpPrimaryAddr     = 0x060a;
constexpr uint32_t kHubpPrimaryAddrHigh = 0x060b;

constexpr uint32_t kCursorPixels = 128;               // fixed 128x128 slot
constexpr uint32_t kCursorPitchCode = 1;              // 0=64,1=128,2=256 px
constexpr uint32_t kCursorBytes  = kCursorPixels * kCursorPixels * 4;
} // namespace

bool RDNA4FB::initHWCursor() {
	if (!hwCursorRequested || !ipDiscovery.isValid() || !rmmio ||
	    fbPhysBase == 0 || fbLength == 0)
		return false;

	uint32_t lo = regReadDmu(2, kHubpPrimaryAddr);
	uint32_t hi = regReadDmu(2, kHubpPrimaryAddrHigh);
	if (lo == 0xFFFFFFFF || (lo == 0 && hi == 0)) {
		FBLOG("cursor: scanout MC address unreadable, staying with software cursor");
		return false;
	}
	uint64_t scanoutMc = (static_cast<uint64_t>(hi & 0xffff) << 32) | lo;
	scanoutMcAddr = scanoutMc;

	uint32_t ctest = 0;
	if (PE_parse_boot_argn("rdna4-curtest", &ctest, sizeof(ctest)) && ctest != 0) {
		hwCursorTest = true;
		FBLOG("cursor: TEST MODE: sprite will fetch from the scanout base");
	}

	// Address-routing state (read-only): the DCHUBBUB FB/AGP windows and the
	// per-HUBP system aperture decide where cursor memory requests actually
	// go. Out-of-window requests return zeros without latching any error.
	FBLOG("cursor: vm: fb_loc base=0x%08x top=0x%08x agp base=0x%08x "
	      "bot=0x%08x top=0x%08x sys_ap=0x%08x/0x%08x l1_tlb=0x%08x",
	      regReadDmu(2, 0x0475), regReadDmu(2, 0x0476),
	      regReadDmu(2, 0x047a), regReadDmu(2, 0x0478), regReadDmu(2, 0x0479),
	      regReadDmu(2, 0x062c), regReadDmu(2, 0x062d), regReadDmu(2, 0x063a));

	// Sprite right after the framebuffer, 8 KiB aligned. The CPU sees VRAM
	// through the BAR at the same linear offsets as the MC addresses, so the
	// CPU-visible sprite address is fbPhysBase plus the identical delta.
	cursorMcAddr = (scanoutMc + fbLength + 0x1FFFULL) & ~0x1FFFULL;
	uint64_t delta = cursorMcAddr - scanoutMc;
	// Stay well inside the 256 MiB non-ReBAR VRAM window.
	if (delta + kCursorBytes > 192ULL * 1024 * 1024) {
		FBLOG("cursor: sprite offset 0x%llx outside safe aperture window", delta);
		return false;
	}

	IODeviceMemory *mem = IODeviceMemory::withRange(fbPhysBase + delta, kCursorBytes);
	if (!mem)
		return false;
	cursorMap = mem->map();
	mem->release();
	if (!cursorMap) {
		FBLOG("cursor: failed to map sprite VRAM");
		return false;
	}
	cursorVram = reinterpret_cast<volatile uint32_t *>(cursorMap->getVirtualAddress());

	cursorStage = static_cast<uint32_t *>(IOMalloc(kCursorBytes));
	if (!cursorStage) {
		cursorMap->release();
		cursorMap = nullptr;
		cursorVram = nullptr;
		return false;
	}

	// Release the OTG master update lock if the GOP left it held: with the
	// lock asserted, every double-buffered cursor/CM write stays pending
	// forever (writes read back fine, hardware never changes).
	uint32_t lock = regReadDmu(2, kOtgMasterUpdateLock);
	uint32_t dbc  = regReadDmu(2, kOtgDoubleBufferCtl);
	FBLOG("cursor: OTG lock=0x%08x (status=%u) dbufctl=0x%08x dppctl=0x%08x",
	      lock, (lock >> 8) & 1, dbc, regReadDmu(2, kDppTopControl));
	if (lock & 1) {
		regWriteDmu(2, kOtgMasterUpdateLock, 0);
		FBLOG("cursor: released OTG master update lock (was 0x%08x, now 0x%08x)",
		      lock, regReadDmu(2, kOtgMasterUpdateLock));
	}

	// Ensure the VUPDATE latch pulse exists. Without it, no pipe update we
	// ever queue becomes live.
	uint32_t vstartup = regReadDmu(2, kOtgVStartupParam);
	uint32_t vupdate  = regReadDmu(2, kOtgVUpdateParam);
	uint32_t sync     = regReadDmu(2, kOtgGlobalSyncStatus);
	FBLOG("cursor: global sync: vstartup=0x%08x vupdate=0x%08x vready=0x%08x "
	      "status=0x%08x (vupdate_occurred=%u)",
	      vstartup, vupdate, regReadDmu(2, kOtgVReadyParam), sync,
	      (sync >> 8) & 1);
	if (((vupdate >> 16) & 0x3ff) == 0) {
		// Pulse of 2 lines right at VSTARTUP. If VSTARTUP is also
		// unprogrammed, place it inside the vertical blank (the Samsung
		// timing has 62 blank lines; 40 is safely within any mode's blank).
		if ((vstartup & 0x3ff) == 0)
			regWriteDmu(2, kOtgVStartupParam, 40);
		regWriteDmu(2, kOtgVUpdateParam, (2u << 16));
		FBLOG("cursor: programmed VUPDATE pulse (vstartup=0x%08x vupdate=0x%08x)",
		      regReadDmu(2, kOtgVStartupParam), regReadDmu(2, kOtgVUpdateParam));
	}

	FBLOG("cursor: HW cursor armed: scanout MC 0x%llx, sprite MC 0x%llx "
	      "(cpu 0x%llx), mode %u", scanoutMc, cursorMcAddr,
	      fbPhysBase + delta, hwCursorMode);
	hwCursorReady = true;
	return true;
}

IOReturn RDNA4FB::setCursorImage(void *cursorImage) {
	if (!hwCursorReady)
		return kIOReturnUnsupported;

	IOHardwareCursorDescriptor desc {};
	desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
	desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
	desc.height   = kCursorPixels;
	desc.width    = kCursorPixels;
	desc.bitDepth = 32;   // direct ARGB

	IOHardwareCursorInfo info {};
	info.majorVersion = kHardwareCursorInfoMajorVersion;
	info.minorVersion = kHardwareCursorInfoMinorVersion;
	info.hardwareCursorData = reinterpret_cast<UInt8 *>(cursorStage);

	if (!convertCursorImage(cursorImage, &desc, &info))
		return kIOReturnUnsupported;
	uint32_t w = info.cursorWidth, h = info.cursorHeight;
	if (w == 0 || h == 0 || w > kCursorPixels || h > kCursorPixels)
		return kIOReturnUnsupported;

	// Staging buffer is tightly packed (w*4); the VRAM slot has a fixed
	// 128-pixel pitch. Clear the slot so stale pixels never show at edges.
	for (uint32_t i = 0; i < kCursorPixels * kCursorPixels; i++)
		cursorVram[i] = 0;
	for (uint32_t row = 0; row < h; row++)
		for (uint32_t col = 0; col < w; col++)
			cursorVram[row * kCursorPixels + col] = cursorStage[row * w + col];

	// Data-integrity check for the first images: find an opaque staging
	// pixel and read the same location back from VRAM. Registers verifying
	// while the pointer stays invisible means either the conversion made a
	// fully transparent sprite (staging max-alpha 0) or the CPU->VRAM window
	// is not where we think (VRAM readback mismatch).
	if (cursorImgLogs < 3) {
		uint32_t opaqueIdx = 0, opaqueVal = 0;
		for (uint32_t i = 0; i < w * h; i++) {
			if ((cursorStage[i] >> 24) > (opaqueVal >> 24)) {
				opaqueVal = cursorStage[i];
				opaqueIdx = i;
			}
			if ((opaqueVal >> 24) == 0xff)
				break;
		}
		uint32_t row = opaqueIdx / w, col = opaqueIdx % w;
		uint32_t vramVal = cursorVram[row * kCursorPixels + col];
		FBLOG("cursor: pixel check: stage[%u,%u]=0x%08x vram=0x%08x %s",
		      col, row, opaqueVal, vramVal,
		      opaqueVal == vramVal ? "(match)" : "(MISMATCH - aperture wrong)");
	}

	uint64_t spriteAddr = hwCursorTest ? scanoutMcAddr : cursorMcAddr;
	regWriteDmu(2, kCurAddrHigh, static_cast<uint32_t>(spriteAddr >> 32) & 0xffff);
	regWriteDmu(2, kCurAddr, static_cast<uint32_t>(spriteAddr));
	regWriteDmu(2, kCurSize, h | (w << 16));
	// setCursorState receives hotspot-adjusted top-left coords, so the
	// hardware hotspot stays zero.
	regWriteDmu(2, kCurHotSpot, 0);
	// Fetch scheduling (missing on first hardware try — sprite armed but
	// invisible): chunk handle deadline adjust + lines per fetch chunk.
	regWriteDmu(2, kCurSettings, kCurChunkHdlAdjust);
	cursorCtlBase = (cursorLinesPerChunk(w) << kCurLpcShift) |
	                (kCursorPitchCode << kCurPitchShift) |
	                (hwCursorMode << kCurModeShift) |
	                kCurReqMode;
	regWriteDmu(2, kCurControl, cursorCtlBase | (hwCursorVisible ? 1u : 0u));
	// The FP scale stage: without FP16 1.0 here the sprite is multiplied
	// to invisibility (found on hardware 2026-07-12).
	regWriteDmu(2, kCmCur0FpScaleBiasGY, kCurFpScaleOne);
	regWriteDmu(2, kCmCur0FpScaleBiasRB, kCurFpScaleOne);
	regWriteDmu(2, kCmCur0MatrixMode, 0);   // matrix bypass
	regWriteDmu(2, kCmCur0Control,
	            (hwCursorMode << kCur0ModeShift) | kCur0WorkingBits |
	            (hwCursorVisible ? 1u : 0u));

	if (cursorImgLogs < 3) {
		cursorImgLogs++;
		FBLOG("cursor: image %ux%u px0=0x%08x rb: ctl=0x%08x size=0x%08x "
		      "addr=0x%08x/%04x set=0x%08x cm=0x%08x fp=0x%08x/0x%08x "
		      "hubp_cntl=0x%08x cnvc=0x%08x/0x%08x",
		      w, h, cursorStage[0],
		      regReadDmu(2, kCurControl), regReadDmu(2, kCurSize),
		      regReadDmu(2, kCurAddr), regReadDmu(2, kCurAddrHigh) & 0xffff,
		      regReadDmu(2, kCurSettings), regReadDmu(2, kCmCur0Control),
		      regReadDmu(2, kCmCur0FpScaleBiasGY),
		      regReadDmu(2, kCmCur0FpScaleBiasRB),
		      regReadDmu(2, 0x05f4),           // HUBP0_DCHUBP_CNTL
		      regReadDmu(2, 0x0ccf),           // CNVC surface pixel format
		      regReadDmu(2, 0x0cd0));          // CNVC format control
	}
	return kIOReturnSuccess;
}

IOReturn RDNA4FB::setCursorState(SInt32 x, SInt32 y, bool visible) {
	if (!hwCursorReady)
		return kIOReturnUnsupported;

	// Signed coords can go negative when the pointer overlaps the top/left
	// edge; clamp (v1 accepts the sprite pinning at the edge there).
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	regWriteDmu(2, kCurPosition,
	            (static_cast<uint32_t>(y) & 0x7fff) |
	            (static_cast<uint32_t>(x) << kCurXShift));
	// Cursor fetch deadline (hubp2_cursor_set_position): the source x offset
	// scaled from pixel time to refclk time. 100 MHz refclk over the 533.25
	// MHz boot pixel clock; precision is uncritical (it is a deadline hint).
	regWriteDmu(2, kCurDstOffset,
	            (static_cast<uint32_t>(x) * 100000u) / 533250u);
	// Only touch the double-buffered control registers on visibility
	// changes — rewriting them every move re-arms UPDATE_PENDING and hides
	// whether latching ever completes.
	if (visible != hwCursorVisible) {
		if (cursorCtlBase)   // 0 until the first setCursorImage
			regWriteDmu(2, kCurControl, cursorCtlBase | (visible ? 1u : 0u));
		regWriteDmu(2, kCmCur0Control,
		            (hwCursorMode << kCur0ModeShift) | kCur0WorkingBits |
		            (visible ? 1u : 0u));
	}
	hwCursorVisible = visible;

	// Log the first few calls, then a sparse sample of later ones — the
	// later samples show whether CUR0_UPDATE_PENDING (cm bit 16) ever
	// clears and whether VUPDATE events occur (sync bit 8) once the
	// control registers are left alone between visibility changes.
	cursorPosCalls++;
	if (cursorPosLogs < 6 ||
	    (cursorPosLogs < 14 && (cursorPosCalls & 0x1ff) == 0)) {
		cursorPosLogs++;
		FBLOG("cursor: state#%u x=%d y=%d vis=%d rb: pos=0x%08x ctl=0x%08x "
		      "cm=0x%08x sync=0x%08x",
		      cursorPosCalls, (int)x, (int)y, visible,
		      regReadDmu(2, kCurPosition), regReadDmu(2, kCurControl),
		      regReadDmu(2, kCmCur0Control),
		      regReadDmu(2, kOtgGlobalSyncStatus));
	}
	return kIOReturnSuccess;
}

void RDNA4FB::initForPM() {
	if (pmRegistered)
		return;
	pmRegistered = true;

	// States and flags mirror IONDRVFramebuffer::initForPM: 0 = sleep,
	// 1 = doze (display blanked, framebuffer preserved), 2 = wake.
	static IOPMPowerState powerStates[3] = {
		{ 1, 0,                0,            0,            0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, 0,                0,            kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
	};

	// Like Apple's IOBootNDRV (the EFI fallback framebuffer): we cannot
	// reprogram the display pipe after the GPU loses power, so a wake from
	// system sleep would come back to a black screen. Veto system sleep from
	// every state until native mode setting exists. Display sleep (doze) is
	// unaffected.
	for (auto &state : powerStates)
		state.capabilityFlags |= kIOPMPreventSystemSleep;

	// Register with the policy maker (IOFramebuffer::start already did
	// PMinit + joinPMtree). Without this no power states exist for this
	// device and setPowerState is never called.
	registerPowerDriver(this, powerStates, 3);
	// No sleep until children (the displays) allow it.
	temporaryPowerClampOn();
	// Do not drop below doze until system sleep.
	changePowerStateTo(1);
	if (pciDevice)
		pciDevice->setProperty("IOPMIsPowerManaged", true);  // key is SDK-private
	FBLOG("power: registered power states (doze-only, system sleep vetoed)");
}

bool RDNA4FB::regWriteDmu(uint8_t baseIdx, uint32_t dwordOffset, uint32_t value) {
	uint32_t byteOffset;
	if (!ipDiscovery.isValid() ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwDmu, 0, baseIdx, dwordOffset, byteOffset))
		return false;
	if (!rmmio || byteOffset + 4 > rmmioSize)
		return false;
	rmmio[byteOffset / 4] = value;
	return true;
}

void RDNA4FB::tryForce8bpc() {
	uint32_t v = 0;
	if (!PE_parse_boot_argn("rdna4-8bpc", &v, sizeof(v)) || v == 0)
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

void RDNA4FB::probeMemSize() {
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

IOService *RDNA4FB::probe(IOService *provider, SInt32 *score) {
	// Kill switch: boot-arg "rdna4-off=1" (no leading dash) disables the
	// driver without removing it, so a bad build can be recovered from the
	// OpenCore boot-args instead of Safe Mode / filesystem surgery.
	uint32_t off = 0;
	if (PE_parse_boot_argn("rdna4-off", &off, sizeof(off)) && off != 0) {
		FBLOG("disabled by rdna4-off boot-arg");
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
	uint16_t device = static_cast<uint16_t>(vendorDevice >> 16);
	FBLOG("probe on PCI device 0x%08x", vendorDevice);

	// Vendor 0x1002 (AMD), Navi 48 family: 0x7550 (RX 9070 / 9070 XT),
	// 0x7551 (Radeon AI PRO R9700 — same die, compute bring-up proven on
	// macOS by lemonade-sdk/mac-amdgpu). Same DCN 4.1.0 display core.
	if ((vendorDevice & 0xffff) != 0x1002 ||
	    (device != 0x7550 && device != 0x7551)) {
		FBLOG("device id mismatch, not matching");
		return nullptr;
	}
	FBLOG("Navi 48 variant: %s",
	      device == 0x7550 ? "RX 9070 / 9070 XT" : "R9700");

	if (score) *score += 20000;
	return this;
}

bool RDNA4FB::start(IOService *provider) {
	pciDevice = OSDynamicCast(IOPCIDevice, provider);
	if (!pciDevice) {
		FBLOG("start: no PCI provider");
		return false;
	}

	uint32_t cmap = 0;
	if (PE_parse_boot_argn("rdna4-cmap", &cmap, sizeof(cmap)) && cmap <= 5) {
		channelMap = cmap;
		FBLOG("start: using channel map %u", channelMap);
	}

	uint32_t nosleep = 0;
	if (PE_parse_boot_argn("rdna4-nosleep", &nosleep, sizeof(nosleep)) && nosleep != 0) {
		displaySleepEnabled = false;
		FBLOG("start: display sleep disabled by rdna4-nosleep");
	}

	uint32_t vbl = 0;
	if (PE_parse_boot_argn("rdna4-vbl", &vbl, sizeof(vbl)) && vbl != 0)
		vblRequested = true;

	uint32_t hwcur = 0;
	if (PE_parse_boot_argn("rdna4-hwcursor", &hwcur, sizeof(hwcur)) && hwcur != 0) {
		hwCursorRequested = true;
		uint32_t cmode = 0;
		if (PE_parse_boot_argn("rdna4-curmode", &cmode, sizeof(cmode)) &&
		    (cmode == 2 || cmode == 3))   // 2 premult, 3 straight alpha
			hwCursorMode = cmode;
	}

	// Grab the bootloader-provided framebuffer before anything else touches it.
	if (!captureConsoleInfo()) {
		FBLOG("start: could not capture console framebuffer, aborting");
		return false;
	}

	// Enable memory space so the aperture is reachable; do NOT enable bus
	// mastering — we never issue DMA.
	pciDevice->setMemoryEnable(true);

	// Precise model on the PCI nub (System Report / ioreg diagnostics).
	uint32_t vd = pciDevice->configRead32(kIOPCIConfigVendorID);
	const char *model = (vd >> 16) == 0x7551 ? "AMD Radeon AI PRO R9700"
	                                         : "AMD Radeon RX 9070 XT";
	pciDevice->setProperty("model", model);
	setProperty("GPU,Variant", model);

	// Registers first: the on-die discovery fallback needs MMIO.
	bool haveMmio = mapRegisters();

	// Best effort: connector layout and firmware info for later native
	// mode-setting work. The framebuffer itself does not depend on this.
	loadVBIOS();

	// If the VBIOS route did not yield IP discovery (no full flash dump
	// injected), read the PSP's on-die copy from top-of-VRAM — makes the
	// ATY,bin_image injection optional.
	if (haveMmio && !ipDiscovery.isValid())
		loadOnDieDiscovery();

	// Best effort: prove register MMIO works with one safe read (VRAM size),
	// then dump the DCN output-colour registers (read-only) for diagnosis.
	if (haveMmio) {
		probeMemSize();
		dumpDCN();
		tryForce8bpc();
		probeEDID();
		dumpModeState();
		initHWCursor();
		dmubPing();
		dmubCursorTest();
	}

	if (!super::start(provider)) {
		FBLOG("start: super::start failed");
		return false;
	}

	FBLOG("started");
	return true;
}

void RDNA4FB::stop(IOService *provider) {
	FBLOG("stop");
	vblEnabled = false;
	vblProc = nullptr;
	if (vblTimer) {
		vblTimer->cancelTimeout();
		if (getWorkLoop())
			getWorkLoop()->removeEventSource(vblTimer);
		vblTimer->release();
		vblTimer = nullptr;
	}
	hwCursorReady = false;
	if (cursorStage) {
		IOFree(cursorStage, kCursorBytes);
		cursorStage = nullptr;
	}
	if (cursorMap) {
		cursorMap->release();
		cursorMap = nullptr;
		cursorVram = nullptr;
	}
	if (onDieDisc) {
		IOFree(onDieDisc, 10 << 10);
		onDieDisc = nullptr;
	}
	unmapRegisters();
	freeVBIOS();
	super::stop(provider);
}

// ---------------------------------------------------------------------------
// IOFramebuffer — bring-up
// ---------------------------------------------------------------------------

IOReturn RDNA4FB::enableController() {
	// The controller is already "enabled": the firmware programmed the display
	// pipe and scanout address. We just re-validate the console geometry.
	if (fbPhysBase == 0 && !captureConsoleInfo())
		return kIOReturnNoResources;

	// Register power states here, not in start(): IOFramebuffer's power
	// machinery is only ready once the framebuffer is opened (this is where
	// IONDRVFramebuffer::enableController calls its initForPM too).
	// Registering from start() hung boot: registerPowerDriver kicks off an
	// immediate power change into IOFramebuffer::setPowerState before the
	// controller/workloop state it needs exists.
	initForPM();

	FBLOG("enableController: adopting firmware scanout %ux%u", fbWidth, fbHeight);
	return kIOReturnSuccess;
}

bool RDNA4FB::isConsoleDevice() {
	return true;
}

// ---------------------------------------------------------------------------
// IOFramebuffer — memory
// ---------------------------------------------------------------------------

IODeviceMemory *RDNA4FB::getApertureRange(IOPixelAperture aperture) {
	if (aperture != kIOFBSystemAperture)
		return nullptr;
	if (fbPhysBase == 0 || fbLength == 0)
		return nullptr;

	// A fresh instance (retain) per contract with the caller.
	return IODeviceMemory::withRange(fbPhysBase, fbLength);
}

IODeviceMemory *RDNA4FB::getVRAMRange() {
	// We expose only the scanout region as "VRAM"; we do not manage the card's
	// full VRAM because we have no memory controller driver.
	if (fbPhysBase == 0 || fbLength == 0)
		return nullptr;
	return IODeviceMemory::withRange(fbPhysBase, fbLength);
}

// ---------------------------------------------------------------------------
// IOFramebuffer — pixel / mode description
// ---------------------------------------------------------------------------

const char *RDNA4FB::getPixelFormats() {
	// NUL-separated, double-NUL terminated list. IO32BitDirectPixels is a
	// string literal macro from IOGraphicsTypes.h.
	static const char formats[] = IO32BitDirectPixels "\0";
	return formats;
}

IOItemCount RDNA4FB::getDisplayModeCount() {
	return 1;
}

IOReturn RDNA4FB::getDisplayModes(IODisplayModeID *allDisplayModes) {
	if (!allDisplayModes)
		return kIOReturnBadArgument;
	allDisplayModes[0] = kRDNA4DisplayModeID;
	return kIOReturnSuccess;
}

IOReturn RDNA4FB::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                  IODisplayModeInformation *info) {
	if (displayMode != kRDNA4DisplayModeID || !info)
		return kIOReturnBadArgument;

	bzero(info, sizeof(*info));
	info->nominalWidth  = fbWidth;
	info->nominalHeight = fbHeight;
	info->refreshRate   = 60 << 16;   // 60.0 Hz in 16.16 fixed point
	info->maxDepthIndex = 0;
	info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
	return kIOReturnSuccess;
}

UInt64 RDNA4FB::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
	// Obsolete: must return 0.
	return 0;
}

IOReturn RDNA4FB::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                         IOPixelAperture aperture,
                                         IOPixelInformation *pixelInfo) {
	if (displayMode != kRDNA4DisplayModeID || depth != 0 ||
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

IOReturn RDNA4FB::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
	if (displayMode) *displayMode = kRDNA4DisplayModeID;
	if (depth)       *depth       = 0;
	return kIOReturnSuccess;
}

IOReturn RDNA4FB::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
	// Only one mode/depth exists; accept it, reject anything else.
	if (displayMode != kRDNA4DisplayModeID || depth != 0)
		return kIOReturnUnsupported;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// IOFramebuffer — attributes / connection
// ---------------------------------------------------------------------------

IOReturn RDNA4FB::getAttribute(IOSelect attribute, uintptr_t *value) {
	switch (attribute) {
		case kIOHardwareCursorAttribute:
			// Report the DCN cursor plane when armed; otherwise force the
			// software cursor.
			if (value) *value = hwCursorReady ? 1 : 0;
			return kIOReturnSuccess;
		default:
			return super::getAttribute(attribute, value);
	}
}

IOReturn RDNA4FB::setAttribute(IOSelect attribute, uintptr_t value) {
	switch (attribute) {
		case kIOPowerAttribute:
			// IOFramebuffer asks the subclass to carry out its power state
			// change here: 0 = off, 1 = doze, 2 = on. Doze and off both mean
			// "stop showing pixels"; the framebuffer contents survive either
			// way since we never power gate anything.
			if (value >= 2) {
				setDisplayPower(true);
				handleEvent(kIOFBNotifyDidPowerOn);
			} else {
				handleEvent(kIOFBNotifyWillPowerOff);
				setDisplayPower(false);
			}
			return kIOReturnSuccess;
		default:
			return super::setAttribute(attribute, value);
	}
}

IOItemCount RDNA4FB::getConnectionCount() {
	return 1;
}

IOReturn RDNA4FB::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
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
		case kConnectionSupportsHLDDCSense:
			// Success (no value) tells IODisplay it may call hasDDCConnect()
			// and getDDCBlock() for the real EDID.
			return edidLen ? kIOReturnSuccess : kIOReturnUnsupported;
		default:
			return super::getAttributeForConnection(connectIndex, attribute, value);
	}
}

IOReturn RDNA4FB::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               uintptr_t value) {
	switch (attribute) {
		case kConnectionPower:
			// Display-off path used by IODisplay (Energy Saver display
			// sleep): 0 = off, nonzero = on.
			setDisplayPower(value != 0);
			return kIOReturnSuccess;
		default:
			return super::setAttributeForConnection(connectIndex, attribute, value);
	}
}

// ---------------------------------------------------------------------------
// IOFramebuffer — DDC/EDID
// ---------------------------------------------------------------------------

bool RDNA4FB::hasDDCConnect(IOIndex connectIndex) {
	return connectIndex == 0 && edidLen != 0;
}

IOReturn RDNA4FB::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                 IOSelect blockType, IOOptionBits options,
                                 UInt8 *data, IOByteCount *length) {
	if (connectIndex != 0 || blockType != kIODDCBlockTypeEDID || !data || !length)
		return kIOReturnUnsupported;

	// Serve the blocks cached at start() (1 = base EDID, 2 = CTA extension).
	// blockNumber is 1-based per the IOFramebuffer contract.
	if (blockNumber < 1 || blockNumber * 128 > edidLen)
		return kIOReturnNotFound;

	IOByteCount n = *length < 128 ? *length : 128;
	memcpy(data, edidData + (blockNumber - 1) * 128, n);
	*length = n;
	return kIOReturnSuccess;
}
