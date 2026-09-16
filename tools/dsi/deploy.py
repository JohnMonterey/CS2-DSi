#!/usr/bin/env python3
"""DSi deployment client. Python standard library only; native on Apple Silicon."""
import argparse
import contextlib
import fcntl
import ipaddress
import os
import re
from pathlib import Path
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

HERE = Path(__file__).resolve().parent
# Ports match common/protocol.h. The environment overrides exist for the host tests.
PORT = int(os.environ.get("DSIDEV_PORT", 17491))
LOG_PORT = int(os.environ.get("DSIDEV_LOG_PORT", 17493))
HEADER = struct.Struct("!8sIIII16s")
ACK = struct.Struct("!8sII")
QUERY = b"DSIDEV1?"
ERRORS = {100: "invalid request/no stored build", 101: "token mismatch (reinstall dsidev.nds)",
          102: "SD write/sync error", 103: "checksum mismatch", 104: "invalid NDS image",
          105: "application is running; return to the loader first", 106: "invalid file name"}
OPERATIONS = {"run": 1, "store": 2, "launch": 3, "asset": 4, "put": 5, "update-loader": 6}
LIMITS = {4: 8, 5: 16}  # megabytes; everything else is an NDS image at 32
NAME = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}\Z")  # \Z, not $: no trailing newline


def check_name(name):
    # Mirrors dev_name_valid() on the device: one plain name, no path, no traversal.
    if not NAME.match(name):
        raise ValueError(f"{name}: use letters, digits, dot, dash or underscore (max 64, no leading dot)")
    return name


def token(path, create=False):
    path = Path(path)
    if create and not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as f:
            f.write(secrets.token_bytes(16))
        path.chmod(0o600)
    value = path.read_bytes()
    if len(value) != 16:
        raise ValueError(f"{path}: expected exactly 16 token bytes")
    return value


def write_token_header(path, secret):
    body = ",".join(f"0x{byte:02x}" for byte in secret)
    text = ("// Generated from token.bin by deploy.py. Not for committing or sharing: this is\n"
            "// the secret that authorises deployments to your console.\n"
            "#pragma once\n"
            f"static const unsigned char DEV_TOKEN[16] = {{{body}}};\n")
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Rewrite only on change so the loader does not rebuild on every make.
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def broadcasts():
    addresses = {"255.255.255.255"}
    # macOS lists per-interface broadcast addresses; ignore loopback and IPv6.
    try:
        import re
        result = subprocess.run(["ifconfig"], capture_output=True, text=True, check=True)
        addresses.update(re.findall(r"broadcast (\d+\.\d+\.\d+\.\d+)", result.stdout))
    except (OSError, subprocess.CalledProcessError):
        pass
    return sorted(addresses)


def discover(ip=None, duration=0.7):
    found = {}
    targets = [str(ipaddress.IPv4Address(ip))] if ip else broadcasts()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(0.1)
        deadline, next_send = time.monotonic() + duration, 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_send:
                for target in targets:
                    try:
                        sock.sendto(QUERY, (target, PORT))
                    except OSError:
                        if ip:
                            raise
                next_send = time.monotonic() + 0.25
            try:
                data, peer = sock.recvfrom(256)
                fields = data.decode("ascii").split()
                if len(fields) != 4 or fields[0] != "DSIDEV1" or fields[1] not in ("app", "loader"):
                    continue
                if ip and peer[0] != ip:
                    continue
                found[peer[0]] = (fields[1], int(fields[2], 16), int(fields[3]))
            except (socket.timeout, UnicodeError, ValueError):
                continue
    return found


