# Optimization (Performance)

Standalone LeviLauncher mod for Minecraft Bedrock **1.26.50 / 1.26.51**.

## Module: Performance

| Option | Default | What it does |
|--------|---------|----------------|
| Unlock FPS | ON | `eglSwapInterval(0)` — allows 120Hz on capable tablets |
| Target FPS | 120 | Soft frame pacing. **0** = uncapped. If a frame is late (chunk hitch), sleep is skipped so you recover faster |
| Fullbright | 0 | **0** = normal · **10** = max light (same style as BedrockTools) |

## Chunk freezes — honest limits

Short freezes when **new chunks mesh** are mostly **engine main-thread work**. A small native mod cannot fully remove that without deep chunk-builder hooks.

What actually helps on Snapdragon 7s Gen 4:

1. Lower **Render Distance** (biggest win for hitch length)
2. Lower **Simulation Distance** if available
3. This mod: 120Hz unlock + no extra delay after a hitch
4. **SimplyNoShading.mcpack** — less shading cost, flatter look, good FPS

## Build

Target name: `Optimization` → `Optimization.levipack` / `libOptimization.so`
