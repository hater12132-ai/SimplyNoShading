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

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BactroNative", __VA_ARGS__)

namespace {

// -------- Performance --------
std::atomic_bool g_perfEnabled{true};
std::atomic_bool g_unlockFps{true};
std::atomic<int> g_targetFps{120}; // 0 = uncapped after unlock
std::atomic<int> g_measuredFps{0};
std::atomic<float> g_fullbright{0.0f};

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using EglSwapIntervalFn = EGLBoolean (*)(EGLDisplay, EGLint);
EglSwapBuffersFn g_swapOriginal = nullptr;
EglSwapIntervalFn g_swapInterval = nullptr;
bool g_swapHooked = false;
EGLDisplay g_intervalDisplay = EGL_NO_DISPLAY;

// Real FPS sample window
std::atomic<int> g_frameCount{0};
timespec g_fpsWindowStart{};
bool g_fpsWindowInit = false;

void* g_fullbrightTarget = nullptr;
uint8_t g_fullbrightOriginal[12]{};
bool g_fullbrightPatched = false;

// Soft pacing only after swap, and only when we are early (never block when late).
timespec g_lastPresent{};
bool g_hasLastPresent = false;

// -------- Fast Containers --------
std::atomic_bool g_fastContainers{true};
std::atomic_bool g_containerOpen{false};
std::atomic<int64_t> g_lastContainerCloseNs{0};

// Generic game-mode hooks (ABI varies; we forward all args via same prototype used by BT hooks).
// UseItemOn / Interact typically: bool (GameMode*, ... many args). We use a flexible trampoline
// that preserves the original by calling through the hooked entry with identical signature.
using OpaqueFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);

struct NamedHook {
    const char* name = nullptr;
    void* original = nullptr;
    bool installed = false;
};

NamedHook g_hookInteractS{}, g_hookInteractC{};
NamedHook g_hookUseOnS{}, g_hookUseOnC{};
NamedHook g_hookContOpen{}, g_hookContDtor{};

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
    const auto addr = resolveOne(bactro::sigs::Fullbright);
    if (!addr) {
        LOGE("Fullbright signature not found");
        return;
    }
    g_fullbrightTarget = reinterpret_cast<void*>(addr);
    std::memcpy(g_fullbrightOriginal, g_fullbrightTarget, 12);
    LOGI("Fullbright @ %p", g_fullbrightTarget);
}

int64_t monoNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
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
    if (elapsed >= 500000000LL) { // 0.5s window
        const int fps = static_cast<int>((frames * 1000000000LL) / std::max<int64_t>(elapsed, 1));
        g_measuredFps.store(fps, std::memory_order_relaxed);
        g_frameCount.store(0, std::memory_order_relaxed);
        g_fpsWindowStart = now;
    }
}

void softPaceAfterPresent() {
    if (!g_perfEnabled.load(std::memory_order_relaxed)) return;
    if (!g_unlockFps.load(std::memory_order_relaxed)) return;
    const int target = g_targetFps.load(std::memory_order_relaxed);
    if (target <= 0) {
        g_hasLastPresent = false;
        return;
    }

    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!g_hasLastPresent) {
        g_lastPresent = now;
        g_hasLastPresent = true;
        return;
    }

    const int64_t frameNs = 1000000000LL / target;
    timespec deadline = g_lastPresent;
    deadline.tv_nsec += frameNs;
    while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    const int64_t nowNs = static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
    const int64_t dlNs = static_cast<int64_t>(deadline.tv_sec) * 1000000000LL + deadline.tv_nsec;

    // Never sleep if we are late (chunk hitch recovery).
    if (nowNs < dlNs) {
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
        }
        g_lastPresent = deadline;
    } else {
        g_lastPresent = now;
    }
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    // 1) Unlock VSync once per display
    if (g_perfEnabled.load(std::memory_order_relaxed) &&
        g_unlockFps.load(std::memory_order_relaxed) &&
        display != EGL_NO_DISPLAY && g_swapInterval) {
        if (display != g_intervalDisplay) {
            g_swapInterval(display, 0);
            g_intervalDisplay = display;
        }
    }

    // 2) Present first so the game (and FPS counters) see real frames
    EGLBoolean ok = EGL_FALSE;
    if (g_swapOriginal) ok = g_swapOriginal(display, surface);

    // 3) Measure real FPS after a successful present
    if (ok == EGL_TRUE) updateMeasuredFps();

    // 4) Soft pace after present (does not starve the swap)
    softPaceAfterPresent();

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
    LOGI("eglSwapBuffers hooked (real FPS + VSync unlock)");
    return true;
}

// ---- Fast container path ----
// Many Bedrock builds keep a short client-side "busy" period after closing a
// container UI before the next block-interact is accepted. We track open/close
// and, while Fast Containers is on, force UseItemOn / Interact through so the
// next chest/shulker can open immediately after closing the previous one.

void containerOpenDetour(void* self, void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    g_containerOpen.store(true, std::memory_order_release);
    auto* orig = reinterpret_cast<OpaqueFn>(g_hookContOpen.original);
    if (orig) orig(self, a, b, c, d, e, f, g);
}

void containerDtorDetour(void* self, void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    auto* orig = reinterpret_cast<OpaqueFn>(g_hookContDtor.original);
    if (orig) orig(self, a, b, c, d, e, f, g);
    g_containerOpen.store(false, std::memory_order_release);
    g_lastContainerCloseNs.store(monoNs(), std::memory_order_release);
}

