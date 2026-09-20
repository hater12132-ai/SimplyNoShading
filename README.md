# BactroNative 1.2.1

## Full signature table
All **BedrockTools** `SignatureId` patterns for **1.26.50 / 1.26.51** are embedded.

```cpp
#include "bactro/Signatures.hpp"
bactro::memory::resolveAll();                    // once, prefer background thread
auto addr = bactro::memory::resolve(SignatureId::GameModeUseItemOn);
bactro::memory::hook(SignatureId::..., detour, &original);
```

Resolved **once in the background** on enable (no main-thread ANR).

## Modules
- **Performance** — VSync unlock + Fullbright 0–10
- **Fast Containers** — after closing a chest/shulker, next open forces `firstEvent` and retries once (client-side; works on multiplayer as far as the client can; server may still add a tick of latency)

## Multiplayer note
The server still authorizes container opens. This mod removes **client** wait/fail between hops; it cannot remove network RTT.
