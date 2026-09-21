#include "bactro/HealthCache.hpp"
#include "bactro/Status.hpp"

#include <android/log.h>

#include <atomic>
#include <cstdio>
#include <bitset>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>

#define HC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::health {
namespace {

std::mutex g_mu;
std::unordered_map<uint64_t, EntityHealth> g_map;
uint64_t g_lastRuntimeId = 0;
EntityHealth g_lastHealth{};
bool g_hasLast = false;
std::unordered_map<uint64_t, std::string> g_names;
uint64_t g_self = 0;
std::vector<uint64_t> g_hurt;
std::vector<uint64_t> g_myHit;
int g_seenMask = 0; // which packet ids we already logged once

// Solstice-style predicted HP when server never sends real remote HP
struct Pred {
    float health = 20.f;
    float maxHealth = 20.f;
    float absorption = 0.f;
    float lastAbsorption = 0.f;
    float pendingDamage = 0.f; // damage to apply on next confirmed hurt
    bool trusted = false;      // true once real UA (current < max) seen
    std::chrono::steady_clock::time_point lastHurt{};
    std::chrono::steady_clock::time_point lastHeal{};
};
std::unordered_map<uint64_t, Pred> g_pred;
uint64_t g_lastSwingTarget = 0;
std::chrono::steady_clock::time_point g_lastSwingTime{};

void logOnce(int bit, const char* what) {
    {
        std::lock_guard lock(g_mu);
        if (g_seenMask & (1 << bit)) return;
        g_seenMask |= (1 << bit);
    }
    bactro::statusLine(what);
}

void pushMyHit(uint64_t id) {
    std::lock_guard lock(g_mu);
    if (g_myHit.size() < 32) g_myHit.push_back(id);
}

void pushHurt(uint64_t id) {
    std::lock_guard lock(g_mu);
    if (g_hurt.size() < 32) g_hurt.push_back(id);
}

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

// Skip AttributeModifier list (shared by all layouts).
bool skipModifiers(Reader& r) {
    uint32_t modCount = 0;
    if (!r.readVarU32(modCount)) return false;
    for (uint32_t i = 0; i < modCount && i < 64; ++i) {
        std::string id, n;
        float amount;
        if (!r.readString(id) || !r.readString(n) || !r.readF32(amount)) return false;
        if (r.left() < 4 + 4 + 1) return false;
        r.p += 4 + 4; // op + operand
        uint8_t ser;
        if (!r.readU8(ser)) return false;
    }
    return true;
}

// Modern Bedrock (1.19+ / 1.26): name, min, max, current, default, modifiers[]
bool parseAttributeNameFirst(Reader& r, std::string& name, float& minV, float& maxV, float& cur) {
    float def;
    if (!r.readString(name)) return false;
    if (!r.readF32(minV) || !r.readF32(maxV) || !r.readF32(cur) || !r.readF32(def)) return false;
    return skipModifiers(r);
}

// Older: min, max, current, default, name, modifiers[]
bool parseAttributeFloat4(Reader& r, std::string& name, float& minV, float& maxV, float& cur) {
    float def;
    if (!r.readF32(minV) || !r.readF32(maxV) || !r.readF32(cur) || !r.readF32(def)) return false;
    if (!r.readString(name)) return false;
    return skipModifiers(r);
}

// Endstone-style: min, max, current, defaultMin, defaultMax, default, name, modifiers[]
bool parseAttributeFloat6(Reader& r, std::string& name, float& minV, float& maxV, float& cur) {
    float dMin, dMax, def;
    if (!r.readF32(minV) || !r.readF32(maxV) || !r.readF32(cur)) return false;
    if (!r.readF32(dMin) || !r.readF32(dMax) || !r.readF32(def)) return false;
    if (!r.readString(name)) return false;
    return skipModifiers(r);
}

using AttrParser = bool (*)(Reader&, std::string&, float&, float&, float&);

bool tryParseAttributes(Reader base, AttrParser parser, float& healthCur, float& healthMax, float& absorp,
                        int& parsed, std::string& firstName) {
    uint32_t count = 0;
    if (!base.readVarU32(count) || count == 0 || count > 64) return false;
    healthCur = healthMax = absorp = -1.f;
    parsed = 0;
    firstName.clear();
    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        float mn, mx, cur;
        if (!parser(base, name, mn, mx, cur)) return false;
        if (firstName.empty()) firstName = name;
        ++parsed;
        if (name == "minecraft:health" || name == "health") {
            healthCur = cur;
            healthMax = mx;
        } else if (name == "minecraft:absorption" || name == "absorption" ||
                   name == "minecraft:player.absorption") {
            absorp = cur;
        }
    }
    return healthCur >= 0.f || absorp >= 0.f || parsed > 0;
}

