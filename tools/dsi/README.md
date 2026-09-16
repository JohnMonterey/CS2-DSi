# Wireless DSi development workflow

`make run-dsi` builds the game, sends it to a physical Nintendo DSi over Wi-Fi, and
launches it — no SD card removal, no menu navigation, no cable.

```
make setup-dsi        # once: check the toolchain, build the loader, stage the SD payload
# copy tools/dsi/.build/sd/* to the root of the DSi SD card, once
make run-dsi          # every iteration: build -> upload -> launch
make run-dsi LOGS=1   # same, and stay attached to the DSi's log output
```

The only manual step per session is opening `dsidev.nds` on the DSi once. After that the
console stays on the loader screen (or in the game) and the Mac drives everything.

## How it works

```
  Mac (tools/dsi/deploy.py)                  DSi
  ─────────────────────────                  ───────────────────────────────────
  discover        ──UDP 17491 broadcast──▶   loader or game answers with its state
  upload + CRC32  ──TCP 17491───────────▶    written to sd:/dsidev/appN.nds, fsync'd,
                                             verified by re-reading, then committed
  "G" (launch)    ──TCP 17491───────────▶    loader chainloads the new build
  logs            ◀─UDP 17493───────────     dsidev_log() from the running game
  reset           ──UDP 17491───────────▶    game exits back to the loader
```

Three pieces:

- **`loader/`** — `dsidev.nds`, a small homebrew app that lives at the SD root. It joins
  Wi-Fi, serves the deployment protocol, writes incoming builds to the card, and
  chainloads them. Built from our code plus devkitPro's `nds-hb-menu` chainloader and
  `nds-bootloader`, pinned to exact commits by `fetch.py`.
- **`runtime/dsidev.[ch]`** — ~70 lines linked into development builds of the game
  (`DSIDEV=1`). It serves the same protocol from inside the game so the Mac can see that
  the build actually started, stream logs, hot-reload assets, and ask the game to return
  to the loader. It compiles to nothing in release builds.
- **`deploy.py`** — the Mac client. Python standard library only, no dependencies, runs
  natively on Apple Silicon.

`common/` is shared by the loader and the game runtime: the wire protocol, the CRC32 and
NDS-header checks, and the crash-safe A/B store on the SD card.

## One-time setup

**1. devkitPro on Apple Silicon.** Install the macOS pacman installer from
<https://github.com/devkitPro/pacman/releases>, then:

```
sudo dkp-pacman -Syu
sudo dkp-pacman -S nds-dev          # devkitARM, libnds, libfat, dswifi, calico, tools
make doctor-dsi                     # verifies every path this workflow needs
```

The game also needs Fewnity's Nitro Engine fork. `/opt/devkitpro` is root-owned, so the
simplest place for it is beside this repository — the build finds it there automatically:

```
git clone https://github.com/Fewnity/nitro-engine.git ../nitro-engine
make -C ../nitro-engine
```

`$DEVKITPRO/nitro-engine` is used instead when it exists, and `NITRO_ENGINE=/path`
overrides both. The loader and the sample do not need it at all. If devkitPro lives
somewhere else, pass `DEVKITPRO=/path`.

**2. DSi Wi-Fi.** Configure the connection in the DSi's own *System Settings → Internet*
(not the DS-mode game settings). Homebrew running in DSi mode uses those slots through
dswifi, which supports open, WEP, and WPA/WPA2-PSK (TKIP or AES) there. The Mac and the
DSi must be on the same subnet, and the access point must not have client isolation
("AP isolation"/"guest mode") enabled — that blocks the discovery broadcast and the
upload alike.

**3. SD payload.**

```
make setup-dsi
```

writes exactly one file, `tools/dsi/.build/sd/dsidev.nds`. Copy it to the **root** of the
SD card — the name and location are fixed, because the return-to-loader stub boots
`sd:/dsidev.nds` by name. Everything else the console needs (the `dsidev/` directory that
holds deployed builds) the loader creates by itself.

The 16-byte secret that authorises deployments is generated on the Mac as
`tools/dsi/token.bin` (git-ignored, mode 600), compiled into `dsidev.nds`, and handed to
each launched build on its command line. So the card carries no secret of its own, and
**`dsidev.nds` is not a file to share** — anyone holding it can deploy to your console
while it is on their network. Re-run `make stage-dsi` and copy the file across again
whenever the loader changes; day-to-day game deployment never touches the card.

After that first copy the card never needs touching again: `make update-loader-dsi`
replaces `sd:/dsidev.nds` over Wi-Fi (verified as a real NDS image before the working one
is dropped), and data files go over the network too — see below.

### No card reader?

You do not need one. Serve the file from the Mac and pull it down on a phone that can
write to the card:

```
python3 -m http.server 8000 --directory tools/dsi/.build/sd
```

Open `http://<your-mac-ip>:8000/dsidev.nds` on the phone (`ipconfig getifaddr en0` prints
the address), download it, then move it to the SD card root with any file manager. It is a
single file at the top level, so there is no directory structure to recreate. Stop the
server with Ctrl-C when you are done.

