#include "Signatures.hpp"
#include "Version.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BactroNative", __VA_ARGS__)

namespace {

// -------- Performance (safe: no main-thread signature spam, no sleep on present) --------
std::atomic_bool g_perfEnabled{true};
std::atomic_bool g_unlockFps{true};
std::atomic<int> g_measuredFps{0};
std::atomic<float> g_fullbright{0.0f};

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using EglSwapIntervalFn = EGLBoolean (*)(EGLDisplay, EGLint);
EglSwapBuffersFn g_swapOriginal = nullptr;
EglSwapIntervalFn g_swapInterval = nullptr;
bool g_swapHooked = false;
EGLDisplay g_intervalDisplay = EGL_NO_DISPLAY;

std::atomic<int> g_frameCount{0};
timespec g_fpsWindowStart{};
bool g_fpsWindowInit = false;

void* g_fullbrightTarget = nullptr;
uint8_t g_fullbrightOriginal[12]{};
bool g_fullbrightPatched = false;
std::atomic_bool g_fullbrightReady{false};

// -------- Fast Containers (lightweight flag only until a safe delay offset exists) --------
std::atomic_bool g_fastContainers{false};

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
    if (!g_fullbrightReady.load(std::memory_order_acquire)) return;
    applyFullbrightPatch(g_fullbright.load(std::memory_order_relaxed) >= 9.5f);
}

// Resolve Fullbright OFF the main thread (signature scan is slow and caused ANR).
void resolveFullbrightAsync() {
    if (g_fullbrightTarget) {
        g_fullbrightReady.store(true, std::memory_order_release);
        syncFullbright();
        return;
    }
    std::thread([] {
        const auto addr = resolveOne(bactro::sigs::Fullbright);
        if (!addr) {
            LOGE("Fullbright signature not found");
            return;
        }
        g_fullbrightTarget = reinterpret_cast<void*>(addr);
        std::memcpy(g_fullbrightOriginal, g_fullbrightTarget, 12);
        g_fullbrightReady.store(true, std::memory_order_release);
        LOGI("Fullbright @ %p (async)", g_fullbrightTarget);
        syncFullbright();
    }).detach();
}

void updateMeasuredFps() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!g_fpsWindowInit) {
        g_fpsWindowStart = now;
        g_fpsWindowInit = true;
        g_frameCount.store(0, std::memory_order_relaxed);
        return;
    }
    const int frames = g_frameCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t elapsed = (static_cast<int64_t>(now.tv_sec) - g_fpsWindowStart.tv_sec) * 1000000000LL +
                            (now.tv_nsec - g_fpsWindowStart.tv_nsec);
    if (elapsed >= 500000000LL) {
        const int fps = static_cast<int>((frames * 1000000000LL) / std::max<int64_t>(elapsed, 1));
        g_measuredFps.store(fps, std::memory_order_relaxed);
        g_frameCount.store(0, std::memory_order_relaxed);
        g_fpsWindowStart = now;
    }
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    // Unlock VSync only — no nanosleep (sleep on present was risky / contributed to freezes).
    if (g_perfEnabled.load(std::memory_order_relaxed) &&
        g_unlockFps.load(std::memory_order_relaxed) &&
        display != EGL_NO_DISPLAY && g_swapInterval) {
        if (display != g_intervalDisplay) {
            g_swapInterval(display, 0);
            g_intervalDisplay = display;
        }
    }

    EGLBoolean ok = EGL_FALSE;
    if (g_swapOriginal) ok = g_swapOriginal(display, surface);
    if (ok == EGL_TRUE) updateMeasuredFps();
    return ok;
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
        LOGE("eglSwapBuffers missing");
        return false;
    }
    void* orig = nullptr;
    if (pl::memory::hook(swapSym, reinterpret_cast<void*>(&swapBuffersDetour), &orig) != 0) {
        LOGE("eglSwapBuffers hook failed");
        return false;
    }
    g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(orig);
    g_swapHooked = true;
    LOGI("eglSwapBuffers hooked (VSync unlock, no sleep)");
    return true;
}

void onPerfToggle(std::string_view /*id*/, bool enabled) {
    g_perfEnabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        applyFullbrightPatch(false);
        if (g_swapInterval && g_intervalDisplay != EGL_NO_DISPLAY)
            g_swapInterval(g_intervalDisplay, 1);
        g_intervalDisplay = EGL_NO_DISPLAY;
    } else {
        installSwapHook();
        resolveFullbrightAsync();
    }
}

void onFastToggle(std::string_view /*id*/, bool enabled) {
    // Safe stub: no game-function hooks (wrong ABI + multi-signature scan caused ANR).
    // Flag kept for menu/UI; real no-delay needs a verified interact-timer offset later.
    g_fastContainers.store(enabled, std::memory_order_release);
    LOGI("Fast Containers %s (client flag only — no heavy hooks)", enabled ? "ON" : "OFF");
}

void onPerfConfig(std::string_view /*id*/, std::string_view key, std::string_view value) {
    try {
        if (key == "unlockFps") {
            g_unlockFps.store(value == "true" || value == "1", std::memory_order_relaxed);
            if (!g_unlockFps.load() && g_swapInterval && g_intervalDisplay != EGL_NO_DISPLAY) {
                g_swapInterval(g_intervalDisplay, 1);
                g_intervalDisplay = EGL_NO_DISPLAY;
            }
        } else if (key == "fullbright") {
            g_fullbright.store(std::clamp(std::stof(std::string(value)), 0.0f, 10.0f),
                               std::memory_order_relaxed);
            syncFullbright();
        }
    } catch (...) {
    }
}

void onFastConfig(std::string_view, std::string_view, std::string_view) {}

void registerMenus() {
    {
        pl::modmenu::ModuleBuilder b("bactro.performance", "Performance");
        b.description("Unlock FPS for 120Hz (VSync off). Fullbright 0-10. Safe: no frame sleep.")
            .defaultEnabled(true)
            .onToggle(onPerfToggle)
            .onConfigChanged(onPerfConfig);
        b.config("unlockFps", "Unlock FPS (disable VSync)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
        b.config("fullbright", "Fullbright", pl::modmenu::ConfigType::SliderFloat, "0", "0", "10", "");
        b.registerModule();
    }
    {
        pl::modmenu::ModuleBuilder b("bactro.fastcontainers", "Fast Containers");
        b.description("Reserved. Previous version ANR'd from signature scans on main thread; safe rebuild pending.")
            .defaultEnabled(false)
            .onToggle(onFastToggle)
            .onConfigChanged(onFastConfig);
        b.registerModule();
    }
}

} // namespace

class BactroNativeMod {
public:
    static BactroNativeMod& instance() {
        static BactroNativeMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        LOGI("load %s %s", bactro::Name.data(), bactro::Version.data());
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        registerMenus();
        // Only cheap work on this thread:
        installSwapHook();          // dlsym + one hook — fast
        resolveFullbrightAsync();   // signature scan OFF main thread
        g_perfEnabled.store(true, std::memory_order_release);
        LOGI("BactroNative enabled (safe path)");
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        onPerfToggle("bactro.performance", false);
        g_fastContainers.store(false, std::memory_order_release);
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        onPerfToggle("bactro.performance", false);
        g_fastContainers.store(false, std::memory_order_release);
        return true;
    }
};

PL_REGISTER_MOD(BactroNativeMod, BactroNativeMod::instance())
