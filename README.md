# BactroNative 1.1.0

## Fast Containers (real)
Uses **BedrockTools** signatures and calling conventions:
- `ContainerScreenControllerOpen` / `Dtor` (8-arg ScreenFn)
- `GameModeUseItemOn` / `SurvivalModeUseItemOn` (`InteractionResultValue(…)`)
- `GameModeInteract` / `SurvivalModeInteract` (`bool(…)`)

Resolved in **one background batch** (no main-thread multi-scan ANR).

After you close a chest/shulker, the next use/interact is retried once if it fails
(within 750ms) so you can hop containers faster.

## Performance
- VSync unlock via `eglSwapInterval(0)`
- Fullbright 0–10 (async signature resolve)

## Note
If your build is 1.26.51.x and some signatures miss, logcat will show
`Fast Containers hooks installed: N/6`. Need matching patterns for that build.
