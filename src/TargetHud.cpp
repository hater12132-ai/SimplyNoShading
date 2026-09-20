#include "bactro/TargetHud.hpp"
#include "bactro/HealthCache.hpp"
#include "bactro/Signatures.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#define TH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::targethud {
namespace {

using bactro::memory::SignatureId;

constexpr const char* kModuleId = "bactro.targethud";
constexpr const char* kSteveHeadId = "bactro.targethud.steve";
constexpr int kHeadSize = 64;

std::atomic_bool g_enabled{true};
std::atomic_bool g_showOnLook{true};
std::atomic_bool g_showOnHit{true};
std::atomic_bool g_playersOnly{true};
std::atomic<float> g_liveTime{5.0f};
std::atomic<float> g_lookRange{3.0f};
std::atomic<float> g_scale{1.0f};
std::atomic<float> g_cardW{168.0f};
std::atomic<float> g_cardH{44.0f};

// ---- target state ----
struct Target {
    void* actor = nullptr;
    std::string name = "Player";
    std::string headKey;
    bool hasHead = false;
    float health = 20.f;
    float maxHealth = 20.f;
    float displayHealth = 20.f;
    float absorption = 0.f;
    float displayAbsorption = 0.f;
    uint64_t runtimeId = 0; // bound after first matching UpdateAttributes
    bool liveHealth = false; // true once packet HP applied
    bool valid = false;
    bool dead = false;
    std::chrono::steady_clock::time_point lastSeen{};
    std::chrono::steady_clock::time_point diedAt{};
    std::chrono::steady_clock::time_point lastHit{};
    // ProtoHax-style head reaction after each hit (red flash + punch scale)
    float hurtFlash = 0.f;
    int hitCount = 0;
};
Target g_target;
std::mutex g_mutex;
float g_anim = 0.f;
bool g_steveReady = false;

// ---- helpers ----
// NOTE: ActorGetNameTag returns std::string by value — calling a mismatched
// signature hard-crashes. We never call it from attack detours; name is best-effort
// only from a null-checked pointer with a try/catch, and we prefer a static label.
using ActorGetNameTagFn = std::string (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using AttackFn = bool (*)(void*, void*, bool);

ActorGetNameTagFn g_getNameTag = nullptr;
ActorIsPlayerFn g_isPlayer = nullptr;
AttackFn g_attackOrig = nullptr;
bool g_attackHooked = false;

// Never touch game memory below this; avoids bad actor* from hit results
constexpr std::uintptr_t kMinActorPtr = 0x10000;

std::uint32_t withAlpha(std::uint32_t c, float a) {
    a = std::clamp(a, 0.f, 1.f);
    const auto aa = static_cast<std::uint32_t>(std::clamp(((c >> 24) & 0xFFu) * a, 0.f, 255.f));
    return (aa << 24) | (c & 0x00FFFFFFu);
}

// ProtoHax-style bar: green → yellow → red
std::uint32_t healthColor(float ratio) {
    ratio = std::clamp(ratio, 0.f, 1.f);
    float r, g, b;
    if (ratio > 0.5f) {
        // green → yellow
        float t = (ratio - 0.5f) * 2.f;
        r = 1.f - t;
        g = 1.f;
        b = 0.15f * (1.f - t);
    } else {
        // yellow → red
        float t = ratio * 2.f;
        r = 1.f;
        g = t;
        b = 0.f;
    }
    auto ch = [](float x) -> std::uint32_t {
        return static_cast<std::uint32_t>(std::clamp(x, 0.f, 1.f) * 255.f);
    };
    return 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

std::string cleanName(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA7) {
            i += 2;
            if (i < s.size()) ++i;
            continue;
        }
        if (c == 0xA7) {
            i += std::min<size_t>(2, s.size() - i);
            continue;
        }
        out.push_back(s[i++]);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    if (out.empty()) out = "Player";
    return out;
}

std::string ellipsize(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    if (maxChars < 2) return s.substr(0, maxChars);
    return s.substr(0, maxChars - 1) + "…";
}

void ensureSteveHead() {
    if (g_steveReady) return;
    std::array<uint8_t, kHeadSize * kHeadSize * 4> px{};
    // Classic Steve-ish face (simplified 8x8 upscaled)
    // skin tone + eyes + mouth
    const int up = kHeadSize / 8;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            uint8_t r = 198, g = 150, b = 112; // skin
            if (y == 2 && (x == 2 || x == 5)) {
                r = 40;
                g = 30;
                b = 20;
            } // eyes
            if (y == 3 && (x == 2 || x == 5)) {
                r = 90;
                g = 140;
                b = 200;
            } // iris
            if (y == 5 && x >= 3 && x <= 4) {
                r = 120;
                g = 70;
                b = 50;
            } // mouth
            if (y == 0 || y == 1) {
                r = 60;
                g = 40;
                b = 25;
            } // hair
            for (int sy = 0; sy < up; ++sy)
                for (int sx = 0; sx < up; ++sx) {
                    auto* d = px.data() + ((y * up + sy) * kHeadSize + x * up + sx) * 4;
                    d[0] = r;
                    d[1] = g;
                    d[2] = b;
                    d[3] = 255;
                }
        }
    }
    pl::modmenu::registerImage(kSteveHeadId, px, kHeadSize, kHeadSize);
    g_steveReady = true;
}

