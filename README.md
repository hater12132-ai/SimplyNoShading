# BactroNative

LeviLauncher PL module for Minecraft Bedrock **1.26.51.1**.

## Modules
- **Performance** — VSync unlock, fullbright
- **Hand Shader** — first-person hand/item additive glow (budgeted so world draws are not affected), hide hand

TargetHUD and Fast Containers were removed.

## Build
GitHub Actions / local NDK:
```
xmake f -p android -a arm64-v8a -m release --ndk=<path>
xmake
```
Produces `BactroNative.levipack`.

## Version
1.5.6 — hand draw budget (max 6/frame), tighter mesh size filter (`< 1500`), fixed levipack manifest.
