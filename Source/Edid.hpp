//
//  Edid.hpp
//  RX9070XT
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

} // namespace Edid

#endif /* Edid_hpp */
