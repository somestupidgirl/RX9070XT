//
//  AtomBios.hpp
//  RX9070XT
//
//  Minimal, bounds-checked AtomBIOS (atomfirmware-era) parser for the data
//  tables needed to eventually drive Navi 48's DCN display engine.
//
//  Deliberately freestanding: no IOKit, no libkern, no allocation — it only
//  reads from a caller-provided buffer. The same code compiles in the kext
//  and in the host-side `atomdump` tool, which `make test` runs against the
//  real ROM in firmware/ so the parsing logic is verified without hardware.
//
//  Layout facts below were validated byte-for-byte against
//  firmware/Sapphire.RX9070XT.16384.241213.rom (Navi 48, ATOM header rev 2.3,
//  master data table v2.1 with 35 entries) and cross-checked with Linux
//  drivers/gpu/drm/amd/include/atomfirmware.h.
//

#ifndef AtomBios_hpp
#define AtomBios_hpp

#include <stdint.h>
#include <stddef.h>

class AtomBios {
public:
	// Indices into the v2.1 master list of data tables (atomfirmware.h).
	enum DataTable : uint32_t {
		UtilityPipeline      = 0,
		MultimediaInfo       = 1,
		SmcDpmInfo           = 2,
		FirmwareInfo         = 4,
		LcdInfo              = 6,
		SmuInfo              = 8,
		VramUsageByFirmware  = 11,
		GpioPinLut           = 12,
		GfxInfo              = 14,
		PowerPlayInfo        = 15,
		DisplayObjectInfo    = 22,
		IndirectIoAccess     = 23,
		UmcInfo              = 24,
		DceInfo              = 26,
		VramInfo             = 27,
		VoltageObjectInfo    = 31,
		MaxDataTable         = 35,
	};

	// atom_common_table_header
	struct TableHeader {
		uint16_t size;
		uint8_t  formatRev;
		uint8_t  contentRev;
	};

	// Subset of atom_firmware_info_v3_x shared by v3.1..v3.5. Bootup clocks
	// may legitimately be 0 on SMU-managed boards (they are on this ROM).
	struct FirmwareInfo3 {
		TableHeader header;
		uint32_t firmwareRevision;
		uint32_t bootupSclk10KHz;
		uint32_t bootupMclk10KHz;
		uint32_t firmwareCapability;
	};

	// One display path from display_object_info_table_v1_5.
	// atom_display_object_path_v3 is 16 bytes:
	//   u16 display_objid, u16 disp_recordoffset, u16 encoderobjid,
	//   u16 reserved[3], u16 device_tag, u16 reserved
	struct DisplayPath {
		uint16_t connectorObjId;
		uint16_t recordOffset;
		uint16_t encoderObjId;
		uint16_t deviceTag;
	};

	// Per-connector wiring decoded from a path's record chain
	// (atom_i2c_record + atom_hpd_int_record). ddcLine selects which DDC/AUX
	// pair services the connector; hpdPin is the hot-plug detect pin id.
	// Both index into the GPIO pin LUT (gpio_id 0x90+line for DDC, pin id
	// for HPD).
	struct PathRecords {
		bool    hasI2c { false };
		uint8_t i2cId  { 0 };         // raw i2c_id byte
		bool    i2cHwCapable { false }; // bit 7 of i2c_id
		uint8_t ddcLine { 0 };        // i2c_id & 0x0f
		bool    hasHpd { false };
		uint8_t hpdPin { 0 };
		uint8_t hpdPlugState { 0 };
	};

	// One atom_gpio_pin_assignment from gpio_pin_lut v2.1 (8 bytes each).
	// regIndex is a dword register index (byte address = regIndex * 4).
	struct GpioPin {
		uint32_t regIndex;
		uint8_t  shift;
		uint8_t  maskShift;
		uint8_t  gpioId;
	};

	static constexpr size_t MaxGpioPins = 32;

	enum ConnectorType : uint8_t {
		ConnectorUnknown = 0,
		ConnectorDP,        // object id low byte 0x13
		ConnectorHDMIA,     // object id low byte 0x0c
		ConnectorDVID,      // 0x02 / 0x04
		ConnectorVGA,       // 0x05
		ConnectorUSBC,      // 0x17
	};

	static constexpr size_t MaxDisplayPaths = 8;

