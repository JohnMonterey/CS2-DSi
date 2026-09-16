# CS:GO movement physics for CS:DS — design

**Date:** 2026-09-14
**Status:** implemented; builds and passes host tests. Unverified on hardware.
**Target:** real DSi hardware, verified by the human operator

## Goal

Replace CS:DS's instant-on/instant-off player movement with the Source engine movement
model as CS:GO ships it, so the game *feels* like CS:GO: momentum, counter-strafing, air
control on jumps, stamina-limited jumping, and crouch.

The repo already sits on a CS-calibrated scale, so this is a change of *model*, not of
numbers. Top speeds stay where Fewnity put them.

### Non-goals

- Bots keep waypoint movement. They are not physics-driven and will not be.
- No step-up onto ledges. The existing ramp/stairs trigger system stays as it is.
- No ladders, no surf ramps — the maps have no slope geometry to use them.
- Crouch is local-only in v1. Remote players will not show a duck (see *Deferred*).

## What is wrong today

`main.c:1178` zeroes `xspeed` and `zspeed` **every frame**, then `MovePlayer()` re-adds a
constant derived from the current weapon. There is no acceleration, no friction, no
momentum, no air control. Three consequences worth naming:

1. **Movement is binary.** Velocity is either zero or full. Nothing in CS feels like that.
2. **Diagonals are ~41% too fast.** `MovePlayer()` adds the forward and strafe vectors
   without clamping the result. Source clamps `wishvel` to `maxspeed`
   (`gamemovement.cpp:1950`).
3. **Jumping is floaty and too high.** Gravity `0.0065` ≈ 914 u/s² against CS:GO's 800;
   `JumpForce 620` ≈ 363 u/s against CS:GO's 301.99. That is a 72-unit jump where CS:GO
   gives 54.6.

Ground state is currently inferred from `yspeed == 0` plus a two-frame debounce
(`CanJumpRealTimer`). That is good enough for a game with no physics model and not good
enough for one with friction, acceleration and stamina all branching on it.

## Sources and licensing

Two reference sources, treated differently on purpose:

- **`ValveSoftware/source-sdk-2013`** — public and licensed. `game/shared/gamemovement.cpp`
  contains the real `Friction()`, `Accelerate()`, `AirAccelerate()`, `WalkMove()` and
  `CheckJumpButton()`. This is the Quake → GoldSrc → Source lineage and it is the
  structural reference for our implementation.
- **`cs_gamemovement.cpp` from the leaked `cstrike15_src`** — CS:GO's stamina system and
  `PreventBunnyJumping()` exist nowhere in the public SDK. It was read to confirm the
  algorithm shape and constant values. **No code from it is copied into this repo.** The
  implementation is written from the public SDK's structure; the constants are public
  convar defaults (`sv_friction`, `sv_staminamax`, …) that any CS:GO client prints on
  request.

This distinction matters because CS:DS is MIT-licensed.

## Unit system

The scale is already right. The player hull is 1.8 game units tall against Source's 72, so:

> **1 game unit = 40 CS units.**

Velocity lives in NE's f32 (1.0 unit = 4096) per tick; a tick is one `GameLoop` iteration,
nominally 1/60 s.

| conversion | factor |
| --- | --- |
| CS u/s → f32/tick | × 1.706667 |
| CS u/s² → f32/tick² | × 0.0284444 |
| f32/tick → CS u/s | × 0.5859375 |

That the existing weapon table lands on real CS numbers under this conversion is the
evidence the scale is right:

| weapon class | `WalkSpeed` × 2 | CS u/s | CS:GO reference |
| --- | --- | --- | --- |
| default / knife | 440 | 258 | 250 (knife) |
| `LIGHT` | 420 | 246 | ~240 (pistols) |
| `MEDIUM` | 390 | 229 | ~230 (SMGs) |
| `MEDIUM_HIGH` | 360 | 211 | ~215 (AK/M4) |
| `HEAVY` | 350 | 205 | 200 (AWP) |

**The weapon table is not touched.** `WalkSpeed * 2` becomes `maxspeed` directly.

### Constants

