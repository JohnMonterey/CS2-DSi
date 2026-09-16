# melonDS emulator targets. Included from the root Makefile with:
#     -include tools/emu/emu.mk
#
# The emulator runs the PLAIN build (no DSIDEV=1): the dsidev runtime exists to talk to
# a real console over Wi-Fi, and melonDS has no console to talk to. That is the whole
# difference between `make run-dsi` and `make emu-run`.

EMU_PY    ?= $(PYTHON)
EMU_ROM   ?= Counter-Strike-nds/Counter-Strike-nds.nds
EMU_SD    ?= Counter-Strike-nds/Counter Strike DS package sample Emulator/counter_strike_sd.raw
EMU_SCALE ?= 2
# Where melonDS.app lives, if it is not in /Applications or a path the harness already
# knows. The Homebrew cask was disabled on 2026-09-01, so a source build is the normal
# case and it can sit anywhere.
EMU_APP   ?=
EMU_ENV    = PYTHONPATH=tools $(if $(EMU_APP),MELONDS_APP="$(EMU_APP)",)
# `make test-ui UI=boot` runs one test; empty runs all of them.
UI        ?=

.PHONY: emu-build emu-run emu-doctor test-ui test-ui-update emu-clean

emu-build:
	$(MAKE) -C Counter-Strike-nds $(if $(NITRO_ENGINE),NITRO_ENGINE=$(NITRO_ENGINE),)

emu-run: emu-build
	$(EMU_ENV) $(EMU_PY) -m emu.launch --rom "$(EMU_ROM)" --scale $(EMU_SCALE)

emu-doctor:
	$(EMU_ENV) $(EMU_PY) -m emu.doctor --rom "$(EMU_ROM)" --sd "$(EMU_SD)"

test-ui: emu-build
	$(EMU_ENV) $(EMU_PY) -m emu.uitest --rom "$(EMU_ROM)" --scale $(EMU_SCALE) $(UI)

test-ui-update: emu-build
	$(EMU_ENV) $(EMU_PY) -m emu.uitest --rom "$(EMU_ROM)" --scale $(EMU_SCALE) --update $(UI)

emu-clean:
	$(RM) -r tests/ui/.out
