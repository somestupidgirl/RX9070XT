//
//  atomdump.cpp
//  RDNA4FB
//
//  Host-side harness for the kext's AtomBios parser. Compiles the exact same
//  src/atombios.cpp the kext uses and runs it against a ROM dump, so the
//  parsing logic is verified on the developer machine without GPU hardware.
//
//    make atomdump
//    ./build/atomdump firmware/Sapphire.RX9070XT.16384.241213.rom
//
//  Exits nonzero if the image fails to parse or expected tables are missing,
//  so `make test` can gate on it.
//

#include "../src/atombios.hpp"
#include "../src/ipdiscovery.hpp"
#include "../src/edid.hpp"
#include "../src/otgtiming.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The Samsung LS27DG702 (Odyssey G70D) full 256-byte EDID (base + CTA-861
// extension) captured from the live system's IODisplayEDID (2026-07-11) —
// fixture for the DTD/CTA parsers and the OTG timing computation.
static const char kSamsungEdidHex[] =
	"00ffffffffffff004c2d6fe0000000000e220104b53c22783b32b5ad5045a125"
	"0e505421880081c0810081809500a9c0b300010101014dd000a0f0703e803020"
	"3500ba882100001a000000fd0c309045458f010a202020202020000000fc004f"
	"64797373657920473730440a000000ff004831414b3530303030300a2020027a"
	"02033af04761103f04035f762309070783010000e305c301741a0000030f3090"
	"000060085b3690000000000000e6060501605b00e5018b84903d565e00a0a0a0"
	"295030203500ba882100001a6fc200a0a0a0555030203500ba882100001a0474"
	"801871382d40582c4500ba882100001e00000000000000000000000000000005";

static bool hexToBytes(const char *hex, uint8_t *out, size_t outLen) {
	for (size_t i = 0; i < outLen; i++) {
		unsigned v;
		if (sscanf(hex + 2 * i, "%2x", &v) != 1)
			return false;
		out[i] = static_cast<uint8_t>(v);
	}
	return true;
}