bool actorPtrOk(void* actor) {
    const auto p = reinterpret_cast<std::uintptr_t>(actor);
    return actor != nullptr && p >= kMinActorPtr && (p & 0x7) == 0;
}

std::string readName(void* actor) {
    if (!actorPtrOk(actor)) return "Player";
    // Calling Bedrock std::string-return APIs is fragile across versions.
    // Keep disabled for crash safety; HUD still shows "Player" + HP estimate.
    (void)g_getNameTag;
    (void)actor;
    return "Player";
}

bool isPlayer(void* actor) {
    if (!actorPtrOk(actor)) return false;
    // Skip ActorIsPlayer native call — same ABI risk. Assume player targets.
    (void)g_isPlayer;
    return true;
}

void applyTarget(void* actor, bool fromHit) {
    if (!actorPtrOk(actor)) return;
    // playersOnly still respected conceptually; we cannot safely filter without
    // ActorIsPlayer, so we accept the pointer when show-on-hit is enabled.

    std::lock_guard lock(g_mutex);
    const bool same = g_target.valid && g_target.actor == actor;
    g_target.actor = actor;
    // Only refresh name on first bind — never call native getNameTag on hit path
    if (!same || g_target.name.empty()) {
        g_target.name = "Player";
    }
    g_target.valid = true;
    g_target.dead = false;
    g_target.lastSeen = std::chrono::steady_clock::now();
    if (fromHit) {
        g_target.lastHit = g_target.lastSeen;
        g_target.hurtFlash = 1.f; // full red flash, decays in tickAnim
        ++g_target.hitCount;
    }

    if (!same) {
        // New target: assume full HP (no packet hook — crash-safe mode)
        g_target.health = 20.f;
        g_target.maxHealth = 20.f;
        g_target.displayHealth = 20.f;
        g_target.absorption = 0.f;
        g_target.displayAbsorption = 0.f;
        g_target.runtimeId = 0;
        g_target.liveHealth = false;
        g_target.hasHead = false;
        g_target.headKey.clear();
        g_target.hurtFlash = fromHit ? 1.f : 0.f;
        g_target.hitCount = fromHit ? 1 : 0;
    } else if (fromHit && !g_target.liveHealth) {
        // Fallback estimate: ~1 heart per hit until packet HP is safe again
        g_target.health = std::max(0.f, g_target.health - 1.f);
        if (g_target.health <= 0.01f) {
            g_target.dead = true;
            g_target.diedAt = g_target.lastSeen;
        }
    }
}

// Pull ProtoHax-style packet HP into the active target.
// 1) If we already bound a runtimeId, use HealthCache directly.
// 2) Else, if we recently hit someone, bind the most recent health update (1v1).
void syncPacketHealth() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();

    std::lock_guard lock(g_mutex);
    if (!g_target.valid || g_target.dead) return;

    if (g_target.runtimeId != 0) {
        if (auto h = bactro::health::get(g_target.runtimeId)) {
            g_target.health = h->current;
            g_target.maxHealth = h->max > 0.f ? h->max : g_target.maxHealth;
            g_target.absorption = h->absorption;
            g_target.liveHealth = true;
            if (h->current <= 0.01f) {
                g_target.dead = true;
                g_target.diedAt = now;
            }
        }
        return;
    }

    // No runtimeId yet: bind most recent UpdateAttributes within 1.25s of a hit (1v1 PvP)
    if (g_target.lastHit.time_since_epoch().count() == 0) return;
    const float sinceHit = std::chrono::duration<float>(now - g_target.lastHit).count();
    if (sinceHit > 1.25f) return;

    if (auto last = bactro::health::lastUpdate()) {
        g_target.runtimeId = last->first;
        g_target.health = last->second.current;
        g_target.maxHealth = last->second.max > 0.f ? last->second.max : 20.f;
        g_target.absorption = last->second.absorption;
        g_target.liveHealth = true;
        if (last->second.current <= 0.01f) {
            g_target.dead = true;
            g_target.diedAt = now;
        }
    }
}

