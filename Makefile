# Makefile — cross-compile the RDNA4FB kext for x86_64 (Intel hackintosh)
# from any host, including Apple Silicon. Targets macOS 11 (Big Sur) ABI.
#
#   make            build RDNA4FB.kext
#   make clean      remove build products
#
# The kext links against Lilu at load time (OSBundleLibraries); Lilu symbols
# are left undefined in the -kext bundle and resolved by the kernel loader.

PRODUCT      := RDNA4FB
BUNDLE_ID    := com.hackintosh.RDNA4FB
VERSION      := 0.0.1

ARCH         := x86_64
DEPLOY       := 11.0
SDK          := $(shell xcrun --sdk macosx --show-sdk-path)

MKSDK        := MacKernelSDK

BUILD        := build
KEXT         := $(BUILD)/$(PRODUCT).kext
MACOS        := $(KEXT)/Contents/MacOS
EXEC         := $(MACOS)/$(PRODUCT)

CXX          := clang++
CC           := clang

# --- sources -----------------------------------------------------------------
CXX_SRCS := \
	src/framebuffer.cpp \
	src/atombios.cpp \
	src/ipdiscovery.cpp \
	src/edid.cpp \
	src/otgtiming.cpp

C_SRCS := \
	src/kmod_info.c

OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(CXX_SRCS)) \
        $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))

# --- flags -------------------------------------------------------------------
COMMON_FLAGS := \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos$(DEPLOY) \
	-isysroot $(SDK) \
	-I$(MKSDK)/Headers \
	-mmacosx-version-min=$(DEPLOY) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-D_FORTIFY_SOURCE=0 \
	-nostdinc \
	-fno-builtin -fno-common -fno-stack-protector -mkernel -fapple-kext \
	-Wall -Os -g

CXXFLAGS := $(COMMON_FLAGS) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new
CFLAGS   := $(COMMON_FLAGS)

LDFLAGS := \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos$(DEPLOY) \
	-isysroot $(SDK) \
	-nostdlib \
	-Xlinker -kext \
	-L$(MKSDK)/Library/$(ARCH) \
	-lkmodc++ -lkmod \
	-Wl,-no_deduplicate

# --- host-side tools (native arch, run on the build Mac) ---------------------
ATOMDUMP := $(BUILD)/atomdump
FIRMWARE := firmware/Sapphire.RX9070XT.16384.241213.rom

# --- rules -------------------------------------------------------------------
.PHONY: all clean test
all: $(KEXT)

$(ATOMDUMP): tools/atomdump.cpp src/atombios.cpp src/atombios.hpp src/ipdiscovery.cpp src/ipdiscovery.hpp src/edid.cpp src/edid.hpp src/otgtiming.cpp src/otgtiming.hpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -Wall -O2 -o $@ tools/atomdump.cpp src/atombios.cpp src/ipdiscovery.cpp src/edid.cpp src/otgtiming.cpp

atomdump: $(ATOMDUMP)

# Runs the kext's AtomBIOS parser (compiled for the host) against the real
# ROM dump — verifies parsing logic without GPU hardware.
test: $(ATOMDUMP)
	$(ATOMDUMP) $(FIRMWARE)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Ensure kmod_info.o is linked in the required position: user objects,
# then -lkmodc++, then kmod_info.o, then -lkmod.
KMOD_OBJ := $(BUILD)/src/kmod_info.o
USER_OBJS := $(filter-out $(KMOD_OBJ),$(OBJS))

$(EXEC): $(OBJS)
	@mkdir -p $(MACOS)
	$(CXX) $(LDFLAGS) $(USER_OBJS) $(KMOD_OBJ) -o $@

$(KEXT): $(EXEC) Info.plist
	@mkdir -p $(KEXT)/Contents
	cp Info.plist $(KEXT)/Contents/Info.plist
	@# Minimal, ad-hoc code signature so kextutil is happier during testing.
	codesign --force --sign - $(KEXT) 2>/dev/null || true
	@echo "Built $(KEXT) for $(ARCH) (min macOS $(DEPLOY))"
	@file $(EXEC)

clean:
	rm -rf $(BUILD)