void parseUpdateAttributesPayload(Reader& r) {
    // After packet header already consumed
    uint64_t runtimeId = 0;
    if (!r.readVarU64(runtimeId)) return;

    // Snapshot so we can retry alternate layouts
    const uint8_t* save = r.p;
    const size_t saveLeft = r.left();

    float healthCur = -1.f, healthMax = -1.f, absorp = -1.f;
    int parsed = 0;
    std::string firstName;
    const char* layout = "none";

    struct Try {
        AttrParser fn;
        const char* name;
    };
    const Try tries[] = {
        {&parseAttributeNameFirst, "name-first"},
        {&parseAttributeFloat4, "float4"},
        {&parseAttributeFloat6, "float6"},
    };
    for (const auto& t : tries) {
        Reader trial{save, save + saveLeft};
        float hc = -1.f, hm = -1.f, ab = -1.f;
        int n = 0;
        std::string fn;
        if (tryParseAttributes(trial, t.fn, hc, hm, ab, n, fn) && (hc >= 0.f || ab >= 0.f)) {
            healthCur = hc;
            healthMax = hm;
            absorp = ab;
            parsed = n;
            firstName = fn;
            layout = t.name;
            r.p = trial.p; // consume
            break;
        }
        // Keep best partial for diagnostics
        if (n > parsed) {
            parsed = n;
            firstName = fn;
            layout = t.name;
        }
    }

    {
        static int s_uaLog = 0;
        if (s_uaLog < 12) {
            char b[160];
            std::snprintf(b, sizeof(b),
                          "UA rid=%llu layout=%s attrs=%d first=%.24s hp=%.1f/%.1f abs=%.1f",
                          (unsigned long long)runtimeId, layout, parsed,
                          firstName.empty() ? "-" : firstName.c_str(), healthCur, healthMax, absorp);
            bactro::statusLine(b);
            ++s_uaLog;
        }
    }

    if (healthCur < 0.f && absorp < 0.f) return;

    EntityHealth h{};
    bool hadPrev = false;
    {
        std::lock_guard lock(g_mu);
        auto it = g_map.find(runtimeId);
        if (it != g_map.end()) {
            h = it->second;
            hadPrev = h.valid;
        }
    }
    if (hadPrev && healthCur >= 0.f && healthCur < h.current - 0.01f) pushHurt(runtimeId);
    if (healthCur >= 0.f) {
        h.current = healthCur;
        if (healthMax > 0.f) h.max = healthMax;
        h.valid = true;
    }
    if (absorp >= 0.f) h.absorption = absorp;
    setHealth(runtimeId, h.current, h.max, h.absorption);
}

void parseStartGame(Reader& r) {
    uint64_t uniqueId = 0, runtimeId = 0; // unique id is zigzag varint64 (same byte layout)
    if (!r.readVarU64(uniqueId) || !r.readVarU64(runtimeId)) return;
    {
        std::lock_guard lock(g_mu);
        g_self = runtimeId;
    }
    char b[80];
    std::snprintf(b, sizeof(b), "pkt StartGame selfRuntimeId=%llu", (unsigned long long)runtimeId);
    bactro::statusLine(b);
}

void parseAddPlayer(Reader& r) {
    if (r.left() < 16) return;
    r.p += 16; // uuid
    std::string name;
    uint64_t runtimeId = 0;
    if (!r.readString(name) || !r.readVarU64(runtimeId)) return;
    {
        std::lock_guard lock(g_mu);
        g_names[runtimeId] = name;
    }
    char b[160];
    std::snprintf(b, sizeof(b), "pkt AddPlayer runtime=%llu name=%.40s", (unsigned long long)runtimeId,
                  name.c_str());
    logOnce(2, b);
}