void markDead() {
    std::lock_guard lock(g_mutex);
    if (!g_target.valid) return;
    g_target.dead = true;
    g_target.health = 0.f;
    g_target.diedAt = std::chrono::steady_clock::now();
}

// ---- attack detour ----
// GameModeAttack pattern on 1.26.51 is very short and ambiguous; a wrong match
// crashes the moment you (or the game) calls that site. We do NOT install it.
// TargetHUD still works via HUD editor preview + estimated HP if bound later.
bool attackDetour(void* gm, void* target, bool someFlag) {
    // Extremely defensive: never touch target beyond pointer validity; never
    // call getNameTag/isPlayer from this path.
    if (g_enabled.load() && g_showOnHit.load() && actorPtrOk(target)) {
        try {
            applyTarget(target, true);
        } catch (...) {
        }
    }
    if (g_attackOrig) return g_attackOrig(gm, target, someFlag);
    return false;
}

void tryInstallAttackHook() {
    // DISABLED for crash safety — short GameModeAttack signature is unsafe.
    // Re-enable only after a unique, verified prologue pattern for 1.26.51.1.
    TH_LOGI("TargetHUD: GameModeAttack NOT hooked (crash-safe)");
    g_attackHooked = false;
    g_attackOrig = nullptr;
    (void)attackDetour;
}

void resolveActorFns() {
    // Do not bind getNameTag / isPlayer — calling them with wrong ABI crashes.
    g_getNameTag = nullptr;
    g_isPlayer = nullptr;
    TH_LOGI("TargetHUD: native name/isPlayer calls disabled (crash-safe)");
}

