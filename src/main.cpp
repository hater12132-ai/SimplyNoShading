#include "Signatures.hpp"
#include "Version.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Optimization", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Optimization", __VA_ARGS__)

namespace {

std::atomic_bool g_enabled{true};
std::atomic_bool g_unlockFps{true};
std::atomic<int> g_targetFps{120};
std::atomic<float> g_fullbright{0.0f};

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using EglSwapIntervalFn = EGLBoolean (*)(EGLDisplay, EGLint);

EglSwapBuffersFn g_swapOriginal = nullptr;
EglSwapIntervalFn g_swapInterval = nullptr;
bool g_swapHooked = false;

EGLDisplay g_intervalDisplay = EGL_NO_DISPLAY;
bool g_hasLastFrame = false;
timespec g_lastFrameTime{};

void* g_fullbrightTarget = nullptr;
uint8_t g_fullbrightOriginal[12]{};
bool g_fullbrightPatched = false;

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
    const size_t len = (addr + size) - reinterpret_cast<std::uintptr_t>(page) + static_cast<size_t>(pageSize);
    if (mprotect(page, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    std::memcpy(target, data, size);
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target) + size);
    return true;
}

void applyFullbrightPatch(bool enable) {
    if (!g_fullbrightTarget) return;
    if (enable && !g_fullbrightPatched) {
        const uint8_t patch[12] = {
            0x40, 0x8F, 0xA8, 0x52,
            0x00, 0x00, 0x27, 0x1E,
            0xC0, 0x03, 0x5F, 0xD6
        };
        if (patchMemory(g_fullbrightTarget, patch, sizeof(patch))) {
            g_fullbrightPatched = true;
            LOGI("fullbright ON");
        }
    } else if (!enable && g_fullbrightPatched) {
        if (patchMemory(g_fullbrightTarget, g_fullbrightOriginal, 12)) {
            g_fullbrightPatched = false;
            LOGI("fullbright OFF");
        }
    }
}

void syncFullbright() {
    applyFullbrightPatch(g_fullbright.load(std::memory_order_relaxed) >= 9.5f);
}

void resolveFullbright() {
    if (g_fullbrightTarget) return;
    const auto addr = resolveOne(optimization::sigs::Fullbright);
    if (!addr) {
        LOGE("Fullbright signature not found");
        return;
    }
    g_fullbrightTarget = reinterpret_cast<void*>(addr);
    std::memcpy(g_fullbrightOriginal, g_fullbrightTarget, 12);
    LOGI("Fullbright @ %p", g_fullbrightTarget);
}

void paceFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const int target = g_targetFps.load(std::memory_order_relaxed);
    if (target <= 0) {
        g_hasLastFrame = false;
        return;
    }

    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!g_hasLastFrame) {
        g_lastFrameTime = now;
        g_hasLastFrame = true;
        return;
    }

    const long long frameNs = 1000000000LL / target;
    timespec deadline = g_lastFrameTime;
    deadline.tv_nsec += frameNs;
    while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    const long long nowNs = static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec;
    const long long dlNs = static_cast<long long>(deadline.tv_sec) * 1000000000LL + deadline.tv_nsec;

    // If chunk meshing made us late, do not sleep — recover immediately.
    if (nowNs < dlNs) {
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
        }
        g_lastFrameTime = deadline;
    } else {
        g_lastFrameTime = now;
    }
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    if (g_enabled.load(std::memory_order_relaxed) &&
        g_unlockFps.load(std::memory_order_relaxed) &&
        display != EGL_NO_DISPLAY) {
        if (display != g_intervalDisplay && g_swapInterval) {
            g_swapInterval(display, 0);
            g_intervalDisplay = display;
        }
    }

    paceFrame();

    if (g_swapOriginal) return g_swapOriginal(display, surface);
    return EGL_FALSE;
}

bool installSwapHook() {
    if (g_swapHooked) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl) {
        LOGE("dlopen libEGL failed");
        return false;
    }
    void* swapSym = dlsym(egl, "eglSwapBuffers");
    g_swapInterval = reinterpret_cast<EglSwapIntervalFn>(dlsym(egl, "eglSwapInterval"));
    if (!swapSym) {
        LOGE("eglSwapBuffers not found");
        return false;
    }
    void* orig = nullptr;
    if (pl::memory::hook(swapSym, reinterpret_cast<void*>(&swapBuffersDetour), &orig) != 0) {
        LOGE("eglSwapBuffers hook failed");
        return false;
    }
    g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(orig);
    g_swapHooked = true;
    LOGI("eglSwapBuffers hooked");
    return true;
}

void onToggle(std::string_view /*id*/, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        applyFullbrightPatch(false);
        if (g_swapInterval && g_intervalDisplay != EGL_NO_DISPLAY)
            g_swapInterval(g_intervalDisplay, 1);
        g_intervalDisplay = EGL_NO_DISPLAY;
        g_hasLastFrame = false;
    } else {
        resolveFullbright();
        syncFullbright();
        installSwapHook();
    }
}

void onConfigChanged(std::string_view /*id*/, std::string_view key, std::string_view value) {
    try {
        if (key == "unlockFps") {
            g_unlockFps.store(value == "true" || value == "1", std::memory_order_relaxed);
            if (!g_unlockFps.load() && g_swapInterval && g_intervalDisplay != EGL_NO_DISPLAY) {
                g_swapInterval(g_intervalDisplay, 1);
                g_intervalDisplay = EGL_NO_DISPLAY;
            }
        } else if (key == "targetFps") {
            // Slider may send float string
            g_targetFps.store(std::clamp(static_cast<int>(std::stof(std::string(value))), 0, 240),
                              std::memory_order_relaxed);
            g_hasLastFrame = false;
        } else if (key == "fullbright") {
            g_fullbright.store(std::clamp(std::stof(std::string(value)), 0.0f, 10.0f),
                               std::memory_order_relaxed);
            syncFullbright();
        }
    } catch (...) {
    }
}

void registerMenu() {
    pl::modmenu::ModuleBuilder builder("optimization.performance", "Performance");
    builder.description(
               "FPS unlock for 120Hz + soft pacing. Fullbright 0-10. "
               "Chunk freezes = engine mesh work; lower Render Distance helps most.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfigChanged);

    builder.config("unlockFps", "Unlock FPS (disable VSync)", pl::modmenu::ConfigType::Toggle,
                   "true", "", "", "");
    builder.config("targetFps", "Target FPS (0=uncapped)", pl::modmenu::ConfigType::SliderFloat,
                   "120", "0", "240", "");
    builder.config("fullbright", "Fullbright", pl::modmenu::ConfigType::SliderFloat,
                   "0", "0", "10", "");
    builder.registerModule();
}

} // namespace

class OptimizationMod {
public:
    static OptimizationMod& instance() {
        static OptimizationMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        LOGI("load %s %s", optimization::Name.data(), optimization::Version.data());
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        registerMenu();
        installSwapHook();
        resolveFullbright();
        syncFullbright();
        g_enabled.store(true, std::memory_order_release);
        LOGI("Performance enabled");
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        onToggle("optimization.performance", false);
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        onToggle("optimization.performance", false);
        return true;
    }
};

PL_REGISTER_MOD(OptimizationMod, OptimizationMod::instance())