void parseActorEvent(Reader& r) {
    uint64_t runtimeId = 0;
    uint8_t ev = 0;
    if (!r.readVarU64(runtimeId) || !r.readU8(ev)) return;
    if (ev == 2) { // HURT_ANIMATION
        pushHurt(runtimeId);
        logOnce(3, "pkt ActorEvent hurt seen");
        {
            static int s_hurtLog = 0;
            if (s_hurtLog < 8) {
                char b[80];
                std::snprintf(b, sizeof(b), "hurt event rid=%llu", (unsigned long long)runtimeId);
                bactro::statusLine(b);
                ++s_hurtLog;
            }
        }
    } else if (ev == 3) { // DEATH
        EntityHealth h{};
        h.current = 0.f;
        h.max = 20.f;
        h.valid = true;
        setHealth(runtimeId, 0.f, 20.f, 0.f);
        pushHurt(runtimeId);
        {
            char b[64];
            std::snprintf(b, sizeof(b), "death event rid=%llu", (unsigned long long)runtimeId);
            bactro::statusLine(b);
        }
    }
}

// SetActorData (id 39): runtimeId + metadata entries. Some builds put health as a float property.
void parseSetActorData(Reader& r) {
    uint64_t runtimeId = 0;
    if (!r.readVarU64(runtimeId)) return;
    // Metadata: repeated until end — id (unsigned varint), type (unsigned varint), value
    // Types: 0=u8, 1=i16, 2=i32, 3=f32, 4=string, 5=nbt, 6=i64, 7=vec3, ...
    static int s_log = 0;
    int floats = 0;
    float lastFloat = -1.f;
    while (r.ok() && r.left() >= 2) {
        uint32_t key = 0, type = 0;
        if (!r.readVarU32(key) || !r.readVarU32(type)) break;
        if (type == 0) { // byte
            uint8_t v;
            if (!r.readU8(v)) break;
        } else if (type == 1) { // short
            if (r.left() < 2) break;
            r.p += 2;
        } else if (type == 2) { // int
            if (r.left() < 4) break;
            r.p += 4;
        } else if (type == 3) { // float — candidate for health
            float v = 0.f;
            if (!r.readF32(v)) break;
            ++floats;
            lastFloat = v;
            // Player health is typically 0..40 (max with effects). Prefer 0..20 range updates.
            if (v >= 0.f && v <= 40.f && runtimeId != 0) {
                // Only apply if it looks like a health snapshot (not random floats)
                if (v <= 20.01f) {
                    auto prev = get(runtimeId);
                    float prevCur = prev ? prev->current : 20.f;
                    // Accept if lower than previous or first time and not a default noise
                    if (v < prevCur - 0.05f || (v < 19.5f && (!prev || !prev->valid))) {
                        setHealth(runtimeId, v, prev ? prev->max : 20.f, prev ? prev->absorption : 0.f);
                        if (s_log < 10) {
                            char b[96];
                            std::snprintf(b, sizeof(b), "SetActorData rid=%llu float=%.1f key=%u -> HP",
                                          (unsigned long long)runtimeId, v, key);
                            bactro::statusLine(b);
                            ++s_log;
                        }
                    }
                }
            }
        } else if (type == 4) { // string
            std::string s;
            if (!r.readString(s)) break;
        } else if (type == 6) { // long
            if (r.left() < 8) break;
            r.p += 8;
        } else if (type == 7) { // vec3
            if (r.left() < 12) break;
            r.p += 12;
        } else {
            // Unknown type — abort this packet to avoid desync
            break;
        }
    }
    (void)floats;
    (void)lastFloat;
}

