# ShadeFix (standalone)

LeviLauncher native mod for Minecraft Bedrock **1.26.50 / 1.26.51**.

Softens dark **block-face shading** using the six `BlockTessellatorTessellateFace*` hooks
plus `TessellatorColor`. **Does not force white** — grass / leaves / water tint is preserved.

## Install
1. Build on CI or locally (`xmake f -p android -a arm64-v8a -m release && xmake`).
2. Install `ShadeFix.levipack` in LeviLauncher.
3. Enable **Shade Fix** in the mod menu. Reload chunks (relog / move) if needed.

## Not part of BedrockTools / BedrockToolsPlus
This is a separate preload-native `.so` / `.levipack`.
