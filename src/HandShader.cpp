#include "bactro/HandShader.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#define HS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLenum = unsigned int;
using GLuint = unsigned int;

namespace bactro::handshader {
namespace {

constexpr const char* kModuleId = "bactro.handshader";

std::atomic_bool g_enabled{true};
std::atomic_bool g_hideVanillaHand{false};
std::atomic<float> g_handScale{1.25f};
std::atomic<float> g_handOffsetX{0.0f};
std::atomic<float> g_handOffsetY{0.0f};
std::atomic<float> g_handOffsetZ{0.0f};

// True from first-person hand render until next buffer swap (hand is usually drawn late).
std::atomic_bool g_handPhase{false};
std::atomic_int g_matrixHits{0};
std::atomic_int g_drawHits{0};

using RenderFirstPersonFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

using GlUniformMatrix4fvFn = void (*)(GLint location, GLsizei count, unsigned char transpose, const GLfloat* value);
using GlDrawElementsFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void* indices);
using GlDrawArraysFn = void (*)(GLenum mode, GLint first, GLsizei count);

GlUniformMatrix4fvFn g_glUniformMatrix4fv = nullptr;
GlDrawElementsFn g_glDrawElements = nullptr;
GlDrawArraysFn g_glDrawArrays = nullptr;
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

// Scale + translate a 4x4 column-major matrix in-place (view / model matrix).
void transformMatrix(GLfloat* m) {
    if (!m) return;
    const float s = g_handScale.load(std::memory_order_relaxed);
    const float ox = g_handOffsetX.load(std::memory_order_relaxed);
    const float oy = g_handOffsetY.load(std::memory_order_relaxed);
    const float oz = g_handOffsetZ.load(std::memory_order_relaxed);

    // Scale basis vectors (columns 0..2)
    if (std::fabs(s - 1.f) > 0.001f) {
        for (int col = 0; col < 3; ++col) {
            m[col * 4 + 0] *= s;
            m[col * 4 + 1] *= s;
            m[col * 4 + 2] *= s;
        }
    }
    // Translate (column 3)
    m[12] += ox;
    m[13] += oy;
    m[14] += oz;
}

void glUniformMatrix4fvDetour(GLint location, GLsizei count, unsigned char transpose, const GLfloat* value) {
    if (g_enabled.load(std::memory_order_relaxed) && g_handPhase.load(std::memory_order_acquire) && value &&
        count > 0 && !g_hideVanillaHand.load(std::memory_order_relaxed)) {
        // Only touch the first matrix (model/view). Copy then transform.
        GLfloat tmp[16];
        std::memcpy(tmp, value, 16 * sizeof(GLfloat));
        transformMatrix(tmp);
        const int n = g_matrixHits.fetch_add(1, std::memory_order_relaxed);
        if (n < 6) {
            logLine("HandShader: matrix scale=%.2f hit #%d loc=%d", g_handScale.load(), n, (int)location);
        }
        if (g_glUniformMatrix4fv) {
            if (count == 1) {
                g_glUniformMatrix4fv(location, 1, transpose, tmp);
                return;
            }
            // count > 1: first matrix transformed, rest copied
            if (count <= 4) {
                GLfloat buf[64];
                std::memcpy(buf, value, static_cast<size_t>(count) * 16 * sizeof(GLfloat));
                std::memcpy(buf, tmp, 16 * sizeof(GLfloat));
                g_glUniformMatrix4fv(location, count, transpose, buf);
                return;
            }
        }
    }
    if (g_glUniformMatrix4fv) g_glUniformMatrix4fv(location, count, transpose, value);
}

void glDrawElementsDetour(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_handPhase.load(std::memory_order_acquire)) {
        const int n = g_drawHits.fetch_add(1, std::memory_order_relaxed);
        if (n < 4) logLine("HandShader: drawElements in hand phase #%d count=%d", n, (int)count);
    }
    if (g_glDrawElements) g_glDrawElements(mode, count, type, indices);
}

void glDrawArraysDetour(GLenum mode, GLint first, GLsizei count) {
    if (g_handPhase.load(std::memory_order_acquire)) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_glDrawArrays) g_glDrawArrays(mode, first, count);
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

    int ok = 0;
    if (void* p = dlsym(lib, "glUniformMatrix4fv")) {
        void* o = nullptr;
        if (pl::memory::hook(p, reinterpret_cast<void*>(&glUniformMatrix4fvDetour), &o) == 0) {
            g_glUniformMatrix4fv = reinterpret_cast<GlUniformMatrix4fvFn>(o);
            ++ok;
        }
    }
    if (void* p = dlsym(lib, "glDrawElements")) {
        void* o = nullptr;
        if (pl::memory::hook(p, reinterpret_cast<void*>(&glDrawElementsDetour), &o) == 0) {
            g_glDrawElements = reinterpret_cast<GlDrawElementsFn>(o);
            ++ok;
        }
    }
    if (void* p = dlsym(lib, "glDrawArrays")) {
        void* o = nullptr;
        if (pl::memory::hook(p, reinterpret_cast<void*>(&glDrawArraysDetour), &o) == 0) {
            g_glDrawArrays = reinterpret_cast<GlDrawArraysFn>(o);
            ++ok;
        }
    }

    g_glHooked = ok > 0;
    logLine("HandShader: GLES matrix/draw hooks %d/3 (no color tint)", ok);
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

    // Hand phase stays on until end of this call. Matrix/draw hooks only act while set.
    // (Uniforms for hand are often set inside this call tree even if color vec4s are not.)
    g_handPhase.store(true, std::memory_order_release);
    if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    g_handPhase.store(false, std::memory_order_release);

    static int s_log = 0;
    if (s_log < 5) {
        logLine("HandShader: renderFP ok matrix=%d draw=%d scale=%.2f", g_matrixHits.load(),
                g_drawHits.load(), g_handScale.load());
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
        if (key == "handScale")
            g_handScale.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "offsetX")
            g_handOffsetX.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "offsetY")
            g_handOffsetY.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "offsetZ")
            g_handOffsetZ.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "hideHand")
            g_hideVanillaHand.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Shader");
    b.description("Viewmodel scale/offset for first-person hand. Color tint removed (broke world). Hide hand works.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("handScale", "Hand scale", pl::modmenu::ConfigType::SliderFloat, "1.25", "0.4", "2.5", "");
    b.config("offsetX", "Offset X", pl::modmenu::ConfigType::SliderFloat, "0", "-1", "1", "");
    b.config("offsetY", "Offset Y", pl::modmenu::ConfigType::SliderFloat, "0", "-1", "1", "");
    b.config("offsetZ", "Offset Z", pl::modmenu::ConfigType::SliderFloat, "0", "-1", "1", "");
    b.config("hideHand", "Hide hand / item", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() { tryInstallHooks(); }
void onFrame() {}
void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    g_handPhase.store(false, std::memory_order_release);
}

} // namespace bactro::handshader
