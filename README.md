# RDNA4FB.kext

A **display driver** for the AMD Radeon **RX 9070 XT** (Navi 48 / RDNA 4,
PCI `0x1002:0x7550`) on x86_64 Hackintosh, built as a standalone IOKit kext
against MacKernelSDK. Cross-compiles on Apple Silicon. No 3D/Metal
acceleration (yet — see Scope below); rendering is software.

**Status: working.** Verified on real hardware (Ryzen 9 5950X, Big Sur
11.7.10): boots to a 4K desktop with correct colors, real display identity
via EDID (DP over AUX, HDMI over the DDC I2C engine), working display sleep
(DP stream + sink DPCD power), and proven BAR5 register MMIO. Hardware
cursor and emulated VBL are implemented behind boot-args pending hardware
verification. Remaining major limitation: only the boot display lights up
(mode setting for additional pipes is in progress).

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
> support ends at RDNA 2 (Navi 2x), and spoofing is impossible (the RDNA 2
> driver would emit command streams Navi 48 can't decode). This project
> **started** as pure framebuffer adoption — take the scanout OpenCore's GOP
> already programmed and hand it to WindowServer, Linux `efifb`/`simpledrm`
> style. It has outgrown that description: the driver now actively operates
> the display controller — AUX and DDC-I2C engines for EDID, DPCD sink power
> and stream blanking for display sleep, the cursor plane, discovery-derived
> register addressing — all ported piecewise from `amdgpu`'s display core
> (DC), which is this project's reference and upstream in spirit. Think of it
> as an early, hand-rolled KMS driver growing toward native mode setting.
> **What it still is not:** a GPU accelerator. 3D/Metal needs the GFX12
> command processor, ring buffers, memory management and a Metal userland —
> a much larger project, acknowledged as the long-term goal rather than
> disclaimed. Much of the groundwork here (IP-discovery-driven registers,
> AtomBIOS/EDID/timing parsers with host-side tests) is deliberately
> ASIC-portable and would carry over to other RDNA 4 cards with little more
> than wider PCI matching.

## How it works

macOS's `IOPlatformExpert::getConsoleInfo()` returns `PE_state.video` — the base
address, stride, width, height and depth of the framebuffer the bootloader set
up. `RDNA4FB` is an `IOFramebuffer` subclass that:

1. Matches the Navi 48 family (`0x7550` RX 9070/9070 XT, `0x7551` R9700;
   `GPU,Variant` in ioreg shows which). Navi 44 (RX 9060, `0x7590`) shares
   DCN 4.1.0 and is a likely future addition once someone can test it.
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
   for scanout. The scanout surface and link training are never reprogrammed
   and no DMA is issued; BAR5 MMIO drives the narrow, restorable state the
   driver does own: AUX/DC_I2C transactions (EDID), the DP stream enable and
   sink DPCD power for display sleep, and the cursor plane (opt-in).

At startup the kext also parses the VBIOS (AtomBIOS tables + AMD IP discovery
binary) and publishes the results as registry properties (`AtomBIOS,*`,
`Discovery,*`, `Console,*`, `VRAM,TotalMB`, `MMIO,Verified`) so the state of
every bring-up layer is visible in `ioreg` without kernel logs.

## Boot arguments

All parsed without a leading dash (`name=1`, not `-name=1`):

| Boot-arg | Effect |
|----------|--------|
| `rdna4-off=1` | Kill switch: `probe()` declines to match, restoring the macOS fallback framebuffer. Recovers from a bad build via OpenCore boot-args alone — no Safe Mode or filesystem surgery. |
| `rdna4-cmap=N` | Diagnostic: permute the advertised R/G/B component masks (0–5). Note: WindowServer ignores these for 32-bit modes; kept for documentation of that finding. |
| `rdna4-lutbypass=1` | Force the MPC MCM stages (shaper/3D LUT/1D LUT) to bypass on all pipes. |
| `rdna4-8bpc=1` | Experiment: switch the active DP stream 10 bpc → 8 bpc and update the MSA to match (first proven live register write). |
| `rdna4-noedid=1` | Skip the EDID-over-AUX probe (default on; verified on hardware 2026-07-11). Use if a sink misbehaves on DDC. |
| `rdna4-nosleep=1` | Disable display sleep handling (power changes become no-ops again, screen stays on). Escape hatch if blank/unblank misbehaves. |
| `rdna4-modedump=1` | Read-only survey of the mode-setting registers: all OTG timings/enables, DIG front/back-ends, HUBP surface addresses, DCCG clock muxes, DMUB status. The lit DP pipe is the template for HDMI pipe bring-up. |
| `rdna4-hwcursor=1` | Enable the DCN hardware cursor plane (sprite in VRAM after the framebuffer, overlaid at scanout). Fixes software-cursor lag under heavy repaint. Opt-in until hardware-verified. |
| `rdna4-curmode=N` | Cursor pixel mode when hwcursor is on: 2 = premultiplied ARGB (default), 3 = straight alpha. Flip to 3 if the pointer shows dark/bright fringes. |
| `rdna4-curtest=1` | Cursor bisect: fetch the sprite from the scanout base (proven-fetchable memory). A floating square of screen content proves the cursor engine and isolates the bug to sprite addressing. |
| `rdna4-dmubping=1` | First contact with the DMUB display firmware: resolve the inbox1 ring through the DMCUB region windows, submit one QUERY_FEATURE_CAPS command, verify RPTR advances. Proves the mailbox route for mode setting. |
| `rdna4-dmubhist=1` | Read-only decode of the GOP's recorded DMUB command ring — the exact VBIOS-family command sequence (encoder/transmitter/pixel-clock) the firmware accepted to light the DP display, as the template for HDMI-pipe mode setting. |
| `rdna4-dmubcursor=1` | (with `rdna4-hwcursor=1`) Ask the DMUB firmware to program the cursor plane from our register images (DMUB_CMD__UPDATE_CURSOR_INFO, two chained ring entries). White 64x64 square at (100,100) = firmware can light the cursor. |
| `rdna4-smuping=1` | Read-only SMU (power-management firmware) handshake: TestMessage + PMFW/interface version queries over the MP1 mailbox. No DPM changes. Publishes `SMU,FirmwareVersion` / `SMU,Verified`. Prerequisite check for future clock control. |
| `rdna4-ihdump=1` | Read-only interrupt-delivery survey: OSSSYS IH ring state, per-OTG vertical-interrupt line config, PCI MSI/MSI-X capability words. Groundwork for real VBL interrupts replacing the timer emulation. |
| `rdna4-pspdump=1` | Read-only PSP (security processor) survey: bootloader/sOS/GPCOM-ring status from the MPASP scratch registers. Publishes `PSP,Alive` / `PSP,SOSVersion`. Decides whether loading fresh firmware (e.g. a current DMUB) is viable. |
| `rdna4-vbl=1` | Provide an emulated vertical-blank interrupt (workloop timer at the EDID refresh rate). Engages IOFramebuffer's frame pacing (CVDisplayLink timestamps, deferred cursor sync) that is otherwise absent without a hardware IRQ handler. |

## Files

| File | Purpose |
|------|---------|
| `src/framebuffer.{hpp,cpp}` | The `IOFramebuffer` subclass (`RDNA4FB`): scanout adoption, AUX/DC_I2C engines, EDID/DDC service, display power, HW cursor, VBL. |
| `src/atombios.{hpp,cpp}` | Freestanding, bounds-checked AtomBIOS parser: data tables (connectors, GPIO LUT, firmwareinfo) + command-function directory. |
| `src/ipdiscovery.{hpp,cpp}` | Parser for AMD's IP discovery binary — per-card IP versions and register segment bases (what amdgpu uses instead of hardcoded offsets; the key to ASIC portability). |
| `src/edid.{hpp,cpp}` | EDID detailed-timing and CTA-861 extension parsers (sink timings and capabilities). |
| `src/otgtiming.{hpp,cpp}` | EDID timing → DCN OTG register images, per amdgpu's `optc1_program_timing` (mode-set groundwork). |
| `src/kmod_info.c` | Hand-written kmod glue (Xcode normally generates this). |
| `tools/atomdump.cpp` | Host test harness: runs the parsers against the real ROM and captured EDID fixtures (`make test`). |
| `Info.plist` | PCI framebuffer personality (`IOPCIPrimaryMatch`: 0x7550 RX 9070/XT, 0x7551 R9700 — the Navi 48 family). |
| `Makefile` | Cross-compiles x86_64 on any host, assembles the `.kext`. |

## Building (works on Apple Silicon)

```sh
make            # -> build/RDNA4FB.kext  (x86_64, min macOS 11)
make test       # build host-side atomdump, verify the AtomBIOS parser
                # against firmware/Sapphire.RX9070XT.16384.241213.rom
make clean
```

Verify the output:

```sh
file build/RDNA4FB.kext/Contents/MacOS/RDNA4FB   # Mach-O 64-bit kext bundle x86_64
```

## Installing (on the Intel target)

1. **Do not add RDNA4FB.kext to OpenCore `Kernel → Add`** — injection cannot
   work (see box above). Instead, on the running system (same commands to
   update an existing install; remove the old bundle first):

   ```sh
   sudo rm -rf /Library/Extensions/RDNA4FB.kext
   sudo rm -rf /Library/Extensions/RX9070XT.kext   # pre-rename installs (≤2026-07)
   sudo cp -R build/RDNA4FB.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/RDNA4FB.kext
   sudo chmod -R 755 /Library/Extensions/RDNA4FB.kext
   sudo kmutil install --volume-root / --update-all
   sudo reboot
   ```

   **Renamed 2026-07:** the bundle was `RX9070XT.kext` and the boot-args
   were `rx9070xt-*`. The old bundle *must* be removed (two copies would
   race for the same PCI device), and old `rx9070xt-*` boot-args in the
   OpenCore config are inert against the new build — including the old kill
   switch; the working one is now `rdna4-off=1`.

   SIP must permit unsigned kexts (`csr-active-config` with
   `CSR_ALLOW_UNTRUSTED_KEXTS`; standard hackintosh values like `0x03000067`
   qualify). If `kmutil` complains about approval, allow the extension in
   System Preferences → Security & Privacy and re-run.

2. *(Optional since the on-die discovery fallback)* Inject the **full
   2 MiB flash dump** (the `.rom` in `firmware/`) as `ATY,bin_image` under
   the GPU's PciRoot path in OpenCore `DeviceProperties`. Without it the
   driver reads the PSP's IP-discovery copy from the top-of-VRAM TMR via
   MM_INDEX/MM_DATA (`Discovery,Source` in ioreg shows which path won) and
   the VBIOS data tables from the PCI expansion ROM.

3. Recommended while bringing this up: `-v keepsyms=1` boot-args, and disable
   other GPU-related kexts (WhateverGreen) so nothing fights over the device.

**Recovery:** if a build misbehaves, add `rdna4-off=1` to boot-args — the
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
   HDMI/DVI sinks are read too, via the DC_I2C hardware engine (per amdgpu's
   `dce_i2c_hw.c`: offset write + block read queued as two transactions in a
   single GO) — verified on hardware 2026-07-12 against a Lenovo 1080p sink.
3. **Native mode setting (DCN 4.1.0)** — program HUBP/DPP/OPP/OTG to change
   resolution and light additional connectors; this is where
   `enableController()` stops being a no-op. The register-offset workflow
   (Linux `dcn_4_1_0_offset.h` + IP discovery segment bases) is established
   from the color investigation. **Key constraint (verified against this
   ROM):** the VBIOS carries *no* display command tables — only `asic_init`
   survives; `setpixelclock`/`dig1transmittercontrol` are absent because
   DCN 3.1+ moved that work to DMUB firmware mailbox commands. So the
   PHY/PLL step needs either the DMUB mailbox (if the GOP left DMUB
   running) or register-level reverse engineering from lit-vs-dormant pipe
   diffs (`rdna4-modedump=1`). OTG timing and DIG encoder registers are
   directly programmable either way.
4. ~~**Display power management**~~ — **done** (verified on hardware
   2026-07-11). The driver registers sleep/doze/wake power states with PM
   (`registerPowerDriver` from `enableController()`, mirroring
   `IONDRVFramebuffer::initForPM` — the subclass must do this itself, and
   must not do it from `start()`, which hangs boot). On power changes it
   disables the DP video stream (`DP_VID_STREAM_CNTL`) and puts the sink in
   D3 via a native-AUX DPCD `SET_POWER` write (monitor enters true standby),
   reversing both on wake. The timing generator and clocks are untouched.
   **System sleep is deliberately vetoed** (`kIOPMPreventSystemSleep`, like
   Apple's `IOBootNDRV`): after GPU power loss we cannot reprogram the
   display pipe until native mode setting exists, so allowing it would mean
   waking to a black screen. Opt out of display sleep handling with
   `rdna4-nosleep=1`.
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
- [x] Kill-switch boot-arg (`rdna4-off=1`) for safe iteration
- [x] DP AUX software engine + EDID read over I2C-over-AUX (verified on
      hardware: Samsung 4K sink on AUX0), served via `getDDCBlock()`
- [x] HDMI/DVI EDID over the DC_I2C hardware engine (verified on hardware:
      Lenovo 1080p sink on ddc2; the engine must be woken — soft-reset
      deasserted, RAM out of light sleep, DDC clock enabled — before
      arbitration is requested)
- [x] Display sleep verified on hardware: stream blank + sink D3 over native
      AUX on sleep, D0 + stream re-enable on wake (system sleep vetoed until
      mode setting exists)
- [ ] Hardware cursor via the DCN cursor plane (implemented behind
      `rdna4-hwcursor=1`; needs hardware verification)
- [ ] Native mode setting (DCN 4.1.0) / multiple displays
- [ ] Acceleration / Metal
