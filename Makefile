# Bare-metal nRF52840 build for Faketec v4. Uses the system arm-none-eabi-gcc and the project venv.
#   make lab         radio lab / bring-up firmware (RX only)
#   make repeater    low-power MeshCore repeater
#   make release     clean repeater build with shipping defaults -> build/release (hex, zip, -sd.zip, uf2)
#   make test        host unit tests
#   make flash-<target> PORT=/dev/ttyACM0

TP       := third_party
# host tools: a Python with adafruit-nrfutil + pyserial (e.g. a venv: make PYTHON=venv/bin/python)
PYTHON   ?= python3
NRFUTIL  ?= adafruit-nrfutil
PORT     ?= /dev/ttyACM0
BUILD    := build

CROSS    := arm-none-eabi-
CC       := $(CROSS)gcc
CXX      := $(CROSS)g++
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size

CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
DEFS     := -DNRF52840_XXAA -DCONFIG_NFCT_PINS_AS_GPIOS \
            -D__STACK_SIZE=16384 -D__HEAP_SIZE=49152 -DCFG_TUSB_MCU=OPT_MCU_NRF5X
INCS     := -Isrc -Isrc/hal -I$(TP)/cmsis -I$(TP)/nrfx -I$(TP)/nrfx/mdk -I$(TP)/nrfx/hal \
            -I$(TP)/nrfx/templates -I$(TP)/nrfx/drivers/include -I$(TP)/nrfx/drivers/src \
            -I$(TP)/tinyusb/src
OPT      ?= -Os
COMMON   := $(CPUFLAGS) $(OPT) -g3 -ffunction-sections -fdata-sections -fno-common -Wall \
            -Wno-unused-parameter -MMD -MP $(DEFS) $(INCS)
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
LDFLAGS  := $(CPUFLAGS) -Tld/app.ld -L$(TP)/nrfx/mdk -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs \
            -Wl,--print-memory-usage

HAL_SRC  := $(wildcard src/hal/*.c) \
            $(TP)/nrfx/mdk/system_nrf52840.c $(TP)/nrfx/mdk/gcc_startup_nrf52840.S
USB_SRC  := $(TP)/tinyusb/src/tusb.c $(TP)/tinyusb/src/common/tusb_fifo.c \
            $(TP)/tinyusb/src/device/usbd.c $(TP)/tinyusb/src/device/usbd_control.c \
            $(TP)/tinyusb/src/class/cdc/cdc_device.c \
            $(TP)/tinyusb/src/portable/nordic/nrf5x/dcd_nrf5x.c

# Nordic S140 6.1.1 SoftDevice hex, only for the *-sd.zip packages (not shipped here: see docs/DEVELOPMENT.md)
SD_HEX   ?= third_party/softdevice/s140_nrf52_6.1.1_softdevice.hex

# ---------------------------------------------------------------- targets
LAB_SRC  := src/lab/main.c $(HAL_SRC) $(USB_SRC)

-include repeater.mk

# host unit tests (flash file system with simulated power loss)
HOSTCXX ?= g++
.PHONY: test
test:
	@mkdir -p $(BUILD)/host
	$(HOSTCXX) -std=gnu++17 -O1 -g -fsanitize=address,undefined -Isrc/compat -Isrc -Isrc/hal \
	  test/test_flashfs.cpp src/compat/flashfs.cpp -o $(BUILD)/host/test_flashfs
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/host/test_flashfs   # power-loss longjmps leak on purpose

.PHONY: all lab clean
all: lab

lab: $(BUILD)/lab.hex $(BUILD)/lab.zip

obj = $(patsubst %,$(BUILD)/obj/%.o,$(1))

$(BUILD)/lab.elf: $(call obj,$(LAB_SRC))
	$(CC) $(LDFLAGS) -Wl,-Map=$(BUILD)/lab.map -o $@ $^ -lm
	$(SIZE) $@

$(BUILD)/obj/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NET_DEFS) -c $< -o $@

$(BUILD)/obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

%.hex: %.elf
	$(OBJCOPY) -O ihex $< $@

# application-only DFU package, requires S140 6.1.1 already on the chip (bootloader checks sd-req)
%.zip: %.hex
	$(NRFUTIL) dfu genpkg --dev-type 0x0052 --sd-req 0x00B6 --application $< $@ >/dev/null

# SoftDevice + application package (use once if the SoftDevice was erased)
%-sd.zip: %.hex
	$(NRFUTIL) dfu genpkg --dev-type 0x0052 --sd-req 0xFFFE --softdevice $(SD_HEX) --application $< $@ >/dev/null

# UF2 for drag-and-drop onto the bootloader's USB drive (double-tap RESET); also needs S140 6.1.1
UF2CONV  := tools/uf2conv.py
%.uf2: %.hex
	$(PYTHON) $(UF2CONV) -c -f 0xADA52840 -o $@ $< >/dev/null

flash-%: $(BUILD)/%.zip
	$(PYTHON) tools/dfu.py $(PORT) $<

flashsd-%: $(BUILD)/%-sd.zip
	$(PYTHON) tools/dfu.py $(PORT) $<

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