void dispatchPacket(const uint8_t* d, size_t n) {
    if (!d || n < 1) return;
    Reader r{d, d + n};
    uint32_t id = 0;
    if (!readHeader(r, id)) return;
    {
        static std::bitset<1024> seen;
        static int distinct = 0;
        bool fresh = false;
        {
            std::lock_guard lock(g_mu);
            if (!seen.test(id) && distinct < 60) {
                seen.set(id);
                ++distinct;
                fresh = true;
            }
        }
        if (fresh) {
            char b[96];
            std::snprintf(b, sizeof(b), "in: new packet id=%u len=%zu", id, n);
            bactro::statusLine(b);
        }
    }
    switch (id) {
    case 11: parseStartGame(r); break;
    case 12: parseAddPlayer(r); break;
    case 27: parseActorEvent(r); break;
    case 29:
        logOnce(4, "pkt UpdateAttributes seen");
        parseUpdateAttributesPayload(r);
        break;
    case 39: parseSetActorData(r); break;
    default: break;
    }
}

// ---- outgoing (our own packets) ----
// InventoryTransaction (id 30), ItemUseOnEntity (type 3), action Attack (1):
//   varint32 legacyRequestId(0), varuint32 type, varuint32 actionCount(0),
//   varuint64 targetRuntimeId, varuint32 actionType, ...
void parseOutInventoryTransaction(const uint8_t* whole, size_t wholeLen, Reader r) {
    static std::atomic<int> dumped{0};
    const bool dump = dumped.fetch_add(1) < 8;
    bool hit = false;
    uint64_t rid = 0;
    uint32_t req = 0, type = 0, n = 0, act = 0;
    // Standard: type 3 = UseItemOnEntity, act 1 = Attack
    if (r.readVarU32(req) && r.readVarU32(type)) {
        if (type == 3 && r.readVarU32(n) && r.readVarU64(rid) && r.readVarU32(act)) {
            if (act == 1 && rid != 0) {
                hit = true;
                pushMyHit(rid);
            }
        }
    }
    if (dump || hit) {
        static std::atomic<int> hitLogs{0};
        if (!hit || hitLogs.fetch_add(1) < 20) {
            char b[240];
            int o = std::snprintf(b, sizeof(b), "out: InvTx len=%zu type=%u hit=%d rid=%llu:", wholeLen, type,
                                  hit ? 1 : 0, (unsigned long long)rid);
            for (size_t i = 0; i < wholeLen && i < 22 && o < (int)sizeof(b) - 4; ++i)
                o += std::snprintf(b + o, sizeof(b) - o, " %02x", whole[i]);
            bactro::statusLine(b);
        }
    }
}

void dispatchOutPacket(const uint8_t* d, size_t n) {
    if (!d || n < 1) return;
    Reader r{d, d + n};
    uint32_t id = 0;
    if (!readHeader(r, id)) return;
    if (id == 30) parseOutInventoryTransaction(d, n, r);
}

// Preferred: buffer is a batch [varuint len][packet]... that partitions exactly.
bool parseBatch(const uint8_t* data, size_t size) {
    if (!data || size < 2 || size > 1 << 22) return false;
    struct Span { const uint8_t* p; size_t n; };
    std::vector<Span> pk;
    Reader r{data, data + size};
    while (r.ok()) {
        uint32_t len = 0;
        if (!r.readVarU32(len)) return false;
        if (len == 0 || len > r.left()) return false;
        if (pk.size() >= 512) return false;
        pk.push_back({r.p, len});
        r.p += len;
    }
    if (pk.empty()) return false;
    for (const auto& s : pk) dispatchPacket(s.p, s.n);
    logOnce(0, "in: buffers parse as length-prefixed batches");
    return true;
}

// Fallback (old behaviour): buffer is one bare packet, or unknown framing (id 29 only).
void tryParseOne(const uint8_t* data, size_t size) {
    if (!data || size < 2 || size > 1 << 20) return;
    Reader r{data, data + size};
    uint32_t packetId = 0;
    if (!readHeader(r, packetId)) return;
    if (packetId == 29) parseUpdateAttributesPayload(r);
}

} // namespace

