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

	bool captureConsoleInfo();

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
