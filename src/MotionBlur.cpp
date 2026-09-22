#include "bactro/MotionBlur.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#define MB_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

// ---- GLES2 types / constants (avoid requiring system GLES headers) ----
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLbitfield = unsigned int;
using GLchar = char;

constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE1 = 0x84C1;
constexpr GLenum GL_TEXTURE10 = 0x84CA;
constexpr GLenum GL_TEXTURE11 = 0x84CB;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x00004000;

namespace bactro::motionblur {
namespace {

constexpr const char* kModuleId = "bactro.motionblur";

// Natural Motion Blur–style: accumulate history toward current with strength-weighted mix.
// Visual goal: smooth camera motion without stroboscopic stutter (frame blending mode).
std::atomic_bool g_enabled{true};
std::atomic<float> g_strength{1.0f};   // NMB default ~1
std::atomic<float> g_opacity{0.85f};   // how much blur shows vs sharp
std::atomic_bool g_fpsScale{true};     // refresh-rate style scaling

std::mutex g_glMu;
bool g_glReady = false;
bool g_hasHistory = false;
GLuint g_prog = 0;
GLuint g_texCurrent = 0;
GLuint g_texHistory = 0;
GLuint g_vbo = 0;
GLuint g_ibo = 0;
GLint g_aPos = -1, g_aUv = -1;
GLint g_uCur = -1, g_uHist = -1, g_uBlend = -1, g_uOpacity = -1;
int g_texW = 0, g_texH = 0;

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
EglSwapBuffersFn g_swapOriginal = nullptr;
bool g_swapHooked = false;

// GLES function pointers
using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGenTextures = void (*)(GLsizei, GLuint*);
using PFN_glBindTexture = void (*)(GLenum, GLuint);
using PFN_glTexParameteri = void (*)(GLenum, GLenum, GLint);
using PFN_glTexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFN_glCopyTexSubImage2D = void (*)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBufferData = void (*)(GLenum, GLsizei, const void*, GLenum);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glActiveTexture = void (*)(GLenum);
using PFN_glUniform1i = void (*)(GLint, GLint);
using PFN_glUniform1f = void (*)(GLint, GLfloat);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glDrawElements = void (*)(GLenum, GLsizei, GLenum, const void*);
using PFN_glViewport = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glDisable = void (*)(GLenum);
using PFN_glEnable = void (*)(GLenum);
using PFN_glGetIntegerv = void (*)(GLenum, GLint*);
using PFN_glDeleteShader = void (*)(GLuint);

PFN_glCreateShader p_glCreateShader = nullptr;
PFN_glShaderSource p_glShaderSource = nullptr;
PFN_glCompileShader p_glCompileShader = nullptr;
PFN_glGetShaderiv p_glGetShaderiv = nullptr;
PFN_glCreateProgram p_glCreateProgram = nullptr;
PFN_glAttachShader p_glAttachShader = nullptr;
PFN_glLinkProgram p_glLinkProgram = nullptr;
PFN_glGetProgramiv p_glGetProgramiv = nullptr;
PFN_glGetAttribLocation p_glGetAttribLocation = nullptr;
PFN_glGetUniformLocation p_glGetUniformLocation = nullptr;
PFN_glGenTextures p_glGenTextures = nullptr;
PFN_glBindTexture p_glBindTexture = nullptr;
PFN_glTexParameteri p_glTexParameteri = nullptr;
PFN_glTexImage2D p_glTexImage2D = nullptr;
PFN_glCopyTexSubImage2D p_glCopyTexSubImage2D = nullptr;
PFN_glGenBuffers p_glGenBuffers = nullptr;
PFN_glBindBuffer p_glBindBuffer = nullptr;
PFN_glBufferData p_glBufferData = nullptr;
PFN_glUseProgram p_glUseProgram = nullptr;
PFN_glActiveTexture p_glActiveTexture = nullptr;
PFN_glUniform1i p_glUniform1i = nullptr;
PFN_glUniform1f p_glUniform1f = nullptr;
PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray = nullptr;
PFN_glVertexAttribPointer p_glVertexAttribPointer = nullptr;
PFN_glDrawElements p_glDrawElements = nullptr;
PFN_glViewport p_glViewport = nullptr;
PFN_glDisable p_glDisable = nullptr;
PFN_glEnable p_glEnable = nullptr;
PFN_glGetIntegerv p_glGetIntegerv = nullptr;
PFN_glDeleteShader p_glDeleteShader = nullptr;

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    MB_LOGI("%s", buf);
}

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    void* lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
    return lib ? dlsym(lib, name) : nullptr;
}

#define LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(glProc(#name))