**4. Launch the loader.** Open `dsidev.nds` from TWiLight Menu++ (or Unlaunch, or hbmenu).
It must run in DSi mode with SD access; the loader says so on screen if it does not. The
loader prints its IP address and waits.

## The iteration loop

```
make run-dsi
```

1. Builds `Counter-Strike-nds-dev.nds` with `DSIDEV=1` (separate `build-dsidev/` tree, so
   release builds are untouched).
2. Finds the DSi by UDP broadcast — or `make run-dsi DSI_IP=192.168.1.42` to skip it.
3. If the previous build is still running, asks it to return to the loader.
4. Uploads, unless the console already holds the identical image — an unchanged build is
   detected by size+CRC32 and skipped entirely.
5. Launches, then waits until the build reports "ready" from inside the game.

`deploy.py` prints the transfer time, so you can see what your network actually does;
DS-era Wi-Fi is the bottleneck, roughly 100–500 KB/s in practice.

Other commands:

| Command | Effect |
| --- | --- |
| `make run-dsi LOGS=1` | stream `dsidev_log()` output until Ctrl-C (the game keeps running) |
| `make logs-dsi` | attach a log listener from another terminal at any time |
| `make reset-dsi` | send a running build back to the loader |
| `make discover-dsi` | list DSis answering on the LAN, with their state and build CRC |
| `make asset-dsi FILE=x` | push one file into the *running* build (see below) |
| `make put-dsi FILE=x` | send a data file to sit beside the build on the card |
| `make update-loader-dsi` | replace the loader on the card over Wi-Fi |
| `make run-dsi-sample` | launch/return/log smoke test that does not need Nitro Engine |
| `make test-dsi` | run the host tests for the deployment protocol |

## Remote logging

In a development build, `dsidev_log("...")` sends a UDP line to the Mac that deployed the
build. The game's existing `debug.c` already routes its messages there, so no extra work
is needed to see them:

```
[192.168.1.42] DSILOG1 1f3c9ab2 7 Nitro Engine: ...
```

The prefix is the build CRC and a sequence number, so you can tell which build produced a
line and whether any were dropped (logs are UDP and best-effort by design — logging must
never stall a frame). Lines are sanitised before printing: no terminal escapes from the
network. macOS may ask to allow incoming connections the first time; allow it, or nothing
will arrive on UDP 17493.

## Runtime data files

The game reads `soundbank.bin` and `counter_strike_music.raw` from the card at runtime.
They live in `sd:/dsidev/` beside the deployed build — which is the working directory a
launched build gets — and `make run-dsi` pushes them on every deploy:

```
make put-dsi FILE=path/to/anything.bin    # send one file on its own
```

Each file is content-addressed the same way builds are, so an unchanged file costs one
round trip (well under a second) and is skipped. Change a `.wav`, and the next
`make run-dsi` rebuilds the soundbank, notices it differs, and ships it. Add further
runtime files to `DSI_DATA` in the root `Makefile`.

Names are validated on both sides: one plain file name, no directory separators, no
leading dot, nothing that could escape `sd:/dsidev/`.

## Asset hot reload

`make asset-dsi FILE=path` uploads a file to the running build and invokes the callback:

```c
static void reload(const char *path) { /* re-read `path` from the SD card */ }
dsidev_set_asset_callback(reload);   // before dsidev_init()
```

The file is written to `sd:/dsidev/assetN.bin` with the same crash-safe commit as a build,
and the callback runs from `dsidev_poll()` — on the main loop, never from an interrupt, so
it is safe to touch the engine from it. The sample (`sample/main.c`) demonstrates the
whole path. The game does not register a callback yet: its data is compiled in, so there
is nothing to re-read. Wire one up when a real asset moves to the SD card.

## Returning to the loader

Development builds return to `sd:/dsidev.nds` rather than to the system menu, which is
what makes the loop tight. Three ways in:

- `make reset-dsi`, or automatically at the start of the next `make run-dsi`;
- the standard L+R+Start+Select combo that calico already handles;
- the game exiting normally.

`developmentExit()` in `main.c` runs first (stops the music stream, saves), then the
bootstub reloads the loader, which is ready for the next deploy a few seconds later.

## Safety

The SD card is the only thing here that can be corrupted, so the store is deliberately
conservative:

- **A/B slots.** A new build is written to the slot that is *not* live. A power loss or a
  crash mid-upload cannot damage the build you already have.
- **Verify before publish.** The upload lands in `upload.part`, is flushed and `fsync`ed,
  then re-opened and re-read: CRC32 over the whole file, plus the NDS header checksum and
  a bounds check on every ARM9/ARM7/ARM9i/ARM7i section. Only then is it renamed into the
  slot and a `.rec` record (itself CRC-protected) written.
- **Recovery on boot.** The loader picks the newest generation whose record *and* file
  both verify, so a half-written slot is simply ignored.
- **Authentication.** Every upload and every reset carries the 16-byte token, which is
  compiled into the loader and passed to launched builds via argv. Traffic is plaintext on
  your LAN; the token prevents accidents and casual mischief, not a determined attacker on
  your network. A build launched any other way (straight from TWiLight, say) never gets the
  token and keeps the service switched off entirely.
