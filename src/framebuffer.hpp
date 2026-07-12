//
//  framebuffer.hpp
//  RDNA4FB
//
//  A non-accelerated IOFramebuffer driver for the AMD Radeon RX 9070 XT
//  (Navi 48 / RDNA 4, PCI 0x1002:0x7550).
//
//  This driver does NOT talk to the GPU's command processor or display
//  controller directly. Instead it adopts the linear framebuffer that the
//  firmware / bootloader (OpenCore GOP) has already programmed and that XNU
//  exposes through PE_state.video (IOPlatformExpert::getConsoleInfo).
//
//  It presents that buffer to WindowServer as a single fixed display mode so
//  the OS can composite the desktop in software. There is no Metal, no 2D/3D
//  acceleration and no mode switching — the point is simply to reach a usable
//  desktop on hardware macOS otherwise has no driver for.
//

#ifndef RDNA4FB_hpp
#define RDNA4FB_hpp

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <pexpert/pexpert.h>

#include "atombios.hpp"
#include "ipdiscovery.hpp"

// Single, driver-defined display mode id. Must be in 0x1..0x7fffffff.
#define kRDNA4DisplayModeID  ((IODisplayModeID)1)

class RDNA4FB : public IOFramebuffer {
	OSDeclareDefaultStructors(RDNA4FB)

	using super = IOFramebuffer;

	// PCI provider (kept for register access / property injection later).
	IOPCIDevice *pciDevice { nullptr };

	// Console framebuffer geometry taken from PE_state.video.
	IOPhysicalAddress64 fbPhysBase { 0 };
	uint64_t            fbLength   { 0 };
	uint32_t            fbWidth    { 0 };
	uint32_t            fbHeight   { 0 };
	uint32_t            fbRowBytes { 0 };
	uint32_t            fbDepth    { 32 };

	// Channel-order compensation. The Navi 48 DCN scanout maps framebuffer
	// bytes to panel guns in an order that does not match the ARGB layout we
	// advertise, so WindowServer's pixels come out channel-rotated. Since
	// WindowServer writes bytes per the masks we report, permuting those
	// masks pre-compensates. Selectable at runtime via boot-arg
	// "rdna4-cmap=N" (no leading dash) while we pin down the right order.
	//   0 = R:byte2 G:byte1 B:byte0  (standard ARGB, default)
	//   1 = R:byte1 G:byte0 B:byte2  (cancels observed (G,B,R) rotation)
	//   2 = R:byte0 G:byte1 B:byte2  (BGR swap)
	//   3 = R:byte2 G:byte0 B:byte1
	//   4 = R:byte1 G:byte2 B:byte0
	//   5 = R:byte0 G:byte2 B:byte1
	uint32_t channelMap { 0 };

	// VBIOS image (owned copy) and its parsed view. Populated by loadVBIOS();
	// absence is non-fatal — the framebuffer works without it, but connector
	// knowledge is needed for future native mode setting.
	uint8_t  *vbiosData { nullptr };
	size_t    vbiosSize { 0 };
	AtomBios  atomBios;

	// Register bases from the IP discovery binary (present when the full
	// 2 MiB flash dump is injected as ATY,bin_image; the PSP region holding
	// it is not visible through the expansion ROM).
	IpDiscovery ipDiscovery;

	// Register MMIO aperture (PCI BAR5). Mapped read-only usage for now:
	// the only access is the RCC_CONFIG_MEMSIZE smoke-test read.
	IOMemoryMap       *rmmioMap { nullptr };
	volatile uint32_t *rmmio    { nullptr };
	size_t             rmmioSize { 0 };

	bool captureConsoleInfo();
	bool loadVBIOS();
	bool copyVBIOSFromProperty();
	bool copyVBIOSFromExpansionROM();
	void publishVBIOSInfo();
	void freeVBIOS();

	bool mapRegisters();
	void unmapRegisters();
	// Bounds-checked 32-bit MMIO read; returns 0xFFFFFFFF (all-ones, the PCIe
	// master-abort pattern) when unmapped or out of range.
	uint32_t regRead32(uint32_t byteOffset) const;
	// Read a DCN/DMU register by IP base-segment index + dword offset, using
	// the segment bases from the IP discovery table. Returns 0xFFFFFFFF if
	// discovery is unavailable or the address is out of range.
	uint32_t regReadDmu(uint8_t baseIdx, uint32_t dwordOffset) const;
	// Write counterpart; returns false (and writes nothing) if discovery is
	// unavailable or the address is out of range.
	bool regWriteDmu(uint8_t baseIdx, uint32_t dwordOffset, uint32_t value);
	// Experiment (boot-arg "rdna4-8bpc=1"): switch the active DP stream
	// from 10 bpc to 8 bpc and update the MSA to match, testing whether the
	// monitor's colour processing misclassifies the GOP's 10 bpc SDR signal.
	// Bandwidth-reducing, so the link cannot overflow; a reboot restores the
	// firmware configuration.
	void tryForce8bpc();
	void probeMemSize();
	// Read-only: log the DCN output-pipe colour registers (surface format,
	// output CSC, output formatter) so the channel-rotation/blue-offset can
	// be located before any register is written. Writes nothing to hardware.
	void dumpDCN();

