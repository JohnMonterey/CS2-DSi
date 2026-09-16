#!/usr/bin/env python3
"""Read-only checks for the native macOS build environment."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

dkp = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
arm = Path(os.environ.get("DEVKITARM", str(dkp / "devkitARM")))
checks = [arm / "bin/arm-none-eabi-gcc", arm / "ds_rules", dkp / "libnds/include/nds.h",
          dkp / "calico/include/calico.h", dkp / "calico/bin/ds7_maine.elf",
          dkp / "libnds/lib/libdswifi9.a", dkp / "libnds/lib/libfat.a"]
print(f"Host: {platform.system()} {platform.machine()}, Python {platform.python_version()}")
missing = []
for path in checks:
    present = path.exists()
    print(f"{'OK' if present else 'MISSING'} {path}")
    if not present:
        missing.append(path)
for name in ("ndstool", "bin2s", "mmutil"):
    found = shutil.which(name) or (dkp / "tools/bin" / name if (dkp / "tools/bin" / name).exists() else None)
    print(f"{'OK' if found else 'MISSING'} {name}: {found or ''}")
    if not found:
        missing.append(name)
if checks[0].exists():
    subprocess.run([str(checks[0]), "--version"], check=True)
# Same search order as Counter-Strike-nds/Makefile: devkitPro tree, then a checkout
# beside the repository, then an explicit NITRO_ENGINE override.
repo = Path(__file__).resolve().parents[2]
candidates = [Path(os.environ["NITRO_ENGINE"])] if os.environ.get("NITRO_ENGINE") else []
candidates += [dkp / "nitro-engine", repo.parent / "nitro-engine"]
engine = next((c for c in candidates if (c / "lib/libNE.a").exists()), None)
if engine:
    print(f"OK Nitro Engine: {engine}/lib/libNE.a")
else:
    found = next((c for c in candidates if c.exists()), None)
    print(f"Game dependency: Nitro Engine not built. {'Run: make -C ' + str(found) if found else 'Clone https://github.com/Fewnity/nitro-engine and run make in it.'}")
    print("  The standalone loader/sample do not need it.")
if missing:
    print("Install/update devkitPro pacman and the nds-dev group; see tools/dsi/README.md.")
    sys.exit(1)
print("SDK files found. Build the loader/sample, then validate on the physical DSi.")
