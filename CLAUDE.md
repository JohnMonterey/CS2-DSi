# Counter-Strike DS — working notes for agents

Homebrew game (Fewnity's CS:DS fork) with two targets:

- **Real DSi hardware.** Builds go over Wi-Fi via `tools/dsi/`. This is the target that
  counts — sound, framerate, input feel and Wi-Fi only exist here.
- **melonDS on the Mac.** A plain DS-mode build you can launch, drive and screenshot
  without a human in the room. This is where you check your own UI work before spending
  someone's attention on it.

**You cannot see the console.** On hardware the human is your hands and eyes: they watch
the screen, press buttons, and power-cycle. In the emulator you have both yourself. Use
the emulator to answer "does this look right", and hardware for everything the emulator
cannot model.

## The loop

```bash
# Emulator — yours alone
make emu-run          # build and launch the plain build in melonDS
make test-ui          # scripted input + screenshot diff. UI tests only, for now.
make emu-doctor       # why is the emulator not behaving

# Hardware — needs the human
make run-dsi          # build, push, launch on the DSi
make logs-dsi         # remote log stream (run in the background, read the file after)
make discover-dsi     # is the console reachable, and what is it running?
make reset-dsi        # send a running build back to the loader
make test-dsi         # 33 host tests for the deployment protocol
make test-movement    # host tests for the CS:GO movement core
make test-anim        # host tests for character animation, the player rig, online smoothing and tombstones
```

`make run-dsi` returns a running game to the loader, pushes any changed data files, pushes
the build, launches it, and waits until the build reports itself alive. A code change costs
~15 s of transfer; unchanged files are skipped by checksum in well under a second each.

Add `DSI_IP=192.168.0.166` to any of the `-dsi` targets to skip discovery if broadcast is
flaky.

---

# The emulator

## What it is and is not

melonDS runs the **plain** build (`Counter-Strike-nds.nds`, no `DSIDEV=1`). The dsidev
runtime exists to talk to a real console over Wi-Fi; there is no console here, so the
emulator build leaves it out. `make emu-build` and `make run-dsi` therefore produce two
different ROMs from the same sources — check you are looking at the one you think.

The emulator is honest about layout, menus, text, sprites, 3D geometry and touch hit-boxes.
It is **not** honest about framerate, audio timing, Wi-Fi, the chainload, DSi-only hardware,
or anything in `tools/dsi/`. Never close a hardware bug on emulator evidence.

melonDS has no scripting or automation API — the developers say so themselves, and their
suggested alternative (BizHawk) does not run usefully on Apple Silicon. So the harness in
`tools/emu/` drives melonDS from outside, through macOS: synthetic key and mouse events,
`screencapture` on the window, and `sips` to normalise. That is why the setup below is
fussier than "install an emulator", and why a failure can be the harness rather than the
game. `make emu-doctor` tells the two apart.

## One-time setup

1. **Build melonDS from source.** The Homebrew cask was disabled on 2026-09-01: Homebrew
   5.0 stopped shipping casks that fail the macOS Gatekeeper check, and melonDS's macOS
   builds are unsigned. Building locally sidesteps that — a binary you compiled yourself
   is not quarantined — and gets a current version rather than a stale one.

   ```bash
   brew install git pkg-config cmake sdl2 qt@6 libarchive enet zstd faad2
   git clone https://github.com/melonDS-emu/melonDS && cd melonDS
   cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6);$(brew --prefix libarchive)" \
         -DMACOS_BUNDLE_LIBS=ON
   cmake --build build -j$(sysctl -n hw.logicalcpu)
   ```

   The result is `build/melonDS.app`. Move it to `/Applications`, or leave it and pass
   `EMU_APP=/path/to/melonDS.app` (the harness also reads `MELONDS_APP`).
   `-DMACOS_BUNDLE_LIBS=ON` copies the Homebrew dylibs into the bundle so it survives a
   `brew upgrade`.

   The alternative is the prebuilt universal binary from melonds.kuribo64.net or the
   project's GitHub releases. It is unsigned and needs an explicit Gatekeeper exception
   (right-click → Open, or Privacy & Security → Open Anyway) — a real decision about
   running an unsigned binary, which building from source avoids having to make.

2. Launch melonDS once so it writes its config.
3. **Config → Emu settings → DLDI**: tick *Enable DLDI*, and point the image at
   `Counter-Strike-nds/Counter Strike DS package sample Emulator/counter_strike_sd.raw`.
   Upstream ships that 64 MB image for exactly this; without it the game finds no SD card
   and loads no sounds or `movement.cfg`.
4. **Screen layout must be vertical ("Natural") with a screen gap of 0.** The harness maps
   captures and touch coordinates off the window geometry; a gap shifts every one of them.
5. Grant the program you run tests from — Terminal, iTerm, your editor — both
   **Accessibility** (to post key events) and **Screen Recording** (to capture the window),
   in System Settings → Privacy & Security.
6. `make emu-doctor` until it reports nothing failing.

No BIOS or firmware dump is needed: this is a DS-mode build and melonDS boots it with its
own HLE BIOS.

## UI tests

**Tests are UI tests only for now.** Do not add emulator tests for logic that can be tested
on the host — movement belongs in `make test-movement`, the deploy protocol in
`make test-dsi`, the server in `make test-server`. An emulator test costs ten seconds of
wall clock and can fail for reasons that have nothing to do with your change; spend that
only on what genuinely needs pixels.

```bash
make test-ui                 # every UI test
make test-ui UI=boot         # one
make test-ui-update          # rewrite the reference images
```

A test is a module in `tests/ui/`:

```python
NAME = "boot to the first interactive screen"
TOLERANCE = {"max_mean": 3.0, "max_changed": 0.02}

def run(emu, shot):
    emu.wait(3.0)
    shot("splash")
    emu.press("Start")
    emu.wait(2.0)
    shot("after_start")
```

`shot(label)` captures both screens, normalises them to 256×384, and compares against
`tests/ui/refs/<test>/<label>.bmp`. Captures, viewable PNGs and a diff image land in
`tests/ui/.out/<test>/` on every run, pass or fail — look at them before theorising.

On `emu`: `wait(seconds)`, `press(button, frames=6)`, `hold(button, seconds)`,
`touch(x, y)` and `swipe((x1,y1), (x2,y2))` in DS touch coordinates (0–255 × 0–191), and
`focus()`. Buttons are `A B X Y L R Start Select Up Down Left Right`; the harness reads
your actual melonDS key bindings out of its config rather than assuming a layout.
`tests/ui/_template.py.txt` is a copyable starting point.

### Rules for reference images

- **Look at a reference before committing it.** `--update` will happily record a broken
  build, and from then on the test proves that the build stays broken.
- **One screen or one transition per test.** A failure at the seventh checkpoint of a long
  flow tells you nothing about the first six.
- **Re-record deliberately, never to make red go green.** If a diff is legitimate — you
  changed the HUD on purpose — re-record and say so in the commit. If you cannot explain
  the diff, it is a bug.
- References are display-dependent. They are recorded at a fixed 256×384, which survives
  Retina and window scaling, but a genuinely different GPU or melonDS version can still
  shift a pixel or two — that is what `TOLERANCE` absorbs. Widening tolerance to silence a
  real change is the failure mode to avoid.

### When a UI test fails and the game is fine

In rough order of likelihood: melonDS was not frontmost and the keys went elsewhere;
Screen Recording is not granted, so the capture is desktop wallpaper rather than the
window; the window got resized or the screen gap is not 0; the capture landed mid-animation
(raise the `wait` before the checkpoint, do not raise the tolerance); DLDI is off, so the
game is running without its data. `make emu-doctor` checks all but the timing one.

### Bring-up note

The harness's own logic — config parsing, key translation, touch maths, image diffing, the
runner — is covered by `tools/emu/verify_*.py` and passes. The macOS-facing half (event
posting, window geometry, capture) has not yet been run end to end on this machine, and
there are no reference images yet. The first `make emu-doctor` and `make test-ui-update`
are the bring-up step; delete this section once they have been through once.

---

# Hardware

## What you can do alone

- **Deploy and launch.** `make run-dsi` tells you a great deal by itself: it fails loudly
  if the console is unreachable, if a transfer is rejected, or if the launched build never
  reports in.
- **Read the console's mind.** `make discover-dsi` prints mode (`loader`/`app`), the build
  CRC and the store generation. `app <crc>` means your build is alive and serving.
- **Instrument.** `dsidev_log("...")` from anywhere in the game sends a line to the Mac.
  The game's `debugPrint()` and the Nitro Engine error handler already route there. Prefer
  adding a log line and redeploying over asking the human to describe something.
- **Push data without rebuilding.** `make put-dsi FILE=x` sends a file to `sd:/dsidev/`;
  `make asset-dsi FILE=x` pushes into the *running* build and fires its asset callback.
- **Replace the loader over Wi-Fi.** `make update-loader-dsi`. The card should never need
  to come out again.

## What only the human can do

Ask for these explicitly, one at a time, and say what you will do with the answer. Check
the emulator first — if the question is "does this menu look right", answer it yourself.

- **Describe the screen.** The decisive question whenever the console stops answering: "is
  the game on screen and playable, or is it black/frozen?" Those two answers point at
  completely different causes.
- **Press buttons** — including L+R+Start+Select to return a build to the loader by hand.
- **Power-cycle and reopen `dsidev.nds`** from TWiLight Menu++. Needed once per power
  cycle, and after any crash that leaves the console silent.
- **Confirm behaviour.** Sound, framerate, input feel, visual glitches. You cannot verify
  any of it. Do not claim a hardware-facing fix works until they say so.

## Reading failures

| What you see | What it means | Do |
| --- | --- | --- |
| `No DSi response` | Loader not open, console asleep, or a crashed build | Ask them to power-cycle and open `dsidev.nds` |
| `Application ready` never arrives, console still answers as `loader` | The launch failed; the loader prints why on screen | Ask what the loader screen says |
| Console answers as `app` but with the *old* CRC | Your build never took over | Re-check the deploy output |
| Console stops answering entirely after a launch | The build crashed, or its Wi-Fi never came up | Ask about the screen — playable means Wi-Fi, dead means a crash |
| `Already present` | Dedup; the file was identical | Nothing, this is normal |
| `application is running; return to the loader first` | Builds and data go through the loader | `make reset-dsi` (or let `run-dsi` do it) |

---

## Facts worth not rediscovering

- **The toolchain is strict.** devkitARM r68 ships GCC 16: C23 by default (`()` means
  `(void)`) and type mismatches are errors. `Counter-Strike-nds/Makefile` pins `-std=gnu17`
  and demotes two diagnostics in its `COMPAT` variable. Removing it means fixing ~40 UI
  callback signatures first.
- **Nitro Engine lives outside the repo** at `../nitro-engine` (Fewnity's fork, built
  there). The game Makefile finds it there, in `$DEVKITPRO/nitro-engine`, or via
  `NITRO_ENGINE=`. If `-lNE` stops resolving, rebuild it.
- **A launched build's working directory is `sd:/dsidev/`**, not the card root — libfat
  chdirs to the running `.nds`. Relative paths in game code resolve there, which is why
  runtime data is pushed there. New runtime file? Add it to `DSI_DATA` in the root Makefile
  and it ships with every deploy. In the emulator the same files come from the DLDI image
  instead, so a new runtime file needs adding to `counter_strike_sd.raw` too or the
  emulator build will not find it.
- **The ARM9 stack lives in DTCM (~16 KB).** No large stack buffers in code that runs on
  the console; make them static.
- **dswifi quirks:** byte-order helpers are in `<arpa/inet.h>` (not `<netinet/in.h>`),
  non-blocking I/O is `ioctl(fd, FIONBIO, &one)`, and `SHUT_RDWR` is 3, not 2. Avoid
  file-scope names that collide with newlib (`on_exit` is a real function).
- **Release the radio before chainloading.** Both hand-off paths call `Wifi_DisconnectAP()`
  + `wlmgrStop()`; a build that inherits a live association cannot re-init dswifi.
- **`dsidev.nds` contains the deploy secret.** Never commit or share it. `*.nds` is
  git-ignored already.

## Testing what you write

Five test suites, in ascending order of cost and descending order of certainty:

- `make test-movement` — the CS:GO movement core, compiled for the Mac and checked against
  real reference numbers. No emulator, no console.
- `make test-anim` — the character animation core and the rig file the game embeds
  (`data/player_rig.bin`, rebuilt by `tools/assets/player_rig.py`), how online players
  move between position snapshots, and the tombstones the dead leave.
- `make test-dsi` — 33 host tests driving the *real* deployment service over loopback with
  the *real* client. `tools/dsi/tests/host_service.c` mirrors the loader and runtime control
  flow; if you change how either behaves, change it there too or the tests stop meaning
  anything. Put portable logic in `tools/dsi/common/` so it lands in this suite.
- `make test-server` — the multiplayer protocol.
- `make test-ui` — pixels, in the emulator. Slow and the only suite that can fail for
  environmental reasons.

Hardware-only paths — Wi-Fi, the chainload, the return stub — have no test. Change them
carefully and get the human to confirm with `make run-dsi-sample`, which exercises all
four without involving the game.

**Before you call anything done:** the build compiles, `make test-dsi` passes, and
`make test-ui` passes if you touched anything that draws. None of that proves it works on
the console; only the human does.

## A worked example

The "no sound" bug went: user reports it → read `sounds.c` and find the game loads
`soundbank.bin` from the card → notice it checks three paths but opens the *relative* one,
which resolves to `sd:/dsidev/` → fix it to open whichever path exists → `make run-dsi`
(which now also ships the soundbank) → ask the user to confirm. No guessing, one round
trip on the console, and the fix was in the game rather than a workaround in the tooling.

See `tools/dsi/README.md` for the deployment system's architecture, and `tools/emu/` for
the UI harness.