// ---- draw (ProtoHax style) ----
void submitHud() {
    ensureSteveHead();

    Target snap;
    float anim;
    {
        std::lock_guard lock(g_mutex);
        snap = g_target;
        anim = g_anim;
    }

    // Always show a ghost card in HUD editor context is handled by PL when module is HUD;
    // we only submit when visible.
    if (!g_enabled.load()) {
        pl::modmenu::submitDrawCommands(kModuleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    const float fade = std::clamp(anim, 0.f, 1.f);
    if (fade < 0.02f && !snap.valid) {
        pl::modmenu::submitDrawCommands(kModuleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }
    if (!snap.valid && fade < 0.02f) {
        pl::modmenu::submitDrawCommands(kModuleId, std::span<const pl::modmenu::DrawCommand>{});
        return;
    }

    const float s = g_scale.load();
    const float cardW = g_cardW.load() * s;
    const float cardH = g_cardH.load() * s;
    // PL places HUD modules; use 0,0 local coords inside the element
    const float x = 0.f;
    const float y = 0.f;
    const float rad = cardH * 0.5f; // full pill

    std::vector<pl::modmenu::DrawCommand> cmds;
    cmds.reserve(16);
    static thread_local std::vector<std::string> texts;
    texts.clear();

    auto rect = [&](float rx, float ry, float rw, float rh, float r, std::uint32_t col) {
        pl::modmenu::DrawCommand c{};
        c.type = pl::modmenu::DrawCommandType::RectFilled;
        c.x = rx;
        c.y = ry;
        c.w = rw;
        c.h = rh;
        c.x3 = r;
        c.color = col;
        cmds.push_back(c);
    };
    auto circle = [&](float cx, float cy, float d, std::uint32_t col) {
        pl::modmenu::DrawCommand c{};
        c.type = pl::modmenu::DrawCommandType::CircleFilled;
        c.x = cx;
        c.y = cy;
        c.w = d;
        c.h = d;
        c.color = col;
        cmds.push_back(c);
    };
    auto text = [&](float tx, float ty, float size, std::uint32_t col, std::string str) {
        texts.push_back(std::move(str));
        pl::modmenu::DrawCommand c{};
        c.type = pl::modmenu::DrawCommandType::Text;
        c.x = tx;
        c.y = ty;
        c.w = cardW;
        c.h = size + 4.f;
        c.size = size;
        c.color = col;
        c.text = texts.back();
        cmds.push_back(c);
    };
    auto image = [&](float ix, float iy, float iw, float ih, const std::string& id, std::uint32_t col) {
        pl::modmenu::DrawCommand c{};
        c.type = pl::modmenu::DrawCommandType::Image;
        c.x = ix;
        c.y = iy;
        c.w = iw;
        c.h = ih;
        c.imageId = id;
        c.color = col;
        cmds.push_back(c);
    };

    // Pill background (dark, no white glow) — ProtoHax compact card
    const std::uint32_t bg = withAlpha(0xE0121218u, fade);
    rect(x, y, cardW, cardH, rad, bg);

    // Circular head (Steve fallback; real skin when available)
    // Hurt reaction: slight punch scale + red tint that decays after each hit
    const float headPad = 5.5f * s;
    const float headBase = cardH - headPad * 2.f;
    const float hurt = std::clamp(snap.hurtFlash, 0.f, 1.f);
    const float punch = 1.f + 0.12f * hurt; // scale up briefly on hit
    const float headD = headBase * punch;
    const float headX = x + headPad - (headD - headBase) * 0.5f;
    const float headY = y + headPad - (headD - headBase) * 0.5f;
    circle(headX, headY, headD, withAlpha(0xFF1E1E26u, fade));
    {
        const float inset = 1.5f * s;
        const std::string& img = (snap.hasHead && !snap.headKey.empty()) ? snap.headKey : kSteveHeadId;
        // White head, then red overlay intensity based on hurtFlash (ProtoHax hit react)
        const std::uint32_t headTint = withAlpha(0xFFFFFFFFu, fade);
        image(headX + inset, headY + inset, headD - inset * 2.f, headD - inset * 2.f, img, headTint);
        if (hurt > 0.02f) {
            // Soft red flash over the head circle (no white glow)
            const float redA = fade * hurt * 0.55f;
            circle(headX, headY, headD, withAlpha(0xFFE53935u, redA));
        }
    }

    // Name only (no emoji)
    const float textLeft = x + headPad + headBase + 8.f * s;
    const float nameY = y + 7.f * s;
    const float nameSize = 12.5f * s;
    {
        const std::string shown = ellipsize(snap.name, 12);
        text(textLeft, nameY, nameSize, withAlpha(0xFFFFFFFFu, fade), shown);
    }

    // HP numbers top-right above the bar: "20/20"
    // With absorption (gapple / enchanted gapple): "/max" turns gold and includes extra HP
    // e.g. full + gapple abs 4 → "20/24" with the "/24" in gold
    const float dispHp = std::max(0.f, snap.displayHealth);
    const float dispMax = std::max(1.f, snap.maxHealth);
    const float dispAbs = std::max(0.f, snap.displayAbsorption);
    const int curI = static_cast<int>(std::lround(dispHp));
    const int maxI = static_cast<int>(std::lround(dispMax));
    const int absI = static_cast<int>(std::lround(dispAbs));
    const float hpSize = 11.f * s;
    const float hpY = nameY; // same row as name, right-aligned region
    {
        // Build "cur" (white) + "/total" (white or gold if absorption)
        char curBuf[16];
        std::snprintf(curBuf, sizeof(curBuf), "%d", curI);
        const std::string curStr = curBuf;
        char maxBuf[24];
        if (absI > 0) {
            // Extra hearts from gapple: show effective max (max + abs) in gold
            std::snprintf(maxBuf, sizeof(maxBuf), "/%d", maxI + absI);
        } else {
            std::snprintf(maxBuf, sizeof(maxBuf), "/%d", maxI);
        }
        const std::string maxStr = maxBuf;

        // Approximate right-align: estimate glyph width ~0.55 * size
        const float glyph = hpSize * 0.55f;
        const float totalW = (curStr.size() + maxStr.size()) * glyph;
        const float hpRight = x + cardW - 10.f * s;
        const float hpX = hpRight - totalW;
        text(hpX, hpY, hpSize, withAlpha(0xFFFFFFFFu, fade), curStr);
        const std::uint32_t maxCol =
            absI > 0 ? withAlpha(0xFFFFC107u, fade) /* gold */ : withAlpha(0xFFB0B0B8u, fade);
        text(hpX + curStr.size() * glyph, hpY, hpSize, maxCol, maxStr);
    }

    // Health bar (thin, rounded) under name — green→yellow→red
    const float barX = textLeft;
    const float barW = cardW - (textLeft - x) - 10.f * s;
    const float barH = 4.2f * s;
    const float barY = y + cardH - 11.f * s;
    rect(barX, barY, barW, barH, barH * 0.5f, withAlpha(0xFF1A1A22u, fade));

    const float ratio =
        dispMax > 0.01f ? std::clamp(dispHp / dispMax, 0.f, 1.f) : 0.f;
    if (ratio > 0.01f) {
        rect(barX, barY, barW * ratio, barH, barH * 0.5f, withAlpha(healthColor(ratio), fade));
    }
    // Absorption overlay (gold strip on the right of remaining bar capacity)
    if (dispAbs > 0.05f && dispMax > 0.01f) {
        const float absRatio = std::clamp(dispAbs / dispMax, 0.f, 1.f - ratio);
        if (absRatio > 0.01f) {
            const float ax = barX + barW * ratio;
            rect(ax, barY, barW * absRatio, barH, barH * 0.5f, withAlpha(0xFFFFC107u, fade * 0.9f));
        }
    }

    pl::modmenu::submitDrawCommands(kModuleId, cmds);
}

void tickAnim() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    float targetAnim = 0.f;
    {
        std::lock_guard lock(g_mutex);
        if (g_target.valid) {
            if (g_target.dead) {
                const float since =
                    std::chrono::duration<float>(now - g_target.diedAt).count();
                if (since < 1.0f) {
                    targetAnim = 1.f - since;
                    g_target.displayHealth = 0.f;
                } else {
                    g_target.valid = false;
                    targetAnim = 0.f;
                }
            } else {
                const float since =
                    std::chrono::duration<float>(now - g_target.lastSeen).count();
                if (since > g_liveTime.load()) {
                    g_target.valid = false;
                    targetAnim = 0.f;
                } else {
                    targetAnim = 1.f;
                    // smooth HP + absorption (gapple gold hearts)
                    g_target.displayHealth +=
                        (g_target.health - g_target.displayHealth) * 0.18f;
                    g_target.displayAbsorption +=
                        (g_target.absorption - g_target.displayAbsorption) * 0.18f;
                    // ProtoHax head hit flash decays ~0.35s
                    if (g_target.hurtFlash > 0.f) {
                        g_target.hurtFlash = std::max(0.f, g_target.hurtFlash - 0.045f);
                    }
                }
            }
        }
    }
    g_anim += (targetAnim - g_anim) * 0.22f;
    if (std::fabs(g_anim) < 0.001f) g_anim = 0.f;
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled);
    if (!enabled) {
        std::lock_guard lock(g_mutex);
        g_target.valid = false;
        g_anim = 0.f;
        pl::modmenu::submitDrawCommands(kModuleId, std::span<const pl::modmenu::DrawCommand>{});
    }
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "showOnLook") g_showOnLook.store(value == "true" || value == "1");
        else if (key == "showOnHit") g_showOnHit.store(value == "true" || value == "1");
        else if (key == "playersOnly") g_playersOnly.store(value == "true" || value == "1");
        else if (key == "liveTime") g_liveTime.store(std::stof(std::string(value)));
        else if (key == "lookRange") g_lookRange.store(std::stof(std::string(value)));
        else if (key == "scale") g_scale.store(std::stof(std::string(value)));
        else if (key == "cardW") g_cardW.store(std::stof(std::string(value)));
        else if (key == "cardH") g_cardH.store(std::stof(std::string(value)));
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Target HUD");
    b.description("Target card (crash-safe): head, 20/20, green→yellow→red bar. No combat hooks. Drag in HUD Editor.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("showOnHit", "Show on hit", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("showOnLook", "Show when looking (3m)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("playersOnly", "Players only", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("liveTime", "Visible time (s)", pl::modmenu::ConfigType::SliderFloat, "5", "1", "15", "");
    b.config("lookRange", "Look range", pl::modmenu::ConfigType::SliderFloat, "3", "1", "8", "");
    b.config("scale", "Scale", pl::modmenu::ConfigType::SliderFloat, "1", "0.5", "2", "");
    b.config("cardW", "Card width", pl::modmenu::ConfigType::SliderFloat, "168", "100", "280", "");
    b.config("cardH", "Card height", pl::modmenu::ConfigType::SliderFloat, "44", "32", "72", "");
    b.registerModule();
    ensureSteveHead();
    TH_LOGI("TargetHUD module registered");
}

void onSignaturesReady() {
    resolveActorFns();
    tryInstallAttackHook();
}

void onFrame() {
    if (!g_enabled.load()) return;
    try {
        syncPacketHealth();
        tickAnim();
        submitHud();
    } catch (...) {
        // Never let HUD draw take down the game
    }
}

void onAttack(void* targetActor) {
    if (g_enabled.load() && g_showOnHit.load()) applyTarget(targetActor, true);
}

void shutdown() {
    g_enabled.store(false);
    pl::modmenu::submitDrawCommands(kModuleId, std::span<const pl::modmenu::DrawCommand>{});
}

} // namespace bactro::targethud
