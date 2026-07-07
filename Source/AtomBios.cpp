//
//  AtomBios.cpp
//  RX9070XT
//
//  Freestanding AtomBIOS parser — see AtomBios.hpp for scope and the ROM the
//  layout was validated against.
//

#include "AtomBios.hpp"

// Offsets within the legacy VBIOS image (validated against the Navi 48 ROM).
// Image signature 0x55 0xAA sits at offset 0x00.
static constexpr size_t kRomSizeBlocksOffset  = 0x02;   // length in 512-byte blocks
static constexpr size_t kAtomRomHeaderPointer = 0x48;   // u16 -> atom_rom_header
// atom_rom_header_v2_x field offsets (relative to the header):
static constexpr size_t kAtomSignatureOffset  = 0x04;   // "ATOM"
static constexpr size_t kConfigFilenameOffset = 0x0c;   // u16 image-relative ptr
static constexpr size_t kSubsysVidOffset      = 0x18;
static constexpr size_t kSubsysIdOffset       = 0x1a;
static constexpr size_t kMasterDataTableOffset= 0x20;   // u16 image-relative ptr

bool AtomBios::readU8(size_t off, uint8_t &v) const {
	if (off >= size) return false;
	v = data[off];
	return true;
}

bool AtomBios::readU16(size_t off, uint16_t &v) const {
	if (off + 2 > size) return false;
	v = static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
	return true;
}

bool AtomBios::readU32(size_t off, uint32_t &v) const {
	if (off + 4 > size) return false;
	v = static_cast<uint32_t>(data[off]) |
	    (static_cast<uint32_t>(data[off + 1]) << 8) |
	    (static_cast<uint32_t>(data[off + 2]) << 16) |
	    (static_cast<uint32_t>(data[off + 3]) << 24);
	return true;
}

bool AtomBios::readTableHeader(size_t absOffset, TableHeader &out) const {
	uint16_t tsize; uint8_t frev, crev;
	if (!readU16(absOffset, tsize) || !readU8(absOffset + 2, frev) || !readU8(absOffset + 3, crev))
		return false;
	if (tsize < 4 || absOffset + tsize > size)
		return false;
	out.size = tsize;
	out.formatRev = frev;
	out.contentRev = crev;
	return true;
}

bool AtomBios::validateImage(size_t offset) {
	uint8_t sig0, sig1, blocks;
	if (!readU8(offset, sig0) || !readU8(offset + 1, sig1) || sig0 != 0x55 || sig1 != 0xAA)
		return false;
	if (!readU8(offset + kRomSizeBlocksOffset, blocks) || blocks == 0)
		return false;

	size_t imgLen = static_cast<size_t>(blocks) * 512;
	if (offset + imgLen > size)
		return false;

	// Legacy option-ROM checksum: all image bytes sum to 0 (mod 256).
	uint8_t sum = 0;
	for (size_t i = 0; i < imgLen; i++)
		sum = static_cast<uint8_t>(sum + data[offset + i]);
	if (sum != 0)
		return false;

	// Follow the ATOM ROM header pointer and check the "ATOM" signature.
	uint16_t hdr;
	if (!readU16(offset + kAtomRomHeaderPointer, hdr) || hdr == 0)
		return false;
	size_t atomHdr = offset + hdr;
	uint8_t a, t, o, m;
	if (!readU8(atomHdr + kAtomSignatureOffset, a) || !readU8(atomHdr + kAtomSignatureOffset + 1, t) ||
	    !readU8(atomHdr + kAtomSignatureOffset + 2, o) || !readU8(atomHdr + kAtomSignatureOffset + 3, m) ||
	    a != 'A' || t != 'T' || o != 'O' || m != 'M')
		return false;

	TableHeader th;
	if (!readTableHeader(atomHdr, th))
		return false;

	uint16_t mdt;
	if (!readU16(atomHdr + kMasterDataTableOffset, mdt) || mdt == 0)
		return false;
	TableHeader mdtHdr;
	if (!readTableHeader(offset + mdt, mdtHdr) || mdtHdr.formatRev != 2)
		return false;

	readU16(atomHdr + kSubsysVidOffset, subsysVid);
	readU16(atomHdr + kSubsysIdOffset, subsysId);

	base = offset;
	length = imgLen;
	masterDataTable = offset + mdt;
	return true;
}