// Verify the EDID DTD parser against the known 4K60 preferred timing.
static int testEdidParser() {
	int failures = 0;
	uint8_t edid[256];
	if (!hexToBytes(kSamsungEdidHex, edid, sizeof(edid))) {
		fprintf(stderr, "FAIL: EDID fixture is malformed\n");
		return 1;
	}

	Edid::DetailedTiming t {};
	if (!Edid::preferredTiming(edid, sizeof(edid), t)) {
		fprintf(stderr, "FAIL: preferred timing did not parse\n");
		return 1;
	}

	printf("\nedid fixture (Samsung Odyssey G70D):\n");
	printf("  preferred: %ux%u@%u.%03uHz pclk=%ukHz\n", t.hActive, t.vActive,
	       t.refreshMilliHz() / 1000, t.refreshMilliHz() % 1000, t.pixelClockKHz);
	printf("  h: blank=%u fp=%u sw=%u pol=%c   v: blank=%u fp=%u sw=%u pol=%c\n",
	       t.hBlank, t.hSyncOffset, t.hSyncWidth, t.hSyncPositive ? '+' : '-',
	       t.vBlank, t.vSyncOffset, t.vSyncWidth, t.vSyncPositive ? '+' : '-');

	struct { const char *name; uint32_t got, want; } checks[] = {
		{ "hActive",  t.hActive,        3840 },
		{ "vActive",  t.vActive,        2160 },
		{ "pclk",     t.pixelClockKHz,  533250 },
		{ "hBlank",   t.hBlank,         160 },
		{ "vBlank",   t.vBlank,         62 },
		{ "hSyncOff", t.hSyncOffset,    48 },
		{ "hSyncW",   t.hSyncWidth,     32 },
		{ "vSyncOff", t.vSyncOffset,    3 },
		{ "vSyncW",   t.vSyncWidth,     5 },
		{ "hTotal",   t.hTotal(),       4000 },
		{ "vTotal",   t.vTotal(),       2222 },
	};
	for (auto &c : checks) {
		if (c.got != c.want) {
			fprintf(stderr, "FAIL: EDID %s = %u, expected %u\n", c.name, c.got, c.want);
			failures++;
		}
	}
	// 533250000 / (4000*2222) = 59.99 Hz
	if (t.refreshMilliHz() / 100 != 599) {
		fprintf(stderr, "FAIL: refresh %u mHz, expected ~59.99 Hz\n", t.refreshMilliHz());
		failures++;
	}

	// OTG register images per optc1_program_timing. These exact raw values
	// must appear in the OTG0 line of a rdna4-modedump log (the GOP
	// programmed the same timing) — hardware validation of the math before
	// any OTG is written by us.
	OtgTiming::Regs r {};
	if (!OtgTiming::compute(t, r)) {
		fprintf(stderr, "FAIL: OTG computation rejected the 4K60 timing\n");
		return failures + 1;
	}
	printf("  expected OTG0 (diff vs modedump): h_total=0x%08x h_blank=0x%08x "
	       "h_sync=0x%08x\n                                    v_total=0x%08x "
	       "v_blank=0x%08x v_sync=0x%08x\n",
	       r.hTotal, r.hBlankStartEnd, r.hSyncA, r.vTotal, r.vBlankStartEnd, r.vSyncA);
	struct { const char *name; uint32_t got, want; } otgChecks[] = {
		{ "OTG_H_TOTAL",           r.hTotal,         3999 },
		{ "OTG_H_SYNC_A",          r.hSyncA,         32u << 16 },
		{ "OTG_H_BLANK_START_END", r.hBlankStartEnd, 3952u | (112u << 16) },
		{ "OTG_V_TOTAL",           r.vTotal,         2221 },
		{ "OTG_V_SYNC_A",          r.vSyncA,         5u << 16 },
		{ "OTG_V_BLANK_START_END", r.vBlankStartEnd, 2219u | (59u << 16) },
	};
	for (auto &c : otgChecks) {
		if (c.got != c.want) {
			fprintf(stderr, "FAIL: %s = 0x%08x, expected 0x%08x\n", c.name, c.got, c.want);
			failures++;
		}
	}
	if (r.hSyncPolInvert != false || r.vSyncPolInvert != true) {
		fprintf(stderr, "FAIL: sync polarity inversion (h=%d v=%d), expected h=0 v=1\n",
		        r.hSyncPolInvert, r.vSyncPolInvert);
		failures++;
	}

	// CTA-861 extension: capabilities the HDMI mode-set must respect.
	Edid::CtaCaps cta {};
	if (!Edid::parseCtaBlock(edid + 128, cta)) {
		fprintf(stderr, "FAIL: CTA extension did not parse\n");
		return failures + 1;
	}
	printf("  cta: rev %u underscan=%d audio=%d ycbcr444=%d ycbcr422=%d "
	       "vics=%zu hdmi_vsdb=%d max_tmds=%ukHz dtds=%zu\n",
	       cta.revision, cta.underscan, cta.basicAudio, cta.ycbcr444,
	       cta.ycbcr422, cta.vicCount, cta.hasHdmiVsdb, cta.maxTmdsKHz,
	       cta.dtdCount);
	if (cta.dtdCount)
		printf("  cta first dtd: %ux%u pclk=%ukHz\n",
		       cta.firstDtd.hActive, cta.firstDtd.vActive, cta.firstDtd.pixelClockKHz);
	if (cta.revision != 3 || !cta.basicAudio || !cta.ycbcr444 || !cta.ycbcr422) {
		fprintf(stderr, "FAIL: CTA flags/revision mismatch\n");
		failures++;
	}
	bool has4k60 = false, has1080p60 = false;
	for (size_t i = 0; i < cta.vicCount; i++) {
		if (cta.vics[i] == 97) has4k60 = true;    // 3840x2160p60
		if (cta.vics[i] == 16) has1080p60 = true; // 1920x1080p60
	}
	if (!has4k60 || !has1080p60) {
		fprintf(stderr, "FAIL: expected VICs 97 and 16 in the video block\n");
		failures++;
	}
	// This is a DisplayPort sink: no HDMI LLC vendor block expected.
	if (cta.hasHdmiVsdb) {
		fprintf(stderr, "FAIL: unexpected HDMI VSDB on a DP sink\n");
		failures++;
	}
	return failures;
}

