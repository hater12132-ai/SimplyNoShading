# BactroNative 1.4.0

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

## Target HUD (1.4.0)
Network-layer, ProtoHax-style (verified against libminecraftpe 1.26.51.1):
- **Who you hit**: the outgoing batch (`CompressedNetworkPeer::sendPacket`, id `NetworkPeerReceive` — legacy name) is scanned for an
  `InventoryTransaction` (id 30) of type ItemUseOnEntity / action Attack → exact target runtime id.
- **Name + health**: the incoming batch (`CompressedNetworkPeer::receivePacket`, id `CompressedPeerReceive`) is scanned for
  `AddPlayer` (12, names), `StartGame` (11, own id), `UpdateAttributes` (29, health).
- The tiny `GameMode::attack` stubs are never hooked (that crashed the game). Optional debug toggle hooks the real body
  (`GameModeAttackInternal` @ 0xf89f6c0) and only logs.

Debug: `cat /sdcard/Android/media/org.levimc.launcher/bactro_status.txt` — look for `peer SEND/RECV hook`, `out: InvTx … hit=1`,
`in: receive`, `pkt AddPlayer`, `TargetHUD: hurt runtime=… -> card`.
