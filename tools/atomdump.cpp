//
//  atomdump.cpp
//  RX9070XT
//
//  Host-side harness for the kext's AtomBios parser. Compiles the exact same
//  Source/AtomBios.cpp the kext uses and runs it against a ROM dump, so the
//  parsing logic is verified on the developer machine without GPU hardware.
//
//    make atomdump
//    ./build/atomdump firmware/Sapphire.RX9070XT.16384.241213.rom
//
//  Exits nonzero if the image fails to parse or expected tables are missing,
//  so `make test` can gate on it.
//

#include "../Source/AtomBios.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

	if (failures) {
		fprintf(stderr, "\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