void onRawGamePacket(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    {
        static std::atomic<int> dumps{0};
        const int k = dumps.fetch_add(1);
        if (k < 5 || (k == 200) || (k == 2000)) {
            char b[200];
            int o = std::snprintf(b, sizeof(b), "in: buf#%d size=%zu:", k, size);
            for (size_t i = 0; i < size && i < 16 && o < (int)sizeof(b) - 4; ++i)
                o += std::snprintf(b + o, sizeof(b) - o, " %02x", data[i]);
            bactro::statusLine(b);
        }
    }
    if (parseBatch(data, size)) return;
    logOnce(1, "in: buffer is NOT a clean batch (fallback scan for id 29 only)");
    tryParseOne(data, size);
    if (size > 16) {
        for (size_t i = 0; i + 8 < size && i < size - 8; ++i) {
            const uint8_t b = data[i];
            if ((b & 0x3F) == 29 || b == 29) tryParseOne(data + i, size - i);
        }
    }
}

void onOutgoingBatch(const uint8_t* data, size_t size) {
    if (!data || size < 2 || size > 1 << 22) return;
    Reader r{data, data + size};
    struct Span { const uint8_t* p; size_t n; };
    Span pk[64];
    size_t cnt = 0;
    while (r.ok()) {
        uint32_t len = 0;
        if (!r.readVarU32(len) || len == 0 || len > r.left() || cnt >= 64) return; // not a clean batch
        pk[cnt++] = {r.p, len};
        r.p += len;
    }
    for (size_t i = 0; i < cnt; ++i) dispatchOutPacket(pk[i].p, pk[i].n);
}

bool popMyHit(uint64_t& runtimeId) {
    std::lock_guard lock(g_mu);
    if (g_myHit.empty()) return false;
    runtimeId = g_myHit.front();
    g_myHit.erase(g_myHit.begin());
    return true;
}

void setHealth(uint64_t runtimeId, float current, float max, float absorption) {
    std::lock_guard lock(g_mu);
    auto& e = g_map[runtimeId];
    e.current = current;
    e.max = max > 0.f ? max : (e.max > 0.f ? e.max : 20.f);
    e.absorption = absorption;
    e.valid = true;
    e.fromPacket = (current < e.max - 0.25f) || (absorption > 0.05f);
    g_lastRuntimeId = runtimeId;
    g_lastHealth = e;
    g_hasLast = true;

    auto& pred = g_pred[runtimeId];
    if (e.fromPacket) {
        pred.health = current;
        pred.maxHealth = e.max;
        pred.trusted = true;
    }
    if (absorption < pred.lastAbsorption - 0.05f)
        pred.pendingDamage = std::max(pred.pendingDamage, pred.lastAbsorption - absorption);
    pred.absorption = absorption;
    pred.lastAbsorption = absorption;
    if (e.max > 0.f) pred.maxHealth = e.max;

    static int s_log;
    if ((++s_log % 4) == 1) {
        char b[96];
        std::snprintf(b, sizeof(b), "packet HP runtime=%llu cur=%.1f max=%.1f abs=%.1f real=%d",
                      (unsigned long long)runtimeId, current, e.max, absorption, e.fromPacket ? 1 : 0);
        bactro::statusLine(b);
        HC_LOGI("%s", b);
    }
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
    g_names.clear();
    g_hurt.clear();
    g_myHit.clear();
    g_pred.clear();
    g_hasLast = false;
    g_lastSwingTarget = 0;
}

size_t playerCount() {
    std::lock_guard lock(g_mu);
    return g_names.size();
}

std::string playerName(uint64_t runtimeId) {
    std::lock_guard lock(g_mu);
    auto it = g_names.find(runtimeId);
    return it == g_names.end() ? std::string() : it->second;
}

uint64_t selfRuntimeId() {
    std::lock_guard lock(g_mu);
    return g_self;
}

bool popHurt(uint64_t& runtimeId) {
    std::lock_guard lock(g_mu);
    if (g_hurt.empty()) return false;
    runtimeId = g_hurt.front();
    g_hurt.erase(g_hurt.begin());
    return true;
}


