//
//  RX9070XTFB.cpp
//  RX9070XT
//

#include "RX9070XTFB.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/IOPM.h>

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

	// Boot-arg "rx9070xt-lutbypass=1": force all MCM stages to bypass.
	// Bypass is the hardware's pass-through state, so this cannot make the
	// image worse than a wrong LUT; a reboot restores firmware state.
	uint32_t fix = 0;
	if (PE_parse_boot_argn("rx9070xt-lutbypass", &fix, sizeof(fix)) && fix != 0) {
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

uint32_t RX9070XTFB::auxDword(uint8_t inst, uint32_t reg) const {
	return kAuxBase0 + static_cast<uint32_t>(inst) * kAuxStride + reg;
}

int RX9070XTFB::auxTransaction(uint8_t inst, uint8_t action, uint32_t address,
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

bool RX9070XTFB::readEDID(uint8_t inst, uint8_t *edid, size_t count,
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

void RX9070XTFB::probeEDID() {
	// Default-on since the AUX path was verified on hardware (2026-07-11);
	// "rx9070xt-noedid=1" opts out if a sink misbehaves.
	uint32_t noedid = 0;
	if (PE_parse_boot_argn("rx9070xt-noedid", &noedid, sizeof(noedid)) && noedid != 0) {
		FBLOG("edid: probing disabled by rx9070xt-noedid");
		return;
	}
	if (!ipDiscovery.isValid() || !rmmio) {
		FBLOG("edid: MMIO/discovery unavailable, skipping");
		return;
	}

	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = atomBios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	bool any = false;
	for (size_t i = 0; i < n; i++) {
		AtomBios::ConnectorType ct =
		    AtomBios::connectorType(paths[i].connectorObjId);
		// Only DisplayPort/USB-C sinks carry DDC on the AUX channel; HDMI/DVI
		// EDID travels over the separate I2C controller (not yet implemented).
		if (ct != AtomBios::ConnectorDP && ct != AtomBios::ConnectorUSBC)
			continue;

		AtomBios::PathRecords rec {};
		atomBios.getPathRecords(paths[i], rec);
		uint8_t inst = rec.ddcLine;  // AUX engine index == DDC line on this board

		uint8_t edid[128] {};
		if (!readEDID(inst, edid, sizeof(edid), 0)) {
			FBLOG("edid: connector %zu (AUX%u): no reply", i, inst);
			continue;
		}

		static const uint8_t sig[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
		if (memcmp(edid, sig, sizeof(sig)) != 0) {
			FBLOG("edid: connector %zu (AUX%u): data but bad header "
			      "%02x %02x %02x %02x", i, inst, edid[0], edid[1], edid[2], edid[3]);
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
		// Preferred timing = first 18-byte detailed descriptor (byte 54).
		const uint8_t *d = edid + 54;
		uint32_t hres = d[2] | ((static_cast<uint32_t>(d[4] & 0xf0)) << 4);
		uint32_t vres = d[5] | ((static_cast<uint32_t>(d[7] & 0xf0)) << 4);
		FBLOG("edid: connector %zu (AUX%u): %s product 0x%04x EDID %u.%u "
		      "preferred %ux%u", i, inst, mfg, product, edid[18], edid[19],
		      hres, vres);

		char key[40];
		snprintf(key, sizeof(key), "EDID,AUX%u", inst);
		setProperty(key, edid, sizeof(edid));
		snprintf(key, sizeof(key), "EDID,AUX%u-Vendor", inst);
		setProperty(key, mfg);

		// Cache the first sink's EDID for hasDDCConnect()/getDDCBlock(). Only
		// the boot display is scanned out, and that is AUX0/DP0 on this board.
		if (edidLen == 0) {
			memcpy(edidData, edid, 128);
			edidLen = 128;
			sinkAuxInst = inst;
			sinkAuxValid = true;
			// One CTA extension block is the norm on EDID 1.4 sinks; fetch it
			// so the OS sees the full timing/audio capabilities.
			if (edid[126] > 0 &&
			    readEDID(inst, edidData + 128, 128, 128)) {
				edidLen = 256;
				FBLOG("edid: connector %zu (AUX%u): read extension block "
				      "(tag 0x%02x)", i, inst, edidData[128]);
			}
		}
	}
	if (!any)
		FBLOG("edid: no DisplayPort EDID read (HDMI DDC over I2C not yet supported)");
}

void RX9070XTFB::setDisplayPower(bool on) {
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

void RX9070XTFB::initForPM() {
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

	uint32_t nosleep = 0;
	if (PE_parse_boot_argn("rx9070xt-nosleep", &nosleep, sizeof(nosleep)) && nosleep != 0) {
		displaySleepEnabled = false;
		FBLOG("start: display sleep disabled by rx9070xt-nosleep");
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
		probeEDID();
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

IOReturn RX9070XTFB::setAttribute(IOSelect attribute, uintptr_t value) {
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
		case kConnectionSupportsHLDDCSense:
			// Success (no value) tells IODisplay it may call hasDDCConnect()
			// and getDDCBlock() for the real EDID.
			return edidLen ? kIOReturnSuccess : kIOReturnUnsupported;
		default:
			return super::getAttributeForConnection(connectIndex, attribute, value);
	}
}

IOReturn RX9070XTFB::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
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

bool RX9070XTFB::hasDDCConnect(IOIndex connectIndex) {
	return connectIndex == 0 && edidLen != 0;
}

IOReturn RX9070XTFB::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
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
