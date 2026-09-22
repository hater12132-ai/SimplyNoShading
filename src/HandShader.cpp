#include "bactro/HandShader.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
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
using GLuint = unsigned int;

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
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
constexpr GLenum GL_TRIANGLE_FAN = 0x0006;

namespace bactro::handshader {
namespace {

constexpr const char* kModuleId = "bactro.handshader";

std::atomic_bool g_enabled{true};
std::atomic_bool g_hideVanillaHand{false};
std::atomic_bool g_glow{true};

// Stays true from renderFirstPerson until eglSwapBuffers (hand is drawn late / deferred).
std::atomic_bool g_handPhase{false};
std::atomic_int g_drawHits{0};
std::atomic_int g_glowHits{0};

// Hard cap on how many draw calls within one hand-phase window can be treated as "the hand".
// The window is intentionally wide (renderFirstPerson -> eglSwapBuffers) because the hand mesh
// is submitted late/deferred, but a wide window alone is unsafe: ordinary terrain is rendered
// as many small per-chunk-section draw calls that can also be under the vertex-count threshold,
// so without a budget the world ends up getting the additive glow + relaxed depth-func treatment
// too (this was the cause of chunks flickering/glowing — the "glitching world" bug). The hand +
// held item together are only ever a handful of draw calls (arm, item mesh, occasionally a
// second layer), so a small budget reset each time the hand phase starts is a safe backstop even
// if the window ends up wider than expected on a given frame.
constexpr int kMaxHandDrawsPerPhase = 6;
std::atomic_int g_handDrawBudget{0};

using RenderFirstPersonFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

using GlDrawElementsFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void* indices);
using GlDrawArraysFn = void (*)(GLenum mode, GLint first, GLsizei count);
using GlDrawElementsInstancedFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei primcount);
using GlDrawArraysInstancedFn = void (*)(GLenum mode, GLint first, GLsizei count, GLsizei primcount);
using GlDrawRangeElementsFn = void (*)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                       const void* indices);

using GlEnableFn = void (*)(GLenum);
using GlDisableFn = void (*)(GLenum);
using GlBlendFuncFn = void (*)(GLenum, GLenum);
using GlGetIntegervFn = void (*)(GLenum, GLint*);
using GlIsEnabledFn = GLboolean (*)(GLenum);
using GlDepthFuncFn = void (*)(GLenum);

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);

GlDrawElementsFn g_glDrawElements = nullptr;
GlDrawArraysFn g_glDrawArrays = nullptr;
GlDrawElementsInstancedFn g_glDrawElementsInstanced = nullptr;
GlDrawArraysInstancedFn g_glDrawArraysInstanced = nullptr;
GlDrawRangeElementsFn g_glDrawRangeElements = nullptr;

GlEnableFn g_glEnable = nullptr;
GlDisableFn g_glDisable = nullptr;
GlBlendFuncFn g_glBlendFunc = nullptr;
GlGetIntegervFn g_glGetIntegerv = nullptr;
GlIsEnabledFn g_glIsEnabled = nullptr;
GlDepthFuncFn g_glDepthFunc = nullptr;

EglSwapBuffersFn g_eglSwapBuffers = nullptr;
bool g_glHooked = false;
bool g_swapHooked = false;

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HS_LOGI("%s", buf);
}

void* resolveGl(const char* name) {
    // eglGetProcAddress returns the pointer the game actually calls (ANGLE / driver).
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    void* lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
    if (!lib) return nullptr;
    return dlsym(lib, name);
}

struct BlendSnap {
    GLboolean blendOn = 0;
    GLint srcRgb = GL_ONE, dstRgb = GL_ONE_MINUS_SRC_ALPHA;
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

bool isTriangleMode(GLenum mode) {
    return mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
}

// Only glow small-ish draws (hand/item), skip huge world chunks if phase is wide.
bool looksLikeHandDraw(GLsizei count) {
    // Hand/item meshes are small (typically a few dozen to a few hundred vertices).
    // 4000 was far too permissive — plenty of individual world-chunk-section draw calls
    // fall under that too, which is what let the glow leak onto terrain. Tightened to a
    // range that still comfortably covers hand + item geometry but excludes most chunk
    // batches. Combined with the per-phase draw budget below as a second safety net.
    return count > 0 && count < 1500;
}

// True only while we still have "hand draw" budget left for this phase. Consumed by the
// detours below; see kMaxHandDrawsPerPhase for why this exists.
bool hasHandDrawBudget() { return g_handDrawBudget.load(std::memory_order_relaxed) > 0; }

void applyGlowSecondPass(void (*drawOnce)(void*), void* ctx) {
    if (!g_glow.load(std::memory_order_relaxed) || !g_glEnable || !g_glBlendFunc) {
        drawOnce(ctx);
        return;
    }
    const BlendSnap snap = saveBlend();
    drawOnce(ctx); // normal
    g_glEnable(GL_BLEND);
    g_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    if (g_glDepthFunc) g_glDepthFunc(GL_LEQUAL);
    drawOnce(ctx); // additive
    restoreBlend(snap);
    g_handDrawBudget.fetch_sub(1, std::memory_order_relaxed);
    const int n = g_glowHits.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) logLine("HandShader: glow pass #%d", n);
}

