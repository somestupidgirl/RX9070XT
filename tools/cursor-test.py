#!/usr/bin/env python3
# rdna4-curdump.py — dump DCN cursor-path registers under Linux/amdgpu
# (working hardware cursor) for diffing against the macOS RDNA4FB values.
# Usage: sudo python3 rdna4-curdump.py   [GPU at 0000:56:00.0, BAR5]
import mmap, os

BDF = "0000:56:00.0"
DMU_SEG2 = 0x34c0   # DCN 4.1.0 base_idx 2 segment (from IP discovery, same as macOS)

REGS = [  # (name, dword offset within base_idx 2)
    ("HUBP0_DCHUBP_CNTL",              0x05f4),
    ("HUBP0_DCSURF_SURFACE_CONFIG",    0x05e5),
    ("HUBPREQ0_DCSURF_SURFACE_PITCH",  0x0607),
    ("HUBPREQ0_PRIMARY_SURFACE_ADDR",  0x060a),
    ("HUBPREQ0_PRIMARY_SURFACE_HI",    0x060b),
    ("HUBPREQ0_DCN_CUR0_TTU_CNTL0",    0x0627),
    ("HUBPREQ0_DCN_CUR0_TTU_CNTL1",    0x0628),
    ("HUBPREQ0_CURSOR_SETTINGS",       0x0653),
    ("CURSOR0_0_CURSOR_CONTROL",       0x0679),
    ("CURSOR0_0_CURSOR_SURFACE_ADDR",  0x067a),
    ("CURSOR0_0_CURSOR_SURFACE_HI",    0x067b),
    ("CURSOR0_0_CURSOR_SIZE",          0x067c),
    ("CURSOR0_0_CURSOR_POSITION",      0x067d),
    ("CURSOR0_0_CURSOR_HOT_SPOT",      0x067e),
    ("CURSOR0_0_CURSOR_DST_OFFSET",    0x0680),
    ("DPP_TOP0_DPP_CONTROL",           0x0cc5),
    ("CNVC_CFG0_SURFACE_PIXEL_FORMAT", 0x0ccf),
    ("CNVC_CFG0_FORMAT_CONTROL",       0x0cd0),
    ("CNVC_CFG0_PRE_DEALPHA",          0x0cde),
    ("CM_CUR0_CURSOR0_CONTROL",        0x0cf1),
    ("CM_CUR0_CURSOR0_COLOR0",         0x0cf2),
    ("CM_CUR0_CURSOR0_COLOR1",         0x0cf3),
    ("CM_CUR0_FP_SCALE_BIAS_G_Y",      0x0cf4),
    ("CM_CUR0_FP_SCALE_BIAS_RB",       0x0cf5),
    ("CM_CUR0_CUR0_MATRIX_MODE",       0x0cf6),
    ("OTG0_OTG_CONTROL",               0x1b43),
    ("OTG0_OTG_DOUBLE_BUFFER_CONTROL", 0x1b5c),
    ("OTG0_OTG_VSTARTUP_PARAM",        0x1b85),
    ("OTG0_OTG_VUPDATE_PARAM",         0x1b86),
    ("OTG0_OTG_GLOBAL_SYNC_STATUS",    0x1b88),
    ("OTG0_OTG_MASTER_UPDATE_LOCK",    0x1b89),
]

path = f"/sys/bus/pci/devices/{BDF}/resource5"
size = os.path.getsize(path)
with open(path, "r+b") as f:
    m = mmap.mmap(f.fileno(), size)
    for name, dword in REGS:
        byte = (DMU_SEG2 + dword) * 4
        val = int.from_bytes(m[byte:byte+4], "little")
        print(f"{name:34s} @0x{byte:06x} = 0x{val:08x}")
    m.close()