	// Locate and validate a VBIOS image inside `romData`. Handles both a raw
	// legacy image (0x55AA at offset 0) and AMD's unified PSP-wrapped ROM
	// (this card: 2 MiB file, image at 0x40000 with backup at 0x120000).
	// Returns false if no image with a valid ATOM signature+checksum exists.
	bool init(const uint8_t *romData, size_t romSize);

	bool isValid() const { return valid; }

	// Offset of the VBIOS image within the buffer passed to init().
	size_t   imageOffset() const { return base; }
	// Image length from the 512-byte-block count at image offset 2.
	size_t   imageLength() const { return length; }

	uint16_t subsystemVendorId() const { return subsysVid; }
	uint16_t subsystemId()       const { return subsysId; }

	// NUL-terminated config-file name (e.g. "NAVI48.bin"); returns bytes
	// copied (0 on failure). Trailing spaces are trimmed.
	size_t configName(char *out, size_t outLen) const;

	// Absolute offset (within the init() buffer) of a data table, or 0 if
	// the table is absent. `header` receives the table header when present.
	size_t dataTable(DataTable table, TableHeader *header = nullptr) const;

	bool getFirmwareInfo(FirmwareInfo3 &out) const;

	// Fills `paths` (up to maxPaths) from displayobjectinfo v1.5; returns the
	// number of display paths, or 0 when the table is absent/unsupported.
	size_t getDisplayPaths(DisplayPath *paths, size_t maxPaths) const;

	// Walks a path's record chain (recordOffset is relative to the
	// displayobjectinfo table) and extracts the I2C/AUX and HPD assignments.
	bool getPathRecords(const DisplayPath &path, PathRecords &out) const;

	// gpio_pin_lut v2.1 accessors; findGpioPin looks up one id (e.g.
	// 0x90 + ddcLine for a DDC/AUX pair, or an HPD pin id).
	size_t getGpioPins(GpioPin *pins, size_t maxPins) const;
	bool   findGpioPin(uint8_t gpioId, GpioPin &out) const;

	// --- Command functions (AtomBIOS bytecode) -----------------------------
	// The master command function table lists the bytecode routines the VBIOS
	// exposes; amdgpu executes these for PHY/PLL work (SetPixelClock,
	// transmitter control) instead of programming those blocks directly.
	// Indices from atomfirmware.h atom_master_list_of_command_functions_v2_1.
	enum CmdFunction : uint8_t {
		CmdAsicInit               = 0,
		CmdDigEncoderControl      = 4,
		CmdSetPixelClock          = 12,
		CmdEnableDispPowerGating  = 13,
		CmdBlankCrtc              = 34,
		CmdEnableCrtc             = 35,
		CmdSelectCrtcSource       = 42,
		CmdSetDceClock            = 46,
		CmdSetCrtcUsingDtdTiming  = 49,
		CmdDig1TransmitterControl = 76,
		CmdProcessAuxChannel      = 78,
		CmdFunctionCount          = 81,
	};

	struct CmdTableInfo {
		size_t   offset;      // absolute offset of the bytecode blob
		uint16_t size;        // blob size incl. header
		uint8_t  formatRev;
		uint8_t  contentRev;
	};

	// Look up one command function; returns false if the VBIOS does not
	// implement it (offset 0) or the entry is out of bounds.
	bool getCommandTable(uint8_t index, CmdTableInfo &out) const;

	static ConnectorType connectorType(uint16_t connectorObjId);
	static const char   *connectorName(ConnectorType type);

private:
	const uint8_t *data { nullptr };
	size_t size   { 0 };
	size_t base   { 0 };      // VBIOS image offset in data
	size_t length { 0 };      // VBIOS image length
	size_t masterDataTable { 0 };  // absolute offset of master data table
	size_t masterCmdTable  { 0 };  // absolute offset of master command table
	uint16_t subsysVid { 0 };
	uint16_t subsysId  { 0 };
	bool valid { false };

	bool validateImage(size_t offset);
	bool readTableHeader(size_t absOffset, TableHeader &out) const;

	// Bounds-checked little-endian readers (absolute offsets).
	bool readU8 (size_t off, uint8_t  &v) const;
	bool readU16(size_t off, uint16_t &v) const;
	bool readU32(size_t off, uint32_t &v) const;
};

#endif /* AtomBios_hpp */
