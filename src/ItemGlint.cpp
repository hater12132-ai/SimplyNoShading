#include "bactro/ItemGlint.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>

#define IG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::itemglint {
namespace {

// Matches Bedrock Color { r,g,b,a } floats used by ActorShaderManager.
struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.itemglint";

std::atomic_bool g_enabled{true};
// Pure white outline/glint (user request). Opacity scales the effect.
std::atomic<float> g_opacity{1.0f};
std::atomic<float> g_intensity{1.0f}; // multiplies RGB before opacity

Color makeWhiteGlint() {
    const float op = g_opacity.load(std::memory_order_relaxed);
    const float inten = g_intensity.load(std::memory_order_relaxed);
    // Pre-multiplied style like BedrockTools GlintColorModule
    const float c = inten * op;
    return {c, c, c, op};
}

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    IG_LOGI("%s", buf);
}

// ---- ABI from BedrockToolsPlus GlintColorModule (1.26.x) ----

using SetEntityConstantsFn = void (*)(
    void*, void*, const Color*, const void*, const void*, const Color*, const Color*, const Color*,
    const Color*, const void*, const void*, float, float, float, float);

using SetupActorGlintFn = void (*)(
    void*, void*, void*, const Color*, const Color*, const Color*, const Color*, float, float, float,
    float, const void*, const void*, float, std::uint8_t, const void*);

using SetupFoilFn = void (*)(void*, const Color*, const Color*, const Color*, const void*);

using SetupGlintFn = void (*)(
    void*, const Color*, const Color*, const Color*, const Color*, float, float, float, float,
    const void*, const void*, float);

SetEntityConstantsFn g_setEntityConstants = nullptr;
SetupActorGlintFn g_setupActorGlint = nullptr;
SetupFoilFn g_setupFoil = nullptr;
SetupGlintFn g_setupGlint = nullptr;

bool g_hookedEntity = false;
bool g_hookedActor = false;
bool g_hookedFoil = false;
bool g_hookedGlint = false;

void setEntityConstantsDetour(
    void* entityConstants, void* renderContext, const Color* tileLightColor, const void* tileLightColorUV,
    const void* blockLightColor, const Color* overlay, const Color* changeColor, const Color* changeColor2,
    const Color* glintColor, const void* glintUVScale, const void* uvAnim, float uvOffset1, float uvOffset2,
    float uvRot1, float uvRot2) {
    if (!g_setEntityConstants) return;
    if (g_enabled.load(std::memory_order_relaxed)) {
        const Color custom = makeWhiteGlint();
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, &custom, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                         overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                         uvOffset2, uvRot1, uvRot2);
}

void setupActorGlintDetour(
    void* screenContext, void* entityContext, void* actor, const Color* overlay, const Color* changeColor,
    const Color* changeColor2, const Color* glintColor, float uvOffset1, float uvOffset2, float uvRot1,
    float uvRot2, const void* glintUVScale, const void* uvAnim, float br, std::uint8_t lightEmission,
    const void* lightEmissionColor) {
    if (!g_setupActorGlint) return;
    if (g_enabled.load(std::memory_order_relaxed)) {
        const Color custom = makeWhiteGlint();
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, &custom,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, glintUVScale, uvAnim, br, lightEmission,
                          lightEmissionColor);
        return;
    }
    g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                      uvOffset1, uvOffset2, uvRot1, uvRot2, glintUVScale, uvAnim, br, lightEmission,
                      lightEmissionColor);
}

void setupFoilDetour(void* screenContext, const Color* overlay, const Color* changeColor,
                     const Color* changeColor2, const void* uvScale) {
    if (!g_setupFoil) return;
    // Foil path is the enchanted-item overlay; force call through (color applied via other hooks).
    g_setupFoil(screenContext, overlay, changeColor, changeColor2, uvScale);
    static int s_log = 0;
    if (g_enabled.load(std::memory_order_relaxed) && s_log < 4) {
        logLine("ItemGlint: foil path hit");
        ++s_log;
    }
}

void setupGlintDetour(void* screenContext, const Color* overlay, const Color* changeColor,
                      const Color* changeColor2, const Color* glintColor, float uvOffset1, float uvOffset2,
                      float uvRot1, float uvRot2, const void* glintUVScale, const void* uvAnim, float br) {
    if (!g_setupGlint) return;
    if (g_enabled.load(std::memory_order_relaxed)) {
        const Color custom = makeWhiteGlint();
        g_setupGlint(screenContext, overlay, changeColor, changeColor2, &custom, uvOffset1, uvOffset2, uvRot1,
                     uvRot2, glintUVScale, uvAnim, br);
        static int s_log = 0;
        if (s_log < 4) {
            logLine("ItemGlint: white glint applied");
            ++s_log;
        }
        return;
    }
    g_setupGlint(screenContext, overlay, changeColor, changeColor2, glintColor, uvOffset1, uvOffset2, uvRot1,
                 uvRot2, glintUVScale, uvAnim, br);
}

void tryInstallHooks() {
    void* o = nullptr;

    if (!g_hookedEntity) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetEntityConstants,
                                 reinterpret_cast<void*>(&setEntityConstantsDetour), &o)) {
            g_setEntityConstants = reinterpret_cast<SetEntityConstantsFn>(o);
            g_hookedEntity = true;
            logLine("ItemGlint: SetEntityConstants hooked");
        } else {
            logLine("ItemGlint: SetEntityConstants hook FAIL");
        }
    }

    if (!g_hookedActor) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                                 reinterpret_cast<void*>(&setupActorGlintDetour), &o)) {
            g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
            g_hookedActor = true;
            logLine("ItemGlint: ActorGlint hooked");
        } else {
            logLine("ItemGlint: ActorGlint hook FAIL");
        }
    }

    if (!g_hookedFoil) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupFoilShaderParameters,
                                 reinterpret_cast<void*>(&setupFoilDetour), &o)) {
            g_setupFoil = reinterpret_cast<SetupFoilFn>(o);
            g_hookedFoil = true;
            logLine("ItemGlint: Foil hooked");
        } else {
            logLine("ItemGlint: Foil hook FAIL");
        }
    }

    if (!g_hookedGlint) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersGlint,
                                 reinterpret_cast<void*>(&setupGlintDetour), &o)) {
            g_setupGlint = reinterpret_cast<SetupGlintFn>(o);
            g_hookedGlint = true;
            logLine("ItemGlint: Glint hooked");
        } else {
            logLine("ItemGlint: Glint hook FAIL");
        }
    }

    logLine("ItemGlint: hooks entity=%d actor=%d foil=%d glint=%d", g_hookedEntity ? 1 : 0,
            g_hookedActor ? 1 : 0, g_hookedFoil ? 1 : 0, g_hookedGlint ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    IG_LOGI("ItemGlint %s", enabled ? "ON" : "OFF");
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "opacity")
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "intensity")
            g_intensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Item Glint");
    b.description(
         "White glowing enchantment-style glint/outline on items (engine foil/glint shaders). "
         "Strongest on enchanted items; forces white glint color wherever the game draws glint.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("opacity", "Glint opacity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.2", "1", "");
    b.config("intensity", "Glint intensity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.3", "2", "");
    b.registerModule();
}

void onSignaturesReady() { tryInstallHooks(); }

void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::itemglint
