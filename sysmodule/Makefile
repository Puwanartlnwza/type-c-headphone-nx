ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set")
endif

export PATH := $(DEVKITPRO)/devkitA64/bin:$(DEVKITPRO)/tools/bin:$(PATH)

TARGET  := usbaudio
TID     := 4200000000005B1A
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -O2 -Wall -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ -I$(DEVKITPRO)/libnx/include
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections
LIBS    := -L$(DEVKITPRO)/libnx/lib -lnx -lm
SRC     := $(wildcard source/*.c)
HDR     := $(wildcard source/*.h)

all: dist

$(TARGET).elf: $(SRC) $(HDR)
	aarch64-none-elf-gcc $(CFLAGS) $(SRC) $(LDFLAGS) $(LIBS) -o $@

$(TARGET).nso: $(TARGET).elf
	elf2nso $< $@

$(TARGET).npdm: sysmodule.json
	npdmtool $< $@

dist: $(TARGET).nso $(TARGET).npdm
	rm -rf out
	mkdir -p out/atmosphere/contents/$(TID)/exefs out/atmosphere/contents/$(TID)/flags
	cp $(TARGET).nso out/atmosphere/contents/$(TID)/exefs/main
	cp $(TARGET).npdm out/atmosphere/contents/$(TID)/exefs/main.npdm
	touch out/atmosphere/contents/$(TID)/flags/boot2.flag

clean:
	rm -rf out $(TARGET).elf $(TARGET).nso $(TARGET).npdm
