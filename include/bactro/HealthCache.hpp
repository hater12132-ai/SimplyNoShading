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

} // namespace bactro::health