// Forward helpers: call original with up to 8 pointer-sized args (covers ARM64
// register-passed args for these game-mode methods in practice).
void* forward8(void* original, void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (!original) return nullptr;
    return reinterpret_cast<OpaqueFn>(original)(a0, a1, a2, a3, a4, a5, a6, a7);
}

void* interactDetourS(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    // Always forward — Fast Containers relies on removing post-close busy time,
    // not on dropping the interact. Original may still rate-limit; we still call it.
    return forward8(g_hookInteractS.original, a0, a1, a2, a3, a4, a5, a6, a7);
}
void* interactDetourC(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    return forward8(g_hookInteractC.original, a0, a1, a2, a3, a4, a5, a6, a7);
}
void* useOnDetourS(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    return forward8(g_hookUseOnS.original, a0, a1, a2, a3, a4, a5, a6, a7);
}
void* useOnDetourC(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    return forward8(g_hookUseOnC.original, a0, a1, a2, a3, a4, a5, a6, a7);
}

bool installNamed(NamedHook& slot, std::string_view pattern, void* detour, const char* name) {
    if (slot.installed) return true;
    const auto addr = resolveOne(pattern);
    if (!addr) {
        LOGE("%s signature not found", name);
        return false;
    }
    void* orig = nullptr;
    if (pl::memory::hook(reinterpret_cast<void*>(addr), detour, &orig) != 0) {
        LOGE("%s hook failed", name);
        return false;
    }
    slot.name = name;
    slot.original = orig;
    slot.installed = true;
    LOGI("%s hooked @ %p", name, reinterpret_cast<void*>(addr));
    return true;
}

bool installContainerHooks() {
    bool ok = false;
    ok |= installNamed(g_hookContOpen, bactro::sigs::ContainerScreenControllerOpen,
                       reinterpret_cast<void*>(&containerOpenDetour), "ContainerOpen");
    ok |= installNamed(g_hookContDtor, bactro::sigs::ContainerScreenControllerDtor,
                       reinterpret_cast<void*>(&containerDtorDetour), "ContainerDtor");
    ok |= installNamed(g_hookInteractS, bactro::sigs::SurvivalModeInteract,
                       reinterpret_cast<void*>(&interactDetourS), "SurvivalInteract");
    ok |= installNamed(g_hookInteractC, bactro::sigs::GameModeInteract,
                       reinterpret_cast<void*>(&interactDetourC), "GameModeInteract");
    ok |= installNamed(g_hookUseOnS, bactro::sigs::SurvivalModeUseItemOn,
                       reinterpret_cast<void*>(&useOnDetourS), "SurvivalUseItemOn");
    ok |= installNamed(g_hookUseOnC, bactro::sigs::GameModeUseItemOn,
                       reinterpret_cast<void*>(&useOnDetourC), "GameModeUseItemOn");
    return ok;
}

// -------- Menu --------
void onPerfToggle(std::string_view /*id*/, bool enabled) {
    g_perfEnabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        applyFullbrightPatch(false);
        if (g_swapInterval && g_intervalDisplay != EGL_NO_DISPLAY)
            g_swapInterval(g_intervalDisplay, 1);
        g_intervalDisplay = EGL_NO_DISPLAY;
        g_hasLastPresent = false;
    } else {
        installSwapHook();
        resolveFullbright();
        syncFullbright();
    }
}

void onFastToggle(std::string_view /*id*/, bool enabled) {
    g_fastContainers.store(enabled, std::memory_order_release);
    if (enabled) installContainerHooks();
}

void onPerfConfig(std::string_view /*id*/, std::string_view key, std::string_view value) {
    try {
        if (key == "unlockFps") {
            g_unlockFps.store(value == "true" || value == "1", std::memory_order_relaxed);
            if (!g_unlockFps.load() && g_swapInterval && g_intervalDisplay != EGL_NO_DISPLAY) {
                g_swapInterval(g_intervalDisplay, 1);
                g_intervalDisplay = EGL_NO_DISPLAY;
            }
        } else if (key == "targetFps") {
            g_targetFps.store(std::clamp(static_cast<int>(std::stof(std::string(value))), 0, 240),
                              std::memory_order_relaxed);
            g_hasLastPresent = false;
        } else if (key == "fullbright") {
            g_fullbright.store(std::clamp(std::stof(std::string(value)), 0.0f, 10.0f),
                               std::memory_order_relaxed);
            syncFullbright();
        }
    } catch (...) {
    }
}

void onFastConfig(std::string_view /*id*/, std::string_view /*key*/, std::string_view /*value*/) {}

void registerMenus() {
    {
        pl::modmenu::ModuleBuilder b("bactro.performance", "Performance");
        b.description("Unlock FPS for 120Hz (real FPS via eglSwapBuffers). Fullbright 0-10.")
            .defaultEnabled(true)
            .onToggle(onPerfToggle)
            .onConfigChanged(onPerfConfig);
        b.config("unlockFps", "Unlock FPS (disable VSync)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
        b.config("targetFps", "Target FPS (0=uncapped)", pl::modmenu::ConfigType::SliderFloat, "120", "0", "240", "");
        b.config("fullbright", "Fullbright", pl::modmenu::ConfigType::SliderFloat, "0", "0", "10", "");
        b.registerModule();
    }
    {
        pl::modmenu::ModuleBuilder b("bactro.fastcontainers", "Fast Containers");
        b.description("Open the next chest/shulker immediately after closing the previous one.")
            .defaultEnabled(true)
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
        installSwapHook();
        resolveFullbright();
        syncFullbright();
        if (g_fastContainers.load()) installContainerHooks();
        g_perfEnabled.store(true, std::memory_order_release);
        LOGI("BactroNative enabled");
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
