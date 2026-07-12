//
//  otgtiming.hpp
//  RDNA4FB
//
//  Compute DCN OTG (timing generator) register images from an EDID detailed
//  timing, following amdgpu's optc1_program_timing exactly. The OTG's pixel
//  counter starts at the sync pulse (H/V_SYNC_A_START is programmed 0), so
//    blank_start = total - front_porch      (where active ends)
//    blank_end   = blank_start - active     (where active begins)
//
//  Freestanding: shared by the kext and the host test harness, where the
//  computed values for the boot display's EDID can be diffed against the
//  GOP-programmed OTG0 registers from a rdna4-modedump log — hardware
//  validation of this math before any OTG is ever written.
//

#ifndef OtgTiming_hpp
#define OtgTiming_hpp

#include "edid.hpp"

namespace OtgTiming {

// Raw register images (dcn_4_1_0 field layout: *_START at bit 0, *_END /
// WIDTH at bit 16).
struct Regs {
	uint32_t hTotal;          // OTG_H_TOTAL
	uint32_t hSyncA;          // OTG_H_SYNC_A       (start 0, end = sync width)
	uint32_t hBlankStartEnd;  // OTG_H_BLANK_START_END
	uint32_t vTotal;          // OTG_V_TOTAL
	uint32_t vSyncA;          // OTG_V_SYNC_A
	uint32_t vBlankStartEnd;  // OTG_V_BLANK_START_END
	// OTG_H/V_SYNC_A_CNTL POL bit: 0 = positive polarity, 1 = negative.
	bool hSyncPolInvert;
	bool vSyncPolInvert;
};

// Returns false if the timing is degenerate (zero active/total or blanking
// smaller than sync + front porch).
bool compute(const Edid::DetailedTiming &t, Regs &out);

} // namespace OtgTiming

#endif /* OtgTiming_hpp */
