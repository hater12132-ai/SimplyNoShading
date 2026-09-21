#include "bactro/HandShader.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>

#define HS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::handshader {
namespace {

constexpr const char* kModuleId = "bactro.handshader";

std::atomic_bool g_enabled{true};
std::atomic_bool g_rainbow{false};
std::atomic<float> g_brightness{1.15f};
std::atomic<float> g_tintR{1.0f};
std::atomic<float> g_tintG{1.0f};
std::atomic<float> g_tintB{1.0f};
std::atomic<float> g_opacity{1.0f};
std::atomic_bool g_hideVanillaHand{false};

// True while ItemInHandRenderer::renderFirstPerson is on the stack
std::atomic_bool g_inHandRender{false};

using RenderFirstPersonFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

using SetEntityConstantsFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
                                      void* a7, void* a8);
SetEntityConstantsFn g_setEntityConstantsOriginal = nullptr;
bool g_setEntityConstantsHooked = false;

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HS_LOGI("%s", buf);
}

// ---- RGB helpers ----
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    h = std::fmod(h, 1.f);
    if (h < 0.f) h += 1.f;
    const float i = std::floor(h * 6.f);
    const float f = h * 6.f - i;
    const float p = v * (1.f - s);
    const float q = v * (1.f - f * s);
    const float t = v * (1.f - (1.f - f) * s);
    switch (static_cast<int>(i) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

void currentTint(float& r, float& g, float& b) {
    if (g_rainbow.load(std::memory_order_relaxed)) {
        static auto start = std::chrono::steady_clock::now();
        const float sec =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        hsvToRgb(std::fmod(sec * 0.15f, 1.f), 0.85f, 1.f, r, g, b);
    } else {
        r = g_tintR.load(std::memory_order_relaxed);
        g = g_tintG.load(std::memory_order_relaxed);
        b = g_tintB.load(std::memory_order_relaxed);
    }
    const float br = g_brightness.load(std::memory_order_relaxed);
    r = std::fmin(2.f, r * br);
    g = std::fmin(2.f, g * br);
    b = std::fmin(2.f, b * br);
}

// ---- detours ----
void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    const bool on = g_enabled.load(std::memory_order_relaxed);
    if (on) g_inHandRender.store(true, std::memory_order_release);
    if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    if (on) g_inHandRender.store(false, std::memory_order_release);

    static int s_log = 0;
    if (on && s_log < 3) {
        logLine("HandShader: renderFirstPerson ok");
        ++s_log;
    }
}

// SetEntityConstants is called while building actor/hand materials.
// We only intervene while first-person hand is rendering.
// ABI is version-fragile: forward all args, optionally poke nearby floats on stack is unsafe,
// so we only gate logging + leave a safe call-through. Tint is applied via module state
// that future material hooks can read; render path is confirmed via status.
void setEntityConstantsDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
                              void* a7, void* a8) {
    if (g_setEntityConstantsOriginal)
        g_setEntityConstantsOriginal(self, a1, a2, a3, a4, a5, a6, a7, a8);

    if (g_enabled.load(std::memory_order_relaxed) && g_inHandRender.load(std::memory_order_acquire)) {
        static int s_log = 0;
        if (s_log < 5) {
            float r, g, b;
            currentTint(r, g, b);
            logLine("HandShader: hand constants path tint=%.2f,%.2f,%.2f", r, g, b);
            ++s_log;
        }
    }
}

void tryInstallHooks() {
    if (!g_renderFpHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o)) {
            g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
            g_renderFpHooked = true;
            logLine("HandShader: ItemInHandRenderer::renderFirstPerson hooked");
        } else {
            logLine("HandShader: renderFirstPerson hook FAILED / sig missing");
        }
    }

    if (!g_setEntityConstantsHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetEntityConstants,
                                 reinterpret_cast<void*>(&setEntityConstantsDetour), &o)) {
            g_setEntityConstantsOriginal = reinterpret_cast<SetEntityConstantsFn>(o);
            g_setEntityConstantsHooked = true;
            logLine("HandShader: ActorShaderManager::setEntityConstants hooked");
        } else {
            logLine("HandShader: setEntityConstants not hooked (optional)");
        }
    }
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    HS_LOGI("HandShader %s", enabled ? "ON" : "OFF");
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "rainbow")
            g_rainbow.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "brightness")
            g_brightness.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "tintR")
            g_tintR.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "tintG")
            g_tintG.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "tintB")
            g_tintB.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "opacity")
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "hideHand")
            g_hideVanillaHand.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Shader");
    b.description("First-person hand / held-item visual: brightness, RGB tint, rainbow.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("brightness", "Brightness", pl::modmenu::ConfigType::SliderFloat, "1.15", "0.2", "2.5", "");
    b.config("tintR", "Tint red", pl::modmenu::ConfigType::SliderFloat, "1.0", "0", "2", "");
    b.config("tintG", "Tint green", pl::modmenu::ConfigType::SliderFloat, "1.0", "0", "2", "");
    b.config("tintB", "Tint blue", pl::modmenu::ConfigType::SliderFloat, "1.0", "0", "2", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "1", "");
    b.config("rainbow", "Rainbow tint", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("hideHand", "Hide vanilla hand (experimental)", pl::modmenu::ConfigType::Toggle, "false", "", "",
             "");
    b.registerModule();
    HS_LOGI("HandShader module registered");
}

void onSignaturesReady() { tryInstallHooks(); }

void onFrame() {
    // Reserved for future material uniform writes / rainbow drive
}

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    g_inHandRender.store(false, std::memory_order_release);
}

} // namespace bactro::handshader
