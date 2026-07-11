# RX9070XT.kext

A **non-accelerated framebuffer driver** for the AMD Radeon **RX 9070 XT**
(Navi 48 / RDNA 4, PCI `0x1002:0x7550`) on x86_64 Hackintosh, built as a
standalone IOKit kext against MacKernelSDK. Cross-compiles on Apple Silicon.

**Status: working.** Verified on real hardware (Ryzen 9 5950X, Big Sur
11.7.10): boots to a 4K desktop with correct colors, WindowServer composites
on this framebuffer, and BAR5 register MMIO (read *and* write) is proven.
Known v0 limitations: software cursor lags under load, and only the boot
display lights up (single adopted scanout).

> **Why not a Lilu plugin / OpenCore injection?** An `IOFramebuffer` subclass
> must link against `com.apple.iokit.IOGraphicsFamily`, which on Big Sur+
> lives in the *System* kernel collection — OpenCore can only inject into the
> *Boot* KC, so injection fails with "Dependency ... was not found" (verified
> on hardware). This kext therefore installs to `/Library/Extensions`, where
> `kmutil` links it into the Aux KC with IOGraphicsFamily available. That
> environment cannot resolve OC-injected Lilu symbols either, so the kext is
> deliberately Lilu-free; Lilu integration can return later as a separate
> boot-KC plugin if kernel patching becomes necessary.

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
   **Hard-won detail:** `v_baseAddr` carries flag bits in its low bits (this
   machine reads `0x840000001`); they must be masked off. Passing the raw
   value shifts WindowServer's writes one byte from the true scanout base,
   which recolours every pixel (R'=G, G'=B, B'=previous pixel's alpha) — it
   presents as "inverted colors with a blue cast" and cost a week of DCN
   register archaeology to trace. The hardware was configured correctly the
   whole time.
3. Exposes exactly **one** 32bpp display mode at that geometry.
4. Returns that physical range from `getApertureRange()` so IOGraphics maps it
   for scanout. Scanout/link registers are never reprogrammed; no DMA is
   issued. BAR5 MMIO is used for read-mostly diagnostics (see boot-args).

At startup the kext also parses the VBIOS (AtomBIOS tables + AMD IP discovery
binary) and publishes the results as registry properties (`AtomBIOS,*`,
`Discovery,*`, `Console,*`, `VRAM,TotalMB`, `MMIO,Verified`) so the state of
every bring-up layer is visible in `ioreg` without kernel logs.

## Boot arguments

All parsed without a leading dash (`name=1`, not `-name=1`):

| Boot-arg | Effect |
|----------|--------|
| `rx9070xt-off=1` | Kill switch: `probe()` declines to match, restoring the macOS fallback framebuffer. Recovers from a bad build via OpenCore boot-args alone — no Safe Mode or filesystem surgery. |
| `rx9070xt-cmap=N` | Diagnostic: permute the advertised R/G/B component masks (0–5). Note: WindowServer ignores these for 32-bit modes; kept for documentation of that finding. |
| `rx9070xt-lutbypass=1` | Force the MPC MCM stages (shaper/3D LUT/1D LUT) to bypass on all pipes. |
| `rx9070xt-8bpc=1` | Experiment: switch the active DP stream 10 bpc → 8 bpc and update the MSA to match (first proven live register write). |
| `rx9070xt-noedid=1` | Skip the EDID-over-AUX probe (default on; verified on hardware 2026-07-11). Use if a sink misbehaves on DDC. |
| `rx9070xt-nosleep=1` | Disable display sleep handling (power changes become no-ops again, screen stays on). Escape hatch if blank/unblank misbehaves. |

## Files

| File | Purpose |
|------|---------|
| `Source/RX9070XTFB.{hpp,cpp}` | The `IOFramebuffer` subclass (the part that reaches the desktop). |
| `Source/AtomBios.{hpp,cpp}` | Freestanding, bounds-checked AtomBIOS data-table parser (groundwork for native mode setting). |
| `Source/IpDiscovery.{hpp,cpp}` | Parser for AMD's IP discovery binary — per-card IP versions and register segment bases (what amdgpu uses instead of hardcoded offsets). |
| `Source/kmod_info.c` | Hand-written kmod glue (Xcode normally generates this). |
| `tools/atomdump.cpp` | Host harness: runs the kext's parser against the real ROM (`make test`). |
| `Info.plist` | PCI framebuffer personality (`IOPCIPrimaryMatch 0x75501002`). |
| `Makefile` | Cross-compiles x86_64 on any host, assembles the `.kext`. |

## Building (works on Apple Silicon)

```sh
make            # -> build/RX9070XT.kext  (x86_64, min macOS 11)
make test       # build host-side atomdump, verify the AtomBIOS parser
                # against firmware/Sapphire.RX9070XT.16384.241213.rom
make clean
```

Verify the output:

```sh
file build/RX9070XT.kext/Contents/MacOS/RX9070XT   # Mach-O 64-bit kext bundle x86_64
```

## Installing (on the Intel target)