bool loadGles() {
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetAttribLocation);
    LOAD(glGetUniformLocation);
    LOAD(glGenTextures);
    LOAD(glBindTexture);
    LOAD(glTexParameteri);
    LOAD(glTexImage2D);
    LOAD(glCopyTexSubImage2D);
    LOAD(glGenBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glUseProgram);
    LOAD(glActiveTexture);
    LOAD(glUniform1i);
    LOAD(glUniform1f);
    LOAD(glEnableVertexAttribArray);
    LOAD(glVertexAttribPointer);
    LOAD(glDrawElements);
    LOAD(glViewport);
    LOAD(glDisable);
    LOAD(glEnable);
    LOAD(glGetIntegerv);
    LOAD(glDeleteShader);
    return p_glCreateShader && p_glUseProgram && p_glCopyTexSubImage2D && p_glDrawElements;
}

// NMB-inspired fragment: linear-ish mix of current + history, strength controls ghosting.
static constexpr const char* kVS = R"(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vUv;
void main() {
    gl_Position = aPosition;
    vUv = aTexCoord;
}
)";

// Frame-blending similar to Natural Motion Blur accumulation / mix:
// history is previous displayed frame; we mix in linear-ish space then return.
static constexpr const char* kFS = R"(
precision mediump float;
varying vec2 vUv;
uniform sampler2D uCurrent;
uniform sampler2D uHistory;
uniform float uBlend;   // 0 = sharp, closer to 1 = stronger temporal blur
uniform float uOpacity; // overall effect amount
void main() {
    vec3 cur = texture2D(uCurrent, vUv).rgb;
    vec3 hist = texture2D(uHistory, vUv).rgb;
    // cheap sRGB <-> linear
    vec3 curL = cur * cur;
    vec3 histL = hist * hist;
    vec3 blurL = mix(curL, histL, uBlend);
    vec3 blur = sqrt(max(blurL, vec3(0.0)));
    gl_FragColor = vec4(mix(cur, blur, uOpacity), 1.0);
}
)";

bool compileShader(GLenum type, const char* src, GLuint& out) {
    out = p_glCreateShader(type);
    p_glShaderSource(out, 1, &src, nullptr);
    p_glCompileShader(out);
    GLint ok = 0;
    p_glGetShaderiv(out, GL_COMPILE_STATUS, &ok);
    return ok != 0;
}