| CS:GO convar / constant | value | f32/tick |
| --- | --- | --- |
| `sv_gravity` | 800 u/s² | 22.7556 |
| `sv_jump_impulse` | 301.993377 u/s | 515.40 |
| `sv_stopspeed` | 80 u/s | 136.53 |
| air speed cap | 30 u/s | 51.20 |
| `sv_friction` | 5.2 | rate × dt = 0.086667/tick |
| `sv_accelerate` | 5.5 | rate × dt = 0.091667/tick |
| `sv_airaccelerate` | 12 | rate × dt = 0.200000/tick |
| `BUNNYJUMP_MAX_SPEED_FACTOR` | 1.1 | — |
| `CS_PLAYER_SPEED_DUCK_MODIFIER` | 0.34 | — |
| `CS_PLAYER_DUCK_SPEED_IDEAL` | 8.0 | — |
| crouch spam penalty | 2.0 per duck | — |
| `sv_staminamax` | 80 | — |
| `STAMINA_RANGE` | 100 | — |
| `sv_staminarecoveryrate` | 60 /s | 1.0/tick |
| `sv_staminajumpcost` | 0.080 × impulse | 24.16 per jump |
| `sv_staminalandcost` | 0.050 × landing speed | — |

Sanity check on the jump: the continuous formula gives `515.40² / (2 × 22.7556) = 57.0 CS
units`, but **the engine does not produce that**, and neither does CS:GO -- see *Jump
height* below. The measured value is 54.5.

## Architecture

The decision is to keep NE_Physics as the integrator and collider, and replace only the
velocity computation. Every wall in this game is an axis-aligned box, and NE's per-axis
resolution already zeroes the blocked axis — which is what `ClipVelocity` does for
axis-aligned surfaces. The fidelity loss is near zero and the blast radius is small:
raycasting, networking, shadows, occlusion and the stairs system all keep reading the same
data they read now.

### Modules

```
source/player/playermove_core.c   pure f32 math, no NE / no console deps  → host-testable
source/player/playermove_core.h   MoveState, MoveInput, MoveTunables
source/player/playermove.c        NE_Physics glue, crouch hull, ground detection
source/player/playermove.h
source/player/movement_cfg.c      movement.cfg parser (shared host/console)
```

`playermove_core.c` is the important boundary. It takes a `MoveState` (velocity, stamina,
duck amount, ground flag) plus a `MoveInput` (wish direction, buttons) plus `MoveTunables`,
and returns a mutated `MoveState`. It never touches an `NE_Physics`, a `Player`, or the
filesystem. That is what makes it testable on the Mac, which is the only verification
available to the agent.

`playermove.c` owns everything that needs hardware: reading the d-pad, resizing the NE
bounding box for crouch, writing velocity into `PlayerPhysic`, and deriving ground contact
from the post-update model position.

## The tick

Order mirrors Source's `FullWalkMove`. Called once per `GameLoop`, replacing the block at
`main.c:1178-1189`.

```
1.  ReduceTimers
      stamina -= 1.0            (sv_staminarecoveryrate 60/s at 60 Hz)
      clamp stamina >= 0
      duckSpeed = Approach(8.0, duckSpeed, dt * 3.0)

2.  CategorizePosition
      onGround  <- resolved by the previous tick's collision (see Ground detection)

2b. Duck
      advance duckAmount toward the button state (see Crouch)
      resize the NE hull; in air, raise the centre by the half-extent delta

3.  StartGravity                                       // BEFORE the jump check
      vy -= gravity * dt * 0.5

4.  CheckParameters
      maxspeed = weapon WalkSpeed * 2
      staminaFactor = clamp(1 - stamina/100, 0, 1)
      maxspeed *= staminaFactor * staminaFactor      // squared — CS:GO does this
      maxspeed *= duckModifier(duckAmount)           // lerp 1.0 -> 0.34

5.  CheckJumpButton                                   (edge-triggered; no pogo on hold)
      if onGround and jump pressed:
          PreventBunnyJumping:  clamp |horizontal| to 1.1 * rawMaxSpeed
          vy += 515.40 * staminaFactor               // ADDS; sets only when ducked
          vy -= gravity * dt * 0.5                   // Source's in-jump FinishGravity
          stamina = clamp(stamina + 24.16, 0, 80)
          onGround = false

6.  Move
      wishdir, wishspeed from d-pad, clamped to maxspeed
      if onGround:  Friction()  then  Accelerate(wishdir, wishspeed, 5.5)
      else:                           AirAccelerate(wishdir, wishspeed, 12)

7.  Write vx, vy, vz into PlayerPhysic; NE_PhysicsUpdate integrates and resolves

7b. FinishGravity
      vy -= gravity * dt * 0.5     (skipped if the y axis resolved against a surface)

8.  Post-update
      derive onGround; if we just landed:
          stamina = clamp(stamina + 0.050 * landingSpeedCSu, 0, 80)
          play the existing land sound
```

