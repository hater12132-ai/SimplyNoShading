#include "bactro/HandShader.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#define HS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

using GLint = int;
using GLsizei = int;
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLbitfield = unsigned int;

// GLES2 constants we need
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_BLEND_SRC_RGB = 0x80C9;
constexpr GLenum GL_BLEND_DST_RGB = 0x80C8;
constexpr GLenum GL_BLEND_SRC_ALPHA = 0x80CB;
constexpr GLenum GL_BLEND_DST_ALPHA = 0x80CA;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_DEPTH_FUNC = 0x0B74;
constexpr GLenum GL_LEQUAL = 0x0203;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_FUNC_ADD = 0x8006;
constexpr GLenum GL_BLEND_EQUATION_RGB = 0x8009;
constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;
constexpr GLenum GL_STENCIL_TEST = 0x0B90;
constexpr GLenum GL_ALWAYS = 0x0207;
constexpr GLenum GL_KEEP = 0x1E00;
constexpr GLenum GL_REPLACE = 0x1E01;
constexpr GLenum GL_NOTEQUAL = 0x0205;
constexpr GLenum GL_EQUAL = 0x0202;
constexpr GLenum GL_STENCIL_BUFFER_BIT = 0x00000400;

namespace bactro::handshader {
namespace {

constexpr const char* kModuleId = "bactro.handshader";

std::atomic_bool g_enabled{true};
std::atomic_bool g_hideVanillaHand{false};
std::atomic_bool g_glow{true};
std::atomic<float> g_glowStrength{0.65f}; // 0..1 blend toward additive

std::atomic_bool g_handPhase{false};
std::atomic_int g_drawHits{0};
std::atomic_int g_glowHits{0};

using RenderFirstPersonFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

using GlDrawElementsFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void* indices);
using GlDrawArraysFn = void (*)(GLenum mode, GLint first, GLsizei count);
using GlEnableFn = void (*)(GLenum);
using GlDisableFn = void (*)(GLenum);
using GlBlendFuncFn = void (*)(GLenum, GLenum);
using GlBlendFuncSeparateFn = void (*)(GLenum, GLenum, GLenum, GLenum);
using GlGetIntegervFn = void (*)(GLenum, GLint*);
using GlIsEnabledFn = GLboolean (*)(GLenum);
using GlDepthFuncFn = void (*)(GLenum);
using GlColorMaskFn = void (*)(GLboolean, GLboolean, GLboolean, GLboolean);
using GlLineWidthFn = void (*)(GLfloat);

GlDrawElementsFn g_glDrawElements = nullptr;
GlDrawArraysFn g_glDrawArrays = nullptr;
GlEnableFn g_glEnable = nullptr;
GlDisableFn g_glDisable = nullptr;
GlBlendFuncFn g_glBlendFunc = nullptr;
GlGetIntegervFn g_glGetIntegerv = nullptr;
GlIsEnabledFn g_glIsEnabled = nullptr;
GlDepthFuncFn g_glDepthFunc = nullptr;
GlColorMaskFn g_glColorMask = nullptr;
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

struct BlendSnap {
    GLboolean blendOn = 0;
    GLint srcRgb = GL_ONE, dstRgb = GL_ONE_MINUS_SRC_ALPHA;
    GLint srcA = GL_ONE, dstA = GL_ONE_MINUS_SRC_ALPHA;
    GLint depthFunc = GL_LESS;
    GLboolean depthOn = 1;
};

BlendSnap saveBlend() {
    BlendSnap s{};
    if (g_glIsEnabled) {
        s.blendOn = g_glIsEnabled(GL_BLEND);
        s.depthOn = g_glIsEnabled(GL_DEPTH_TEST);
    }
    if (g_glGetIntegerv) {
        g_glGetIntegerv(GL_BLEND_SRC_RGB, &s.srcRgb);
        g_glGetIntegerv(GL_BLEND_DST_RGB, &s.dstRgb);
        g_glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.srcA);
        g_glGetIntegerv(GL_BLEND_DST_ALPHA, &s.dstA);
        g_glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    }
    return s;
}

void restoreBlend(const BlendSnap& s) {
    if (!g_glEnable || !g_glDisable || !g_glBlendFunc) return;
    if (s.blendOn) g_glEnable(GL_BLEND);
    else g_glDisable(GL_BLEND);
    g_glBlendFunc((GLenum)s.srcRgb, (GLenum)s.dstRgb);
    if (g_glDepthFunc) g_glDepthFunc((GLenum)s.depthFunc);
    if (s.depthOn) g_glEnable(GL_DEPTH_TEST);
    else g_glDisable(GL_DEPTH_TEST);
}

