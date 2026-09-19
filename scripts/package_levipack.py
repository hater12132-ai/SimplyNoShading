#!/usr/bin/env python3
import argparse, json, sys, zipfile
from pathlib import Path

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--icon", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    lib, icon, out = args.library.resolve(), args.icon.resolve(), args.output.resolve()
    if not lib.is_file():
        print(f"missing library {lib}", file=sys.stderr); return 1
    if not icon.is_file():
        print(f"missing icon {icon}", file=sys.stderr); return 1
    manifest = {
        "type": "preload-native",
        "name": "ShadeFix",
        "author": "hater12132-ai",
        "version": "1.1.0",
        "entry": "libShadeFix.so",
        "icon": "icon.png",
        "minecraft_versions": ["1.26.5X.X"],
        "description": "Softens block-face shading without removing grass/leaf/water tint.",
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists(): out.unlink()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        z.write(lib, "libShadeFix.so")
        z.write(icon, "icon.png")
    print(out)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