void noteOutgoingSwing(uint64_t runtimeIdHint) {
    std::lock_guard lock(g_mu);
    using clock = std::chrono::steady_clock;
    g_lastSwingTime = clock::now();
    if (runtimeIdHint != 0) g_lastSwingTarget = runtimeIdHint;
    // Default pending melee damage (iron-ish, armor-unknown). Applied only on next hurt.
    constexpr float kMelee = 4.0f;
    if (runtimeIdHint != 0) {
        auto& p = g_pred[runtimeIdHint];
        if (!p.trusted) p.pendingDamage = std::max(p.pendingDamage, kMelee);
    } else if (g_lastSwingTarget != 0) {
        auto& p = g_pred[g_lastSwingTarget];
        if (!p.trusted) p.pendingDamage = std::max(p.pendingDamage, kMelee);
    }
}

void noteHurt(uint64_t runtimeId) {
    if (runtimeId == 0) return;
    std::lock_guard lock(g_mu);
    if (runtimeId == g_self) return;
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    auto& p = g_pred[runtimeId];
    if (p.trusted) {
        // Real packet HP is authority; still refresh hurt timer for regen gating
        p.lastHurt = now;
        return;
    }
    // Prefer absorption-delta damage (Solstice); else pending from our swing; else small default
    float dmg = 0.f;
    if (p.pendingDamage > 0.05f) {
        dmg = p.pendingDamage;
        p.pendingDamage = 0.f;
    } else if (p.absorption < p.lastAbsorption - 0.05f) {
        dmg = p.lastAbsorption - p.absorption;
    } else {
        dmg = 2.0f; // half heart minimum on confirmed hurt animation
    }
    // If our swing was recent, this hurt is likely ours — full pending; else still apply dmg
    if (g_lastSwingTarget == runtimeId || g_lastSwingTarget == 0) {
        const float since = std::chrono::duration<float>(now - g_lastSwingTime).count();
        if (since < 0.9f && dmg < 2.f) dmg = std::max(dmg, 2.f);
    }
    p.health = std::max(0.f, p.health - dmg);
    p.lastHurt = now;
    // Mirror into g_map so get()/display stay consistent
    auto& e = g_map[runtimeId];
    e.current = p.health;
    e.max = p.maxHealth;
    e.absorption = p.absorption;
    e.valid = true;
    e.fromPacket = false;
    static int s_log = 0;
    if (s_log < 15) {
        char b[96];
        std::snprintf(b, sizeof(b), "predict HP rid=%llu dmg=%.1f -> %.1f/%.1f",
                      (unsigned long long)runtimeId, dmg, p.health, p.maxHealth);
        bactro::statusLine(b);
        ++s_log;
    }
}

void tickPrediction() {
    std::lock_guard lock(g_mu);
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    for (auto& [rid, p] : g_pred) {
        if (p.trusted) continue;
        if (rid == g_self) continue;
        // Solstice: +1 HP about every 4s if not recently hurt
        const float sinceHurt = p.lastHurt.time_since_epoch().count() == 0
                                    ? 999.f
                                    : std::chrono::duration<float>(now - p.lastHurt).count();
        if (sinceHurt < 3.0f) continue;
        const float sinceHeal = p.lastHeal.time_since_epoch().count() == 0
                                    ? 999.f
                                    : std::chrono::duration<float>(now - p.lastHeal).count();
        if (sinceHeal < 4.0f) continue;
        if (p.health < p.maxHealth - 0.01f) {
            p.health = std::min(p.maxHealth, p.health + 1.f);
            p.lastHeal = now;
            auto& e = g_map[rid];
            e.current = p.health;
            e.max = p.maxHealth;
            e.valid = true;
        }
    }
}

EntityHealth displayHealth(uint64_t runtimeId) {
    std::lock_guard lock(g_mu);
    EntityHealth out{};
    auto it = g_map.find(runtimeId);
    auto pit = g_pred.find(runtimeId);
    if (it != g_map.end() && it->second.valid && it->second.fromPacket) {
        return it->second;
    }
    if (pit != g_pred.end()) {
        out.current = pit->second.health;
        out.max = pit->second.maxHealth;
        out.absorption = pit->second.absorption;
        out.valid = true;
        out.fromPacket = pit->second.trusted;
        return out;
    }
    if (it != g_map.end() && it->second.valid) return it->second;
    out.current = 20.f;
    out.max = 20.f;
    out.valid = false;
    return out;
}

} // namespace bactro::health