	// DP AUX software engine (BAR5 MMIO, one block per connector). This is the
	// path to reading a sink's EDID — and later link/mode setting — without an
	// accelerator. The register layout and programming sequence follow Navi 48's
	// dcn_4_1_0 headers and amdgpu's dce_aux.c, which are unchanged across DCE
	// and DCN generations.

	// dword offset (all AUX regs are IP base_idx 2) of AUX register `reg`
	// within engine instance `inst`.
	uint32_t auxDword(uint8_t inst, uint32_t reg) const;
	// Perform one AUX request on engine `inst`. `action` is an
	// I2CAUX_TRANSACTION_ACTION_* value; for writes `data`/`len` are the
	// payload, for reads `len` is the byte count requested and the reply lands
	// in `reply` (up to `replyCap`, actual count in `*replyBytes`). Returns the
	// AUX reply code (0 == ACK, 0x2/0x8 == AUX/I2C defer, else NACK) or -1 on
	// engine error / HPD-low / timeout. Touches only the AUX engine registers.
	int auxTransaction(uint8_t inst, uint8_t action, uint32_t address,
	                   const uint8_t *data, uint8_t len,
	                   uint8_t *reply, uint8_t replyCap, uint8_t *replyBytes);
	// Read `count` bytes of EDID over I2C-over-AUX (DDC slave 0x50) from the
	// sink on engine `inst`, starting at EDID byte `start` (0 for the base
	// block, 128 for the first extension). Returns true and fills `edid`.
	bool readEDID(uint8_t inst, uint8_t *edid, size_t count, uint8_t start);
	// Same, over the DDC hardware I2C engine (DC_I2C block) — the EDID path
	// for HDMI/DVI sinks, which have no AUX channel. `line` is the AtomBIOS
	// ddc-line (selects DC_I2C_DDC<line+1> and DDC_SELECT). Queues the
	// offset write and the block read as two transactions in one GO, per
	// amdgpu's dce_i2c_hw.c.
	bool readEDIDI2C(uint8_t line, uint8_t *edid, size_t count, uint8_t start);
	// Boot-arg "rdna4-modedump=1": read-only survey of the mode-setting
	// register landscape — every OTG's timing/enable state, every DIG
	// front/back-end, the HUBP surface addresses and the DCCG clock muxes.
	// The active DP pipe is the GOP-programmed reference template for
	// bringing up the HDMI pipe; the dump shows what "lit" looks like
	// versus dormant.
	void dumpModeState();
	// Probe each DisplayPort connector's AUX engine for an EDID, validate and
	// publish it, and cache the first hit for the DDC API below. Issues only
	// AUX transactions, never reprograms scanout. Runs by default (verified on
	// hardware 2026-07-11); "rdna4-noedid=1" skips it.
	void probeEDID();

	// EDID served to IODisplay via hasDDCConnect()/getDDCBlock(): base block
	// plus one CTA extension if present. 0 length = no DDC sink found.
	uint8_t edidData[256] {};
	size_t  edidLen { 0 };
	// AUX engine of the sink whose EDID we cached (the boot display) — the
	// target for DPCD power writes.
	uint8_t sinkAuxInst  { 0 };
	bool    sinkAuxValid { false };

	// Display power (DPMS). Off = disable the DP video stream (DP0, the boot
	// pipe) and put the sink in D3 via a native-AUX DPCD SET_POWER write; on
	// reverses both. Restores exactly the bits it cleared — DCN state, clocks
	// and the timing generator are untouched, so this cannot lose the desktop
	// short of full GPU power loss. "rdna4-nosleep=1" reverts to the old
	// always-on behaviour.
	bool displayPowerOn      { true };
	bool displaySleepEnabled { true };
	void setDisplayPower(bool on);
	// Register this framebuffer's power states (sleep/doze/wake) with power
	// management. IOFramebuffer::start only does PMinit + joinPMtree — the
	// subclass must call registerPowerDriver itself (IONDRVFramebuffer does
	// this in its initForPM), otherwise PM has no states to change and
	// setPowerState / kIOPowerAttribute are never delivered. This was why
	// display sleep did nothing. Must run from enableController() — the
	// framebuffer must be open before PM starts delivering state changes.
	void initForPM();
	bool pmRegistered { false };