### Friction

```
speed = |velocity|
if speed < 0.1: return
control = max(speed, stopspeed)
drop    = control * friction * dt
newspeed = max(speed - drop, 0)
velocity *= newspeed / speed
```

Applied only on the ground. This is what makes counter-strafing a skill: releasing the
d-pad bleeds speed over roughly six ticks rather than stopping dead.

### Accelerate / AirAccelerate

```
Accelerate(wishdir, wishspeed, accel):
    currentspeed = dot(velocity, wishdir)
    addspeed     = wishspeed - currentspeed
    if addspeed <= 0: return
    accelspeed   = min(accel * dt * wishspeed, addspeed)
    velocity    += accelspeed * wishdir

AirAccelerate(wishdir, wishspeed, accel):
    wishspd      = min(wishspeed, 30 CS u/s)      <-- cap applies HERE only
    currentspeed = dot(velocity, wishdir)
    addspeed     = wishspd - currentspeed
    if addspeed <= 0: return
    accelspeed   = min(accel * dt * wishspeed, addspeed)   <-- uncapped wishspeed
    velocity    += accelspeed * wishdir
```

The asymmetry is deliberate and is not a typo: the cap applies to `addspeed` but the
uncapped `wishspeed` scales `accelspeed`. That single inconsistency, inherited from Quake,
is the whole of air-strafing. Reproducing it is the difference between jumps that feel like
CS and jumps that feel like a platformer.

### Stamina

`stamina` is a scalar in CS:GO's own scale (0…80, divided by a range of 100), so the
constants stay readable against the convars.

- recovers at 60/s, i.e. 1.0 per tick, whether grounded or airborne
- a jump costs 24.16; landing costs `0.050 × landing speed in CS u/s`
- jump velocity scales by `(1 - stamina/100)` — linear
- max speed scales by `(1 - stamina/100)²` — squared

A normal jump-and-land therefore spends ~39 stamina, clearing in about 0.65 s. Immediately
after landing, max speed is roughly 37% of normal. That is the "you are slow right after
you land" property that makes CS:GO bunnyhopping pointless, and it is the single most
important thing to get right if the goal is CS:GO feel rather than CS:S feel.

`PreventBunnyJumping()` is belt and braces on top: horizontal speed is clamped to
`1.1 × maxspeed` at the moment of jumping.

## Ground detection

Friction, acceleration, jumping and stamina all branch on `onGround`, so it stops being a
heuristic.

NE gravity is set to **0** and gravity is applied in our own code. This buys two things:
Source-accurate half-before/half-after integration at full precision, and an unambiguous
ground signal — NE's collision resolver zeroes `yspeed` on any y-axis contact, so:

```
wasFalling = (vy_before_update < 0)
blocked    = (yspeed_after_update == 0) and (model->y did not move the full expected delta)
onGround   = wasFalling and blocked
headBump   = (vy_before_update > 0) and blocked
```

`CheckStairs()` in `collisions.c` keeps its ramp handling but sets `onGround` directly
instead of priming `canJump = 10`.

`CanJump`, `CanJumpRealTimer`, `frameCountDuringAir` and `NeedJump` all disappear into the
core.

## Crouch

- New input `CROUCH_BUTTON = 14`; `INPUT_COUNT` 14 → 15, `INPUT_NAMES_COUNT` unchanged.
  **Unbound by default** and exposed in the existing remap menu — every physical DS button
  already has a default binding, so the user chooses.
- `duckAmount` ∈ [0,1], driven the way CS:GO drives it:
  - duck: `Approach(1.0, duckAmount, dt * duckSpeed * 0.8)` — ~0.16 s at full duck speed
  - unduck: `Approach(0.0, duckAmount, dt * max(1.5, duckSpeed))` — ~0.13 s
  - `duckSpeed` recovers toward 8.0 at `dt * 3.0`, and each duck subtracts 2.0 — so crouch
    spam gets progressively slower, as in CS:GO
