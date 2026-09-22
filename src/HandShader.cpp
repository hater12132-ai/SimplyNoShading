#include "bactro/HandShader.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

#define HS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::handshader {
namespace {

constexpr const char* kModuleId = "bactro.handshader";

std::atomic_bool g_enabled{true};
std::atomic_bool g_hideVanillaHand{false};
std::atomic_int g_fpCalls{0};

using RenderFirstPersonFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HS_LOGI("%s", buf);
}

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);
        return;
    }

    if (g_hideVanillaHand.load(std::memory_order_relaxed)) {
        static int s_hideLog = 0;
        if (s_hideLog < 3) {
            logLine("HandShader: hideHand — skipped renderFirstPerson");
            ++s_hideLog;
        }
        return;
    }

    if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);

    const int n = g_fpCalls.fetch_add(1, std::memory_order_relaxed);
    if (n < 4) logLine("HandShader: renderFirstPerson ok (#%d) — glow disabled (unsafe on this path)", n);
}

void tryInstallHooks() {
    if (g_renderFpHooked) return;
    void* o = nullptr;
    if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                             reinterpret_cast<void*>(&renderFirstPersonDetour), &o)) {
        g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
        g_renderFpHooked = true;
        logLine("HandShader: renderFirstPerson hooked (hide-only, no GLES glow)");
    } else {
        logLine("HandShader: renderFirstPerson hook FAILED");
    }
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "hideHand")
            g_hideVanillaHand.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Shader");
    b.description(
         "Hide first-person hand/item. "
         "Glow/outline via GLES was removed: deferred draws cannot be told apart from the world, "
         "so additive glow always leaked and glitched terrain.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("hideHand", "Hide hand / item", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() { tryInstallHooks(); }
void onFrame() {}
void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::handshader
