#!/usr/bin/env python3
import argparse, json, sys, zipfile
from pathlib import Path

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--icon", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    lib, icon, out = a.library.resolve(), a.icon.resolve(), a.output.resolve()
    if not lib.is_file() or not icon.is_file():
        print("missing library or icon", file=sys.stderr)
        return 1
    manifest = {
        "type": "preload-native",
        "name": "BactroNative",
        "author": "hater12132-ai",
        "version": "1.0.1",
        "entry": "libBactroNative.so",
        "icon": "icon.png",
        "minecraft_versions": ["1.26.5X.X"],
        "description": "Performance (real FPS unlock for 120Hz) + Fast Containers (chests/shulkers).",
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        z.write(lib, "libBactroNative.so")
        z.write(icon, "icon.png")
    print(out)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
