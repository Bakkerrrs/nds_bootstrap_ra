#---------------------------------------------------------------------------------
# The WiFi probe: a standalone DSi-mode homebrew that answers whether WiFi works on this
# console at all, before any of it becomes nds-bootstrap's problem. See arm9/source/main.c
# for what it tests and docs/retroachievements.md for why it comes first.
#
# Deliberately outside the main build. `make package-nightly` at the top level does not
# touch this, and this does not touch nds-bootstrap: the probe is only useful as a control,
# and a control that shares code with the thing it is controlling is not one.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

export TARGET := wifiprobe
export TOPDIR := $(CURDIR)

GAME_ICON :=
NITRO_FILES :=

GAME_TITLE     := nds-bootstrap RA WiFi probe
GAME_SUBTITLE1 := Does DSi WiFi work here
GAME_SUBTITLE2 := before the game runs

include $(DEVKITARM)/ds_rules

.PHONY: all dsiwifi arm7/$(TARGET).elf arm9/$(TARGET).elf clean

all: $(TARGET).nds

#---------------------------------------------------------------------------------
# dsiwifi is a submodule and builds into its own tree. Built here rather than expected
# to be installed, so the probe works from a fresh clone with nothing put in $DEVKITPRO.
#---------------------------------------------------------------------------------
dsiwifi:
	@[ -f dsiwifi/Makefile ] || { \
		echo "tools/wifiprobe/dsiwifi is empty -- run: git submodule update --init"; \
		exit 1; \
	}
	@$(MAKE) -C dsiwifi release

#---------------------------------------------------------------------------------
# -u 00030004 marks this a DSi title, which is not cosmetic: launched as an NTR title the
# console never opens SCFG, isDSiMode() is false, the ARM7 skips the extended I/O setup and
# the Atheros chip is simply not on the bus. The probe would then report "no WiFi" for a
# reason that has nothing to do with the question being asked -- the one failure mode a
# control experiment must not have. Same flags the fork's own homebrew build uses.
#---------------------------------------------------------------------------------
$(TARGET).nds: arm7/$(TARGET).elf arm9/$(TARGET).elf
	ndstool -c $(TARGET).nds -7 arm7/$(TARGET).elf -9 arm9/$(TARGET).elf \
		-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)" \
		-g KBWP 01 "RAWIFIPROBE" -z 80040000 -u 00030004
	@echo "built ... $(TARGET).nds"

arm7/$(TARGET).elf: dsiwifi
	$(MAKE) -C arm7

arm9/$(TARGET).elf: dsiwifi
	$(MAKE) -C arm9

clean:
	$(MAKE) -C arm9 clean
	$(MAKE) -C arm7 clean
	$(MAKE) -C dsiwifi clean
	rm -f $(TARGET).nds