1. **Do not add RX9070XT.kext to OpenCore `Kernel → Add`** — injection cannot
   work (see box above). Instead, on the running system (same commands to
   update an existing install; remove the old bundle first):

   ```sh
   sudo rm -rf /Library/Extensions/RX9070XT.kext
   sudo cp -R build/RX9070XT.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/RX9070XT.kext
   sudo chmod -R 755 /Library/Extensions/RX9070XT.kext
   sudo kmutil install --volume-root / --update-all
   sudo reboot
   ```

   SIP must permit unsigned kexts (`csr-active-config` with
   `CSR_ALLOW_UNTRUSTED_KEXTS`; standard hackintosh values like `0x03000067`
   qualify). If `kmutil` complains about approval, allow the extension in
   System Preferences → Security & Privacy and re-run.

2. Inject the **full 2 MiB flash dump** (the `.rom` in `firmware/`) as
   `ATY,bin_image` under the GPU's PciRoot path in OpenCore
   `DeviceProperties` — the IP discovery binary lives in the PSP region of
   the flash, which the PCI expansion ROM does not expose.

3. Recommended while bringing this up: `-v keepsyms=1` boot-args, and disable
   other GPU-related kexts (WhateverGreen) so nothing fights over the device.

**Recovery:** if a build misbehaves, add `rx9070xt-off=1` to boot-args — the
kext declines to match and macOS falls back to its EFI framebuffer. Worst
case (kext wedges boot before the kill switch existed): boot Safe Mode (`-x`),
which skips the Aux KC entirely, then remove the bundle and rebuild the KC.

## Roadmap — from "framebuffer" to "real driver"

Rough order of increasing difficulty. Each step needs iteration on the actual
hardware; the `.rom` (NAVI48.bin AtomBIOS) in `../firmware` and the Linux
`amdgpu` sources (`drivers/gpu/drm/amd/`) are the references.

1. ~~**Confirm scanout adoption**~~ — **done.** Desktop verified on hardware
   with correct colors (after masking the `v_baseAddr` flag bits).
2. ~~**EDID over DP AUX**~~ — **done.** The AUX software engine
   (`auxTransaction`, following amdgpu's `dce_aux.c`) reads each DP sink's
   EDID over I2C-over-AUX; verified on hardware 2026-07-11 (Samsung 4K sink
   on AUX0). The EDID (base + CTA extension) is served to IODisplay via
   `hasDDCConnect()`/`getDDCBlock()` so macOS sees the real display identity.
   *Remaining:* HDMI sinks (EDID lives on the DDC I2C engine, not AUX).
3. **Native mode setting (DCN 4.1.0)** — program HUBP/DPP/OPP/OTG to change
   resolution and light additional connectors; this is where
   `enableController()` stops being a no-op. The register-offset workflow
   (Linux `dcn_4_1_0_offset.h` + IP discovery segment bases) is established
   from the color investigation.
4. **Display power management** *(implemented, awaiting hardware verify)* —
   the driver registers sleep/doze/wake power states with PM
   (`registerPowerDriver`, mirroring `IONDRVFramebuffer::initForPM` — the
   subclass must do this itself; without it `setPowerState` is never called
   and display sleep silently does nothing). On `kIOPowerAttribute`/
   `kConnectionPower` changes it disables the DP video stream
   (`DP_VID_STREAM_CNTL`) and puts the sink in D3 via a native-AUX DPCD
   `SET_POWER` write, reversing both on wake. The timing generator and
   clocks are untouched, so wake restores exactly the firmware state.
   **System sleep is deliberately vetoed** (`kIOPMPreventSystemSleep`, like
   Apple's `IOBootNDRV`): after GPU power loss we cannot reprogram the
   display pipe until native mode setting exists, so allowing it would mean
   waking to a black screen. Opt out of display sleep handling with
   `rx9070xt-nosleep=1`.
5. **Power / clocks** — SMU firmware handshake so the card is stable, not
   stuck at boot clocks.
6. **Acceleration (huge)** — a real accelerator: GFX12 command processor, ring
   buffers, memory controller, and a Metal driver. This is effectively
   reimplementing Apple's `AMDRadeonX6000` family for a new architecture and is
   out of scope for this repo's near term.

## Status

- [x] Cross-compiles on Apple Silicon → x86_64 kext bundle
- [x] Standalone (Lilu-free) kext, loads from /Library/Extensions via Aux KC
- [x] Adopts firmware/GOP linear framebuffer, one fixed 32bpp mode
- [x] **Boots to a 4K desktop on real hardware with correct colors**
      (WindowServer verified compositing on this framebuffer)
- [x] AtomBIOS parser (rom header, master data table, firmwareinfo,
      display paths) verified against the real ROM via `make test`
- [x] Runtime VBIOS acquisition (`ATY,bin_image` property / expansion ROM)
- [x] Per-connector DDC/AUX line + HPD pin mapping (path records +
      gpio_pin_lut), published as `AtomBIOS,Connectors`
- [x] IP discovery parser: GC v12.0.1 / DCN v4.1.0 / NBIF v6.3.1 register
      segment bases extracted from the ROM (`make test` gates on them)
- [x] BAR5 register MMIO confirmed on hardware, read (VRAM size, DCN dumps)
      and write (DP stream registers, MCM bypass)
- [x] Kill-switch boot-arg (`rx9070xt-off=1`) for safe iteration
- [x] DP AUX software engine + EDID read over I2C-over-AUX (verified on
      hardware: Samsung 4K sink on AUX0), served via `getDDCBlock()`
- [ ] Display sleep (implemented: stream blank + sink D3 over native AUX;
      needs hardware verification)
- [ ] Native mode setting (DCN 4.1.0) / multiple displays
- [ ] Acceleration / Metal