- unduck is blocked when there is no headroom
- speed: `maxspeed *= lerp(1.0, 0.34, duckAmount)`
- hull: half-height 0.9 → 0.675 (72 → 54 CS units). NE resolves against the box **centre**,
  so shrinking the box on the ground plants the feet automatically and the centre drops.
- **in the air**, raise the model centre by the full Δ when ducking so the head stays put
  and the feet rise by Δ. (Δ is the change in the *half* extent, 0.225 -- `ySize` in the
  Player struct is a half extent, and `NE_PhysicsSetSize` is handed `ySize * 2`.) This is what makes crouch-jump clear a higher ledge, and it is the
  one place where NE's centre-based box needs a manual correction.
### Eye height is not just the camera

`CameraOffsetY` is not a camera constant. It is the **eye height**, and it is read in eight
places across `raycast.c`, `ai.c`, `grenade.c` and `network.c` — it sets the origin of every
bullet raycast, the AI's line-of-sight origin, and grenade throw positions. Crouching has to
lower the local player's shot origin too, or you would crouch behind cover and still shoot
over it.

Every one of those call sites is **actor-relative**, not local-player-relative:
`raycast.c:66`, `:197` and `:232` use the *shooter's* eye — and the shooter may be the local
player or a bot. `ai.c:586` uses the looking bot's eye. `grenade.c:332` and `:418` use the
thrower's.

So this is not a local-vs-remote split. It is one accessor:

```c
float PlayerEyeOffset(const Player *p);   // 0.7 standing, lerped to 0.475 fully ducked
```

Every site above takes the player it already has in hand and asks for that player's eye
height. Because only the local player can duck in v1, every remote player returns the
standing 0.7 and behaviour is unchanged — but the call sites are already correct, so
enabling remote crouch later is a matter of populating `duckAmount` from the network and
nothing else. No migration surface left behind.

`CameraOffsetY` / `CameraOffsetYMultiplied` stay as the standing constants the accessor
returns, so the hot integer raycast paths keep their `CameraOffsetYMultiplied` form
(`f32`-premultiplied) via a matching `PlayerEyeOffsetMultiplied()`.

**Save compatibility, checked:** `saveManager.c:54` writes `inputs <INPUT_COUNT>` and the
loader reads the count back, defaulting to 14 (`saveManager.c:97`). An existing save
declaring 14 inputs will load 14 bindings and leave the new crouch slot at its default of
unbound. No migration needed.

## Tunables — `movement.cfg`

Physics constants need iteration on hardware, and the agent cannot feel the game. A rebuild
costs ~15 s of transfer; re-pushing one small file costs well under a second.

- plain `key=value`, one per line, `#` comments
- **values in CS:GO units**, so the file reads like a CS config:
  ```
  friction        = 5.2
  accelerate      = 5.5
  airaccelerate   = 12
  stopspeed       = 80
  gravity         = 800
  jump_impulse    = 301.993
  air_speed_cap   = 30
  stamina_max     = 80
  stamina_jumpcost= 0.080
  stamina_landcost= 0.050
  stamina_recovery= 60
  duck_modifier   = 0.34
  bhop_factor     = 1.1
  ```
- conversion to f32/tick happens once, at load
- defaults are compiled in; a missing or malformed file is not an error
- shipped via `DSI_DATA` in the root Makefile, so `make run-dsi` carries it
- `make asset-dsi FILE=movement.cfg` re-applies it **in the running build** via the existing
  asset callback — retuning without a rebuild or a round trip through the loader

## Debug overlay

Toggled by a debug flag, drawn on the main screen:

```
spd 247  ground  stam  0.0  duck 0.00  wish 250
```

Speed in CS u/s so it can be compared directly against CS:GO numbers. In addition, a
`dsidev_log()` line on every landing (landing speed, stamina after, airtime) so the agent
can read behaviour off `make logs-dsi` instead of asking the human to read numbers off a
196-pixel-tall screen.

## Fixed point

The ARM9 has no FPU. The current code uses `sqrtf`/`sinf`/`cosf` freely; the movement core
will not.

- internal velocity in **Q8** (f32 << 8, so 1 unit = 1,048,576) so friction, acceleration
  and gravity accumulate without systematic truncation drift
- rates as Q12 multipliers; products via `mulf32` / int64 where the intermediate needs it
- speed magnitude: shift to the f32 domain first (max ~550, square ~302,500, sum ~605,000 —
  comfortably inside int32), then `sqrt32`, then shift back. Precision ±1 f32 ≈ ±0.59 CS u/s,
  which is far below what friction cares about.
