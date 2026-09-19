# BactroNative 1.0.1

## Why 1.0.0 ANR’d
On enable it ran **several full `libminecraftpe.so` signature scans on the main thread**
(~1.5s each). That blocked the UI long enough for Android to fire an **ANR**.
`ContainerOpen` was also hooked with an incorrect ABI.

## 1.0.1 (safe)
- **Performance**: `eglSwapInterval(0)` only — **no** `nanosleep` on present
- **Fullbright**: resolved **on a background thread**
- **Fast Containers**: menu stub only (default OFF) until a verified, non-blocking approach exists

## Modules
| Module | Default | Notes |
|--------|---------|--------|
| Performance | ON | Unlock FPS + Fullbright 0–10 |
| Fast Containers | OFF | Placeholder — does not hook game code |
