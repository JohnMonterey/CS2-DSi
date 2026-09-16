#!/usr/bin/env python3
"""A multiplayer server for Counter-Strike DS.

The original server (cs.fewnity.com) is closed and was never published, so this
is written from the client's own protocol handling in
Counter-Strike-nds/source/network/network.c.

Run it:      make serve
Then on the DS:  Multiplayer -> enter this machine's LAN IP -> join.

PROTOCOL NOTES, all load-bearing:

  * Messages are `{field;field;...}` with no separator between messages. The
    client SENDS a numeric request code as the first field and RECEIVES a
    string tag. They are not symmetric: `{10;...}` in, `{POS;...}` out.

  * The client parses into `char arr[10][64]`, so a message may carry at most
    TEN fields and each field must be under 64 characters. It also copies into
    `char currentPacket[256]`, so the whole body must stay under 256 bytes.
    Exceeding any of these makes the client silently drop the message.

  * The client reads at most 63 bytes per frame (network.c:1002) and appends
    into a 1024-byte buffer, breaking the receive loop -- disconnecting -- if
    that buffer ever fills. At 60 fps its entire intake is about 3.7 KB/s.
    Position updates are therefore throttled rather than relayed at frame rate;
    see POSITION_HZ.

  * `getKeyResponse()` is stubbed in the public source and always returns -1, so
    the anti-cheat handshake cannot be meaningfully validated. We send a
    challenge because the client waits for one, and accept any answer.
"""

from __future__ import annotations

import argparse
import random
import selectors
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamedata import GUNS, SPAWNS  # noqa: E402

# --- protocol constants, mirrored from network.h -------------------------- #

PING = 0
STPEP = 1
WALLHIT = 2
BOMBPLACE = 3
BOMBPLACING = 4
BOMBDEFUSE = 5
CURGUN = 6
LEAVE = 7
VOTE = 8
GRENADE = 9
POS = 10
SETNAME = 11
SHOOT = 12
HIT = 13
PARTY = 14
BUY = 15
TEAM = 16
KEY = 17
GETBOMB = 40
FRAME = 41
RELOADED = 42
STATUS = 43

# enum ErrorType
ERR_WRONG_VERSION = 1
ERR_SERVER_FULL = 4

# enum RoundState
TRAINING = -1
WAIT_START = 0
PLAYING = 1

TERRORISTS = 0
COUNTERTERRORISTS = 1

MAX_PLAYERS = 10  # MaxPlayer in main.h
INVENTORY_SLOTS = 9  # inventoryCapacity
GAME_VERSION = "1.1.1"

DEFAULT_T_GUN = 1  # DEFAULTTERRORISTGUN, Glock-18
DEFAULT_CT_GUN = 2  # DEFAULTCOUNTERTERRORISTGUN, USB

# How often each player's position is relayed to the others. The client cannot
# absorb 60 Hz from several players at once; see the protocol notes above.
POSITION_HZ = 10.0

TICK_HZ = 30.0
RESPAWN_SECONDS = 4.0
MATCH_SECONDS = 10 * 60


def message(tag: str, *fields) -> bytes:
    """Build one wire message, enforcing the client's parser limits."""
    parts = [str(tag)] + [str(f) for f in fields]
    if len(parts) > 10:
        raise ValueError("%s: %d fields, client parses at most 10" % (tag, len(parts)))
    for part in parts:
        if len(part) >= 64:
            raise ValueError("%s: field %r exceeds 63 characters" % (tag, part[:20]))
    body = ";".join(parts)
    if len(body) >= 256:
        raise ValueError("%s: body of %d bytes exceeds the client's 256" % (tag, len(body)))
    return ("{%s}" % body).encode("ascii", "replace")


