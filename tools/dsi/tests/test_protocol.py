#!/usr/bin/env python3
"""End-to-end tests for the DSi deployment protocol.

The device half (common/protocol.c, common/storage.c, common/service.c) is compiled for
the host and driven by tests/host_service.c, which mirrors the control flow of
loader/main.c and runtime/dsidev.c. The Mac half is the real deploy.py, imported as a
module or run as a CLI. Everything talks over loopback, so a full deploy/launch/return
cycle is exercised without a DSi attached.

Not covered here (needs hardware): Wi-Fi association, libfat SD access, and the
hbmenu chainload/return-to-loader path. See README.md for the on-device checklist.
"""
import importlib.util
import io
import os
from pathlib import Path
import contextlib
import queue
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = ROOT / ".build" / "tests"
DEVICE_SOURCES = [ROOT / "common" / name for name in ("protocol.c", "storage.c", "service.c")]
CC = os.environ.get("CC", "cc")
BINARY = BUILD / "host_service"
PORT = LOG_PORT = 0


def load_deploy():
    spec = importlib.util.spec_from_file_location("dsidev_deploy", ROOT / "deploy.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


deploy = load_deploy()


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def compile_harness(*extra):
    BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run([CC, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    f"-DDEV_PORT={PORT}", f"-DDEV_LOG_PORT={LOG_PORT}", f"-I{ROOT / 'common'}",
                    *extra, str(HERE / "host_service.c"), *map(str, DEVICE_SOURCES),
                    "-o", str(BINARY)], check=True)


def setUpModule():
    global PORT, LOG_PORT
    PORT, LOG_PORT = free_port(), free_port()
    deploy.PORT, deploy.LOG_PORT = PORT, LOG_PORT
    compile_harness()


def crc16(data):
    # The header checksum the DSi firmware (and common/protocol.c) verifies.
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def make_nds(tag=b"build", size=4096, dsi=False, valid_header=True):
    """Build a minimal NDS image that satisfies common/protocol.c's dev_nds_valid()."""
    data = bytearray(size)
    data[0:12] = b"DSIDEV TEST\x00"
    data[0x12] = 0x03 if dsi else 0x00
    struct.pack_into("<I", data, 0x20, 512)    # ARM9 offset / size
    struct.pack_into("<I", data, 0x2C, 512)
    struct.pack_into("<I", data, 0x30, 1024)   # ARM7 offset / size
    struct.pack_into("<I", data, 0x3C, 512)
    if dsi:
        struct.pack_into("<I", data, 0x1C0, 1536)  # ARM9i offset / size
        struct.pack_into("<I", data, 0x1CC, 256)
        struct.pack_into("<I", data, 0x1D0, 1792)  # ARM7i offset / size
        struct.pack_into("<I", data, 0x1DC, 256)
    data[512:512 + len(tag)] = tag  # outside the header checksum: only the CRC32 changes
    struct.pack_into("<H", data, 0x15E, crc16(bytes(data[:0x15E])) ^ (0 if valid_header else 1))
    return bytes(data)


class Device:
    """A running host_service process standing in for the DSi."""

    def __init__(self, mode, root, token_path, crc=0, host="127.0.0.1"):
        self.mode = mode
        self.lines = queue.Queue()
        self.seen = []
        self.process = subprocess.Popen(
            [str(BINARY), str(root), str(token_path), mode, host, f"{crc:08x}"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.wait_for("READY")

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def wait_for(self, prefix, timeout=20):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"{self.mode}: no {prefix!r}; saw {self.seen}")
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                raise AssertionError(f"{self.mode}: no {prefix!r}; saw {self.seen}")
            if line is None:
                raise AssertionError(f"{self.mode}: exited before {prefix!r}; saw {self.seen} "
                                     f"stderr={self.process.stderr.read()}")
            self.seen.append(line)
            if line.startswith(prefix):
                return line

    def quiet_for(self, seconds):
        time.sleep(seconds)
        return self.lines.empty()

    def wait_exit(self, timeout=10):
        return self.process.wait(timeout=timeout)

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)
        self.process.stdout.close()
        self.process.stderr.close()


class ProtocolTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.root = base / "dsidev"          # stands in for sd:/dsidev
        self.token_path = base / "token.bin"
        self.secret = deploy.token(self.token_path, create=True)
        self.devices = []
        self.addCleanup(self.stop_devices)

    def stop_devices(self):
        for device in self.devices:
            device.stop()

    def device(self, mode, crc=0):
        device = Device(mode, self.root, self.token_path, crc=crc)
        self.devices.append(device)
        return device

    def write(self, name, data):
        path = Path(self.temp.name) / name
        path.write_bytes(data)
        return path

    def send(self, path, operation=1, secret=None, corrupt_crc=False, name=None):
        with deploy.snapshot(path, operation) as (image, size, crc):
            sent = crc ^ 0xFFFF if corrupt_crc else crc
            return deploy.transfer("127.0.0.1", secret or self.secret, image, size, sent, operation, name)

    # --- discovery -------------------------------------------------------------

    def test_discovery_reports_loader_state(self):
        self.device("loader")
        address, state = deploy.find_device("127.0.0.1", 10)
        self.assertEqual(address, "127.0.0.1")
        self.assertEqual(state[0], "loader")
        self.assertEqual(state[1:], (0, 0))  # no stored build, generation 0

    def test_discovery_ignores_foreign_traffic(self):
        self.device("loader")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(b"garbage!", ("127.0.0.1", PORT))
        self.assertEqual(deploy.find_device("127.0.0.1", 10)[1][0], "loader")

    # --- upload, commit, launch ------------------------------------------------

    def test_run_commits_and_launches(self):
        loader = self.device("loader")
        image = make_nds(b"first")
        path = self.write("first.nds", image)
        crc, skipped = self.send(path)
        self.assertEqual(crc, zlib.crc32(image))
        self.assertFalse(skipped)
        launch = loader.wait_for("LAUNCH")
        self.assertEqual(launch.split()[1], f"{zlib.crc32(image):08x}")
        stored = Path(launch.split()[2])
        self.assertEqual(stored.read_bytes(), image)
        self.assertEqual(stored.name, "app0.nds")
        self.assertEqual(loader.wait_exit(), 0)
        self.assertFalse((self.root / "upload.part").exists())

    def test_identical_build_is_not_resent(self):
        loader = self.device("loader")
        path = self.write("same.nds", make_nds(b"same"))
        self.send(path)
        loader.wait_for("LAUNCH")
        loader.wait_exit()

        loader = self.device("loader")
        crc, skipped = self.send(path)
        self.assertTrue(skipped, "second deploy of identical bytes should be deduplicated")
        loader.wait_for("LAUNCH")
        self.assertEqual(crc, zlib.crc32(path.read_bytes()))

    def test_second_build_alternates_slot_and_keeps_previous(self):
        loader = self.device("loader")
        first = self.write("a.nds", make_nds(b"aaaa"))
        self.send(first)
        loader.wait_for("LAUNCH")
        loader.wait_exit()

        loader = self.device("loader")
        second = self.write("b.nds", make_nds(b"bbbb", dsi=True))
        self.send(second)
        launch = loader.wait_for("LAUNCH")
        self.assertEqual(Path(launch.split()[2]).name, "app1.nds", "writes must not overwrite the live slot")
        self.assertTrue((self.root / "app0.nds").exists(), "previous build stays on the card")
        loader.wait_exit()

        # A restarted loader recovers the newest verified generation.
        restarted = self.device("loader")
        self.assertEqual(restarted.seen[0].split()[2], f"{zlib.crc32(second.read_bytes()):08x}")

    def test_store_then_launch_without_resending(self):
        loader = self.device("loader")
        path = self.write("stored.nds", make_nds(b"stored"))
        crc, _ = self.send(path, operation=2)          # store only
        self.assertTrue(loader.quiet_for(0.3), "store must not launch")
        detail, skipped = deploy.transfer("127.0.0.1", self.secret, None, 0, 0, 3)  # launch
        self.assertEqual(detail, crc)
        self.assertTrue(skipped)
        loader.wait_for("LAUNCH")

    def test_launch_without_stored_build_is_rejected(self):
        self.device("loader")
        with self.assertRaises(RuntimeError) as caught:
            deploy.transfer("127.0.0.1", self.secret, None, 0, 0, 3)
        self.assertIn("invalid request", str(caught.exception))

    # --- rejections ------------------------------------------------------------

    def test_wrong_token_is_rejected(self):
        self.device("loader")
        path = self.write("x.nds", make_nds())
        with self.assertRaises(RuntimeError) as caught:
            self.send(path, secret=bytes(16))
        self.assertIn("token mismatch", str(caught.exception))
        self.assertFalse(list(self.root.glob("app*.nds")))

    def test_checksum_mismatch_is_rejected(self):
        loader = self.device("loader")
        path = self.write("x.nds", make_nds(b"crc"))
        with self.assertRaises(RuntimeError) as caught:
            self.send(path, corrupt_crc=True)
        self.assertIn("checksum mismatch", str(caught.exception))
        self.assertFalse(list(self.root.glob("app*.nds")), "a corrupt image is never published")
        self.assertTrue(loader.quiet_for(0.2))

    def test_invalid_nds_image_is_rejected(self):
        self.device("loader")
        path = self.write("bad.nds", make_nds(b"bad", valid_header=False))
        with self.assertRaises(RuntimeError) as caught:
            self.send(path)
        self.assertIn("invalid NDS image", str(caught.exception))
        self.assertFalse(list(self.root.glob("app*.nds")))

    def test_oversized_upload_is_rejected_before_any_write(self):
        self.device("loader")
        with socket.create_connection(("127.0.0.1", PORT), timeout=10) as sock:
            sock.sendall(deploy.HEADER.pack(b"DSIUPL1\0", 1, 64 * 1024 * 1024, 0, 0, self.secret))
            with self.assertRaises(RuntimeError) as caught:
                deploy.receive_ack(sock)
        self.assertIn("invalid request", str(caught.exception))
        self.assertFalse((self.root / "upload.part").exists())

    def test_running_application_refuses_builds(self):
        self.device("app")
        path = self.write("x.nds", make_nds())
        with self.assertRaises(RuntimeError) as caught:
            self.send(path)
        self.assertIn("return to the loader", str(caught.exception))

    # --- assets, logs, return --------------------------------------------------

    def test_asset_upload_reaches_the_running_application(self):
        app = self.device("app")
        asset = self.write("level.bin", b"hot reload payload")
        crc, skipped = self.send(asset, operation=4)
        self.assertFalse(skipped)
        stored = Path(app.wait_for("ASSET").split()[1])
        self.assertEqual(stored.read_bytes(), b"hot reload payload")
        self.assertEqual(crc, zlib.crc32(b"hot reload payload"))

        # A restarted application sees the asset immediately, before any new upload.
        app.stop()
        restarted = self.device("app")
        self.assertTrue(any(line.startswith("ASSET") for line in restarted.seen))

    def test_return_to_loader_requires_the_token(self):
        app = self.device("app")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(b"DSIRESET" + bytes(16), ("127.0.0.1", PORT))
            self.assertTrue(app.quiet_for(0.3), "an unauthenticated reset must be ignored")

        def reboot_into_loader():
            # On hardware the bootstub reloads sd:/dsidev.nds once the application exits.
            app.wait_for("RETURN", timeout=30)
            app.wait_exit()
            self.devices.append(Device("loader", self.root, self.token_path))

        stand_in = threading.Thread(target=reboot_into_loader, daemon=True)
        stand_in.start()
        deploy.return_to_loader("127.0.0.1", self.secret, 20)
        stand_in.join(timeout=20)
        self.assertEqual(app.process.returncode, 0)
        self.assertEqual(deploy.find_device("127.0.0.1", 10)[1][0], "loader")

    def test_logs_stream_to_the_mac(self):
        with deploy.Logs("127.0.0.1") as logs:
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                self.device("app", crc=0xABCD1234)
                time.sleep(0.5)
            self.assertIn("application ready", captured.getvalue())
            self.assertIn("abcd1234", captured.getvalue())
            del logs

    def test_log_listener_strips_control_sequences(self):
        with deploy.Logs("127.0.0.1"):
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.sendto(b"DSILOG1 00000000 0 \x1b[31mred\x07", ("127.0.0.1", LOG_PORT))
                time.sleep(0.4)
        self.assertIn("?[31mred?", captured.getvalue())
        self.assertNotIn("\x1b", captured.getvalue())

    # --- data files and loader updates -----------------------------------------

    def test_data_file_lands_beside_the_build(self):
        self.device("loader")
        data = self.write("soundbank.bin", b"samples" * 4096)
        crc, skipped = self.send(data, operation=5, name="soundbank.bin")
        self.assertFalse(skipped)
        self.assertEqual(crc, zlib.crc32(data.read_bytes()))
        stored = self.root / "soundbank.bin"
        self.assertEqual(stored.read_bytes(), data.read_bytes(),
                         "data must land in the directory builds run from")
        self.assertTrue((self.root / "soundbank.bin.rec").exists())

        # Unchanged data is skipped, which is what makes pushing it on every deploy cheap.
        _, skipped = self.send(data, operation=5, name="soundbank.bin")
        self.assertTrue(skipped)

        data.write_bytes(b"different samples")
        _, skipped = self.send(data, operation=5, name="soundbank.bin")
        self.assertFalse(skipped)
        self.assertEqual(stored.read_bytes(), b"different samples")

    def test_data_file_names_cannot_escape_the_directory(self):
        self.device("loader")
        data = self.write("payload.bin", b"payload")
        for name in ("../escape", "sub/dir", ".hidden", "..", "", "a" * 65, "sd:/root"):
            with self.subTest(name=name), self.assertRaises(RuntimeError) as caught:
                self.send(data, operation=5, name=name)
            self.assertIn("invalid", str(caught.exception).lower())
        self.assertEqual(sorted(entry.name for entry in self.root.iterdir()), [],
                         "a rejected name must not create anything")

    def test_running_application_refuses_data_files(self):
        self.device("app")
        data = self.write("payload.bin", b"payload")
        with self.assertRaises(RuntimeError) as caught:
            self.send(data, operation=5, name="payload.bin")
        self.assertIn("return to the loader", str(caught.exception))

    def test_loader_replaces_itself_on_the_card(self):
        loader = self.device("loader")
        card_image = Path(self.temp.name) / "dsidev.nds"   # the store root plus .nds
        replacement = self.write("new-loader.nds", make_nds(b"newer loader"))
        crc, skipped = self.send(replacement, operation=6)
        self.assertFalse(skipped)
        self.assertEqual(card_image.read_bytes(), replacement.read_bytes())
        self.assertEqual(crc, zlib.crc32(replacement.read_bytes()))
        self.assertTrue(loader.quiet_for(0.2), "replacing the loader must not launch anything")

        _, skipped = self.send(replacement, operation=6)
        self.assertTrue(skipped)

    def test_a_corrupt_loader_never_replaces_the_working_one(self):
        self.device("loader")
        good = self.write("good.nds", make_nds(b"good loader"))
        self.send(good, operation=6)
        card_image = Path(self.temp.name) / "dsidev.nds"
        broken = self.write("broken.nds", make_nds(b"broken", valid_header=False))
        with self.assertRaises(RuntimeError) as caught:
            self.send(broken, operation=6)
        self.assertIn("invalid NDS image", str(caught.exception))
        self.assertEqual(card_image.read_bytes(), good.read_bytes(),
                         "the loader on the card must survive a bad update")

    # --- crash safety ----------------------------------------------------------

    def test_power_loss_during_upload_keeps_the_previous_build(self):
        loader = self.device("loader")
        good = self.write("good.nds", make_nds(b"good"))
        self.send(good)
        loader.wait_for("LAUNCH")
        loader.wait_exit()

        loader = self.device("loader")
        big = self.write("big.nds", make_nds(b"big", size=12 * 1024 * 1024))
        part = self.root / "upload.part"
        killed = threading.Event()

        def kill_mid_upload():
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if part.exists() and part.stat().st_size > 256 * 1024:
                    loader.process.send_signal(signal.SIGKILL)  # power loss, no cleanup
                    killed.set()
                    return
                time.sleep(0.001)

        watcher = threading.Thread(target=kill_mid_upload, daemon=True)
        watcher.start()
        with self.assertRaises((OSError, ConnectionError, RuntimeError)):
            self.send(big)
        watcher.join(timeout=5)
        self.assertTrue(killed.is_set(), "the upload finished before it could be interrupted")
        self.assertTrue(part.exists(), "a partial upload is expected to survive the crash")

        restarted = self.device("loader")
        self.assertEqual(restarted.seen[0].split()[2], f"{zlib.crc32(good.read_bytes()):08x}",
                         "the last verified build must still be the active one")
        self.send(good)
        restarted.wait_for("LAUNCH")

    def test_truncated_image_is_not_recovered(self):
        loader = self.device("loader")
        good = self.write("good.nds", make_nds(b"good"))
        self.send(good)
        loader.wait_for("LAUNCH")
        loader.wait_exit()
        stored = self.root / "app0.nds"
        stored.write_bytes(stored.read_bytes()[:-64])  # simulate a bad sector/short write

        restarted = self.device("loader")
        self.assertEqual(restarted.seen[0].split()[2], "00000000", "a damaged image is not offered")
        with self.assertRaises(RuntimeError):
            deploy.transfer("127.0.0.1", self.secret, None, 0, 0, 3)

    # --- device sources --------------------------------------------------------

    def test_token_handover_round_trips_every_byte(self):
        result = subprocess.run([str(BINARY), "selftest"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SELFTEST OK", result.stdout)

    def test_device_sources_compile_without_warnings(self):
        compile_harness("-Werror")


class CommandLineTest(unittest.TestCase):
    """Covers deploy.py as `make run-dsi` invokes it."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.root = base / "dsidev"
        self.token_path = base / "token.bin"
        deploy.token(self.token_path, create=True)
        self.devices = []
        self.addCleanup(lambda: [device.stop() for device in self.devices])
        self.env = {**os.environ, "DSIDEV_PORT": str(PORT), "DSIDEV_LOG_PORT": str(LOG_PORT)}

    def run_cli(self, *args, timeout=90):
        return subprocess.run([sys.executable, str(ROOT / "deploy.py"), *args, "--token", str(self.token_path)],
                              env=self.env, capture_output=True, text=True, timeout=timeout)

    def test_run_deploys_launches_and_waits_for_the_application(self):
        loader = Device("loader", self.root, self.token_path)
        self.devices.append(loader)
        path = Path(self.temp.name) / "game.nds"
        path.write_bytes(make_nds(b"cli"))

        def chainload():
            # Stands in for the loader handing control to the freshly deployed build.
            crc = int(loader.wait_for("LAUNCH", timeout=60).split()[1], 16)
            loader.wait_exit()
            self.devices.append(Device("app", self.root, self.token_path, crc=crc))

        stand_in = threading.Thread(target=chainload, daemon=True)
        stand_in.start()
        result = self.run_cli("run", str(path), "--ip", "127.0.0.1", "--timeout", "30")
        stand_in.join(timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Verified and committed", result.stdout)
        self.assertIn("Application ready", result.stdout)

    def test_run_returns_a_running_application_to_the_loader_first(self):
        app = Device("app", self.root, self.token_path)
        self.devices.append(app)
        path = Path(self.temp.name) / "game.nds"
        path.write_bytes(make_nds(b"cli2"))

        def restart_loader():
            app.wait_for("RETURN", timeout=60)
            app.wait_exit()
            self.devices.append(Device("loader", self.root, self.token_path))

        stand_in = threading.Thread(target=restart_loader, daemon=True)
        stand_in.start()
        result = self.run_cli("run", str(path), "--ip", "127.0.0.1", "--timeout", "30", "--no-wait-start")
        stand_in.join(timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Returning", result.stdout)
        self.assertIn("Verified and committed", result.stdout)

    def test_run_pushes_data_files_before_launching(self):
        loader = Device("loader", self.root, self.token_path)
        self.devices.append(loader)
        game = Path(self.temp.name) / "game.nds"
        game.write_bytes(make_nds(b"with-data"))
        data = Path(self.temp.name) / "soundbank.bin"
        data.write_bytes(b"samples" * 512)
        result = self.run_cli("run", str(game), "--ip", "127.0.0.1", "--timeout", "20",
                              "--no-wait-start", "--data", str(data))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("soundbank.bin", result.stdout)
        self.assertEqual((self.root / "soundbank.bin").read_bytes(), data.read_bytes())
        loader.wait_for("LAUNCH")

    def test_discover_lists_the_device(self):
        self.devices.append(Device("loader", self.root, self.token_path))
        result = self.run_cli("discover", "--ip", "127.0.0.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("127.0.0.1 loader", result.stdout)

    def test_missing_device_fails_with_guidance(self):
        result = self.run_cli("discover", "--ip", "127.0.0.1")
        self.assertEqual(result.stdout.strip(), "")
        result = self.run_cli("reset", "--ip", "127.0.0.1", "--timeout", "2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No DSi response", result.stderr)

    def test_stage_writes_a_single_sd_file(self):
        loader_nds = Path(self.temp.name) / "loader.nds"
        loader_nds.write_bytes(make_nds(b"loader"))
        # Never the real staging directory: that holds the payload bound for the SD card.
        staged = Path(self.temp.name) / "sd"
        result = subprocess.run([sys.executable, str(ROOT / "deploy.py"), "stage", str(loader_nds),
                                 "--out", str(staged)], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((staged / "dsidev.nds").read_bytes(), loader_nds.read_bytes())
        self.assertEqual([entry.name for entry in staged.iterdir()], ["dsidev.nds"],
                         "the SD payload is one file: the secret travels inside the loader")

    def test_token_command_creates_a_stable_secret_and_header(self):
        token_path = Path(self.temp.name) / "token.bin"
        header = Path(self.temp.name) / "token.h"
        command = [sys.executable, str(ROOT / "deploy.py"), "token",
                   "--token", str(token_path), "--header", str(header)]
        self.assertEqual(subprocess.run(command, env=self.env, capture_output=True).returncode, 0)
        secret = token_path.read_bytes()
        self.assertEqual(len(secret), 16)
        self.assertEqual(token_path.stat().st_mode & 0o777, 0o600)
        for byte in secret:
            self.assertIn(f"0x{byte:02x}", header.read_text())
        # Re-running must not touch either file, or the loader would rebuild every time.
        stamp = header.stat().st_mtime_ns
        self.assertEqual(subprocess.run(command, env=self.env, capture_output=True).returncode, 0)
        self.assertEqual(header.stat().st_mtime_ns, stamp)
        self.assertEqual(token_path.read_bytes(), secret)

    def test_concurrent_deployments_are_refused(self):
        import fcntl
        lock_path = ROOT / ".build" / "deploy.lock"
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        with lock_path.open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            result = self.run_cli("discover", "--ip", "127.0.0.1")   # no lock needed
            self.assertEqual(result.returncode, 0)
            result = self.run_cli("reset", "--ip", "127.0.0.1", "--timeout", "5")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Another deployment is already running", result.stderr)


if __name__ == "__main__":
    unittest.main()