struct DECtx {
    GLenum mode;
    GLsizei count;
    GLenum type;
    const void* indices;
};
struct DACtx {
    GLenum mode;
    GLint first;
    GLsizei count;
};
struct DEICtx {
    GLenum mode;
    GLsizei count;
    GLenum type;
    const void* indices;
    GLsizei primcount;
};
struct DAICtx {
    GLenum mode;
    GLint first;
    GLsizei count;
    GLsizei primcount;
};
struct DRECtx {
    GLenum mode;
    GLuint start;
    GLuint end;
    GLsizei count;
    GLenum type;
    const void* indices;
};

void doDE(void* p) {
    auto* c = static_cast<DECtx*>(p);
    if (g_glDrawElements) g_glDrawElements(c->mode, c->count, c->type, c->indices);
}
void doDA(void* p) {
    auto* c = static_cast<DACtx*>(p);
    if (g_glDrawArrays) g_glDrawArrays(c->mode, c->first, c->count);
}
void doDEI(void* p) {
    auto* c = static_cast<DEICtx*>(p);
    if (g_glDrawElementsInstanced)
        g_glDrawElementsInstanced(c->mode, c->count, c->type, c->indices, c->primcount);
}
void doDAI(void* p) {
    auto* c = static_cast<DAICtx*>(p);
    if (g_glDrawArraysInstanced) g_glDrawArraysInstanced(c->mode, c->first, c->count, c->primcount);
}
void doDRE(void* p) {
    auto* c = static_cast<DRECtx*>(p);
    if (g_glDrawRangeElements)
        g_glDrawRangeElements(c->mode, c->start, c->end, c->count, c->type, c->indices);
}

bool inHandGlowWindow() {
    return g_enabled.load(std::memory_order_relaxed) && g_handPhase.load(std::memory_order_acquire) &&
           !g_hideVanillaHand.load(std::memory_order_relaxed) && hasHandDrawBudget();
}

void glDrawElementsDetour(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (inHandGlowWindow() && isTriangleMode(mode) && looksLikeHandDraw(count)) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
        DECtx ctx{mode, count, type, indices};
        applyGlowSecondPass(&doDE, &ctx);
        return;
    }
    if (g_glDrawElements) g_glDrawElements(mode, count, type, indices);
}

void glDrawArraysDetour(GLenum mode, GLint first, GLsizei count) {
    if (inHandGlowWindow() && isTriangleMode(mode) && looksLikeHandDraw(count)) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
        DACtx ctx{mode, first, count};
        applyGlowSecondPass(&doDA, &ctx);
        return;
    }
    if (g_glDrawArrays) g_glDrawArrays(mode, first, count);
}

void glDrawElementsInstancedDetour(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                   GLsizei primcount) {
    if (inHandGlowWindow() && isTriangleMode(mode) && looksLikeHandDraw(count)) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
        DEICtx ctx{mode, count, type, indices, primcount};
        applyGlowSecondPass(&doDEI, &ctx);
        return;
    }
    if (g_glDrawElementsInstanced)
        g_glDrawElementsInstanced(mode, count, type, indices, primcount);
}

void glDrawArraysInstancedDetour(GLenum mode, GLint first, GLsizei count, GLsizei primcount) {
    if (inHandGlowWindow() && isTriangleMode(mode) && looksLikeHandDraw(count)) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
        DAICtx ctx{mode, first, count, primcount};
        applyGlowSecondPass(&doDAI, &ctx);
        return;
    }
    if (g_glDrawArraysInstanced) g_glDrawArraysInstanced(mode, first, count, primcount);
}

void glDrawRangeElementsDetour(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                               const void* indices) {
    if (inHandGlowWindow() && isTriangleMode(mode) && looksLikeHandDraw(count)) {
        g_drawHits.fetch_add(1, std::memory_order_relaxed);
        DRECtx ctx{mode, start, end, count, type, indices};
        applyGlowSecondPass(&doDRE, &ctx);
        return;
    }
    if (g_glDrawRangeElements) g_glDrawRangeElements(mode, start, end, count, type, indices);
}

