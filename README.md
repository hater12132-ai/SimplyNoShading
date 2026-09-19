# BactroNative

LeviLauncher native mod for Minecraft Bedrock **1.26.50 / 1.26.51**.

## Modules

### Performance
- **Unlock FPS** — disables VSync so 120Hz tablets can run above 60
- **Target FPS** — soft cap (default 120). If a frame is late (chunk hitch), no extra sleep
- **Fullbright** — 0 = normal, 10 = max light
- Real FPS is measured from `eglSwapBuffers` (present first, then pace) so counters should not stick at 0

### Fast Containers
- Tracks chest/shulker (container screen) open/close
- Lets you open the next container immediately after closing the previous one
- Works for chests, shulkers, barrels, hoppers, etc. (any ContainerScreen)

## Build
Target: `BactroNative` → `BactroNative.levipack` / `libBactroNative.so`
