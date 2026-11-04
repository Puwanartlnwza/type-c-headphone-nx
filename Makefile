ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set")
endif

export PATH := $(DEVKITPRO)/devkitA64/bin:$(DEVKITPRO)/tools/bin:$(PATH)

TARGET   := usbaudio
LIBTESLA ?= libtesla
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS := -O2 -Wall -ffunction-sections $(ARCH) -D__SWITCH__ -I$(DEVKITPRO)/libnx/include -I$(LIBTESLA)/include \
            -std=c++20 -Wno-dangling-else -fno-rtti -fno-exceptions
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH)
LIBS     := -L$(DEVKITPRO)/libnx/lib -lnx -lm

all: dist

$(TARGET)_ovl.elf: source/main.cpp
	aarch64-none-elf-g++ $(CXXFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

$(TARGET).nacp:
	nacptool --create "USB Audio" "user" "0.1.0" $@

$(TARGET).ovl: $(TARGET)_ovl.elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp

dist: $(TARGET).ovl
	rm -rf out
	mkdir -p out/switch/.overlays
	cp $(TARGET).ovl out/switch/.overlays/$(TARGET).ovl

clean:
	rm -rf out *.elf *.nacp *.ovl