@dataclass
class Player:
    pid: int
    sock: socket.socket
    address: str
    name: str = "Player"
    team: int = TERRORISTS
    health: int = 100
    armor: int = 0
    helmet: bool = False
    money: int = 16000
    kills: int = 0
    deaths: int = 0
    alive: bool = True
    joined: bool = False          # completed the handshake and join burst
    authed: bool = False          # sent a KEY response
    x: int = 0                    # f32 world coordinates, as the client sends them
    y: int = 0
    z: int = 0
    angle: int = 0
    camera: int = 128
    inventory: list = field(default_factory=lambda: [-1] * INVENTORY_SLOTS)
    slot: int = 1                 # currentGunInInventory
    respawn_at: float = 0.0
    last_position_sent: float = 0.0
    position_dirty: bool = False
    inbox: bytes = b""
    outbox: bytearray = field(default_factory=bytearray)
    ping_sent_at: float = 0.0
    ping_ms: int = 0

    def gun_id(self) -> int:
        if 0 <= self.slot < INVENTORY_SLOTS:
            gun = self.inventory[self.slot]
            if gun is not None and 0 <= gun < len(GUNS):
                return gun
        return 0  # knife


class Server:
    def __init__(self, host: str, port: int, map_id: int, mode_id: int,
                 verbose: bool = False):
        self.host = host
        self.port = port
        self.map_id = map_id
        self.mode_id = mode_id
        self.verbose = verbose
        self.players: dict[socket.socket, Player] = {}
        self.next_id = 1
        self.selector = selectors.DefaultSelector()
        self.round_state = TRAINING
        self.match_ends_at = time.monotonic() + MATCH_SECONDS
        self.last_timer_sent = 0.0
        self.spawn_cursor = {TERRORISTS: 0, COUNTERTERRORISTS: 0}

    # --- plumbing --------------------------------------------------------- #

    def log(self, text: str) -> None:
        print("[%s] %s" % (time.strftime("%H:%M:%S"), text), flush=True)

    def debug(self, text: str) -> None:
        if self.verbose:
            self.log("  " + text)

    def send(self, player: Player, data: bytes) -> None:
        player.outbox += data

    def broadcast(self, data: bytes, skip: Player | None = None) -> None:
        for player in self.players.values():
            if player is not skip and player.joined:
                self.send(player, data)

    def flush(self, player: Player) -> None:
        if not player.outbox:
            return
        try:
            sent = player.sock.send(player.outbox)
            del player.outbox[:sent]
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            self.drop(player, "send failed")

    # --- lifecycle -------------------------------------------------------- #

    def start(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((self.host, self.port))
        listener.listen(MAX_PLAYERS)
        listener.setblocking(False)
        self.selector.register(listener, selectors.EVENT_READ, self.accept)
        self.listener = listener

        self.log("Counter-Strike DS server on %s:%d" % (self.host, self.port))
        for address in local_addresses():
            self.log("  players on this network connect to: %s" % address)
        self.log("  map %d, mode %d, %d players max" % (self.map_id, self.mode_id, MAX_PLAYERS))

        next_tick = time.monotonic()
        try:
            while True:
                for key, _ in self.selector.select(timeout=1.0 / TICK_HZ):
                    key.data(key.fileobj)
                now = time.monotonic()
                if now >= next_tick:
                    next_tick = now + 1.0 / TICK_HZ
                    self.tick(now)
                for player in list(self.players.values()):
                    self.flush(player)
        except KeyboardInterrupt:
            self.log("shutting down")
        finally:
            for player in list(self.players.values()):
                player.sock.close()
            listener.close()

    def accept(self, listener: socket.socket) -> None:
        try:
            sock, addr = listener.accept()
        except OSError:
            return
        if len(self.players) >= MAX_PLAYERS:
            sock.sendall(message("ERROR", ERR_SERVER_FULL))
            sock.close()
            self.log("refused %s: server full" % (addr[0],))
            return

        sock.setblocking(False)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        player = Player(pid=self.next_id, sock=sock, address=addr[0])
        self.next_id += 1
        self.players[sock] = player
        self.selector.register(sock, selectors.EVENT_READ, self.readable)

        self.log("connection from %s, assigned id %d" % (addr[0], player.pid))
        # The client waits for a challenge before it will send anything.
        self.send(player, message("KEY", random.randint(1, 2_000_000_000)))

    def drop(self, player: Player, reason: str) -> None:
        if player.sock not in self.players:
            return
        self.log("player %d (%s) left: %s" % (player.pid, player.name, reason))
        try:
            self.selector.unregister(player.sock)
        except (KeyError, ValueError):
            pass
        player.sock.close()
        del self.players[player.sock]
        self.broadcast(message("LEAVE", player.pid))

    def readable(self, sock: socket.socket) -> None:
        player = self.players.get(sock)
        if player is None:
            return
        try:
            chunk = sock.recv(4096)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            self.drop(player, "connection reset")
            return
        if not chunk:
            self.drop(player, "disconnected")
            return

        player.inbox += chunk
        if len(player.inbox) > 8192:
            self.drop(player, "flooded the receive buffer")
            return

        while True:
            start = player.inbox.find(b"{")
            if start < 0:
                player.inbox = b""
                break
            end = player.inbox.find(b"}", start)
            if end < 0:
                player.inbox = player.inbox[start:]
                break
            body = player.inbox[start + 1 : end].decode("ascii", "replace")
            player.inbox = player.inbox[end + 1 :]
            try:
                self.handle(player, body.split(";"))
            except Exception as exc:  # a malformed packet must not kill the server
                self.log("player %d sent unhandled %r: %s" % (player.pid, body[:60], exc))

    # --- gameplay --------------------------------------------------------- #

    def spawn_point(self, team: int):
        points = SPAWNS.get(self.map_id, {}).get(team) or [(0.0, 1.0, 0.0)]
        index = self.spawn_cursor[team] % len(points)
        self.spawn_cursor[team] += 1
        return points[index]

    def place_at_spawn(self, player: Player) -> None:
        x, y, z = self.spawn_point(player.team)
        # The client divides by 4096 on receipt, so send f32 world coordinates.
        player.x = int(x * 4096)
        player.y = int(y * 4096)
        player.z = int(z * 4096)
        player.angle = 0
        self.broadcast(self.position_message(player))

    def position_message(self, player: Player) -> bytes:
        return message("POS", player.pid, player.x, player.y, player.z,
                       player.angle, player.camera)

    def default_inventory(self, player: Player) -> None:
        player.inventory = [-1] * INVENTORY_SLOTS
        player.inventory[0] = 0  # knife
        player.inventory[1] = DEFAULT_T_GUN if player.team == TERRORISTS else DEFAULT_CT_GUN
        player.slot = 1

    def balanced_team(self) -> int:
        counts = {TERRORISTS: 0, COUNTERTERRORISTS: 0}
        for other in self.players.values():
            if other.joined:
                counts[other.team] = counts.get(other.team, 0) + 1
        return TERRORISTS if counts[TERRORISTS] <= counts[COUNTERTERRORISTS] else COUNTERTERRORISTS

    def join(self, player: Player) -> None:
        """Send the burst of state that brings a new client into the party."""
        player.team = self.balanced_team()
        player.joined = True
        player.alive = True
        player.health = 100
        self.default_inventory(player)

        others = [p for p in self.players.values() if p is not player and p.joined]

        self.send(player, message("SETID", player.pid))
        self.send(player, message("SETMAP", self.map_id))
        self.send(player, message("SETMODE", self.mode_id))
        self.send(player, message("SETMONEY", player.money))

        # ADDRANGE carries one id per field and the client parses at most ten
        # fields, so nine ids per message.
        for batch in chunks([p.pid for p in others], 9):
            self.send(player, message("ADDRANGE", *batch))

        for other in others + [player]:
            self.send(player, message("TEAM", other.pid, other.team))
            self.send(player, message("SETNAME", other.pid, sanitise(other.name)))
            self.send(player, message("SETHEALTH", other.pid, other.health,
                                      other.armor, 1 if other.helmet else 0))
            self.send(player, message("SCRBOARD", other.pid, other.kills, other.deaths))
            self.send(player, message("CURGUN", other.pid, other.slot))
            self.send(player, self.position_message(other))

        self.send(player, message("PartyRound", self.round_state))
        minutes, seconds = self.clock()
        self.send(player, message("TimerA", minutes, seconds))
        self.send(player, message("ENDUPDATE"))

        # Tell everyone else about the newcomer.
        self.broadcast(message("ADDRANGE", player.pid), skip=player)
        self.broadcast(message("TEAM", player.pid, player.team), skip=player)
        self.broadcast(message("SETNAME", player.pid, sanitise(player.name)), skip=player)
        self.broadcast(message("CURGUN", player.pid, player.slot), skip=player)

        self.place_at_spawn(player)
        self.log("player %d (%s) joined as %s"
                 % (player.pid, player.name, "T" if player.team == TERRORISTS else "CT"))

    def clock(self):
        remaining = max(0, int(self.match_ends_at - time.monotonic()))
        return remaining // 60, remaining % 60

    def handle(self, player: Player, fields: list[str]) -> None:
        code = int(fields[0])

        if code == KEY:
            # {17;keyResponse;MAC;name;version}. getKeyResponse() is stubbed in
            # the public client and always returns -1, so there is nothing to
            # check; the version, however, is real.
            version = fields[4] if len(fields) > 4 else ""
            if version and version != GAME_VERSION:
                self.send(player, message("ERROR", ERR_WRONG_VERSION))
                self.log("player %d has version %r, expected %r"
                         % (player.pid, version, GAME_VERSION))
                return
            if len(fields) > 3 and fields[3]:
                player.name = fields[3][:16]
            player.authed = True
            self.debug("player %d authed as %r" % (player.pid, player.name))
            return

        if code == PARTY:
            if not player.authed:
                return
            if not player.joined:
                self.join(player)
            return

        if not player.joined:
            return

        if code == POS:
            player.x = int(fields[1])
            player.y = int(fields[2])
            player.z = int(fields[3])
            player.angle = int(fields[4])
            player.camera = int(float(fields[5]))
            player.position_dirty = True

        elif code == SHOOT:
            # The client sends a bare {12}; the gun comes from tracked state.
            self.broadcast(message("SHOOT", player.pid, player.gun_id()), skip=player)

        elif code == HIT:
            self.apply_hits(player, fields[1:])

        elif code == WALLHIT:
            self.broadcast(message("WALLHIT", fields[1], fields[2], fields[3]), skip=player)

        elif code == CURGUN:
            player.slot = int(fields[1])
            self.broadcast(message("CURGUN", player.pid, player.slot), skip=player)

        elif code == SETNAME:
            player.name = fields[1][:16] if len(fields) > 1 else player.name
            self.broadcast(message("SETNAME", player.pid, sanitise(player.name)))

        elif code == TEAM:
            player.team = int(fields[1])
            self.default_inventory(player)
            self.broadcast(message("TEAM", player.pid, player.team))
            self.send(player, message("CURGUN", player.pid, player.slot))
            self.place_at_spawn(player)

        elif code == GRENADE:
            # {9;dirX;dirY;dirZ} -- the client's handler wants the throw
            # position and type too, which only the server knows.
            self.broadcast(
                message("GRENADE", fields[1], fields[2], fields[3],
                        player.x, player.y, player.z, 0),
                skip=player)

        elif code == RELOADED:
            self.broadcast(message("RELOADED", player.pid, 0), skip=player)

        elif code == BUY:
            self.buy(player, int(fields[1]))

        elif code == PING:
            if player.ping_sent_at:
                player.ping_ms = int((time.monotonic() - player.ping_sent_at) * 1000)
            player.ping_sent_at = 0.0

        elif code == LEAVE:
            self.drop(player, "left the party")

        elif code == VOTE:
            # Deathmatch starts immediately; report the vote as already met.
            self.send(player, message("VOTERESULT", 0, 1, 1))

        else:
            self.debug("player %d sent unhandled code %d" % (player.pid, code))

    def buy(self, player: Player, gun_id: int) -> None:
        if not (0 <= gun_id < len(GUNS)):
            self.send(player, message("CONFIRM", 0, gun_id, 0, 0))
            return
        gun = GUNS[gun_id]
        if gun["team"] not in (-1, player.team):
            self.send(player, message("CONFIRM", 0, gun_id, 0, 0))
            return
        # Primary weapons go to slot 2, pistols to slot 1.
        slot = 1 if gun["price"] and gun["price"] <= 800 else 2
        player.inventory[slot] = gun_id
        player.slot = slot
        self.send(player, message("CONFIRM", 0, gun_id, slot, 1))
        self.broadcast(message("CURGUN", player.pid, slot), skip=player)
        self.debug("player %d bought %s into slot %d" % (player.pid, gun["name"], slot))

    def find(self, pid: int) -> Player | None:
        for player in self.players.values():
            if player.pid == pid:
                return player
        return None

    def apply_hits(self, shooter: Player, rest: list[str]) -> None:
        """{13; targetId;head;leg;distance  [;targetId;head;leg;distance ...]}"""
        gun = GUNS[shooter.gun_id()]
        for i in range(0, len(rest) - 3, 4):
            target = self.find(int(rest[i]))
            if target is None or not target.alive or target is shooter:
                continue
            headshot = rest[i + 1] == "1"
            legshot = rest[i + 2] == "1"
            try:
                distance = float(rest[i + 3])
            except ValueError:
                distance = 0.0

            damage = gun["damage"] * (gun["falloff"] ** max(0.0, distance))
            if headshot:
                damage *= 4.0
            elif legshot:
                damage *= 0.75
            if target.armor > 0:
                damage *= 0.5 if (not headshot or target.helmet) else 1.0

            damage = max(1, int(damage))
            target.health -= damage
            self.broadcast(message("HITSOUND", 0, target.pid, 1 if headshot else 0))

            if target.health <= 0:
                self.kill(shooter, target)
            else:
                self.broadcast(message("SETHEALTH", target.pid, target.health,
                                       target.armor, 1 if target.helmet else 0))

    def kill(self, killer: Player, victim: Player) -> None:
        victim.health = 0
        victim.alive = False
        victim.deaths += 1
        victim.respawn_at = time.monotonic() + RESPAWN_SECONDS
        if killer is not victim:
            killer.kills += 1
        self.broadcast(message("SETHEALTH", victim.pid, 0, 0, 0))
        self.broadcast(message("TEXTPLAYER", victim.pid, killer.pid, 0))
        self.broadcast(message("SCRBOARD", victim.pid, victim.kills, victim.deaths))
        self.broadcast(message("SCRBOARD", killer.pid, killer.kills, killer.deaths))
        self.log("%s killed %s (%d-%d)" % (killer.name, victim.name,
                                           killer.kills, killer.deaths))

    def respawn(self, player: Player) -> None:
        player.alive = True
        player.health = 100
        self.default_inventory(player)
        self.place_at_spawn(player)
        self.broadcast(message("SETHEALTH", player.pid, 100, player.armor,
                               1 if player.helmet else 0))
        self.broadcast(message("CURGUN", player.pid, player.slot))
        self.send(player, message("RELOADED", player.pid, 1))

    def tick(self, now: float) -> None:
        # Relay positions at a rate the client can actually absorb.
        interval = 1.0 / POSITION_HZ
        for player in self.players.values():
            if not player.joined or not player.position_dirty:
                continue
            if now - player.last_position_sent < interval:
                continue
            player.last_position_sent = now
            player.position_dirty = False
            self.broadcast(self.position_message(player), skip=player)

        for player in self.players.values():
            if player.joined and not player.alive and now >= player.respawn_at:
                self.respawn(player)

        if now - self.last_timer_sent >= 1.0:
            self.last_timer_sent = now
            minutes, seconds = self.clock()
            self.broadcast(message("TimerA", minutes, seconds))
            if minutes == 0 and seconds == 0:
                self.match_ends_at = now + MATCH_SECONDS
                self.log("match time expired, restarting the clock")

        # Measure round-trip time occasionally.
        for player in self.players.values():
            if player.joined and not player.ping_sent_at and random.random() < 0.01:
                player.ping_sent_at = now
                self.send(player, message("PING", player.ping_ms))


def chunks(items: list, size: int):
    for i in range(0, len(items), size):
        yield items[i : i + size]


def sanitise(name: str) -> str:
    """Names go on the wire unescaped, so strip anything the parser would eat."""
    cleaned = "".join(c for c in name if c.isprintable() and c not in "{};")
    return (cleaned or "Player")[:16]


def local_addresses() -> list[str]:
    found = []
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        found.append(probe.getsockname()[0])
        probe.close()
    except OSError:
        pass
    return found or ["(could not determine; check your network settings)"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="0.0.0.0",
                        help="interface to bind (default: all)")
    parser.add_argument("--port", type=int, default=6003,
                        help="SERVER_PORT from network.h (default: 6003)")
    parser.add_argument("--map", type=int, default=0,
                        help="0 Dust2, 2 Dust2 2x2, 3 Aim, 4 2000, 5 fy_snow, 6 Mirage")
    parser.add_argument("--mode", type=int, default=3,
                        help="3 Deathmatch (default), 0 Competitive, 1 Occasional")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    Server(args.host, args.port, args.map, args.mode, args.verbose).start()
    return 0


if __name__ == "__main__":
    sys.exit(main())
