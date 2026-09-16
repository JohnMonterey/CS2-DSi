#!/usr/bin/env python3
"""Protocol tests for the Counter-Strike DS server.

These drive the real server over a loopback socket with a client that speaks the
same wire format the DS does, so the join handshake, relaying and hit scoring can
be checked without the console.

Run:  make test-server
"""

from __future__ import annotations

import socket
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cs_server  # noqa: E402
from cs_server import Server, message  # noqa: E402


class FakeDS:
    """A client that behaves the way network.c does, including its limits."""

    def __init__(self, port: int, name: str = "Tester"):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(5)
        self.buffer = b""
        self.received: list[list[str]] = []
        self.name = name
        self.pid: int | None = None

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def pump(self, seconds: float = 0.4) -> None:
        """Read whatever has arrived, parsing exactly as treatData() does."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.sock.settimeout(0.05)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            self.buffer += chunk
            while True:
                start = self.buffer.find(b"{")
                if start < 0:
                    break
                end = self.buffer.find(b"}", start)
                if end < 0:
                    break
                body = self.buffer[start + 1 : end].decode("ascii", "replace")
                self.buffer = self.buffer[end + 1 :]
                self.received.append(body.split(";"))

    def send_raw(self, text: str) -> None:
        self.sock.sendall(text.encode("ascii"))

    def tags(self) -> list[str]:
        return [m[0] for m in self.received]

    def first(self, tag: str) -> list[str] | None:
        for m in self.received:
            if m[0] == tag:
                return m
        return None

    def all_of(self, tag: str) -> list[list[str]]:
        return [m for m in self.received if m[0] == tag]

    def clear(self) -> None:
        self.received.clear()

    def handshake(self) -> None:
        self.pump()
        assert self.first("KEY"), "server must challenge first, got %s" % self.tags()
        # getKeyResponse() is stubbed in the real client and returns -1.
        self.send_raw("{%d;-1;001122334455;%s;%s}"
                      % (cs_server.KEY, self.name, cs_server.GAME_VERSION))
        self.send_raw("{%d;0}" % cs_server.PARTY)
        self.pump()
        setid = self.first("SETID")
        assert setid, "no SETID after joining, got %s" % self.tags()
        self.pid = int(setid[1])


class ServerTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.server = Server("127.0.0.1", 0, map_id=0, mode_id=3)
        # Bind on an ephemeral port so tests never collide with a live server.
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        self.port = listener.getsockname()[1]
        listener.close()
        self.server.port = self.port

        self.thread = threading.Thread(target=self.server.start, daemon=True)
        self.thread.start()
        for _ in range(100):
            try:
                probe = socket.create_connection(("127.0.0.1", self.port), timeout=0.2)
                probe.close()
                break
            except OSError:
                time.sleep(0.02)
        self.clients: list[FakeDS] = []

    def tearDown(self) -> None:
        for client in self.clients:
            client.close()
        try:
            self.server.listener.close()
        except Exception:
            pass

    def connect(self, name: str = "Tester") -> FakeDS:
        client = FakeDS(self.port, name)
        self.clients.append(client)
        return client

    # --- tests ---------------------------------------------------------- #

    def test_server_challenges_before_anything_else(self):
        client = self.connect()
        client.pump()
        self.assertEqual(client.tags()[0], "KEY",
                         "the client blocks until it receives a KEY challenge")

    def test_join_sends_the_state_a_client_needs(self):
        client = self.connect("Umut")
        client.handshake()
        tags = client.tags()
        for required in ("SETID", "SETMAP", "SETMODE", "TEAM", "SETNAME",
                         "SETHEALTH", "PartyRound", "TimerA", "ENDUPDATE", "POS"):
            self.assertIn(required, tags, "join burst is missing %s" % required)
        self.assertEqual(client.first("SETMAP")[1], "0")
        self.assertEqual(client.first("SETMODE")[1], "3")
        self.assertEqual(client.first("SETNAME")[2], "Umut")

    def test_spawn_position_is_a_real_map_spawn(self):
        client = self.connect()
        client.handshake()
        positions = [m for m in client.all_of("POS") if int(m[1]) == client.pid]
        self.assertTrue(positions, "the player must be placed somewhere")
        x, y, z = (int(positions[-1][i]) / 4096.0 for i in (2, 3, 4))
        from gamedata import SPAWNS
        valid = SPAWNS[0][0] + SPAWNS[0][1]
        closest = min(abs(x - sx) + abs(y - sy) + abs(z - sz)
                      for sx, sy, sz in valid)
        self.assertLess(closest, 0.01,
                        "spawned at (%.2f, %.2f, %.2f), not on a Dust2 spawn" % (x, y, z))

    def test_second_player_is_announced_to_the_first(self):
        first = self.connect("One")
        first.handshake()
        first.clear()

        second = self.connect("Two")
        second.handshake()
        first.pump()

        self.assertIn("ADDRANGE", first.tags(),
                      "the first player must learn the second exists")
        added = [m for m in first.all_of("ADDRANGE")]
        self.assertTrue(any(str(second.pid) in m[1:] for m in added))
        names = [m for m in first.all_of("SETNAME") if m[2] == "Two"]
        self.assertTrue(names, "the second player's name must be relayed")

    def test_teams_are_balanced(self):
        a = self.connect("A")
        a.handshake()
        b = self.connect("B")
        b.handshake()
        teams = {p.pid: p.team for p in self.server.players.values() if p.joined}
        self.assertEqual(sorted(teams.values()), [0, 1],
                         "two players should land on opposite teams")

    def test_position_is_relayed_to_the_other_player(self):
        a = self.connect("A")
        a.handshake()
        b = self.connect("B")
        b.handshake()
        b.clear()

        a.send_raw("{%d;40960;8192;-16384;64;120}" % cs_server.POS)
        time.sleep(0.25)
        b.pump()

        moved = [m for m in b.all_of("POS") if int(m[1]) == a.pid]
        self.assertTrue(moved, "A's movement must reach B")
        self.assertEqual(moved[-1][2], "40960")
        self.assertEqual(moved[-1][5], "64")

    def test_position_updates_are_throttled(self):
        a = self.connect("A")
        a.handshake()
        b = self.connect("B")
        b.handshake()
        b.clear()

        # Flood at well above the client's intake rate.
        for i in range(120):
            a.send_raw("{%d;%d;0;0;0;128}" % (cs_server.POS, i * 100))
        time.sleep(1.0)
        b.pump(0.3)

        relayed = [m for m in b.all_of("POS") if int(m[1]) == a.pid]
        self.assertLessEqual(
            len(relayed), 15,
            "120 updates in ~1s became %d relays; the DS reads only 63 bytes a "
            "frame and disconnects if its 1024-byte buffer fills" % len(relayed))
        self.assertTrue(relayed, "throttling must not drop movement entirely")

    def test_shoot_is_relayed_with_the_tracked_weapon(self):
        a = self.connect("A")
        a.handshake()
        b = self.connect("B")
        b.handshake()
        b.clear()

        a.send_raw("{%d;17}" % cs_server.BUY)       # AK-47
        a.send_raw("{%d}" % cs_server.SHOOT)
        time.sleep(0.2)
        b.pump()

        shots = b.all_of("SHOOT")
        self.assertTrue(shots, "the shot must be relayed")
        self.assertEqual(int(shots[-1][1]), a.pid)
        self.assertEqual(int(shots[-1][2]), 17, "the relayed gun id must be the AK")

    def test_hits_reduce_health_and_a_kill_scores(self):
        a = self.connect("Shooter")
        a.handshake()
        b = self.connect("Target")
        b.handshake()
        a.send_raw("{%d;17}" % cs_server.BUY)  # AK-47, 35 damage
        time.sleep(0.2)
        b.clear()

        a.send_raw("{%d;%d;0;0;0.0}" % (cs_server.HIT, b.pid))
        time.sleep(0.2)
        b.pump()

        health = [m for m in b.all_of("SETHEALTH") if int(m[1]) == b.pid]
        self.assertTrue(health, "a hit must produce a health update")
        self.assertEqual(int(health[-1][2]), 65, "AK-47 body shot should do 35")

        # Three more body shots kill.
        b.clear()
        for _ in range(3):
            a.send_raw("{%d;%d;0;0;0.0}" % (cs_server.HIT, b.pid))
        time.sleep(0.3)
        b.pump()

        self.assertIn("TEXTPLAYER", b.tags(), "a death must be announced")
        board = [m for m in b.all_of("SCRBOARD") if int(m[1]) == a.pid]
        self.assertTrue(board and int(board[-1][2]) == 1,
                        "the shooter should be credited with one kill")

    def test_headshot_does_more_damage_than_a_body_shot(self):
        a = self.connect("Shooter")
        a.handshake()
        b = self.connect("Target")
        b.handshake()
        a.send_raw("{%d;1}" % cs_server.BUY)  # Glock, 24 damage
        time.sleep(0.2)
        b.clear()

        a.send_raw("{%d;%d;1;0;0.0}" % (cs_server.HIT, b.pid))
        time.sleep(0.2)
        b.pump()
        health = [m for m in b.all_of("SETHEALTH") if int(m[1]) == b.pid]
        self.assertTrue(health)
        self.assertLess(int(health[-1][2]), 100 - 24,
                        "a headshot must hurt more than a body shot")

    def test_dead_player_respawns(self):
        a = self.connect("Shooter")
        a.handshake()
        b = self.connect("Target")
        b.handshake()
        a.send_raw("{%d;23}" % cs_server.BUY)  # AWP, 115 damage: one shot
        time.sleep(0.2)
        b.clear()

        a.send_raw("{%d;%d;0;0;0.0}" % (cs_server.HIT, b.pid))
        time.sleep(0.2)
        b.pump()
        self.assertIn("TEXTPLAYER", b.tags(), "the AWP should kill outright")

        b.clear()
        time.sleep(cs_server.RESPAWN_SECONDS + 0.5)
        b.pump()
        alive = [m for m in b.all_of("SETHEALTH")
                 if int(m[1]) == b.pid and int(m[2]) == 100]
        self.assertTrue(alive, "the player must come back with full health")

    def test_leaving_is_announced(self):
        a = self.connect("A")
        a.handshake()
        b = self.connect("B")
        b.handshake()
        a.clear()

        b.send_raw("{%d}" % cs_server.LEAVE)
        time.sleep(0.3)
        a.pump()
        leaves = [m for m in a.all_of("LEAVE") if int(m[1]) == b.pid]
        self.assertTrue(leaves, "the remaining player must be told")

    def test_a_dropped_connection_is_announced(self):
        a = self.connect("A")
        a.handshake()
        b = self.connect("B")
        b.handshake()
        a.clear()

        b.sock.close()
        time.sleep(0.3)
        a.pump()
        self.assertTrue([m for m in a.all_of("LEAVE") if int(m[1]) == b.pid])

    def test_wrong_version_is_rejected(self):
        client = self.connect()
        client.pump()
        client.send_raw("{%d;-1;001122334455;Old;0.0.1}" % cs_server.KEY)
        client.send_raw("{%d;0}" % cs_server.PARTY)
        client.pump()
        self.assertIn("ERROR", client.tags())
        self.assertNotIn("SETID", client.tags(),
                         "a wrong-version client must not be allowed to join")

    def test_malformed_input_does_not_kill_the_server(self):
        client = self.connect()
        client.handshake()
        client.send_raw("{}{;;;}{9999}{abc}{10;not;a;number;x;y}{")
        time.sleep(0.2)
        # The server must still be answering.
        other = self.connect("Still alive")
        other.handshake()
        self.assertIsNotNone(other.pid)


class MessageLimitTestCase(unittest.TestCase):
    """The client's parser has hard limits; message() must enforce them."""

    def test_at_most_ten_fields(self):
        message("POS", *range(9))  # ten fields total is fine
        with self.assertRaises(ValueError):
            message("POS", *range(10))

    def test_fields_stay_under_64_characters(self):
        with self.assertRaises(ValueError):
            message("SETNAME", 1, "x" * 64)

    def test_body_stays_under_256_bytes(self):
        with self.assertRaises(ValueError):
            message("TEXT", *["y" * 40 for _ in range(8)])

    def test_names_cannot_inject_protocol_characters(self):
        from cs_server import sanitise
        self.assertEqual(sanitise("ev{il;name}"), "evilname")
        self.assertEqual(sanitise(""), "Player")
        self.assertEqual(sanitise("x" * 40), "x" * 16)


if __name__ == "__main__":
    unittest.main(verbosity=2)