bool AtomBios::init(const uint8_t *romData, size_t romSize) {
	data = romData;
	size = romSize;
	valid = false;
	if (!data || size < 512)
		return false;

	// Fast path: raw legacy image at offset 0.
	if (validateImage(0)) {
		valid = true;
		return true;
	}

	// Unified PSP-wrapped ROM: scan 512-byte boundaries for an embedded
	// image (this card's primary lives at 0x40000, backup at 0x120000).
	for (size_t off = 512; off + 512 <= size; off += 512) {
		if (data[off] == 0x55 && data[off + 1] == 0xAA && validateImage(off)) {
			valid = true;
			return true;
		}
	}
	return false;
}

size_t AtomBios::configName(char *out, size_t outLen) const {
	if (!valid || !out || outLen == 0)
		return 0;

	uint16_t hdrPtr, namePtr;
	if (!readU16(base + kAtomRomHeaderPointer, hdrPtr) ||
	    !readU16(base + hdrPtr + kConfigFilenameOffset, namePtr) || namePtr == 0)
		return 0;

	size_t src = base + namePtr;
	size_t n = 0;
	while (n + 1 < outLen) {
		uint8_t c;
		if (!readU8(src + n, c) || c == 0)
			break;
		out[n++] = static_cast<char>(c);
	}
	// Trim trailing spaces (the ROM pads with blanks: "NAVI48.bin  ").
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
	return n;
}

size_t AtomBios::dataTable(DataTable table, TableHeader *header) const {
	if (!valid || table >= MaxDataTable)
		return 0;

	TableHeader mdtHdr;
	if (!readTableHeader(masterDataTable, mdtHdr))
		return 0;
	size_t entries = (mdtHdr.size - 4) / 2;
	if (table >= entries)
		return 0;

	uint16_t rel;
	if (!readU16(masterDataTable + 4 + 2 * table, rel) || rel == 0)
		return 0;

	size_t abs = base + rel;
	TableHeader th;
	if (!readTableHeader(abs, th))
		return 0;
	if (header)
		*header = th;
	return abs;
}

bool AtomBios::getFirmwareInfo(FirmwareInfo3 &out) const {
	TableHeader th;
	size_t off = dataTable(FirmwareInfo, &th);
	if (!off || th.formatRev != 3 || th.size < 20)
		return false;

	out.header = th;
	return readU32(off + 4,  out.firmwareRevision) &&
	       readU32(off + 8,  out.bootupSclk10KHz) &&
	       readU32(off + 12, out.bootupMclk10KHz) &&
	       readU32(off + 16, out.firmwareCapability);
}

size_t AtomBios::getDisplayPaths(DisplayPath *paths, size_t maxPaths) const {
	if (!paths || maxPaths == 0)
		return 0;

	TableHeader th;
	size_t off = dataTable(DisplayObjectInfo, &th);
	// display_object_info_table_v1_5: header, u16 supporteddevices,
	// u8 number_of_path, u8 reserved, then 16-byte path entries.
	if (!off || th.formatRev != 1 || th.contentRev < 4 || th.size < 8)
		return 0;

	uint8_t npaths;
	if (!readU8(off + 6, npaths))
		return 0;
	if (npaths > MaxDisplayPaths)
		npaths = MaxDisplayPaths;

	size_t count = 0;
	for (size_t i = 0; i < npaths && count < maxPaths; i++) {
		size_t p = off + 8 + i * 16;
		if (p + 16 > off + th.size)
			break;
		DisplayPath dp {};
		if (!readU16(p + 0,  dp.connectorObjId) ||
		    !readU16(p + 2,  dp.recordOffset) ||
		    !readU16(p + 4,  dp.encoderObjId) ||
		    !readU16(p + 12, dp.deviceTag))
			break;
		if (dp.connectorObjId == 0)
			continue;
		paths[count++] = dp;
	}
	return count;
}

