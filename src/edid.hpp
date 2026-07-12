//
//  edid.hpp
//  RDNA4FB
//
//  Freestanding EDID detailed-timing-descriptor (DTD) parser. A DTD carries
//  everything the OTG timing generator needs to be programmed with (totals,
//  blanking, sync position/width/polarity and the pixel clock), so this is
//  the bridge from "EDID read" to "mode set".
//
//  No kernel or libc dependencies; usable from both the kext and the host
//  test harness (tools/atomdump).
//

#ifndef Edid_hpp
#define Edid_hpp

#include <stdint.h>
#include <stddef.h>

namespace Edid {

struct DetailedTiming {
	uint32_t pixelClockKHz;   // e.g. 533250 for this Samsung's 4K60
	uint16_t hActive, hBlank; // hTotal = hActive + hBlank
	uint16_t hSyncOffset;     // front porch (active end -> sync start)
	uint16_t hSyncWidth;
	uint16_t vActive, vBlank;
	uint16_t vSyncOffset;
	uint16_t vSyncWidth;
	bool     hSyncPositive;
	bool     vSyncPositive;
	bool     interlaced;

	uint32_t hTotal() const { return static_cast<uint32_t>(hActive) + hBlank; }
	uint32_t vTotal() const { return static_cast<uint32_t>(vActive) + vBlank; }
	// Field/frame rate in millihertz (60000 = 60 Hz).
	uint32_t refreshMilliHz() const {
		uint64_t total = static_cast<uint64_t>(hTotal()) * vTotal();
		if (!total) return 0;
		return static_cast<uint32_t>((static_cast<uint64_t>(pixelClockKHz) * 1000000ULL) / total);
	}
};

// Parse one 18-byte descriptor. Returns false if it is not a detailed timing
// (pixel clock 0 marks display/monitor descriptors) or is malformed.
bool parseDetailedTiming(const uint8_t d[18], DetailedTiming &out);

// Parse the preferred (first) DTD of a 128-byte EDID base block. Validates
// the block header first.
bool preferredTiming(const uint8_t *edid, size_t len, DetailedTiming &out);

// --- CTA-861 extension block ------------------------------------------------
// Sink capabilities needed to pick a legal signal for mode setting: pixel
// repertoire (short video descriptors), color format support, and — for HDMI
// sinks — the maximum TMDS character clock from the HDMI VSDB.

struct CtaCaps {
	uint8_t  revision   { 0 };
	bool     underscan  { false };
	bool     basicAudio { false };
	bool     ycbcr444   { false };
	bool     ycbcr422   { false };
	uint8_t  vics[32]   {};     // short video descriptors (VIC codes, bit7 cleared)
	size_t   vicCount   { 0 };
	bool     hasHdmiVsdb { false };  // IEEE OUI 00-0C-03 vendor block present
	uint32_t maxTmdsKHz { 0 };       // 0 = not advertised
	size_t   dtdCount   { 0 };       // additional 18-byte timings in the block
	DetailedTiming firstDtd {};      // valid when dtdCount > 0
};

// Parse one 128-byte CTA-861 extension (tag 0x02). Returns false if the tag
// or structure is invalid.
bool parseCtaBlock(const uint8_t ext[128], CtaCaps &out);

} // namespace Edid

#endif /* Edid_hpp */
