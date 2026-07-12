//
//  Edid.cpp
//  RX9070XT
//
//  DTD field packing per VESA E-EDID 1.4 §3.10.2.
//

#include "Edid.hpp"

namespace Edid {

bool parseDetailedTiming(const uint8_t d[18], DetailedTiming &out) {
	uint32_t pclk10k = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8);
	if (pclk10k == 0)
		return false;   // a display descriptor, not a timing

	out.pixelClockKHz = pclk10k * 10;
	out.hActive = static_cast<uint16_t>(d[2] | ((d[4] & 0xf0) << 4));
	out.hBlank  = static_cast<uint16_t>(d[3] | ((d[4] & 0x0f) << 8));
	out.vActive = static_cast<uint16_t>(d[5] | ((d[7] & 0xf0) << 4));
	out.vBlank  = static_cast<uint16_t>(d[6] | ((d[7] & 0x0f) << 8));
	out.hSyncOffset = static_cast<uint16_t>(d[8]  | ((d[11] & 0xc0) << 2));
	out.hSyncWidth  = static_cast<uint16_t>(d[9]  | ((d[11] & 0x30) << 4));
	out.vSyncOffset = static_cast<uint16_t>((d[10] >> 4)  | ((d[11] & 0x0c) << 2));
	out.vSyncWidth  = static_cast<uint16_t>((d[10] & 0xf) | ((d[11] & 0x03) << 4));
	out.interlaced  = (d[17] & 0x80) != 0;

	// Sync polarity bits are only defined for digital separate sync
	// (d[17] bits [4:3] == 11); analog/composite variants report positive.
	if ((d[17] & 0x18) == 0x18) {
		out.hSyncPositive = (d[17] & 0x02) != 0;
		out.vSyncPositive = (d[17] & 0x04) != 0;
	} else {
		out.hSyncPositive = true;
		out.vSyncPositive = true;
	}

	if (out.hActive == 0 || out.vActive == 0)
		return false;
	return true;
}

bool preferredTiming(const uint8_t *edid, size_t len, DetailedTiming &out) {
	if (!edid || len < 128)
		return false;
	static const uint8_t sig[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	for (int i = 0; i < 8; i++)
		if (edid[i] != sig[i])
			return false;
	return parseDetailedTiming(edid + 54, out);
}

} // namespace Edid
