#!/usr/bin/env python3
"""Fetch reviewed chainloader sources at immutable commits; prepare the return-path patch."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
PINS = {
    "nds-hb-menu": "3c1f64c12da157fdd14a2d594db139f50e283261",
    "nds-bootloader": "a0f819008a63f28c4994e7cc8a8ace892450bbb5",
}


def run(*args):
    subprocess.run(args, check=True)


def fetch():
    deps = ROOT / ".deps"
    deps.mkdir(exist_ok=True)
    for name, revision in PINS.items():
        path = deps / name
        if not path.exists():
            run("git", "init", "-q", str(path))
            run("git", "-C", str(path), "fetch", "-q", "--depth=1", f"https://github.com/devkitPro/{name}.git", revision)
            run("git", "-C", str(path), "checkout", "-q", "--detach", "FETCH_HEAD")
        actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
        dirty = subprocess.check_output(["git", "-C", str(path), "diff", "--name-only", "HEAD"], text=True)
        if actual != revision or dirty:
            raise RuntimeError(f"Unexpected/modified dependency {path}; preserve it and resolve manually")
    source = (deps / "nds-hb-menu/source/nds_loader_arm9.c").read_text()
    anchor = "static bool dldiPatchLoader(BootLdrHeader* loader)"
    addition = '''// DSIDEV modification: return to our immutable loader file's cluster, not BOOT.NDS.
static u32 devReturnCluster;
bool devSetReturnPath(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0 || st.st_ino < 2) return false;
    devReturnCluster = st.st_ino;
    return true;
}

'''
    assert source.count(anchor) == 1
    source = source.replace(anchor, addition + anchor)
    anchor = "bootloader->isTwlMode = systemIsTwlMode();"
    assert source.count(anchor) == 1
    source = source.replace(anchor, anchor + "\n\tbootloader->storedFileCluster = devReturnCluster;")
    anchor = "\tinstallBootStub(havedsiSD);"
    assert source.count(anchor) == 1
    source = source.replace(anchor, "\tif (!devReturnCluster || !installBootStub(havedsiSD)) return RUN_NDS_STAT_FAILED;")
    output = ROOT / ".build" / "generated"
    output.mkdir(parents=True, exist_ok=True)
    for name, content in {
        "nds_loader_arm9.c": source,
        "nds_loader_arm9.h": (deps / "nds-hb-menu/source/nds_loader_arm9.h").read_text(),
    }.items():
        target = output / name
        if not target.exists() or target.read_text() != content:
            target.write_text(content)
    print("Pinned hbmenu/bootloader sources ready; return path patch applied to generated copy.")


if __name__ == "__main__":
    fetch()
