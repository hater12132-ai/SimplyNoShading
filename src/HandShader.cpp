#include "bactro/HandShader.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#define HS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

using GLint = int;
using GLsizei = int;
using GLfloat = float;

namespace bactro::handshader {
namespace {

constexpr const char* kModuleId = "bactro.handshader";

std::atomic_bool g_enabled{true};
std::atomic_bool g_rainbow{false};
std::atomic<float> g_brightness{1.4f};
std::atomic<float> g_tintR{0.55f};
std::atomic<float> g_tintG{1.0f};
std::atomic<float> g_tintB{1.35f};
std::atomic<float> g_opacity{1.0f};
std::atomic_bool g_hideVanillaHand{false};

std::atomic_bool g_inHandRender{false};
std::atomic_int g_uniformHits{0};

using RenderFirstPersonFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

using GlUniform4fFn = void (*)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
using GlUniform4fvFn = void (*)(GLint location, GLsizei count, const GLfloat* value);
GlUniform4fFn g_glUniform4f = nullptr;
GlUniform4fvFn g_glUniform4fv = nullptr;
bool g_glHooked = false;

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HS_LOGI("%s", buf);
}

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

void currentTint(float& r, float& g, float& b, float& a) {
    if (g_rainbow.load(std::memory_order_relaxed)) {
        static auto start = std::chrono::steady_clock::now();
        const float sec =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        hsvToRgb(std::fmod(sec * 0.2f, 1.f), 0.9f, 1.f, r, g, b);
    } else {
        r = g_tintR.load(std::memory_order_relaxed);
        g = g_tintG.load(std::memory_order_relaxed);
        b = g_tintB.load(std::memory_order_relaxed);
    }
    const float br = g_brightness.load(std::memory_order_relaxed);
    r *= br;
    g *= br;
    b *= br;
    a = g_opacity.load(std::memory_order_relaxed);
}

bool looksLikeColor(GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    auto ok = [](GLfloat x) { return x >= -0.05f && x <= 2.5f; };
    return ok(v0) && ok(v1) && ok(v2) && ok(v3) && (v3 >= 0.f && v3 <= 1.05f);
}

void applyTint(GLfloat& v0, GLfloat& v1, GLfloat& v2, GLfloat& v3) {
    if (!looksLikeColor(v0, v1, v2, v3)) return;
    float tr, tg, tb, ta;
    currentTint(tr, tg, tb, ta);
    v0 *= tr;
    v1 *= tg;
    v2 *= tb;
    v3 *= ta;
    const int n = g_uniformHits.fetch_add(1, std::memory_order_relaxed);
    if (n < 12) {
        logLine("HandShader: tint uniform #%d -> (%.2f,%.2f,%.2f,%.2f)", n, v0, v1, v2, v3);
    }
}

void glUniform4fDetour(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    if (g_enabled.load(std::memory_order_relaxed) && g_inHandRender.load(std::memory_order_acquire)) {
        applyTint(v0, v1, v2, v3);
    }
    if (g_glUniform4f) g_glUniform4f(location, v0, v1, v2, v3);
}

void glUniform4fvDetour(GLint location, GLsizei count, const GLfloat* value) {
    if (g_enabled.load(std::memory_order_relaxed) && g_inHandRender.load(std::memory_order_acquire) &&
        value && count > 0) {
        GLfloat tmp[4] = {value[0], value[1], value[2], value[3]};
        applyTint(tmp[0], tmp[1], tmp[2], tmp[3]);
        if (g_glUniform4fv) {
            if (count == 1) {
                g_glUniform4fv(location, 1, tmp);
                return;
            }
            if (count <= 16) {
                GLfloat buf[64];
                std::memcpy(buf, value, static_cast<size_t>(count) * 4 * sizeof(GLfloat));
                buf[0] = tmp[0];
                buf[1] = tmp[1];
                buf[2] = tmp[2];
                buf[3] = tmp[3];
                g_glUniform4fv(location, count, buf);
                return;
            }
        }
    }
    if (g_glUniform4fv) g_glUniform4fv(location, count, value);
}

void tryHookGles() {
    if (g_glHooked) return;
    void* lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv2.so.2", RTLD_NOW);
    if (!lib) {
        logLine("HandShader: libGLESv2 missing");
        return;
    }

    void* u4f = dlsym(lib, "glUniform4f");
    void* u4fv = dlsym(lib, "glUniform4fv");
    int ok = 0;

    if (u4f) {
        void* o = nullptr;
        if (pl::memory::hook(u4f, reinterpret_cast<void*>(&glUniform4fDetour), &o) == 0) {
            g_glUniform4f = reinterpret_cast<GlUniform4fFn>(o);
            ++ok;
        }
    }
    if (u4fv) {
        void* o = nullptr;
        if (pl::memory::hook(u4fv, reinterpret_cast<void*>(&glUniform4fvDetour), &o) == 0) {
            g_glUniform4fv = reinterpret_cast<GlUniform4fvFn>(o);
            ++ok;
        }
    }

    g_glHooked = ok > 0;
    logLine("HandShader: GLES color hooks %d/2", ok);
}

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);
        return;
    }

    if (g_hideVanillaHand.load(std::memory_order_relaxed)) {
        static int s_hideLog = 0;
        if (s_hideLog < 2) {
            logLine("HandShader: hideHand — skipped renderFirstPerson");
            ++s_hideLog;
        }
        return;
    }

    g_inHandRender.store(true, std::memory_order_release);
    if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    g_inHandRender.store(false, std::memory_order_release);

    static int s_log = 0;
    if (s_log < 3) {
        logLine("HandShader: renderFirstPerson ok (hits=%d)", g_uniformHits.load());
        ++s_log;
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
            logLine("HandShader: renderFirstPerson hook FAILED");
        }
    }
    tryHookGles();
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
    b.description("First-person hand/item color via GLES uniform hooks. Strong default cyan tint.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("brightness", "Brightness", pl::modmenu::ConfigType::SliderFloat, "1.4", "0.2", "3.0", "");
    b.config("tintR", "Tint red", pl::modmenu::ConfigType::SliderFloat, "0.55", "0", "2", "");
    b.config("tintG", "Tint green", pl::modmenu::ConfigType::SliderFloat, "1.0", "0", "2", "");
    b.config("tintB", "Tint blue", pl::modmenu::ConfigType::SliderFloat, "1.35", "0", "2", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "1", "");
    b.config("rainbow", "Rainbow tint", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("hideHand", "Hide hand / item", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() { tryInstallHooks(); }
void onFrame() {}
void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    g_inHandRender.store(false, std::memory_order_release);
}

} // namespace bactro::handshader
