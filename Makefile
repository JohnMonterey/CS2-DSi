.DEFAULT_GOAL := help
PYTHON ?= python3
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
export DEVKITPRO DEVKITARM
DSI_IP ?=
DSI_FLAGS ?=
LOGS ?=
DSI_ADDRESS = $(if $(DSI_IP),--ip $(DSI_IP),)
DSI_OPTIONS = $(DSI_ADDRESS) $(if $(LOGS),--logs,) $(DSI_FLAGS)
DSI_TOOL := $(PYTHON) tools/dsi/deploy.py
NITRO_ENGINE ?=
GAME_VARS = DSIDEV=1 $(if $(NITRO_ENGINE),NITRO_ENGINE=$(NITRO_ENGINE),)
# Runtime data the game reads from the card, pushed with every deploy and skipped when
# unchanged. Add any further runtime files here.
DSI_DATA = $(wildcard Counter-Strike-nds/build-dsidev/soundbank.bin) \
           $(wildcard Counter-Strike-nds/counter_strike_music.raw) \
           Counter-Strike-nds/movement.cfg
DSI_DATA_FLAGS = $(foreach file,$(DSI_DATA),--data $(file))

.PHONY: help setup-dsi doctor-dsi loader-dsi stage-dsi sample-dsi run-dsi run-dsi-sample \
        discover-dsi logs-dsi reset-dsi asset-dsi put-dsi update-loader-dsi test-dsi \
        test-movement test-anim serve test-server gamedata clean-dsi
help:
	@echo 'One-time setup:'
	@echo '  make setup-dsi        check the toolchain, build the loader, stage dsidev.nds'
	@echo '  make stage-dsi        rebuild the one file that goes on the SD card root'
	@echo 'Every iteration:'
	@echo '  make run-dsi          build, upload and launch on the DSi (add LOGS=1 to stay attached)'
	@echo '  make logs-dsi         listen for remote log output'
	@echo '  make reset-dsi        send a running build back to the loader'
	@echo 'Other:'
	@echo '  make run-dsi-sample   launch/return/logging smoke test without Nitro Engine'
	@echo '  make discover-dsi     list DSis answering on the network'
	@echo '  make asset-dsi FILE=x hot-reload one asset into the running build'
	@echo '  make put-dsi FILE=x    send a data file to sit beside the build on the card'
	@echo '  make update-loader-dsi replace the loader on the card over Wi-Fi'
	@echo '  make doctor-dsi       report on the local devkitPro installation'
	@echo '  make test-movement    host tests for the CS:GO movement core'
	@echo '  make test-anim        host tests for character animation, the rig and online smoothing'
	@echo 'Multiplayer:'
	@echo '  make serve            run a multiplayer server on this machine'
	@echo '  make test-server      protocol tests for the server'
	@echo '  make test-dsi         run the host tests for the deployment protocol'
	@echo 'Variables: DSI_IP=<addr> skips discovery, DSI_FLAGS=... passes deploy.py options.'
setup-dsi:
	$(MAKE) doctor-dsi
	$(MAKE) stage-dsi
doctor-dsi:
	$(PYTHON) tools/dsi/doctor.py
loader-dsi:
	$(PYTHON) tools/dsi/fetch.py
	$(MAKE) -C tools/dsi MODE=loader
stage-dsi: loader-dsi
	$(DSI_TOOL) stage tools/dsi/.build/loader/loader.nds
sample-dsi:
	$(MAKE) -C tools/dsi MODE=sample
run-dsi:
	$(MAKE) -C Counter-Strike-nds $(GAME_VARS)
	$(DSI_TOOL) run Counter-Strike-nds/Counter-Strike-nds-dev.nds $(DSI_DATA_FLAGS) $(DSI_OPTIONS)
run-dsi-sample: sample-dsi
	$(DSI_TOOL) run tools/dsi/.build/sample/sample.nds $(DSI_OPTIONS)