// Soft additive pass: draw again with ONE,ONE so the hand/item brightens at edges of coverage.
// Full state restored after — must not leave persistent GL state for the world.
void drawWithGlow(void (*drawFn)(void*), void* ctx) {
    if (!g_glow.load(std::memory_order_relaxed) || !g_glEnable || !g_glBlendFunc) {
        drawFn(ctx);
        return;
    }
    const BlendSnap snap = saveBlend();

    // Normal pass
    drawFn(ctx);

    // Additive glow pass (second draw of same geometry)
    g_glEnable(GL_BLEND);
    g_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    if (g_glDepthFunc) g_glDepthFunc(GL_LEQUAL);
    drawFn(ctx);

    restoreBlend(snap);
    const int n = g_glowHits.fetch_add(1, std::memory_order_relaxed);
    if (n < 6) logLine("HandShader: glow pass #%d", n);
}

struct DrawElementsCtx {
    GLenum mode;
    GLsizei count;
    GLenum type;
    const void* indices;
};
struct DrawArraysCtx {
    GLenum mode;
    GLint first;
    GLsizei count;
};

void doDrawElements(void* p) {
    auto* c = static_cast<DrawElementsCtx*>(p);
    if (g_glDrawElements) g_glDrawElements(c->mode, c->count, c->type, c->indices);
}
void doDrawArrays(void* p) {
    auto* c = static_cast<DrawArraysCtx*>(p);
    if (g_glDrawArrays) g_glDrawArrays(c->mode, c->first, c->count);
}

void glDrawElementsDetour(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_enabled.load(std::memory_order_relaxed) && g_handPhase.load(std::memory_order_acquire) &&
        !g_hideVanillaHand.load(std::memory_order_relaxed) && count > 0) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
        DrawElementsCtx ctx{mode, count, type, indices};
        drawWithGlow(&doDrawElements, &ctx);
        return;
    }
    if (g_glDrawElements) g_glDrawElements(mode, count, type, indices);
}

void glDrawArraysDetour(GLenum mode, GLint first, GLsizei count) {
    if (g_enabled.load(std::memory_order_relaxed) && g_handPhase.load(std::memory_order_acquire) &&
        !g_hideVanillaHand.load(std::memory_order_relaxed) && count > 0) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
        DrawArraysCtx ctx{mode, first, count};
        drawWithGlow(&doDrawArrays, &ctx);
        return;
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

    auto sym = [&](const char* n) { return dlsym(lib, n); };

    g_glEnable = reinterpret_cast<GlEnableFn>(sym("glEnable"));
    g_glDisable = reinterpret_cast<GlDisableFn>(sym("glDisable"));
    g_glBlendFunc = reinterpret_cast<GlBlendFuncFn>(sym("glBlendFunc"));
    g_glGetIntegerv = reinterpret_cast<GlGetIntegervFn>(sym("glGetIntegerv"));
    g_glIsEnabled = reinterpret_cast<GlIsEnabledFn>(sym("glIsEnabled"));
    g_glDepthFunc = reinterpret_cast<GlDepthFuncFn>(sym("glDepthFunc"));
    g_glColorMask = reinterpret_cast<GlColorMaskFn>(sym("glColorMask"));

    int ok = 0;
    if (void* p = sym("glDrawElements")) {
        void* o = nullptr;
        if (pl::memory::hook(p, reinterpret_cast<void*>(&glDrawElementsDetour), &o) == 0) {
            g_glDrawElements = reinterpret_cast<GlDrawElementsFn>(o);
            ++ok;
        }
    }
    if (void* p = sym("glDrawArrays")) {
        void* o = nullptr;
        if (pl::memory::hook(p, reinterpret_cast<void*>(&glDrawArraysDetour), &o) == 0) {
            g_glDrawArrays = reinterpret_cast<GlDrawArraysFn>(o);
            ++ok;
        }
    }

    g_glHooked = ok > 0;
    logLine("HandShader: draw hooks %d/2 glow=%d (state restored each draw)", ok,
            g_glBlendFunc && g_glEnable ? 1 : 0);
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

    g_handPhase.store(true, std::memory_order_release);
    if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    g_handPhase.store(false, std::memory_order_release);

    static int s_log = 0;
    if (s_log < 5) {
        logLine("HandShader: renderFP ok draws=%d glow=%d", g_drawHits.load(), g_glowHits.load());
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
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "glow")
            g_glow.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "glowStrength")
            g_glowStrength.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "hideHand")
            g_hideVanillaHand.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Shader");
    b.description(
         "Hand/item glow (additive second pass, GL state restored). "
         "True colored outline needs a custom shader — not available on this path. Hide hand works.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("glow", "Hand glow", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("glowStrength", "Glow strength", pl::modmenu::ConfigType::SliderFloat, "0.65", "0.1", "1", "");
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
