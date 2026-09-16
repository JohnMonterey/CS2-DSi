# Counter-Strike DS multiplayer server

Fewnity's original server (`cs.fewnity.com`) is closed and its source was never
published. This one is written from the client's own protocol handling in
`Counter-Strike-nds/source/network/network.c`.

```bash
make serve                      # Dust2, deathmatch, port 6003
make serve SERVE_MAP=6          # Mirage
make serve SERVE_FLAGS=-v       # log every unhandled packet
make test-server                # 19 protocol tests, no console needed
```

On startup it prints the address players should type into the game.

## Joining from a DS

1. The console and this machine must be on the **same Wi-Fi network**. A DS only
   speaks open or WEP Wi-Fi, so this is usually a phone hotspot or a spare
   router — see the root `README.md` for per-console setup.
2. In the game: **Multiplayer → enter the IP the server printed → join**.

## What it implements

Deathmatch: join, teams, movement relay, shooting, hit scoring with the real
weapon damage table, kills, scoreboard and respawns.

Not implemented: the bomb, competitive round flow, buy-menu economy (weapons are
free), grenades beyond a basic relay, and private party codes. Those are all
server-side decisions the client will happily follow, so they are additions
rather than rewrites.

## Protocol notes

These were read out of the client and every one of them is load-bearing.

**The protocol is asymmetric.** The client *sends* a numeric request code and
*receives* a string tag. `{10;x;y;z;angle;cam}` goes up, `{POS;id;x;y;z;angle;cam}`
comes down. Sending a numeric tag back to the client is silently ignored.

**The parser has hard limits.** `treatData()` splits into `char arr[10][64]` and
copies through `char currentPacket[256]`, so a message may carry at most **ten
fields**, each under **64 characters**, with a body under **256 bytes**. Break
any of these and the client drops the message without a word. `message()`
raises instead, and `test_server.py` covers all three.

**The client reads 63 bytes per frame.** `network.c:1002` recvs into a 64-byte
buffer once per game loop and appends to a 1024-byte accumulator, breaking the
loop — disconnecting — if that accumulator ever fills. Its entire intake is
about 3.7 KB/s at 60 fps. Relaying several players' positions at frame rate
exceeds that, so positions are throttled to `POSITION_HZ` (10 Hz). This is the
single most important constraint in the file: exceed it and clients drop with no
diagnostic.

**The anti-cheat handshake cannot be validated.** `getKeyResponse()` in
`network/security.c` is stubbed in the public source and returns `-1`
unconditionally — the real generator was never released. The server still sends
a challenge, because the client blocks until it gets one, but any answer is
accepted. The game version *is* checked.

**Names are unescaped on the wire.** A player called `a;b` would inject a field
separator, so `sanitise()` strips `{`, `}` and `;`.

## Keeping the data honest

`gamedata.py` is generated, not written:

```bash
make gamedata
```

It extracts weapon damage, falloff, pellet counts and spawn coordinates straight
out of `gun_data.c` and `map.c`, so the server scores hits with the same numbers
the client renders. Re-run it after rebalancing a weapon. Note it evaluates C
expressions — the shotguns' damage is written `171 / 6`, which a naive parser
reads as zero.

## Exposing it beyond the LAN

`make serve` binds all interfaces but your router will not forward port 6003
from the internet unless you tell it to. Before doing that, note that this
server does no authentication worth the name (see the handshake note above), so
anyone who finds the port can join, and the parser is exposed to arbitrary
input. Keep it to a LAN or a trusted network.