EGLBoolean eglSwapBuffersDetour(EGLDisplay dpy, EGLSurface surface) {
    // End hand phase for this frame (hand was submitted earlier in the frame).
    g_handPhase.store(false, std::memory_order_release);
    g_handDrawBudget.store(0, std::memory_order_relaxed);
    return g_eglSwapBuffers ? g_eglSwapBuffers(dpy, surface) : EGL_FALSE;
}

bool hookSym(void* target, void* detour, void** original) {
    if (!target) return false;
    return pl::memory::hook(target, detour, original) == 0;
}

void tryHookGles() {
    if (g_glHooked) return;

    g_glEnable = reinterpret_cast<GlEnableFn>(resolveGl("glEnable"));
    g_glDisable = reinterpret_cast<GlDisableFn>(resolveGl("glDisable"));
    g_glBlendFunc = reinterpret_cast<GlBlendFuncFn>(resolveGl("glBlendFunc"));
    g_glGetIntegerv = reinterpret_cast<GlGetIntegervFn>(resolveGl("glGetIntegerv"));
    g_glIsEnabled = reinterpret_cast<GlIsEnabledFn>(resolveGl("glIsEnabled"));
    g_glDepthFunc = reinterpret_cast<GlDepthFuncFn>(resolveGl("glDepthFunc"));

    int ok = 0;
    void* o = nullptr;

    if (hookSym(resolveGl("glDrawElements"), reinterpret_cast<void*>(&glDrawElementsDetour), &o)) {
        g_glDrawElements = reinterpret_cast<GlDrawElementsFn>(o);
        ++ok;
    }
    o = nullptr;
    if (hookSym(resolveGl("glDrawArrays"), reinterpret_cast<void*>(&glDrawArraysDetour), &o)) {
        g_glDrawArrays = reinterpret_cast<GlDrawArraysFn>(o);
        ++ok;
    }
    o = nullptr;
    if (hookSym(resolveGl("glDrawElementsInstanced"), reinterpret_cast<void*>(&glDrawElementsInstancedDetour),
                &o)) {
        g_glDrawElementsInstanced = reinterpret_cast<GlDrawElementsInstancedFn>(o);
        ++ok;
    }
    o = nullptr;
    if (hookSym(resolveGl("glDrawArraysInstanced"), reinterpret_cast<void*>(&glDrawArraysInstancedDetour),
                &o)) {
        g_glDrawArraysInstanced = reinterpret_cast<GlDrawArraysInstancedFn>(o);
        ++ok;
    }
    o = nullptr;
    if (hookSym(resolveGl("glDrawRangeElements"), reinterpret_cast<void*>(&glDrawRangeElementsDetour), &o)) {
        g_glDrawRangeElements = reinterpret_cast<GlDrawRangeElementsFn>(o);
        ++ok;
    }

    // Extend phase until end of frame
    if (!g_swapHooked) {
        void* swap = reinterpret_cast<void*>(eglGetProcAddress("eglSwapBuffers"));
        if (!swap) {
            void* egl = dlopen("libEGL.so", RTLD_NOW);
            if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
            if (egl) swap = dlsym(egl, "eglSwapBuffers");
        }
        o = nullptr;
        if (hookSym(swap, reinterpret_cast<void*>(&eglSwapBuffersDetour), &o)) {
            g_eglSwapBuffers = reinterpret_cast<EglSwapBuffersFn>(o);
            g_swapHooked = true;
            logLine("HandShader: eglSwapBuffers hooked (hand phase until present)");
        } else {
            logLine("HandShader: eglSwapBuffers hook FAILED");
        }
    }

    g_glHooked = ok > 0;
    logLine("HandShader: draw hooks %d/5 glow=%d phase=until-swap", ok,
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

    // Stay active until eglSwapBuffers clears it (deferred draws), but only for a bounded
    // number of draw calls — see kMaxHandDrawsPerPhase.
    g_handDrawBudget.store(kMaxHandDrawsPerPhase, std::memory_order_relaxed);
    g_handPhase.store(true, std::memory_order_release);
    if (g_renderFpOriginal) g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    // do NOT clear handPhase here

    static int s_log = 0;
    if (s_log < 6) {
        logLine("HandShader: renderFP armed draws=%d glow=%d", g_drawHits.load(), g_glowHits.load());
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
    if (!enabled) {
        g_handPhase.store(false, std::memory_order_release);
        g_handDrawBudget.store(0, std::memory_order_relaxed);
    }
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "glow")
            g_glow.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "hideHand")
            g_hideVanillaHand.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Shader");
    b.description("Hand/item additive glow (draws after FP until swap). Hide hand. Not a vector outline.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("glow", "Hand glow", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("hideHand", "Hide hand / item", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() { tryInstallHooks(); }
void onFrame() {}
void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    g_handPhase.store(false, std::memory_order_release);
    g_handDrawBudget.store(0, std::memory_order_relaxed);
}

} // namespace bactro::handshader
