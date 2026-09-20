#include "bactro/HealthCache.hpp"

#include <android/log.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#define HC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::health {
namespace {

std::mutex g_mu;
std::unordered_map<uint64_t, EntityHealth> g_map;
uint64_t g_lastRuntimeId = 0;
EntityHealth g_lastHealth{};
bool g_hasLast = false;

// ---- Bedrock binary readers (little-endian + unsigned varints) ----
struct Reader {
    const uint8_t* p{};
    const uint8_t* end{};

    bool ok() const { return p < end; }
    size_t left() const { return static_cast<size_t>(end - p); }

    bool readU8(uint8_t& o) {
        if (p >= end) return false;
        o = *p++;
        return true;
    }
    bool readF32(float& o) {
        if (left() < 4) return false;
        std::memcpy(&o, p, 4);
        p += 4;
        return true;
    }
    bool readVarU32(uint32_t& o) {
        o = 0;
        int shift = 0;
        for (int i = 0; i < 5; ++i) {
            uint8_t b;
            if (!readU8(b)) return false;
            o |= static_cast<uint32_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }
    bool readVarU64(uint64_t& o) {
        o = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            uint8_t b;
            if (!readU8(b)) return false;
            o |= static_cast<uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }
    bool readString(std::string& o) {
        uint32_t len = 0;
        if (!readVarU32(len)) return false;
        if (len > left() || len > 512) return false;
        o.assign(reinterpret_cast<const char*>(p), len);
        p += len;
        return true;
    }
};

// Game packet header: varuint32 = packetId | (subClientSender<<10) | (subClientTarget<<12)
bool readHeader(Reader& r, uint32_t& packetId) {
    uint32_t header = 0;
    if (!r.readVarU32(header)) return false;
    packetId = header & 0x3FF; // 10 bits
    return true;
}

// AttributeData (1.26.x / Endstone r26_u5):
// min, max, current, defaultMin, defaultMax, default, name, modifiers[]
bool parseAttribute(Reader& r, std::string& name, float& minV, float& maxV, float& cur) {
    float dMin, dMax, def;
    if (!r.readF32(minV) || !r.readF32(maxV) || !r.readF32(cur)) return false;
    if (!r.readF32(dMin) || !r.readF32(dMax) || !r.readF32(def)) return false;
    if (!r.readString(name)) return false;
    uint32_t modCount = 0;
    if (!r.readVarU32(modCount)) return false;
    // skip modifiers: string id, string name, f32 amount, i32 op, i32 operand, bool
    for (uint32_t i = 0; i < modCount && i < 64; ++i) {
        std::string id, n;
        float amount;
        // i32 as 4 bytes LE
        if (!r.readString(id) || !r.readString(n) || !r.readF32(amount)) return false;
        if (r.left() < 4 + 4 + 1) return false;
        r.p += 4 + 4; // op + operand
        uint8_t ser;
        if (!r.readU8(ser)) return false;
    }
    return true;
}

void parseUpdateAttributesPayload(Reader& r) {
    // After packet header already consumed
    uint64_t runtimeId = 0;
    if (!r.readVarU64(runtimeId)) return;

    uint32_t count = 0;
    if (!r.readVarU32(count) || count > 64) return;

    float healthCur = -1.f, healthMax = -1.f, absorp = -1.f;
    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        float mn, mx, cur;
        if (!parseAttribute(r, name, mn, mx, cur)) break;
        if (name == "minecraft:health" || name == "health") {
            healthCur = cur;
            healthMax = mx;
        } else if (name == "minecraft:absorption" || name == "absorption") {
            absorp = cur;
        }
    }
    if (healthCur < 0.f && absorp < 0.f) return;

    EntityHealth h{};
    {
        std::lock_guard lock(g_mu);
        auto it = g_map.find(runtimeId);
        if (it != g_map.end()) h = it->second;
    }
    if (healthCur >= 0.f) {
        h.current = healthCur;
        if (healthMax > 0.f) h.max = healthMax;
        h.valid = true;
    }
    if (absorp >= 0.f) h.absorption = absorp;
    setHealth(runtimeId, h.current, h.max, h.absorption);
}

// Batch / single packet: may be one game packet or a batch of them
void tryParseOne(const uint8_t* data, size_t size) {
    if (!data || size < 2 || size > 1 << 20) return;
    Reader r{data, data + size};
    uint32_t packetId = 0;
    if (!readHeader(r, packetId)) return;
    // UpdateAttributesPacket = 29 (0x1D)
    if (packetId == 29) {
        parseUpdateAttributesPayload(r);
    }
}

} // namespace

void onRawGamePacket(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    // Some peers deliver a length-prefixed batch; try direct first, then scan
    tryParseOne(data, size);

    // Also scan for embedded packet id 29 varint patterns in larger buffers
    // (batched payloads sometimes wrap multiple packets)
    if (size > 16) {
        for (size_t i = 0; i + 8 < size && i < size - 8; ++i) {
            // cheap filter: first byte of varint packet header often 0x1d or with flags
            const uint8_t b = data[i];
            if ((b & 0x3F) == 29 || b == 29) {
                tryParseOne(data + i, size - i);
            }
        }
    }
}

void setHealth(uint64_t runtimeId, float current, float max, float absorption) {
    std::lock_guard lock(g_mu);
    auto& e = g_map[runtimeId];
    e.current = current;
    e.max = max > 0.f ? max : (e.max > 0.f ? e.max : 20.f);
    e.absorption = absorption;
    e.valid = true;
    g_lastRuntimeId = runtimeId;
    g_lastHealth = e;
    g_hasLast = true;
    static int s_log;
    if ((++s_log % 8) == 1)
        HC_LOGI("packet HP runtime=%llu cur=%.1f max=%.1f abs=%.1f",
                (unsigned long long)runtimeId, current, e.max, absorption);
}

std::optional<EntityHealth> get(uint64_t runtimeId) {
    std::lock_guard lock(g_mu);
    auto it = g_map.find(runtimeId);
    if (it == g_map.end() || !it->second.valid) return std::nullopt;
    return it->second;
}

std::optional<std::pair<uint64_t, EntityHealth>> lastUpdate() {
    std::lock_guard lock(g_mu);
    if (!g_hasLast) return std::nullopt;
    return std::make_pair(g_lastRuntimeId, g_lastHealth);
}

void clear() {
    std::lock_guard lock(g_mu);
    g_map.clear();
    g_hasLast = false;
}

} // namespace bactro::health
