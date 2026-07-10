//
//  RX9070XTFB.hpp
//  RX9070XT
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

#ifndef RX9070XTFB_hpp
#define RX9070XTFB_hpp

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>

#include "AtomBios.hpp"
#include "IpDiscovery.hpp"

// Single, driver-defined display mode id. Must be in 0x1..0x7fffffff.
#define kRX9070XTDisplayModeID  ((IODisplayModeID)1)

class RX9070XTFB : public IOFramebuffer {
	OSDeclareDefaultStructors(RX9070XTFB)

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
	// "rx9070xt-cmap=N" (no leading dash) while we pin down the right order.
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
	void probeMemSize();
	// Read-only: log the DCN output-pipe colour registers (surface format,
	// output CSC, output formatter) so the channel-rotation/blue-offset can
	// be located before any register is written. Writes nothing to hardware.
	void dumpDCN();

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
	IOItemCount getConnectionCount() override;
	IOReturn    getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
	                                      uintptr_t *value) override;
	IOReturn    setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
	                                      uintptr_t value) override;

	// Report that this framebuffer's console is already usable so IOGraphics
	// does not attempt to reprogram hardware we cannot drive.
	bool isConsoleDevice() override;
};

#endif /* RX9070XTFB_hpp */