- write-back to NE rounds to nearest: `xspeed = (vx_q8 + 128) >> 8`. Velocity itself stays
  precise, so the quantisation is zero-mean rather than cumulative.
- gravity needs no separate accumulator: Q8 velocity already represents 22.7556/tick to
  1/256 of an f32, so the 3% error from NE's integer gravity never arises

Stack discipline: no large stack buffers — the ARM9 stack lives in DTCM (~16 KB). The
`movement.cfg` read buffer is `static`.

## Changes by file

| file | change |
| --- | --- |
| `main.c:1178-1189` | velocity reset + `MovePlayer()` → `PlayerMove_Tick()` |
| `main.c:832-867` | jump / `CanJumpRealTimer` / `frameCountDuringAir` block removed |
| `main.c:1403` | `isLocalPlayerMoving()` becomes "has horizontal speed" |
| `main.h` | `JumpForce` removed; `CROUCH_BUTTON`; `INPUT_COUNT` 14 → 15 |
| `raycast.c`, `grenade.c`, `ai.c` | eye height via `PlayerEyeOffset(player)` (6 call sites) |
| `movements.c` | `MovePlayer()` deleted; look-vectors and remote lerp untouched |
| `player.c:676` | `NE_PhysicsSetGravity(..., 0)`; hull size from duck state |
| `collisions.c` | `CheckStairs()` sets `onGround` instead of `canJump = 10` |
| `network.c:251` | reset stamina / duck / ground on teleport |
| `inputs/input.c` | `CROUCH_BUTTON` entry, unbound by default |
| `graphics/ui.c` | crouch row in the remap menu; debug overlay |
| root `Makefile` | `movement.cfg` added to `DSI_DATA` |

## Testing

`playermove_core.c` has no NE or console dependencies, so it compiles for the host. New
`make test-movement` asserts against real CS:GO numbers:

- jump apex = 57 ± 0.5 CS units from a standing jump
- stopping distance from 250 u/s under friction 5.2 / stopspeed 80
- time to reach 99% of max speed from rest on the ground
- per-tick air-strafe velocity gain for a given view-turn rate
- stamina after one jump-and-land, and ticks to full recovery
- diagonal input produces exactly `maxspeed`, not `maxspeed × √2`
- duck and unduck transition times; crouch-spam penalty accumulation

`make test-dsi` must still pass and the build must compile. Neither proves anything about
how it feels — only the human on the console does.

## Risks

- **NE's `iscolliding` does not report which axis resolved.** Ground detection infers it
  from the y-delta. If that proves unreliable on real geometry, the fallback is to patch
  `NE_PhysicsUpdate` in the local Nitro Engine checkout to expose a per-axis collision mask.
- **Frame rate is not guaranteed 60 Hz.** `NE_WaitForVBL(NE_CAN_SKIP_VBL)` skips the wait
  above 100% CPU, and the game has a dual-screen mode that halves the effective rate.
  Physics is frame-tied today and stays frame-tied — but if the human reports speed changes
  when the bottom screen updates, the fix is a tick accumulator running a whole number of
  fixed sub-ticks per frame. Deliberately not built up front.
- **Stamina tuning is the most likely thing to need iteration.** It is also the cheapest to
  change, via `movement.cfg`.

## Deferred

- **Crouch over the network.** Remote players will not show a duck, and their hitbox on
  other machines stays full height. Because eye height already goes through
  `PlayerEyeOffset(player)` and the hull already reads `duckAmount`, enabling it is a bit in
  the position packet feeding `duckAmount` on the remote-player path — no other changes.
  Explicitly out of v1.
- Bots remain waypoint-driven.
- Step-up onto ledges; ramp triggers stay as they are.
- Ladders, surf.


---

# Implementation notes

Written after the code landed. Where this section and the design above disagree, this
section is what the code does and why.

## Jump height is 54.6 units, not 57

`sv_jump_impulse 301.993377` is `sqrt(2 × 800 × 57)`, so the design asserted a 57-unit apex.
The engine does not produce that, and neither does CS:GO.

`CheckJumpButton()` **adds** the impulse to a velocity that `StartGravity()` has already
decremented, and then calls `FinishGravity()` itself before returning
(`gamemovement.cpp:2461, 2495`). `FullWalkMove()` calls `FinishGravity()` again after the
move. So the jump tick pays a full frame of gravity before it displaces anything, and
displacement on tick *n* is `u − n·g·dt` rather than `u − (n − ½)·g·dt`.