bool initGl() {
    if (g_glReady) return true;
    if (!loadGles()) {
        logLine("MotionBlur: GLES procs missing");
        return false;
    }
    GLuint vs = 0, fs = 0;
    if (!compileShader(GL_VERTEX_SHADER, kVS, vs) || !compileShader(GL_FRAGMENT_SHADER, kFS, fs)) {
        logLine("MotionBlur: shader compile failed");
        return false;
    }
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glLinkProgram(g_prog);
    GLint linked = 0;
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!linked) {
        logLine("MotionBlur: shader link failed");
        return false;
    }

    g_aPos = p_glGetAttribLocation(g_prog, "aPosition");
    g_aUv = p_glGetAttribLocation(g_prog, "aTexCoord");
    g_uCur = p_glGetUniformLocation(g_prog, "uCurrent");
    g_uHist = p_glGetUniformLocation(g_prog, "uHistory");
    g_uBlend = p_glGetUniformLocation(g_prog, "uBlend");
    g_uOpacity = p_glGetUniformLocation(g_prog, "uOpacity");

    p_glGenTextures(1, &g_texCurrent);
    p_glGenTextures(1, &g_texHistory);
    for (GLuint t : {g_texCurrent, g_texHistory}) {
        p_glBindTexture(GL_TEXTURE_2D, t);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Fullscreen quad (pos.xy, uv.xy)
    const GLfloat verts[] = {
        -1.f, 1.f, 0.f, 1.f, -1.f, -1.f, 0.f, 0.f, 1.f, -1.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f,
    };
    const unsigned short idx[] = {0, 1, 2, 0, 2, 3};
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    p_glGenBuffers(1, &g_ibo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    g_glReady = true;
    logLine("MotionBlur: GL ready (NMB-style frame blend)");
    return true;
}

void ensureSize(int w, int h) {
    if (w == g_texW && h == g_texH) return;
    g_texW = w;
    g_texH = h;
    g_hasHistory = false;
    p_glBindTexture(GL_TEXTURE_2D, g_texCurrent);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    p_glBindTexture(GL_TEXTURE_2D, g_texHistory);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

float computeBlend() {
    // Map strength (0..2) → blend factor. NMB default strength 1 ≈ moderate trail.
    float s = g_strength.load(std::memory_order_relaxed);
    if (s < 0.f) s = 0.f;
    if (s > 2.f) s = 2.f;
    // Higher FPS → slightly stronger trail (refresh-rate scaling approximation)
    float fpsFactor = 1.f;
    if (g_fpsScale.load(std::memory_order_relaxed)) {
        static auto last = std::chrono::steady_clock::now();
        static float emaDt = 1.f / 60.f;
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.001f && dt < 0.1f) emaDt = emaDt * 0.9f + dt * 0.1f;
        const float fps = 1.f / std::max(emaDt, 0.001f);
        // Target ~60 Hz perception; at 120 FPS blur a bit more so motion looks continuous
        fpsFactor = std::min(1.6f, std::max(0.7f, fps / 60.f));
    }
    // blend in [0, 0.92] — never full history or image freezes
    float b = (1.f - std::exp2(-3.5f * s)) * 0.92f * fpsFactor;
    if (b > 0.92f) b = 0.92f;
    return b;
}

void processFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    std::lock_guard lock(g_glMu);
    if (!initGl()) return;

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    if (dpy == EGL_NO_DISPLAY || surf == EGL_NO_SURFACE) return;

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return;

    ensureSize(w, h);

    // Capture the fully rendered frame
    p_glBindTexture(GL_TEXTURE_2D, g_texCurrent);
    p_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    if (!g_hasHistory) {
        // Seed history with first frame
        p_glBindTexture(GL_TEXTURE_2D, g_texHistory);
        p_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        g_hasHistory = true;
        return; // no blur on first frame
    }

    // Draw fullscreen blend onto default framebuffer
    p_glViewport(0, 0, w, h);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);

    p_glUseProgram(g_prog);

    p_glActiveTexture(GL_TEXTURE10);
    p_glBindTexture(GL_TEXTURE_2D, g_texCurrent);
    p_glUniform1i(g_uCur, 10);

    p_glActiveTexture(GL_TEXTURE11);
    p_glBindTexture(GL_TEXTURE_2D, g_texHistory);
    p_glUniform1i(g_uHist, 11);

    p_glUniform1f(g_uBlend, computeBlend());
    p_glUniform1f(g_uOpacity, g_opacity.load(std::memory_order_relaxed));

    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    if (g_aPos >= 0) {
        p_glEnableVertexAttribArray((GLuint)g_aPos);
        p_glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat), nullptr);
    }
    if (g_aUv >= 0) {
        p_glEnableVertexAttribArray((GLuint)g_aUv);
        p_glVertexAttribPointer((GLuint)g_aUv, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat),
                                reinterpret_cast<void*>(2 * sizeof(GLfloat)));
    }
    p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    // Update history = what we just displayed (read back blended result)
    p_glBindTexture(GL_TEXTURE_2D, g_texHistory);
    p_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    static int s_log = 0;
    if (s_log < 3) {
        logLine("MotionBlur: frame blend ok %dx%d", w, h);
        ++s_log;
    }
}

EGLBoolean swapDetour(EGLDisplay dpy, EGLSurface surface) {
    // Apply blur on the completed frame, then present
    try {
        processFrame();
    } catch (...) {
    }
    return g_swapOriginal ? g_swapOriginal(dpy, surface) : EGL_FALSE;
}

void tryHookSwap() {
    if (g_swapHooked) return;
    void* swap = reinterpret_cast<void*>(eglGetProcAddress("eglSwapBuffers"));
    if (!swap) {
        void* egl = dlopen("libEGL.so", RTLD_NOW);
        if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
        if (egl) swap = dlsym(egl, "eglSwapBuffers");
    }
    void* o = nullptr;
    if (swap && pl::memory::hook(swap, reinterpret_cast<void*>(&swapDetour), &o) == 0) {
        g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(o);
        g_swapHooked = true;
        logLine("MotionBlur: eglSwapBuffers hooked");
    } else {
        logLine("MotionBlur: eglSwapBuffers hook FAILED");
    }
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        std::lock_guard lock(g_glMu);
        g_hasHistory = false;
    }
    MB_LOGI("MotionBlur %s", enabled ? "ON" : "OFF");
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "strength")
            g_strength.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "opacity")
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "fpsScale")
            g_fpsScale.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Motion Blur");
    b.description(
         "Natural Motion Blur–style temporal frame blending (screen post-process). "
         "Strength 1 ≈ NMB default. Makes camera motion look smoother.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("strength", "Strength", pl::modmenu::ConfigType::SliderFloat, "1.0", "0", "2", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.2", "1", "");
    b.config("fpsScale", "FPS scaling", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.registerModule();
}

void onSignaturesReady() { tryHookSwap(); }

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    std::lock_guard lock(g_glMu);
    g_hasHistory = false;
}

} // namespace bactro::motionblur
