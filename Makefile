ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set")
endif

export PATH := $(DEVKITPRO)/devkitA64/bin:$(DEVKITPRO)/tools/bin:$(PATH)

TARGET  := usbaudio_probe
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -O2 -Wall -ffunction-sections $(ARCH) -D__SWITCH__ -I$(DEVKITPRO)/libnx/include
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH)
LIBS    := -L$(DEVKITPRO)/libnx/lib -lnx

all: $(TARGET).nro

$(TARGET).elf: source/main.c
	aarch64-none-elf-gcc $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

$(TARGET).nacp:
	nacptool --create "USB Audio Probe" "user" "0.1.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp

clean:
	rm -f $(TARGET).elf $(TARGET).nro $(TARGET).nacp
