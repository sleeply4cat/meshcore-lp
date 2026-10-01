# Low power MeshCore repeater (included from Makefile)

# Network defaults for a freshly erased node (all can be changed later from the CLI).
# Defaults are stock MeshCore's; override them for your mesh in config.mk (see config.mk.example).
-include config.mk
LORA_FREQ       ?= 869.618
LORA_BW         ?= 62.5
LORA_SF         ?= 8
LORA_CR         ?= 5
LORA_TX_POWER   ?= 22
PATH_HASH_MODE  ?=
NET_DEFS := -DLORA_FREQ=$(LORA_FREQ) -DLORA_BW=$(LORA_BW) -DLORA_SF=$(LORA_SF) -DLORA_CR=$(LORA_CR) \
  -DLORA_TX_POWER=$(LORA_TX_POWER) $(if $(PATH_HASH_MODE),-DLP_DEFAULT_PATH_HASH_MODE=$(PATH_HASH_MODE))

REP_DEFS := -DNRF52_PLATFORM -DARDUINO=10800 -DUSE_SX1262 $(NET_DEFS) \
  -DSX126X_RX_BOOSTED_GAIN=1 -DLP_QUIET_DEFAULTS=1 \
  -DADVERT_NAME='"LP Repeater"' -DADVERT_LAT=0.0 -DADVERT_LON=0.0 -DADMIN_PASSWORD='"password"' \
  -DMAX_NEIGHBOURS=50 -DENABLE_ADVERT_ON_BOOT=1 -DENABLE_PRIVATE_KEY_IMPORT=1 -DENABLE_PRIVATE_KEY_EXPORT=1 \
  -DFIRMWARE_VERSION='"v1.17.1-lp1"' -DFIRMWARE_BUILD_DATE='"$(shell LC_ALL=C date +'%d %b %Y')"' \
  $(LP_EXTRA)

REP_WARN := -Wno-class-memaccess -Wno-reorder -Wno-sign-compare -Wno-format -Wno-unused-variable -Wno-unused-but-set-variable

REP_INCS := -Isrc/compat -Isrc/meshcore -Isrc/repeater -I$(TP)/crypto -I$(TP)/ed25519

REP_SRC := $(wildcard src/repeater/*.cpp) $(wildcard src/compat/*.cpp) \
  $(wildcard src/meshcore/*.cpp) $(wildcard src/meshcore/helpers/*.cpp) \
  $(wildcard $(TP)/crypto/*.cpp) $(filter-out %/seed.c,$(wildcard $(TP)/ed25519/*.c)) \
  $(HAL_SRC) $(USB_SRC)

REP_OBJ := $(patsubst %,$(BUILD)/rep/%.o,$(REP_SRC))

.PHONY: repeater release
repeater: $(BUILD)/repeater.hex $(BUILD)/repeater.zip

# Shipping build: separate object tree, no test overrides (LP_EXTRA ignored), all package formats.
REL := $(BUILD)/release
# The *-sd.zip package (SoftDevice + app, for chips whose SoftDevice was erased) needs SD_HEX.
REL_PKGS = $(REL)/repeater.hex $(REL)/repeater.zip $(REL)/repeater.uf2 $(if $(wildcard $(SD_HEX)),$(REL)/repeater-sd.zip)
release:
	rm -rf $(REL)
	$(MAKE) --no-print-directory BUILD=$(REL) LP_EXTRA= $(REL_PKGS)
	@test -f $(SD_HEX) || echo "note: $(SD_HEX) not found, repeater-sd.zip skipped"
	cd $(REL) && sha256sum $(notdir $(REL_PKGS)) > SHA256SUMS

$(BUILD)/repeater.elf: $(REP_OBJ)
	$(CXX) $(LDFLAGS) -u _printf_float -Wl,-Map=$(BUILD)/repeater.map -o $@ $^ -lm
	$(SIZE) $@

$(BUILD)/rep/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-sign-compare -Wno-unused-variable $(REP_DEFS) $(REP_INCS) -c $< -o $@

$(BUILD)/rep/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(REP_WARN) $(REP_DEFS) $(REP_INCS) -c $< -o $@

$(BUILD)/rep/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@
