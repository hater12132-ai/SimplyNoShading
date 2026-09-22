# BactroNative 1.6.0

Minecraft Bedrock **1.26.51.1** Levi PL module.

## Modules
- **Performance** — VSync unlock, fullbright
- **Hand Shader** — hide first-person hand/item only
- **Motion Blur** — Natural Motion Blur–style **temporal frame blending** (screen post-process)

## Motion Blur notes
Inspired by [Natural Motion Blur](https://modrinth.com/mod/natural-motion-blur) (Java) frame-blending mode and
[CrackedMatter mcpelauncher-motion-blur](https://github.com/CrackedMatter/mcpelauncher-motion-blur).

Not a full velocity-buffer implementation (Bedrock does not expose that the same way as Java post chains).
Uses history-buffer accumulation with strength + FPS scaling for a similar smooth-camera look.

## Build
```
xmake f -p android -a arm64-v8a -m release --ndk=<path>
xmake
```