bool AtomBios::getPathRecords(const DisplayPath &path, PathRecords &out) const {
	out = PathRecords {};

	TableHeader th;
	size_t table = dataTable(DisplayObjectInfo, &th);
	if (!table || path.recordOffset == 0 || path.recordOffset >= th.size)
		return false;

	// Record chain: atom_common_record_header {u8 type; u8 size;} followed by
	// the body; terminated by type 0/0xff. Verified types on this ROM:
	// I2C (1): u8 i2c_id, u8 i2c_slave_addr
	// HPD (2): u8 pin_id, u8 plugin_pin_state
	size_t p = table + path.recordOffset;
	size_t end = table + th.size;
	while (p + 2 <= end) {
		uint8_t rtype, rsize;
		if (!readU8(p, rtype) || !readU8(p + 1, rsize))
			return false;
		if (rtype == 0 || rtype == 0xFF || rsize < 2 || p + rsize > end)
			break;

		if (rtype == 1 && rsize >= 4) {           // ATOM_I2C_RECORD_TYPE
			uint8_t id;
			if (readU8(p + 2, id)) {
				out.hasI2c = true;
				out.i2cId = id;
				out.i2cHwCapable = (id & 0x80) != 0;
				out.ddcLine = id & 0x0F;
			}
		} else if (rtype == 2 && rsize >= 4) {    // ATOM_HPD_INT_RECORD_TYPE
			uint8_t pin, state;
			if (readU8(p + 2, pin) && readU8(p + 3, state)) {
				out.hasHpd = true;
				out.hpdPin = pin;
				out.hpdPlugState = state;
			}
		}
		p += rsize;
	}
	return out.hasI2c || out.hasHpd;
}

size_t AtomBios::getGpioPins(GpioPin *pins, size_t maxPins) const {
	if (!pins || maxPins == 0)
		return 0;

	TableHeader th;
	size_t off = dataTable(GpioPinLut, &th);
	if (!off || th.formatRev != 2 || th.size < 4 + 8)
		return 0;

	// atom_gpio_pin_assignment: u32 data_a_reg_index, u8 gpio_bitshift,
	// u8 gpio_mask_bitshift, u8 gpio_id, u8 reserved — 8 bytes each.
	size_t npins = (th.size - 4u) / 8;
	size_t count = 0;
	for (size_t i = 0; i < npins && count < maxPins; i++) {
		size_t p = off + 4 + i * 8;
		GpioPin pin {};
		uint8_t resv;
		if (!readU32(p, pin.regIndex) || !readU8(p + 4, pin.shift) ||
		    !readU8(p + 5, pin.maskShift) || !readU8(p + 6, pin.gpioId) ||
		    !readU8(p + 7, resv))
			break;
		if (pin.gpioId == 0)
			continue;
		pins[count++] = pin;
	}
	return count;
}

bool AtomBios::findGpioPin(uint8_t gpioId, GpioPin &out) const {
	GpioPin pins[MaxGpioPins];
	size_t n = getGpioPins(pins, MaxGpioPins);
	for (size_t i = 0; i < n; i++) {
		if (pins[i].gpioId == gpioId) {
			out = pins[i];
			return true;
		}
	}
	return false;
}

AtomBios::ConnectorType AtomBios::connectorType(uint16_t connectorObjId) {
	// Object id: bits 12-14 = object type (3 = connector), bits 8-11 = enum
	// instance, low byte = connector object id (atombios object ids).
	if (((connectorObjId >> 12) & 0x7) != 3)
		return ConnectorUnknown;
	switch (connectorObjId & 0xFF) {
		case 0x13: return ConnectorDP;
		case 0x0C: return ConnectorHDMIA;
		case 0x02:
		case 0x04: return ConnectorDVID;
		case 0x05: return ConnectorVGA;
		case 0x17: return ConnectorUSBC;
		default:   return ConnectorUnknown;
	}
}

const char *AtomBios::connectorName(ConnectorType type) {
	switch (type) {
		case ConnectorDP:    return "DisplayPort";
		case ConnectorHDMIA: return "HDMI-A";
		case ConnectorDVID:  return "DVI-D";
		case ConnectorVGA:   return "VGA";
		case ConnectorUSBC:  return "USB-C";
		default:             return "Unknown";
	}
}