def find_device(ip, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = discover(ip)
        if len(found) > 1:
            raise RuntimeError("Multiple DSis found; select one with --ip: " + ", ".join(found))
        if found:
            address, status = next(iter(found.items()))
            return address, status
    raise TimeoutError("No DSi response. Open the loader, check Wi-Fi/client isolation, or use --ip.")


def return_to_loader(address, secret, timeout):
    print(f"Returning {address} to loader…", flush=True)
    deadline = time.monotonic() + timeout
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        while time.monotonic() < deadline:
            sock.sendto(b"DSIRESET" + secret, (address, PORT))
            state = discover(address, 0.5).get(address)
            if state and state[0] == "loader":
                return
    raise TimeoutError("Return timed out. Check the token; if the game crashed, reset/reopen the loader manually.")


def receive_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("DSi disconnected before acknowledging the transfer")
        data.extend(chunk)
    return bytes(data)


def receive_ack(sock):
    magic, status, detail = ACK.unpack(receive_exact(sock, ACK.size))
    if magic != b"DSIACK1\0":
        raise RuntimeError("Unexpected deployment protocol/version")
    if status >= 100:
        raise RuntimeError(ERRORS.get(status, f"DSi error {status}"))
    if status not in (0, 1, 2):
        raise RuntimeError(f"Unknown acknowledgment {status}")
    return status, detail


@contextlib.contextmanager
def snapshot(path, operation):
    # An open immutable snapshot prevents a later rebuild changing bytes mid-upload.
    with tempfile.TemporaryFile() as image, Path(path).open("rb") as source:
        size, crc = 0, 0
        while chunk := source.read(65536):
            image.write(chunk)
            size += len(chunk)
            crc = zlib.crc32(chunk, crc)
        if not 0 < size <= LIMITS.get(operation, 32) * 1024 * 1024:
            raise ValueError(f"{Path(path).name} is empty or exceeds the loader's size limit")
        if operation in (1, 2, 6) and size < 512:
            raise ValueError(f"{Path(path).name} is not an NDS image (too small)")
        image.seek(0)
        yield image, size, crc


def transfer(address, secret, image, size, crc, operation, name=None):
    encoded = name.encode("ascii") if name else b""
    with socket.create_connection((address, PORT), timeout=20) as sock:
        sock.settimeout(20)
        sock.sendall(HEADER.pack(b"DSIUPL1\0", operation, size, crc, len(encoded), secret))
        if encoded:
            sock.sendall(encoded)
        status, detail = receive_ack(sock)
        if status == 0:
            if image is None:
                raise RuntimeError("Unexpected request for data")
            image.seek(0)
            while chunk := image.read(65536):
                sock.sendall(chunk)
            status, detail = receive_ack(sock)
            if status != 1:
                raise RuntimeError("Transfer was not committed")
        elif status != 2:
            raise RuntimeError("Unexpected initial acknowledgment")
        if operation != 3 and detail != crc:
            raise RuntimeError("DSi acknowledged a different image checksum")
        if operation in (1, 3):
            sock.sendall(b"G")  # Launch only after the Mac received the durable commit ACK.
        return detail, status == 2


def send_file(address, secret, path, operation, name=None):
    with snapshot(path, operation) as (image, size, crc):
        print(f"Deploying {Path(path).name}, {size:,} bytes, to {address} ({crc:08x})…", flush=True)
        started = time.monotonic()
        for attempt in range(2):
            try:
                crc, skipped = transfer(address, secret, image, size, crc, operation, name)
                break
            except (OSError, ConnectionError):
                if attempt:
                    raise
                # A lost final ACK may have committed the file; dedup makes retries safe.
                time.sleep(0.5)
        verb = "Already present" if skipped else "Verified and committed"
        print(f"{verb} in {time.monotonic() - started:.2f}s.", flush=True)
        return crc, skipped


def wait_started(address, crc, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = discover(address, 0.5).get(address)
        if state and state[0] == "app" and state[1] == crc:
            print(f"Application ready on {address} ({crc:08x}).", flush=True)
            return
    raise TimeoutError("Build committed and launch requested, but no application-ready response. "
                       "Check DSi screen/runtime integration; use --no-wait-start for uninstrumented homebrew.")


class Logs:
    def __init__(self, address=None):
        self.address = address
        self.stop = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", LOG_PORT))
        self.sock.settimeout(0.2)
        self.thread = threading.Thread(target=self.listen, daemon=True)

    def listen(self):
        while not self.stop.is_set():
            try:
                data, peer = self.sock.recvfrom(1024)
                if (self.address is None or peer[0] == self.address) and data.startswith(b"DSILOG1 "):
                    # Do not interpret terminal escapes arriving over the network.
                    line = data.decode("utf-8", errors="replace")
                    line = "".join(c if c.isprintable() or c in "\n\t" else "?" for c in line)
                    print(f"[{peer[0]}] {line.rstrip()}", flush=True)
            except socket.timeout:
                pass
            except OSError:
                break

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(timeout=1)
        self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["discover", "run", "store", "launch", "reset", "asset",
                                            "put", "update-loader", "logs", "stage", "token"])
    parser.add_argument("file", nargs="?")
    parser.add_argument("--ip", help="DSi IPv4 address; bypass broadcast discovery")
    parser.add_argument("--token", type=Path, default=HERE / "token.bin")
    parser.add_argument("--header", type=Path, help="token: write the secret as a C header for the loader")
    parser.add_argument("--out", type=Path, help="stage: directory to write the SD payload into")
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--no-wait-start", action="store_true")
    parser.add_argument("--logs", action="store_true", help="listen before launch; remain attached until Ctrl-C")
    parser.add_argument("--name", help="put: name to store the file under (default: its own)")
    parser.add_argument("--data", type=Path, action="append", default=[],
                        help="run: also push this file beside the build; repeatable")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.command == "discover":
        for ip, (mode, crc, generation) in discover(args.ip, 2).items():
            print(f"{ip} {mode} {crc:08x} generation {generation}")
        return
    if args.command == "logs":
        with Logs(args.ip):
            print(f"Listening on UDP {LOG_PORT}; Ctrl-C to detach.", flush=True)
            while True:
                time.sleep(0.5)
    if args.command == "token":
        secret = token(args.token, create=True)
        if args.header:
            write_token_header(args.header, secret)
        return
    if args.command == "stage":
        if not args.file:
            parser.error("stage requires the built loader.nds path")
        output = args.out or HERE / ".build" / "sd"
        output.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.file, output / "dsidev.nds")
        print(f"Copy {output / 'dsidev.nds'} to the SD card root (one file, nothing else).")
        return
    if args.command in ("run", "store", "asset", "put", "update-loader") and not args.file:
        parser.error(f"{args.command} requires a file")
    if args.name and args.command != "put":
        parser.error("--name applies to put")
    if args.data and args.command != "run":
        parser.error("--data applies to run")
    names = {}
    try:
        if args.command == "put":
            names[Path(args.file)] = check_name(args.name or Path(args.file).name)
        for data in args.data:
            names[data] = check_name(data.name)
    except ValueError as exc:
        parser.error(str(exc))
    secret = token(args.token)
    (HERE / ".build").mkdir(exist_ok=True)
    with (HERE / ".build" / "deploy.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another deployment is already running")
        print("Looking for DSi…", flush=True)
        address, state = find_device(args.ip, args.timeout)
        if args.command == "reset":
            return_to_loader(address, secret, args.timeout)
            return
        logger = Logs(address) if args.logs else contextlib.nullcontext()
        with logger:
            if state[0] == "app" and args.command != "asset":
                return_to_loader(address, secret, args.timeout)
            operation = OPERATIONS[args.command]
            # Data files land before the launch, while the loader still owns the card.
            for data in args.data:
                send_file(address, secret, data, OPERATIONS["put"], names[data])
            if args.command == "launch":
                started = time.monotonic()
                crc, skipped = transfer(address, secret, None, 0, 0, operation)
                print(f"Launching stored build in {time.monotonic()-started:.2f}s.", flush=True)
            else:
                crc, skipped = send_file(address, secret, args.file, operation, names.get(Path(args.file)))
            if operation in (1, 3) and not args.no_wait_start:
                wait_started(address, crc, args.timeout)
            if args.logs:
                # Allow another terminal to deploy while this one continues logging.
                fcntl.flock(lock, fcntl.LOCK_UN)
                print("Logs attached; Ctrl-C leaves the game running.", flush=True)
                while True:
                    time.sleep(0.5)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nDetached.")
    except (OSError, ValueError, RuntimeError) as exc:
        sys.exit(f"dsidev: {exc}")
