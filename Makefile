# KaiVesper — Switch 2 native hbmenu app (.nro) for kaiHEN
# Build inside ounce-devkit Docker image:
#   ./build.sh
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment (build inside ounce-devkit Docker image)")
endif

TARGET  := kaivesper
BUILD   := build
PREFIX  := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
include $(DEVKITPRO)/libounce/switch2_cpu.mk

ARCH    := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS  := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -Wstack-usage=16384 -ffunction-sections -fdata-sections \
           -D__SWITCH__ -D__SWITCH2__ \
           -I$(DEVKITPRO)/portlibs/switch2/include -I$(DEVKITPRO)/libounce/include -Isource
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libounce/switch.specs -Wl,--gc-sections
LIBS    := -L$(DEVKITPRO)/portlibs/switch2/lib -L$(DEVKITPRO)/libounce/lib -lkaihen -lnx

SRCS    := source/main.c source/gui.c source/sd_scanner.c source/profile_manager.c source/save_manager.c source/romfs_builder.c source/raw_inflate.c source/console_titles.c source/kaihen_ro.c source/fs_discover.c source/fs_installer.c
OBJS    := $(patsubst source/%.c,$(BUILD)/%.o,$(SRCS))

all: $(TARGET).nro

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: source/%.c | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(PREFIX)gcc $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/$(TARGET).nacp: Makefile | $(BUILD)
	$(DEVKITPRO)/tools/bin/nacptool --create "KaiVesper" "sank-kab" "0.2.1-beta" $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp icon.jpg
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ --nacp=$(BUILD)/$(TARGET).nacp --icon=icon.jpg

clean:
	rm -rf $(BUILD) $(TARGET).nro

.PHONY: all clean