- **The Mac never writes to the card.** `make stage-dsi` only prepares a folder for you to
  copy; nothing here mounts or writes an SD card behind your back.
- **One deployment at a time**, enforced by a lock file, so two terminals cannot interleave
  uploads to the same console.

## Design decisions

**Why a custom loader instead of TWiLight Menu++?** TWiLight is a launcher, not a
deployment target: it cannot receive a build over Wi-Fi, and driving it means navigating
menus by hand on every iteration. It is still how you start `dsidev.nds` the first time —
which is all it is needed for.

**Why not nds-bootstrap?** nds-bootstrap exists to run *retail DS ROMs* from SD by
patching card I/O. Our builds are homebrew and can be chainloaded directly, so it adds a
large dependency, a slower boot, and nothing this workflow needs.

**Why chainload with hbmenu's loader?** It is the same mechanism hbmenu and every other
DSi homebrew launcher uses, it is maintained by devkitPro, and it already handles TWL
mode, the DSi SD, DLDI patching, and argv. `fetch.py` pins `nds-hb-menu` and
`nds-bootloader` to exact commits (the bootloader commit is the one hbmenu itself pins),
fetches them on demand, and applies one small, asserted patch: the return-to-loader
bootstub is pointed at `sd:/dsidev.nds` instead of the default `BOOT.NDS`, and a failed
bootstub install is reported instead of ignored. Nothing is vendored into the repository.

**Why a service inside the game too?** Without it the Mac cannot tell a launched build
from a black screen, there is no log channel, and returning to the loader needs human
hands. It costs ~70 lines, one `dsidev_poll()` per frame, and nothing at all in release
builds.

**Why UDP broadcast for discovery?** It removes the last piece of manual configuration —
no IP to look up after DHCP hands the console a new lease. `DSI_IP=` is there for networks
that block broadcast.

## Verification status

Everything builds with devkitARM r68 (GCC 16.1), libnds 2.0.2, calico 1.2.0 and
dswifi 2.1.0: the loader (297 KB), the sample, and the game in both release and
`DSIDEV=1` form (the development runtime costs ~12 KB).

Run `make test-dsi`: the device-side sources (`common/`) are compiled for the host and
driven by `tests/host_service.c`, which mirrors the loader and runtime control flow, and
tested against the real `deploy.py` over loopback. 25 tests cover the full deploy →
launch → return cycle, deduplication, A/B slot alternation, recovery after a `SIGKILL`
mid-upload, truncated-image rejection, wrong token, CRC mismatch, malformed NDS, oversized
uploads, asset hot reload, log delivery and sanitisation, and the CLI paths behind
`make run-dsi`. The same compile also runs with `-Werror`.

What neither the tests nor the compiler can cover, and what to check on the console the
first time: Wi-Fi association, libfat access to the DSi SD card, the chainload itself, and
the return-to-loader bootstub. `make run-dsi-sample` exercises exactly those four on
hardware without involving the game or Nitro Engine — run it first.

### Toolchain notes

devkitARM r68 ships GCC 16, which defaults to C23 and (since GCC 14) treats several type
mismatches as errors. The game predates both, so `Counter-Strike-nds/Makefile` pins
`-std=gnu17` and keeps `incompatible-pointer-types`/`int-conversion` as warnings in the
`COMPAT` variable. Dropping that variable and fixing the ~40 callback signatures it covers
is a worthwhile cleanup, but it is unrelated to deployment. Four small source fixes were
needed for libnds 2.x regardless: the rumble API renames (`isRumbleInserted`/`setRumble`
to `rumbleIsInserted`/`rumbleSet`), a missing `closeMusicSteam` declaration, and explicit
`<sys/ioctl.h>` and `<unistd.h>` includes that dswifi and libfat used to pull in.

## Files

```
tools/dsi/
  deploy.py          Mac client: discover, upload, launch, reset, assets, logs, staging
  fetch.py           pins/fetches the devkitPro chainloader sources, applies the patch
  doctor.py          reports on the local devkitPro installation
  Makefile           builds the loader (MODE=loader) and the sample (MODE=sample)
  common/            protocol, NDS validation, crash-safe A/B store, TCP/UDP service
  loader/main.c      sd:/dsidev.nds
  runtime/dsidev.[ch] in-game service; no-ops unless built with DSIDEV_ENABLED
  sample/main.c      hardware smoke test
  tests/             host tests for everything above
```

Protocol summary: TCP+UDP 17491 on the DSi, UDP 17493 on the Mac. A 40-byte request
header (`DSIUPL1`, operation, size, CRC32, token) then the payload; 16-byte replies
(`DSIACK1`, status, detail). Status ≥ 100 is an error and `deploy.py` prints what to do
about it. `DSIDEV1?` is the discovery query, `DSIRESET` + token the return request. The
loader starts a build with `--dsidev-host=`, `--dsidev-crc=` and `--dsidev-token=`.