static const char *tableNames[AtomBios::MaxDataTable] = {
	"utilitypipeline", "multimedia_info", "smc_dpm_info", "sw_datatable3",
	"firmwareinfo", "sw_datatable5", "lcd_info", "sw_datatable7", "smu_info",
	"sw_datatable9", "sw_datatable10", "vram_usagebyfirmware", "gpio_pin_lut",
	"sw_datatable13", "gfx_info", "powerplayinfo", "sw_datatable16",
	"sw_datatable17", "sw_datatable18", "sw_datatable19", "sw_datatable20",
	"sw_datatable21", "displayobjectinfo", "indirectioaccess", "umc_info",
	"sw_datatable24", "dce_info", "vram_info", "sw_datatable27",
	"integratedsysteminfo", "asic_profiling_info", "voltageobject_info",
	"sw_datatable32", "sw_datatable33", "sw_datatable34",
};

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <vbios.rom>\n", argv[0]);
		return 2;
	}

	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> rom(static_cast<size_t>(len));
	if (fread(rom.data(), 1, rom.size(), f) != rom.size()) {
		fprintf(stderr, "short read\n");
		fclose(f);
		return 2;
	}
	fclose(f);

	AtomBios bios;
	if (!bios.init(rom.data(), rom.size())) {
		fprintf(stderr, "FAIL: no valid AtomBIOS image found in %s\n", argv[1]);
		return 1;
	}

	char name[64];
	bios.configName(name, sizeof(name));
	printf("VBIOS image     : offset 0x%zx, length %zu bytes, checksum OK\n",
	       bios.imageOffset(), bios.imageLength());
	printf("config name     : %s\n", name);
	printf("subsystem       : %04x:%04x\n", bios.subsystemVendorId(), bios.subsystemId());

	printf("\ndata tables:\n");
	for (uint32_t i = 0; i < AtomBios::MaxDataTable; i++) {
		AtomBios::TableHeader th;
		size_t off = bios.dataTable(static_cast<AtomBios::DataTable>(i), &th);
		if (off)
			printf("  [%2u] %-22s @0x%05zx size=%-5u rev %u.%u\n",
			       i, tableNames[i], off, th.size, th.formatRev, th.contentRev);
	}

	int failures = 0;

	AtomBios::FirmwareInfo3 fw;
	if (bios.getFirmwareInfo(fw)) {
		printf("\nfirmwareinfo v%u.%u:\n", fw.header.formatRev, fw.header.contentRev);
		printf("  firmware revision : 0x%08x\n", fw.firmwareRevision);
		printf("  bootup sclk       : %u kHz\n", fw.bootupSclk10KHz * 10);
		printf("  bootup mclk       : %u kHz\n", fw.bootupMclk10KHz * 10);
		printf("  capability flags  : 0x%08x\n", fw.firmwareCapability);
	} else {
		fprintf(stderr, "FAIL: firmwareinfo not parsed\n");
		failures++;
	}

	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = bios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	if (n) {
		printf("\ndisplay paths (%zu):\n", n);
		for (size_t i = 0; i < n; i++) {
			auto type = AtomBios::connectorType(paths[i].connectorObjId);
			printf("  %zu: %-11s objid=0x%04x encoder=0x%04x devtag=0x%04x\n",
			       i, AtomBios::connectorName(type), paths[i].connectorObjId,
			       paths[i].encoderObjId, paths[i].deviceTag);

			AtomBios::PathRecords rec;
			if (bios.getPathRecords(paths[i], rec)) {
				printf("     i2c: id=0x%02x hw=%d ddc-line=%u   hpd: pin=%u state=%u\n",
				       rec.i2cId, rec.i2cHwCapable, rec.ddcLine, rec.hpdPin, rec.hpdPlugState);
				AtomBios::GpioPin ddc, hpd;
				if (rec.hasI2c && bios.findGpioPin(static_cast<uint8_t>(0x90 | rec.ddcLine), ddc))
					printf("     ddc gpio: reg_index=0x%05x (byte 0x%06x) shift=%u\n",
					       ddc.regIndex, ddc.regIndex * 4, ddc.shift);
				if (rec.hasHpd && bios.findGpioPin(rec.hpdPin, hpd))
					printf("     hpd gpio: reg_index=0x%05x (byte 0x%06x) shift=%u\n",
					       hpd.regIndex, hpd.regIndex * 4, hpd.shift);
			} else {
				fprintf(stderr, "FAIL: path %zu has no I2C/HPD records\n", i);
				failures++;
			}
		}
	} else {
		fprintf(stderr, "FAIL: no display paths parsed\n");
		failures++;
	}

	AtomBios::GpioPin pins[AtomBios::MaxGpioPins];
	size_t np = bios.getGpioPins(pins, AtomBios::MaxGpioPins);
	if (np) {
		printf("\ngpio pin lut (%zu pins):\n", np);
		for (size_t i = 0; i < np; i++)
			printf("  gpio_id=0x%02x reg_index=0x%05x shift=%-2u mask_shift=%u\n",
			       pins[i].gpioId, pins[i].regIndex, pins[i].shift, pins[i].maskShift);
	} else {
		fprintf(stderr, "FAIL: gpio pin lut not parsed\n");
		failures++;
	}

	IpDiscovery disc;
	if (disc.init(rom.data(), rom.size())) {
		printf("\nip discovery binary at 0x%zx, %u IPs on die 0:\n",
		       disc.binaryOffset(), disc.ipCount());
		static const struct { uint16_t id; const char *name; } wanted[] = {
			{ IpDiscovery::HwGc, "GC (gfx)" }, { IpDiscovery::HwDmu, "DMU (DCN)" },
			{ IpDiscovery::HwNbif, "NBIF" },   { IpDiscovery::HwMp0, "MP0 (PSP)" },
			{ IpDiscovery::HwMmhub, "MMHUB" }, { IpDiscovery::HwSdma0, "SDMA0" },
		};
		for (auto &w : wanted) {
			IpDiscovery::IpEntry ip;
			if (disc.findIp(w.id, 0, ip)) {
				printf("  %-9s v%u.%u.%u  bases:", w.name, ip.major, ip.minor, ip.revision);
				for (uint8_t b = 0; b < ip.numBases; b++)
					printf(" 0x%08x", ip.bases[b]);
				printf("\n");
			} else {
				fprintf(stderr, "FAIL: IP hw_id %u missing from discovery\n", w.id);
				failures++;
			}
		}

		// The register this maps is the kext's first MMIO smoke-test read:
		// RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE (NBIF seg 2, dword 0x00c3) —
		// VRAM size in MiB, expected 16384 on this card.
		uint32_t memsize;
		if (disc.regByteOffset(IpDiscovery::HwNbif, 0, 2, 0x00c3, memsize)) {
			printf("  RCC_CONFIG_MEMSIZE MMIO byte offset: 0x%x\n", memsize);
			if (memsize != 0x378c) {
				fprintf(stderr, "FAIL: unexpected RCC_CONFIG_MEMSIZE offset\n");
				failures++;
			}
		} else {
			fprintf(stderr, "FAIL: cannot derive RCC_CONFIG_MEMSIZE offset\n");
			failures++;
		}

		IpDiscovery::IpEntry gc;
		if (disc.findIp(IpDiscovery::HwGc, 0, gc) && gc.major != 12) {
			fprintf(stderr, "FAIL: GC major %u, expected 12 (RDNA4)\n", gc.major);
			failures++;
		}
	} else {
		fprintf(stderr, "FAIL: no IP discovery binary found\n");
		failures++;
	}

	// Command-function inventory. Finding (2026-07-12, this ROM): the display
	// bytecode routines (setpixelclock, transmitter control, encoder control)
	// are ABSENT — on DCN 3.1+ they were replaced by DMUB firmware mailbox
	// commands (DMUB_CMD__VBIOS_*), so mode setting cannot go through an
	// AtomBIOS interpreter on this card. Only asic_init survives.
	static const struct { uint8_t idx; const char *name; bool required; } cmds[] = {
		{ AtomBios::CmdAsicInit,               "asic_init",               true  },
		{ AtomBios::CmdDigEncoderControl,      "digxencodercontrol",      false },
		{ AtomBios::CmdSetPixelClock,          "setpixelclock",           false },
		{ AtomBios::CmdEnableDispPowerGating,  "enabledisppowergating",   false },
		{ AtomBios::CmdBlankCrtc,              "blankcrtc",               false },
		{ AtomBios::CmdEnableCrtc,             "enablecrtc",              false },
		{ AtomBios::CmdSelectCrtcSource,       "selectcrtc_source",       false },
		{ AtomBios::CmdSetDceClock,            "setdceclock",             false },
		{ AtomBios::CmdSetCrtcUsingDtdTiming,  "setcrtc_usingdtdtiming",  false },
		{ AtomBios::CmdDig1TransmitterControl, "dig1transmittercontrol",  false },
		{ AtomBios::CmdProcessAuxChannel,      "processauxchanneltransaction", false },
	};
	printf("\ncommand functions (bytecode):\n");
	for (auto &c : cmds) {
		AtomBios::CmdTableInfo info;
		if (bios.getCommandTable(c.idx, info)) {
			printf("  [%2u] %-28s @0x%05zx size=%-5u rev %u.%u\n", c.idx, c.name,
			       info.offset - bios.imageOffset(), info.size,
			       info.formatRev, info.contentRev);
		} else {
			printf("  [%2u] %-28s absent\n", c.idx, c.name);
			if (c.required) {
				fprintf(stderr, "FAIL: required command function %s missing\n", c.name);
				failures++;
			}
		}
	}

	failures += testEdidParser();

	if (failures) {
		fprintf(stderr, "\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