Summing that at 64 Hz with CS:GO's own numbers gives **54.65 units**, which is exactly the
height CS:GO players measure against boxes. At this game's 60 Hz it comes out at 54.5. The
host test asserts 54.6 ± 1.0.

A first implementation that set velocity instead of adding, and applied gravity after the
jump instead of before, measured 59.55 -- 9% too high and unmistakably floatier.

## Crouch details the design got wrong

- The spam penalty is charged on **every press and every release**
  (`cs_gamemovement.cpp:184-188`), not on the press alone. A duck-and-stand therefore costs
  4.0 of duck speed, not 2.0.
- Below a duck speed of 1.5, `DuckingEnabled()` returns false and the duck button is
  **ignored entirely** (`cs_gamemovement.cpp:149-154`) -- the player stands up rather than
  ducking slowly. It is an anti-camp measure, not a slowdown. Without that floor, six rapid
  crouches drove the duck rate low enough to need 25 seconds for one crouch.
- Ducking therefore takes ~0.20 s, not the 0.156 s the nominal 8.0 duck speed implies: the
  first press has already charged the penalty by the time the transition starts.

## Ground detection, as built

NE gravity is 0 and bounce energy is 0, so `NE_PhysicsUpdate` sets `yspeed` to exactly 0 on
any Y contact and leaves it untouched otherwise. That makes the test unambiguous:

```
yBlocked = (vyBefore != 0) && (vyAfter == 0)
grounded = yBlocked && vyBefore < 0
headBump = yBlocked && vyBefore > 0
```

`vyBefore` is never 0 in practice because `StartGravity` runs first, including while
resting. The same before/after comparison on X and Z absorbs wall clipping, which for
axis-aligned geometry is what `ClipVelocity` would have produced.

## Eye height became one accessor, not a split

The design proposed keeping the standing constant for remote players and a variable for the
local one. That was wrong: all six call sites are **actor**-relative -- `raycast.c:66`,
`:197`, `:232` use the *shooter's* eye, and the shooter may be a bot. It is now
`PlayerEyeOffset(player)` / `PlayerEyeOffsetF32(player)` in `player.c`, which returns the
standing height for anyone who is not the local player. Enabling remote crouch later means
populating `duckAmount` from the network and nothing else.

## Config truncation

`MovementCfg_LoadPath` reports `truncated` when the file overruns its read buffer. This is
not hypothetical: the shipped `movement.cfg` is 2522 bytes and the first buffer was 2048,
so the last four keys were dropped **and the round-trip test still passed**, because those
four happened to equal the compiled defaults. The test now asserts both `!truncated` and
that all 16 keys are applied.

## Pre-existing bugs found along the way

- `raycast.c:94` dereferenced `PlayerPhysic` on non-local players, where it is always NULL.
  A bot aiming at a **remote human** (online, with bots) was a null dereference; it only
  survived because bots usually target player 0. The velocity check it was doing has been
  replaced by `PlayerMove_IsMoving()`, which does not read a per-player physics object.
- `isLocalPlayerMoving()` summed the **signed** velocity components, so `xspeed == -zspeed`
  read as standing still. It gates bomb planting and defusing.

## Emulation

Investigated and ruled out. No DS emulator is installed; Homebrew's `melonds` and `desmume`
casks are both disabled as of 2026-09-01 for failing the macOS Gatekeeper check on macOS 27;
and neither has a headless mode or an input-scripting API on macOS (DeSmuME's Lua is
Windows-only, melonDS has no scripting at all). melonDS's GDB stub is the only lever, and it
is not automatable into a movement test.

The ROM is DSi-*enhanced* rather than DSi-only, so it does boot in plain DS mode, and a
ready-made 64 MiB DLDI SD image already exists in the repo at
`Counter-Strike-nds/Counter Strike DS package sample Emulator/counter_strike_sd.raw`. If a
melonDS build is installed by hand, it will run -- but only for looking at, not for
automated verification.

## Still unverified

Everything about how it *feels*. The host tests prove the arithmetic matches CS:GO's
published constants and that the discrete integration lands on CS:GO's measured jump height.
They prove nothing about the d-pad, the framerate, the camera, or whether momentum on a
handheld is pleasant. That needs the console.
