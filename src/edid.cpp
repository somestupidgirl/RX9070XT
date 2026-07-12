//
//  edid.cpp
//  RDNA4FB
//
//  DTD field packing per VESA E-EDID 1.4 §3.10.2.
//

#include "edid.hpp"

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

bool parseCtaBlock(const uint8_t ext[128], CtaCaps &out) {
	if (!ext || ext[0] != 0x02)
		return false;
	out = CtaCaps {};
	out.revision   = ext[1];
	uint8_t dtdOff = ext[2];          // 0 = no DTDs and no data blocks
	out.underscan  = (ext[3] & 0x80) != 0;
	out.basicAudio = (ext[3] & 0x40) != 0;
	out.ycbcr444   = (ext[3] & 0x20) != 0;
	out.ycbcr422   = (ext[3] & 0x10) != 0;

	// Data block collection: [4, dtdOff). Each block: tag [7:5], length [4:0].
	if (out.revision >= 3 && dtdOff >= 4) {
		size_t pos = 4;
		size_t end = dtdOff < 128 ? dtdOff : 127;
		while (pos < end) {
			uint8_t tag = ext[pos] >> 5;
			uint8_t len = ext[pos] & 0x1f;
			if (pos + 1 + len > end)
				break;
			const uint8_t *p = ext + pos + 1;
			if (tag == 2) {                     // video block: SVDs
				for (uint8_t i = 0; i < len && out.vicCount < 32; i++)
					out.vics[out.vicCount++] = p[i] & 0x7f;
			} else if (tag == 3 && len >= 5 &&  // vendor block, HDMI LLC OUI
			           p[0] == 0x03 && p[1] == 0x0c && p[2] == 0x00) {
				out.hasHdmiVsdb = true;
				if (len >= 7 && p[6])
					out.maxTmdsKHz = static_cast<uint32_t>(p[6]) * 5000;
			}
			pos += 1u + len;
		}
	}

	// 18-byte detailed timings from dtdOff to the checksum byte.
	if (dtdOff >= 4) {
		size_t pos = dtdOff;
		while (pos + 18 <= 127) {
			DetailedTiming t {};
			if (!parseDetailedTiming(ext + pos, t))
				break;
			if (out.dtdCount == 0)
				out.firstDtd = t;
			out.dtdCount++;
			pos += 18;
		}
	}
	return true;
}

} // namespace Edid
