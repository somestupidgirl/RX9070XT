//
//  otgtiming.cpp
//  RDNA4FB
//
//  Mirrors drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c
//  optc1_program_timing (borders are zero for EDID DTDs).
//

#include "otgtiming.hpp"

namespace OtgTiming {

bool compute(const Edid::DetailedTiming &t, Regs &out) {
	uint32_t hTotal = t.hTotal();
	uint32_t vTotal = t.vTotal();
	if (t.hActive == 0 || t.vActive == 0 || hTotal <= t.hActive || vTotal <= t.vActive)
		return false;
	// Blanking must hold front porch + sync pulse.
	if (t.hBlank < static_cast<uint32_t>(t.hSyncOffset) + t.hSyncWidth ||
	    t.vBlank < static_cast<uint32_t>(t.vSyncOffset) + t.vSyncWidth)
		return false;

	// Counter origin is the sync start.
	uint32_t hBlankStart = hTotal - t.hSyncOffset;   // active end
	uint32_t hBlankEnd   = hBlankStart - t.hActive;  // active begin
	uint32_t vBlankStart = vTotal - t.vSyncOffset;
	uint32_t vBlankEnd   = vBlankStart - t.vActive;

	out.hTotal = hTotal - 1;
	out.hSyncA = static_cast<uint32_t>(t.hSyncWidth) << 16;   // start 0
	out.hBlankStartEnd = hBlankStart | (hBlankEnd << 16);
	out.vTotal = vTotal - 1;
	out.vSyncA = static_cast<uint32_t>(t.vSyncWidth) << 16;
	out.vBlankStartEnd = vBlankStart | (vBlankEnd << 16);
	// OTG POL semantics are inverted relative to "positive polarity".
	out.hSyncPolInvert = !t.hSyncPositive;
	out.vSyncPolInvert = !t.vSyncPositive;
	return true;
}

} // namespace OtgTiming
