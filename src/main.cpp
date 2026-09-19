#include "Signatures.hpp"
#include "Version.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ShadeFix", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ShadeFix", __VA_ARGS__)

namespace {

using FaceFn = void (*)(void*, void*, const void*, const void*, const void*);
using TessColorFn = void (*)(void*, float, float, float, float);

struct FaceHook {
    FaceFn original = nullptr;
    bool installed = false;
};

std::array<FaceHook, 6> g_faces{};
TessColorFn g_colorOriginal = nullptr;
bool g_colorInstalled = false;

std::atomic_bool g_enabled{false};
std::atomic<float> g_strength{0.45f};
std::atomic<float> g_sideBoost{0.20f};
std::atomic_bool g_affectUpDown{true};
std::atomic_bool g_affectSides{true};

thread_local int g_activeFace = -1;

std::uintptr_t resolveOne(std::string_view pattern) {
    std::vector<std::string> patterns;
    patterns.emplace_back(std::string(pattern));
    const auto map = pl::memory::resolveSignatures(patterns, "libminecraftpe.so");
    const auto it = map.find(patterns[0]);
    return (it != map.end()) ? it->second : 0;
}

void applyShade(float& r, float& g, float& b) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const int face = g_activeFace;
    if (face < 0) return;
    const bool side = face >= 2;
    if (side && !g_affectSides.load(std::memory_order_relaxed)) return;
    if (!side && !g_affectUpDown.load(std::memory_order_relaxed)) return;

    float strength = g_strength.load(std::memory_order_relaxed);
    if (side) strength += g_sideBoost.load(std::memory_order_relaxed);
    strength = std::clamp(strength, 0.0f, 1.0f);
    if (strength <= 0.001f) return;

    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum <= 0.0001f) return;
    const float lift = 1.0f + strength * (1.0f - std::clamp(lum, 0.0f, 1.0f));
    r = std::min(1.0f, r * lift);
    g = std::min(1.0f, g * lift);
    b = std::min(1.0f, b * lift);
}

void colorDetour(void* self, float r, float g, float b, float a) {
    applyShade(r, g, b);
    if (g_colorOriginal) g_colorOriginal(self, r, g, b, a);
}

template <int Face>
void faceDetour(void* self, void* a, const void* b, const void* c, const void* d) {
    const int prev = g_activeFace;
    g_activeFace = Face;
    if (g_faces[Face].original) g_faces[Face].original(self, a, b, c, d);
    g_activeFace = prev;
}

bool installFace(int i, std::string_view pattern, void* detour) {
    const auto addr = resolveOne(pattern);
    if (!addr) {
        LOGE("face %d signature not found", i);
        return false;
    }
    void* orig = nullptr;
    if (pl::memory::hook(reinterpret_cast<void*>(addr), detour, &orig) != 0) {
        LOGE("face %d hook failed", i);
        return false;
    }
    g_faces[i].original = reinterpret_cast<FaceFn>(orig);
    g_faces[i].installed = true;
    LOGI("face %d hooked @ %p", i, reinterpret_cast<void*>(addr));
    return true;
}

bool installAll() {
    static void* const detours[6] = {
        reinterpret_cast<void*>(&faceDetour<0>),
        reinterpret_cast<void*>(&faceDetour<1>),
        reinterpret_cast<void*>(&faceDetour<2>),
        reinterpret_cast<void*>(&faceDetour<3>),
        reinterpret_cast<void*>(&faceDetour<4>),
        reinterpret_cast<void*>(&faceDetour<5>),
    };
    static const std::string_view patterns[6] = {
        shadefix::sigs::BlockTessellatorTessellateFaceDown,
        shadefix::sigs::BlockTessellatorTessellateFaceUp,
        shadefix::sigs::BlockTessellatorTessellateFaceNorth,
        shadefix::sigs::BlockTessellatorTessellateFaceSouth,
        shadefix::sigs::BlockTessellatorTessellateFaceWest,
        shadefix::sigs::BlockTessellatorTessellateFaceEast,
    };

    bool any = false;
    for (int i = 0; i < 6; ++i) {
        if (!g_faces[i].installed)
            any = installFace(i, patterns[i], detours[i]) || any;
        else
            any = true;
    }

    if (!g_colorInstalled) {
        const auto addr = resolveOne(shadefix::sigs::TessellatorColor);
        if (!addr) {
            LOGE("TessellatorColor signature not found");
        } else {
            void* orig = nullptr;
            if (pl::memory::hook(reinterpret_cast<void*>(addr),
                                 reinterpret_cast<void*>(&colorDetour), &orig) == 0) {
                g_colorOriginal = reinterpret_cast<TessColorFn>(orig);
                g_colorInstalled = true;
                LOGI("TessellatorColor hooked @ %p", reinterpret_cast<void*>(addr));
            } else {
                LOGE("TessellatorColor hook failed");
            }
        }
    }

    return any && g_colorInstalled;
}

void registerMenu() {
    pl::modmenu::ModuleBuilder builder("shadefix", "Shade Fix");
    builder.description(std::string(shadefix::Description))
        .defaultEnabled(true)
        .onToggle([](std::string_view /*id*/, bool enabled) {
            g_enabled.store(enabled, std::memory_order_release);
            if (enabled) installAll();
        })
        .onConfigChanged([](std::string_view key, std::string_view value) {
            try {
                if (key == "strength") g_strength.store(std::stof(std::string(value)), std::memory_order_relaxed);
                else if (key == "sideBoost") g_sideBoost.store(std::stof(std::string(value)), std::memory_order_relaxed);
                else if (key == "affectUpDown") g_affectUpDown.store(value == "true" || value == "1", std::memory_order_relaxed);
                else if (key == "affectSides") g_affectSides.store(value == "true" || value == "1", std::memory_order_relaxed);
            } catch (...) {}
            return true;
        });

    builder.config("strength", "Strength", pl::modmenu::ConfigType::SliderFloat, "0.45", "0", "1");
    builder.config("sideBoost", "Side boost", pl::modmenu::ConfigType::SliderFloat, "0.2", "0", "1");
    builder.config("affectUpDown", "Affect up/down", pl::modmenu::ConfigType::Toggle, "true");
    builder.config("affectSides", "Affect sides", pl::modmenu::ConfigType::Toggle, "true");
    builder.registerModule();
}

} // namespace

class ShadeFixMod {
public:
    static ShadeFixMod& instance() {
        static ShadeFixMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        LOGI("load %s %s", shadefix::Name.data(), shadefix::Version.data());
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        registerMenu();
        const bool ok = installAll();
        g_enabled.store(ok, std::memory_order_release);
        LOGI("enable hooks=%s", ok ? "ok" : "partial/fail");
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        g_enabled.store(false, std::memory_order_release);
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        g_enabled.store(false, std::memory_order_release);
        return true;
    }
};

PL_REGISTER_MOD(ShadeFixMod, ShadeFixMod::instance())
