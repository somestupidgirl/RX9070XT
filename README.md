# RX9070XT.kext

A **non-accelerated framebuffer driver** for the AMD Radeon **RX 9070 XT**
(Navi 48 / RDNA 4, PCI `0x1002:0x7550`) on x86_64 Hackintosh, built as a Lilu
plugin against MacKernelSDK. Cross-compiles on Apple Silicon.

> **Scope, honestly.** macOS has *no* driver for RDNA 3 or RDNA 4 — Apple's AMD
> support ends at RDNA 2 (Navi 2x). This kext does **not** add Metal or GPU
> acceleration and cannot spoof the card as a supported one (the RDNA 2 driver
> would emit command streams Navi 48 can't decode). What it does is adopt the
> linear framebuffer that OpenCore's GOP already programmed and present it to
> macOS as one fixed display mode, so WindowServer composites the desktop **in
> software**. Think Linux `efifb`/`simpledrm`, not `amdgpu`. Goal: reach a
> usable desktop, not fast graphics.

## How it works

macOS's `IOPlatformExpert::getConsoleInfo()` returns `PE_state.video` — the base
address, stride, width, height and depth of the framebuffer the bootloader set
up. `RX9070XTFB` is an `IOFramebuffer` subclass that:

1. Matches the RX 9070 XT PCI device (`IOPCIPrimaryMatch 0x75501002`).
2. Reads the console framebuffer geometry in `start()` / `enableController()`.
3. Exposes exactly **one** 32bpp display mode at that geometry.
4. Returns that physical range from `getApertureRange()` so IOGraphics maps it
   for scanout. No registers are programmed; no DMA is issued.

`kern_start.cpp` is the Lilu plugin half — it logs, reports whether the card is
present, and provides a `onPatcherLoadForce` hook that is the natural place to
add real hardware bring-up later (see Roadmap).

## Files

| File | Purpose |
|------|---------|
| `Source/RX9070XTFB.{hpp,cpp}` | The `IOFramebuffer` subclass (the part that reaches the desktop). |
| `Source/AtomBios.{hpp,cpp}` | Freestanding, bounds-checked AtomBIOS data-table parser (groundwork for native mode setting). |
| `Source/kern_start.cpp` | Lilu plugin entry (`PluginConfiguration`, `pluginStart`). |
| `Source/kmod_info.c` | Hand-written kmod glue (Xcode normally generates this). |
| `tools/atomdump.cpp` | Host harness: runs the kext's parser against the real ROM (`make test`). |
| `Info.plist` | Two personalities: Lilu plugin + PCI framebuffer match. |
| `Makefile` | Cross-compiles x86_64 on any host, assembles the `.kext`. |

## Building (works on Apple Silicon)

```sh
make            # -> build/RX9070XT.kext  (x86_64, min macOS 11)
make test       # build host-side atomdump, verify the AtomBIOS parser
                # against firmware/Sapphire.RX9070XT.16384.241213.rom
make clean
```

The kext links against Lilu at **load** time via `OSBundleLibraries`; Lilu's
symbols are intentionally left undefined in the `-kext` bundle and resolved by
the kernel loader. You do **not** need a prebuilt `Lilu.kext` to compile.

Verify the output:

```sh
file build/RX9070XT.kext/Contents/MacOS/RX9070XT   # Mach-O 64-bit kext bundle x86_64
```

## Installing (on the Intel target)

1. Put `Lilu.kext` **and** `RX9070XT.kext` in `EFI/OC/Kexts`, add both to your
   OpenCore `config.plist` `Kernel > Add` (Lilu first — it must load before its
   plugins).
2. Recommended while bringing this up:
   - `-v keepsyms=1` to see panics.
   - `-rx9070xtdbg` to enable this plugin's debug logging, `-liludbgall` for
     Lilu's.
   - Consider `debug=0x100` and disabling other GPU-related kexts
     (WhateverGreen) so nothing else fights over the device.
3. Because this is unsigned/experimental, you'll want SIP configured for kext
   testing (`csr-active-config`) on the target.

**Do not install this on a machine you can't recover** — a misbehaving
framebuffer kext can black-screen the boot. Keep a known-good EFI to swap back.

## Roadmap — from "framebuffer" to "real driver"

Rough order of increasing difficulty. Each step needs iteration on the actual
hardware; the `.rom` (NAVI48.bin AtomBIOS) in `../firmware` and the Linux
`amdgpu` sources (`drivers/gpu/drm/amd/`) are the references.

1. **Confirm scanout adoption** — get the desktop up on the GOP buffer (this
   kext). Validate stride/format against what you actually see.
2. **Mode setting** — parse the AtomBIOS tables (`atomfirmware.h` in amdgpu)
   and drive **DCN 4.0.1** (Navi 48's display controller) to change resolution
   / light up other connectors. This is where `enableController()` stops being
   a no-op. *Started:* `AtomBios.{hpp,cpp}` parses the ROM header, master data
   table v2.1, firmwareinfo v3.5 and displayobjectinfo v1.5 — verified against
   this card's ROM (`make test`), which reports 2× DisplayPort + 2× HDMI-A.
   At runtime the kext obtains the VBIOS from an injected `ATY,bin_image`
   device property (preferred) or the PCI expansion ROM, and publishes
   `AtomBIOS,*` properties on the framebuffer node.
3. **Multiple connectors & EDID** — read EDID over DP/HDMI AUX, publish real
   timings from `getInformationForDisplayMode`. *Prerequisite done:* the
   parser now decodes each connector's record chain (I2C/AUX + HPD) and the
   GPIO pin LUT, giving the DDC line and HPD pin register mapping per
   connector (verified via `make test`: DP→ddc0/hpd1, DP→ddc1/hpd2,
   HDMI→ddc2/hpd3, HDMI→ddc3/hpd4; DDC regs 0x5d91/95/99/9d, HPD bank
   0x5db5). Next: drive those DC_GPIO registers / AUX engine over BAR5 MMIO.
4. **Power / clocks** — SMU firmware handshake (see `../firmware` power tables)
   so the card is stable, not stuck at boot clocks.
5. **Acceleration (huge)** — a real accelerator: GFX12 command processor, ring
   buffers, memory controller, and a Metal driver. This is effectively
   reimplementing Apple's `AMDRadeonX6000` family for a new architecture and is
   out of scope for this repo's near term.

## Status

- [x] Cross-compiles on Apple Silicon → x86_64 kext bundle
- [x] Lilu plugin bootstrap + PCI framebuffer personality
- [x] Adopts firmware/GOP linear framebuffer, one fixed 32bpp mode
- [x] AtomBIOS parser (rom header, master data table, firmwareinfo,
      display paths) verified against the real ROM via `make test`
- [x] Runtime VBIOS acquisition (`ATY,bin_image` property / expansion ROM)
- [x] Per-connector DDC/AUX line + HPD pin mapping (path records +
      gpio_pin_lut), published as `AtomBIOS,Connectors`
- [x] Adopted console geometry published as `Console,*` registry properties
- [ ] Verified booting to desktop on real RX 9070 XT hardware
- [ ] Native mode setting (DCN 4.0.1)
- [ ] Acceleration / Metal
