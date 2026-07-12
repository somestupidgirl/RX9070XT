//
//  dmub.hpp
//  RDNA4FB
//
//  Minimal DMUB (display microcontroller) mailbox ABI, from Linux
//  drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h. The GOP leaves DMUB
//  running with an initialized inbox1 ring (verified on hardware
//  2026-07-12); commands are 64-byte entries written at the ring write
//  pointer, submitted by advancing DMCUB_INBOX1_WPTR, consumed when the
//  firmware advances RPTR.
//
//  On DCN 3.1+ the display bring-up that used to be AtomBIOS bytecode
//  (transmitter control, pixel clock) is issued through these commands —
//  this is the mode-setting path for the second display.
//
//  Freestanding; shared by the kext and the host test harness.
//

#ifndef dmub_hpp
#define dmub_hpp

#include <stdint.h>

namespace Dmub {

constexpr uint32_t kCmdSize = 64;   // every ring entry, header included

// enum dmub_cmd_type (stable ABI per the header)
constexpr uint8_t CmdNull             = 0;
constexpr uint8_t CmdQueryFeatureCaps = 6;    // harmless: FW reports caps
constexpr uint8_t CmdVbios            = 128;  // "VBIOS" services in FW

// enum dmub_cmd_vbios_type
constexpr uint8_t VbiosDigxEncoderControl     = 0;
constexpr uint8_t VbiosDig1TransmitterControl = 1;
constexpr uint8_t VbiosSetPixelClock          = 2;
constexpr uint8_t VbiosEnableDispPowerGating  = 3;

// struct dmub_cmd_header, encoded manually to avoid bitfield ABI surprises:
//   type[7:0] | sub_type[15:8] | ret_status[16] | multi_cmd_pending[17] |
//   is_reg_based[18] | reserved[23:19] | payload_bytes[29:24] | rsvd[31:30]
inline uint32_t headerWord(uint8_t type, uint8_t subType, uint8_t payloadBytes,
                           bool regBased = false, bool multiPending = false) {
	return static_cast<uint32_t>(type) |
	       (static_cast<uint32_t>(subType) << 8) |
	       (multiPending ? (1u << 17) : 0) |
	       (regBased ? (1u << 18) : 0) |
	       ((static_cast<uint32_t>(payloadBytes) & 0x3f) << 24);
}

} // namespace Dmub

#endif /* dmub_hpp */
