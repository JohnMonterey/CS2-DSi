"""Runner for melonDS UI tests: scripted input, checkpoint captures, reference diffs.

A test is a Python module under tests/ui/ that defines:

    NAME = "boot to main menu"
    TOLERANCE = {"max_mean": 2.0, "max_changed": 0.01}   # optional
    def run(emu, shot):
        emu.wait(4)
        shot("title")
        emu.press("Start")
        emu.wait(2)
        shot("main_menu")

`shot(label)` captures both DS screens, normalised to 256x384, and compares them with
tests/ui/refs/<module>/<label>.bmp. Actual captures, viewable PNGs and diff images are
written to tests/ui/.out/<module>/ whether the test passes or fails.

Usage:
    python3 -m emu.uitest --rom path/to.nds              run every UI test
    python3 -m emu.uitest --rom path/to.nds boot_menu    run one
    python3 -m emu.uitest --rom path/to.nds --update     rewrite the reference images
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import sys
import tempfile
import traceback

from . import image as imglib
from . import mac
from .melonds import DS_TOTAL_HEIGHT, DS_WIDTH, Emulator, MelonDSError

DEFAULT_TOLERANCE = {"max_mean": 2.0, "max_changed": 0.01, "threshold": 12}


class Mismatch(Exception):
    pass


class Shooter:
    """Captures checkpoints and compares them with the stored references."""

    def __init__(self, emu, refs_dir, out_dir, tolerance, update):
        self.emu = emu
        self.refs_dir = refs_dir
        self.out_dir = out_dir
        self.tolerance = tolerance
        self.update = update
        self.results = []
        os.makedirs(out_dir, exist_ok=True)
        if update:
            os.makedirs(refs_dir, exist_ok=True)

    def __call__(self, label: str):
        png = os.path.join(self.out_dir, "%s.png" % label)
        bmp = os.path.join(self.out_dir, "%s.bmp" % label)
        self.emu.focus()
        mac.capture_region(self.emu.content_rect(), png)
        mac.to_bmp(png, bmp, DS_WIDTH, DS_TOTAL_HEIGHT)
        actual = imglib.read_bmp(bmp)

        ref_path = os.path.join(self.refs_dir, "%s.bmp" % label)
        if self.update:
            shutil.copyfile(bmp, ref_path)
            self.results.append((label, "updated", None))
            return
        if not os.path.exists(ref_path):
            self.results.append((label, "no reference", None))
            raise Mismatch(
                "no reference for %r. Check %s looks right, then re-run with --update."
                % (label, png)
            )
        expected = imglib.read_bmp(ref_path)
        result = imglib.diff(
            expected, actual,
            threshold=self.tolerance["threshold"],
            diff_path=os.path.join(self.out_dir, "%s.diff.bmp" % label),
        )
        ok = result.passed(self.tolerance["max_mean"], self.tolerance["max_changed"])
        self.results.append((label, "ok" if ok else "MISMATCH", result))
        if not ok:
            raise Mismatch(
                "%s differs from its reference: mean %.2f (limit %.2f), %.2f%% of pixels "
                "changed (limit %.2f%%). See %s"
                % (label, result.mean_abs, self.tolerance["max_mean"],
                   result.changed_fraction * 100, self.tolerance["max_changed"] * 100,
                   os.path.join(self.out_dir, "%s.diff.bmp" % label))
            )


def load_test(path: str):
    name = os.path.splitext(os.path.basename(path))[0]
    spec = importlib.util.spec_from_file_location("uitest_%s" % name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not hasattr(module, "run"):
        raise MelonDSError("%s defines no run(emu, shot)" % path)
    module.__test_name__ = name
    return module


def discover(tests_dir: str, only):
    found = []
    for entry in sorted(os.listdir(tests_dir)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        name = entry[:-3]
        if only and name not in only:
            continue
        found.append(os.path.join(tests_dir, entry))
    return found


def run_one(path, rom, tests_dir, update, scale, boot_seconds):
    module = load_test(path)
    name = module.__test_name__
    tolerance = dict(DEFAULT_TOLERANCE)
    tolerance.update(getattr(module, "TOLERANCE", {}))
    refs_dir = os.path.join(tests_dir, "refs", name)
    out_dir = os.path.join(tests_dir, ".out", name)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)

    title = getattr(module, "NAME", name)
    print("\n=== %s (%s) ===" % (name, title))
    emu = Emulator(rom, scale=scale, boot_seconds=boot_seconds)
    shot = None
    try:
        emu.start()
        shot = Shooter(emu, refs_dir, out_dir, tolerance, update)
        module.run(emu, shot)
        status = "PASS"
        detail = ""
    except Mismatch as exc:
        status = "FAIL"
        detail = str(exc)
    except Exception as exc:  # noqa: BLE001 - report, do not abort the whole run
        status = "ERROR"
        detail = "%s: %s" % (type(exc).__name__, exc)
        traceback.print_exc()
    finally:
        emu.stop()

    if shot:
        for label, outcome, result in shot.results:
            if result is None:
                print("  %-22s %s" % (label, outcome))
            else:
                print("  %-22s %-9s mean %.2f  changed %.3f%%"
                      % (label, outcome, result.mean_abs, result.changed_fraction * 100))
    print("  -> %s%s" % (status, (": " + detail) if detail else ""))
    return status == "PASS", name, detail


def main(argv=None):
    parser = argparse.ArgumentParser(description="melonDS UI tests")
    parser.add_argument("tests", nargs="*", help="test module names; default is all")
    parser.add_argument("--rom", required=True, help=".nds to load")
    parser.add_argument("--tests-dir", default=None)
    parser.add_argument("--update", action="store_true",
                        help="rewrite reference images instead of comparing")
    parser.add_argument("--scale", type=int, default=2,
                        help="window scale; 2 gives a 512x768 window")
    parser.add_argument("--boot-seconds", type=float, default=6.0,
                        help="wait after launch before a test's first step")
    args = parser.parse_args(argv)

    tests_dir = args.tests_dir or os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "tests", "ui",
    )
    if not os.path.isdir(tests_dir):
        print("no UI tests directory at %s" % tests_dir, file=sys.stderr)
        return 2
    paths = discover(tests_dir, set(args.tests))
    if not paths:
        print("no UI tests matched", file=sys.stderr)
        return 2

    failures = []
    for path in paths:
        ok, name, detail = run_one(
            path, args.rom, tests_dir, args.update, args.scale, args.boot_seconds
        )
        if not ok:
            failures.append((name, detail))

    print("\n%d test(s), %d failed" % (len(paths), len(failures)))
    for name, detail in failures:
        print("  %s: %s" % (name, detail.splitlines()[0] if detail else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
