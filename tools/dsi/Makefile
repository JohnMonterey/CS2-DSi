.DEFAULT_GOAL := all
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
export DEVKITPRO DEVKITARM
# ds_rules bakes the current LIBPATHS into its %.elf rule as it is read, so ours has to
# exist before the include or the libnds search path never reaches the linker.
LIBPATHS := -L$(DEVKITPRO)/libnds/lib
ifeq ($(wildcard $(DEVKITARM)/ds_rules),)
$(error Missing devkitARM. Install the nds-dev group with devkitPro pacman; see README.md)
endif
include $(DEVKITARM)/ds_rules

PYTHON ?= python3
ROOT := $(CURDIR)
MODE ?= loader
OUT := $(ROOT)/.build/$(MODE)
GEN := $(ROOT)/.build/generated
# Matches the flags the pinned hbmenu loader sources are built with upstream.
ARCH := -march=armv5te -mtune=arm946e-s -mthumb -mthumb-interwork
CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections $(ARCH) -DARM9
CFLAGS += -I$(LIBNDS)/include -I$(ROOT)/common -I$(ROOT)/runtime -I$(GEN)
LDFLAGS = -specs=ds_arm9.specs -g $(ARCH) -Wl,--gc-sections,-Map,$(OUT)/$(MODE).map
LIBS := -ldswifi9 -lfat -lnds9
export DEPSDIR := $(OUT)
COMMON := protocol storage service
OFILES := $(addprefix $(OUT)/,$(addsuffix .o,$(COMMON))) $(OUT)/main.o
ifeq ($(MODE),loader)
GAME_TITLE := DSi dev loader
GAME_SUBTITLE1 := Wireless deployment
GAME_SUBTITLE2 := make run-dsi
ifeq ($(wildcard $(GEN)/nds_loader_arm9.c),)
ifeq ($(filter clean,$(MAKECMDGOALS)),)
$(error Missing generated chainloader. Run: python3 tools/dsi/fetch.py)
endif
endif
OFILES += $(OUT)/nds_loader_arm9.o $(OUT)/load.bin.o $(OUT)/bootstub.bin.o
LOADER_DEPS := $(GEN)/token.h
else ifeq ($(MODE),sample)
GAME_TITLE := DSi dev sample
GAME_SUBTITLE1 := Iteration smoke test
GAME_SUBTITLE2 := make run-dsi-sample
CFLAGS += -DDSIDEV_ENABLED
OFILES += $(OUT)/dsidev.o
else
$(error MODE must be loader or sample)
endif
export OFILES
LD := $(CC)

.PHONY: all clean
all: $(OUT)/$(MODE).nds

clean:
	@rm -rf $(ROOT)/.build/loader $(ROOT)/.build/sample $(ROOT)/*.lst

$(OUT):
	@mkdir -p $@
$(OUT)/%.o: common/%.c | $(OUT)
	$(CC) $(_EXTRADEFS) $(CFLAGS) -MMD -MP -c $< -o $@
$(GEN)/token.h:
	$(PYTHON) $(ROOT)/deploy.py token --header $@
$(OUT)/main.o: $(MODE)/main.c $(LOADER_DEPS) | $(OUT)
	$(CC) $(_EXTRADEFS) $(CFLAGS) -MMD -MP -c $< -o $@
$(OUT)/dsidev.o: runtime/dsidev.c | $(OUT)
	$(CC) $(_EXTRADEFS) $(CFLAGS) -MMD -MP -c $< -o $@
$(OUT)/nds_loader_arm9.o: $(GEN)/nds_loader_arm9.c $(GEN)/load_bin.h $(GEN)/bootstub_bin.h | $(OUT)
	$(CC) $(_EXTRADEFS) $(CFLAGS) -MMD -MP -c $< -o $@

$(GEN)/load.bin:
	$(MAKE) -C .deps/nds-bootloader LOADBIN=$(GEN)/load.bin
$(GEN)/bootstub.bin: .deps/nds-hb-menu/bootstub/bootstub.s | $(OUT)
	$(CC) -Wl,-Ttext=0 -x assembler-with-cpp -nostartfiles -nostdlib $< -o $(OUT)/bootstub.elf
	$(OBJCOPY) -O binary $(OUT)/bootstub.elf $@

# bin2s emits both assembly and a C header. Avoid parallel multi-target recipe races.
$(OUT)/%.bin.o: $(GEN)/%.bin | $(OUT)
	bin2s -H $(GEN)/$*_bin.h $< > $(OUT)/$*.bin.s
	$(CC) $(ARCH) -c $(OUT)/$*.bin.s -o $@
$(GEN)/load_bin.h: $(OUT)/load.bin.o
	@test -f $@
$(GEN)/bootstub_bin.h: $(OUT)/bootstub.bin.o
	@test -f $@

$(OUT)/$(MODE).elf: $(OFILES)
-include $(wildcard $(OUT)/*.d)
