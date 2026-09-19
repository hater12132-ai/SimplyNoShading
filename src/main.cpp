#include "Signatures.hpp"
#include "Version.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// Simply No Shading style: remove directional face darkening (0..1, 1 = full remove)
std::atomic_bool g_noShadeEnabled{true};
std::atomic<float> g_noShadeAmount{1.0f};

// Fullbright 0..10 (0 = vanilla, 10 = max light everywhere)
std::atomic<float> g_fullbright{0.0f};
std::atomic_bool g_hooksReady{false};

void* g_fullbrightTarget = nullptr;
uint8_t g_fullbrightOriginal[12]{};
bool g_fullbrightPatched = false;

// Vanilla-ish directional shade factors (Java/Bedrock block face shading).
// Down, Up, North, South, West, East
constexpr float kFaceShade[6] = {0.50f, 1.00f, 0.80f, 0.80f, 0.60f, 0.60f};

thread_local int g_activeFace = -1;

std::uintptr_t resolveOne(std::string_view pattern) {
    std::vector<std::string> patterns;
    patterns.emplace_back(std::string(pattern));
    const auto map = pl::memory::resolveSignatures(patterns, "libminecraftpe.so");
    const auto it = map.find(patterns[0]);
    return (it != map.end()) ? it->second : 0;
}

bool patchMemory(void* target, const void* data, size_t size) {
    if (!target || !data || size == 0) return false;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return false;
    const auto addr = reinterpret_cast<std::uintptr_t>(target);
    const auto page = reinterpret_cast<void*>(addr & ~(static_cast<std::uintptr_t>(pageSize) - 1));
    const size_t len = (addr + size) - reinterpret_cast<std::uintptr_t>(page) + pageSize;
    if (mprotect(page, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    std::memcpy(target, data, size);
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target) + size);
    return true;
}

void applyFullbrightPatch(bool enable) {
    if (!g_fullbrightTarget) return;
    if (enable && !g_fullbrightPatched) {
        // Same 12-byte stub BedrockTools uses: return max brightness, RET
        const uint8_t patch[12] = {
            0x40, 0x8F, 0xA8, 0x52,
            0x00, 0x00, 0x27, 0x1E,
            0xC0, 0x03, 0x5F, 0xD6
        };
        if (patchMemory(g_fullbrightTarget, patch, sizeof(patch))) {
            g_fullbrightPatched = true;
            LOGI("fullbright patch ON");
        }
    } else if (!enable && g_fullbrightPatched) {
        if (patchMemory(g_fullbrightTarget, g_fullbrightOriginal, 12)) {
            g_fullbrightPatched = false;
            LOGI("fullbright patch OFF");
        }
    }
}

void syncFullbrightFromSlider() {
    // Slider 0..10 — only force max light at 10; partial uses color path below.
    const float v = g_fullbright.load(std::memory_order_relaxed);
    applyFullbrightPatch(v >= 9.5f);
}

void applyNoShading(float& r, float& g, float& b) {
    if (!g_noShadeEnabled.load(std::memory_order_relaxed)) return;
    const int face = g_activeFace;
    if (face < 0 || face > 5) return;

    const float amount = std::clamp(g_noShadeAmount.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (amount <= 0.001f) return;

    // Undo directional face multiplier while keeping biome tint ratios.
    const float shade = kFaceShade[face];
    if (shade < 0.999f && shade > 0.001f) {
        const float inv = 1.0f / shade;
        const float blend = 1.0f + (inv - 1.0f) * amount; // amount=1 → full undo
        r = std::min(1.0f, r * blend);
        g = std::min(1.0f, g * blend);
        b = std::min(1.0f, b * blend);
    }
}

void applyFullbrightColor(float& r, float& g, float& b) {
    // Partial fullbright (slider 0..10) when not hard-patched: lift toward white.
    const float level = std::clamp(g_fullbright.load(std::memory_order_relaxed), 0.0f, 10.0f);
    if (level <= 0.01f || level >= 9.5f) return; // 10 uses memory patch
    const float t = level / 10.0f;
    r = r + (1.0f - r) * t;
    g = g + (1.0f - g) * t;
    b = b + (1.0f - b) * t;
}

void colorDetour(void* self, float r, float g, float b, float a) {
    applyNoShading(r, g, b);
    applyFullbrightColor(r, g, b);
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

    bool anyFace = false;
    for (int i = 0; i < 6; ++i) {
        if (!g_faces[i].installed)
            anyFace = installFace(i, patterns[i], detours[i]) || anyFace;
        else
            anyFace = true;
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

    if (!g_fullbrightTarget) {
        const auto addr = resolveOne(shadefix::sigs::Fullbright);
        if (!addr) {
            LOGE("Fullbright signature not found");
        } else {
            g_fullbrightTarget = reinterpret_cast<void*>(addr);
            std::memcpy(g_fullbrightOriginal, g_fullbrightTarget, 12);
            LOGI("Fullbright target @ %p", g_fullbrightTarget);
        }
    }

    syncFullbrightFromSlider();
    g_hooksReady.store(anyFace && g_colorInstalled, std::memory_order_release);
    return g_hooksReady.load(std::memory_order_relaxed);
}

void onToggle(std::string_view /*module_id*/, bool enabled) {
    g_noShadeEnabled.store(enabled, std::memory_order_release);
    if (enabled) installAll();
    if (!enabled) applyFullbrightPatch(false);
}

void onConfigChanged(std::string_view /*module_id*/, std::string_view key, std::string_view value) {
    try {
        if (key == "noShadeAmount") {
            g_noShadeAmount.store(std::clamp(std::stof(std::string(value)), 0.0f, 1.0f),
                                  std::memory_order_relaxed);
        } else if (key == "fullbright") {
            g_fullbright.store(std::clamp(std::stof(std::string(value)), 0.0f, 10.0f),
                               std::memory_order_relaxed);
            syncFullbrightFromSlider();
        }
    } catch (...) {
    }
}

void registerMenu() {
    pl::modmenu::ModuleBuilder builder("shadefix", "Simply No Shading");
    builder.description("Removes block face shading (Like Simply No Shading). Fullbright slider 0-10.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfigChanged);

    // 1 = fully undo directional face darkening (SNS default look)
    builder.config("noShadeAmount", "No shading strength", pl::modmenu::ConfigType::SliderFloat,
                   "1", "0", "1", "");
    // 0 = normal dark, 10 = full fullbright
    builder.config("fullbright", "Fullbright", pl::modmenu::ConfigType::SliderFloat,
                   "0", "0", "10", "");
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
        g_noShadeEnabled.store(true, std::memory_order_release);
        LOGI("enable hooks=%s", ok ? "ok" : "partial/fail");
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        g_noShadeEnabled.store(false, std::memory_order_release);
        applyFullbrightPatch(false);
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        g_noShadeEnabled.store(false, std::memory_order_release);
        applyFullbrightPatch(false);
        return true;
    }
};

PL_REGISTER_MOD(ShadeFixMod, ShadeFixMod::instance())