discover-dsi:
	$(DSI_TOOL) discover $(DSI_ADDRESS)
logs-dsi:
	$(DSI_TOOL) logs $(DSI_ADDRESS)
reset-dsi:
	$(DSI_TOOL) reset $(DSI_ADDRESS)
put-dsi:
	@test -n "$(FILE)" || (echo 'Use make put-dsi FILE=path'; exit 1)
	$(DSI_TOOL) put "$(FILE)" $(DSI_OPTIONS)
update-loader-dsi: loader-dsi
	$(DSI_TOOL) update-loader tools/dsi/.build/loader/loader.nds $(DSI_OPTIONS)
asset-dsi:
	@test -n "$(FILE)" || (echo 'Use make asset-dsi FILE=path'; exit 1)
	$(DSI_TOOL) asset "$(FILE)" $(DSI_OPTIONS)
# The original server was never published, so tools/server speaks the protocol
# read out of the client's own network.c. MODE 3 is deathmatch; MAP 0 is Dust2.
SERVE_MAP ?= 0
SERVE_MODE ?= 3
SERVE_FLAGS ?=
serve:
	$(PYTHON) tools/server/cs_server.py --map $(SERVE_MAP) --mode $(SERVE_MODE) $(SERVE_FLAGS)
test-server:
	$(PYTHON) -m unittest discover -s tools/server -p 'test_*.py' -v
# Re-extract weapon damage and spawn points from the game source.
gamedata:
	$(PYTHON) tools/server/gen_gamedata.py
test-dsi:
	$(PYTHON) -m unittest discover -s tools/dsi/tests -v
# Host tests for the movement core. playermove_core.c and movement_cfg.c have no
# Nitro Engine or libnds dependency, so they compile for the Mac and can be
# checked against real CS:GO reference numbers without the console.
MOVEMENT_TEST_BIN := Counter-Strike-nds/tests/movement/.build/test_movement
MOVEMENT_TEST_SRC := Counter-Strike-nds/tests/movement/test_movement.c \
                     Counter-Strike-nds/source/player/playermove_core.c \
                     Counter-Strike-nds/source/player/movement_cfg.c
test-movement:
	@mkdir -p $(dir $(MOVEMENT_TEST_BIN))
	$(CC) -std=gnu17 -Wall -Wextra -Werror -O2 \
	      -I Counter-Strike-nds/source/player \
	      -o $(MOVEMENT_TEST_BIN) $(MOVEMENT_TEST_SRC)
	$(MOVEMENT_TEST_BIN) Counter-Strike-nds/movement.cfg
# Host tests for the character animation core: the rig file the game embeds, the poses the
# console will draw from it, how online players move between position snapshots, and the
# tombstones the dead leave.
# Regenerate the rig with tools/assets/player_rig.py.
ANIM_TEST_BIN := Counter-Strike-nds/tests/anim/.build/test_anim
ANIM_TEST_SRC := Counter-Strike-nds/tests/anim/test_anim.c \
                 Counter-Strike-nds/source/player/character_anim_core.c \
                 Counter-Strike-nds/source/network/remote_lerp.c \
                 Counter-Strike-nds/source/player/tombstone_core.c
test-anim:
	@mkdir -p $(dir $(ANIM_TEST_BIN))
	$(CC) -std=gnu17 -Wall -Wextra -Werror -O2 \
	      -I Counter-Strike-nds/source/player -I Counter-Strike-nds/source/network \
	      -o $(ANIM_TEST_BIN) $(ANIM_TEST_SRC) -lm
	$(ANIM_TEST_BIN) Counter-Strike-nds/data/player_rig.bin
clean-dsi:
	$(MAKE) -C tools/dsi clean
	$(RM) -r tools/dsi/.build/tests tools/dsi/.build/sd Counter-Strike-nds/build-dsidev \
	         Counter-Strike-nds/tests/movement/.build Counter-Strike-nds/tests/anim/.build
