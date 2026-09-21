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
    bool fromPacket = false; // true if current came from real UA (current < max or abs change)
};

// ProtoHax-style: filled from UpdateAttributesPacket (id 29) payloads.
void onRawGamePacket(const uint8_t* data, size_t size);
void onOutgoingBatch(const uint8_t* data, size_t size);
bool popMyHit(uint64_t& runtimeId);
void setHealth(uint64_t runtimeId, float current, float max, float absorption);
std::optional<EntityHealth> get(uint64_t runtimeId);
std::optional<std::pair<uint64_t, EntityHealth>> lastUpdate();
void clear();

size_t playerCount();
std::string playerName(uint64_t runtimeId);
uint64_t selfRuntimeId();
bool popHurt(uint64_t& runtimeId);

// ---- Solstice-style prediction (when server keeps UA at 20/20 for others) ----
// Call when WE swing at someone (native attack or outbound hit).
void noteOutgoingSwing(uint64_t runtimeIdHint = 0);
// Call when ActorEvent hurt fires for rid. Applies pending damage once.
void noteHurt(uint64_t runtimeId);
// Natural regen tick (call from onFrame).
void tickPrediction();
// Best HP for display: real packet if trusted, else predicted.
EntityHealth displayHealth(uint64_t runtimeId);

} // namespace bactro::health