	// Hardware cursor (the DCN cursor plane on the boot pipe). The sprite is
	// overlaid during scanout, so cursor moves cost two register writes and
	// stay smooth regardless of compositing load — the fix for the laggy
	// software cursor under heavy repaint (Chrome). The sprite lives in VRAM
	// directly after the GOP framebuffer; its GPU address comes from the
	// scanout address in HUBPREQ0 plus the same delta applied to the CPU
	// aperture. Opt-in via rdna4-hwcursor=1 until hardware-verified.
	bool initHWCursor();
	bool hwCursorRequested { false };
	bool hwCursorReady     { false };
	bool hwCursorVisible   { false };
	// CURSOR_MODE (dc_cursor_color_format): 2 = premultiplied ARGB
	// (default), 3 = straight alpha. If the pointer renders with dark or
	// bright fringes, flip with rdna4-curmode=3. (0/1 are mono formats and
	// never correct for a converted macOS cursor.)
	uint32_t hwCursorMode  { 2 };
	IOMemoryMap       *cursorMap  { nullptr };
	volatile uint32_t *cursorVram { nullptr };
	uint32_t *cursorStage { nullptr };  // convertCursorImage staging buffer
	uint64_t  cursorMcAddr { 0 };       // GPU (MC) address of the sprite
	uint64_t  scanoutMcAddr { 0 };      // GPU (MC) address of the framebuffer
	// rdna4-curtest=1: fetch the cursor sprite from the scanout base instead
	// of the private slot. The scanout is proven-fetchable memory with 0xFF
	// alpha, so a visible floating square = cursor engine works and the bug
	// is sprite addressing; still invisible = composition-stage problem.
	bool      hwCursorTest { false };
	// CURSOR_CONTROL image built by setCursorImage (mode/pitch/lines-per-
	// chunk); setCursorState ORs the enable bit in.
	uint32_t  cursorCtlBase { 0 };
	// Bounded diagnostics for cursor bring-up: log the first few calls and
	// register readbacks so an invisible pointer is debuggable from dmesg.
	uint8_t   cursorImgLogs { 0 };
	uint8_t   cursorPosLogs { 0 };
	uint32_t  cursorPosCalls { 0 };

	// Emulated vertical-blank "interrupt" (rdna4-vbl=1). IOFramebuffer
	// only engages its frame-pacing machinery (CVDisplayLink timestamps,
	// deferred cursor moves, vbl throttling) if the subclass provides VBL
	// service via registerForInterruptType — which we cannot do from real
	// hardware without an IH-ring interrupt handler. A workloop timer at the
	// EDID refresh period is the standard substitute for a scanout-less
	// driver and gives WindowServer a steady heartbeat to pace against.
	bool               vblRequested { false };
	IOFBInterruptProc  vblProc   { nullptr };
	OSObject          *vblTarget { nullptr };
	void              *vblRef    { nullptr };
	IOTimerEventSource *vblTimer { nullptr };
	uint32_t           vblPeriodUS { 16667 };  // refined from the sink's EDID
	bool               vblEnabled { false };
	void fireVBL(IOTimerEventSource *sender);

public:
	// IOService
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool       start(IOService *provider) override;
	void       stop(IOService *provider) override;

	// IOFramebuffer — hardware bring-up
	IOReturn enableController() override;

	// IOFramebuffer — memory
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
	IODeviceMemory *getVRAMRange() override;

	// IOFramebuffer — pixel / mode description
	const char *getPixelFormats() override;
	IOItemCount getDisplayModeCount() override;
	IOReturn    getDisplayModes(IODisplayModeID *allDisplayModes) override;
	IOReturn    getInformationForDisplayMode(IODisplayModeID displayMode,
	                                          IODisplayModeInformation *info) override;
	UInt64      getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
	                                          IOIndex depth) override;
	IOReturn    getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
	                                IOPixelAperture aperture,
	                                IOPixelInformation *pixelInfo) override;

	// IOFramebuffer — current mode
	IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
	IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;

	// IOFramebuffer — attributes / connection
	IOReturn    getAttribute(IOSelect attribute, uintptr_t *value) override;
	// Receives kIOPowerAttribute — IOFramebuffer's mechanism for asking the
	// subclass to carry out a power state change (0 off, 1 doze, 2 on).
	IOReturn    setAttribute(IOSelect attribute, uintptr_t value) override;
	IOItemCount getConnectionCount() override;
	IOReturn    getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
	                                      uintptr_t *value) override;
	IOReturn    setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
	                                      uintptr_t value) override;

	// IOFramebuffer — DDC/EDID (serves the block cached by probeEDID so the
	// OS sees the real display identity instead of a generic panel)
	bool     hasDDCConnect(IOIndex connectIndex) override;
	IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
	                     IOSelect blockType, IOOptionBits options,
	                     UInt8 *data, IOByteCount *length) override;

	// IOFramebuffer — hardware cursor
	IOReturn setCursorImage(void *cursorImage) override;
	IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;

	// IOFramebuffer — interrupts (timer-emulated VBL)
	IOReturn registerForInterruptType(IOSelect interruptType,
	                                  IOFBInterruptProc proc, OSObject *target,
	                                  void *ref, void **interruptRef) override;
	IOReturn unregisterInterrupt(void *interruptRef) override;
	IOReturn setInterruptState(void *interruptRef, UInt32 state) override;

	// Report that this framebuffer's console is already usable so IOGraphics
	// does not attempt to reprogram hardware we cannot drive.
	bool isConsoleDevice() override;
};

#endif /* RDNA4FB_hpp */
