#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace bactro::health {

struct EntityHealth {
    float current = 20.f;
    float max = 20.f;
    float absorption = 0.f;
    bool valid = false;
};

// ProtoHax-style: filled from UpdateAttributesPacket (id 29) payloads.
void onRawGamePacket(const uint8_t* data, size_t size);
void setHealth(uint64_t runtimeId, float current, float max, float absorption);
std::optional<EntityHealth> get(uint64_t runtimeId);
// Most recent UpdateAttributes health (for binding target after a hit in 1v1).
std::optional<std::pair<uint64_t, EntityHealth>> lastUpdate();
void clear();

// Packet-derived world knowledge (no native game calls needed):
//  - StartGame(11)  -> own runtime id
//  - AddPlayer(12)  -> runtimeId -> username
//  - ActorEvent(27) hurt / UpdateAttributes health drop -> "hurt" queue
std::string playerName(uint64_t runtimeId);   // "" if unknown (not a player we saw spawn)
uint64_t selfRuntimeId();                     // 0 if unknown
bool popHurt(uint64_t& runtimeId);            // FIFO of entities that just took damage

} // namespace bactro::health
