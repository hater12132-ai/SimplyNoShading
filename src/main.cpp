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
#include <unordered_map>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BactroNative", __VA_ARGS__)

namespace {

// -------- Performance --------
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

// -------- Fast Containers (BedrockTools ABIs) --------
std::atomic_bool g_fastContainers{true};
std::atomic_bool g_containerOpen{false};
std::atomic<int64_t> g_lastContainerCloseNs{0};
std::atomic_bool g_containerHooksReady{false};

// Exact types from BedrockTools GameHooks.cpp
struct InteractionResultValue {
    std::uint8_t value;
};

using UseItemOnFn = InteractionResultValue (*)(void*, void*, const void*, std::uint8_t, const void*, const void*, bool);
using InteractFn = bool (*)(void*, void*, const void*);
using ScreenFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);

UseItemOnFn g_useOnGame = nullptr;
UseItemOnFn g_useOnSurvival = nullptr;
InteractFn g_interactGame = nullptr;
InteractFn g_interactSurvival = nullptr;
ScreenFn g_containerOpenOrig = nullptr;
ScreenFn g_containerCloseOrig = nullptr;

static bool interactionOk(InteractionResultValue r) {
    // Bedrock InteractionResult: non-zero generally means swing/success path
    return r.value != 0;
}

static int64_t monoNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static bool recentlyClosedContainer() {
    const int64_t t = g_lastContainerCloseNs.load(std::memory_order_acquire);
    if (t == 0) return false;
    return (monoNs() - t) < 750000000LL; // 750ms window after close
}

std::uintptr_t resolveOne(std::string_view pattern) {
    std::vector<std::string> patterns{std::string(pattern)};
    const auto map = pl::memory::resolveSignatures(patterns, "libminecraftpe.so");
    const auto it = map.find(patterns[0]);
    return (it != map.end()) ? it->second : 0;
}

// One batch scan — avoids 6× main-thread scans that caused the ANR.
std::unordered_map<std::string, std::uintptr_t> resolveBatch(const std::vector<std::string_view>& views) {
    std::vector<std::string> patterns;
    patterns.reserve(views.size());
    for (auto v : views) patterns.emplace_back(v);
    auto map = pl::memory::resolveSignatures(patterns, "libminecraftpe.so");
    std::unordered_map<std::string, std::uintptr_t> out;
    for (auto& p : patterns) {
        auto it = map.find(p);
        out[p] = (it != map.end()) ? it->second : 0;
    }
    return out;
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
        LOGI("Fullbright @ %p", g_fullbrightTarget);
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
        g_measuredFps.store(static_cast<int>((frames * 1000000000LL) / std::max<int64_t>(elapsed, 1)),
                            std::memory_order_relaxed);
        g_frameCount.store(0, std::memory_order_relaxed);
        g_fpsWindowStart = now;
    }
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    if (g_perfEnabled.load(std::memory_order_relaxed) &&
        g_unlockFps.load(std::memory_order_relaxed) &&
        display != EGL_NO_DISPLAY && g_swapInterval) {
        if (display != g_intervalDisplay) {
            g_swapInterval(display, 0);
            g_intervalDisplay = display;
        }
    }
    EGLBoolean ok = g_swapOriginal ? g_swapOriginal(display, surface) : EGL_FALSE;
    if (ok == EGL_TRUE) updateMeasuredFps();
    return ok;
}

bool installSwapHook() {
    if (g_swapHooked) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl) return false;
    void* swapSym = dlsym(egl, "eglSwapBuffers");
    g_swapInterval = reinterpret_cast<EglSwapIntervalFn>(dlsym(egl, "eglSwapInterval"));
    if (!swapSym) return false;
    void* orig = nullptr;
    if (pl::memory::hook(swapSym, reinterpret_cast<void*>(&swapBuffersDetour), &orig) != 0) return false;
    g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(orig);
    g_swapHooked = true;
    LOGI("eglSwapBuffers hooked");
    return true;
}

// ---- Fast Containers detours (correct ABIs from BedrockTools) ----

void* containerOpenDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    g_containerOpen.store(true, std::memory_order_release);
    return g_containerOpenOrig ? g_containerOpenOrig(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
}

void* containerCloseDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    void* r = g_containerCloseOrig ? g_containerCloseOrig(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
    g_containerOpen.store(false, std::memory_order_release);
    g_lastContainerCloseNs.store(monoNs(), std::memory_order_release);
    return r;
}

InteractionResultValue useOnBoost(UseItemOnFn original, void* gm, void* item, const void* pos,
                                  std::uint8_t face, const void* hit, const void* block, bool firstEvent) {
    if (!original) return {};
    auto result = original(gm, item, pos, face, hit, block, firstEvent);

    // After closing a chest/shulker, the next block-use often needs a fresh
    // firstEvent=true pulse. If the first call fails during the boost window,
    // retry once with firstEvent forced on.
    if (g_fastContainers.load(std::memory_order_relaxed) && recentlyClosedContainer() &&
        !interactionOk(result)) {
        result = original(gm, item, pos, face, hit, block, true);
    }
    return result;
}

InteractionResultValue gameModeUseItemOnDetour(void* gm, void* item, const void* pos, std::uint8_t face,
                                               const void* hit, const void* block, bool firstEvent) {
    return useOnBoost(g_useOnGame, gm, item, pos, face, hit, block, firstEvent);
}

InteractionResultValue survivalModeUseItemOnDetour(void* gm, void* item, const void* pos, std::uint8_t face,
                                                   const void* hit, const void* block, bool firstEvent) {
    return useOnBoost(g_useOnSurvival, gm, item, pos, face, hit, block, firstEvent);
}

bool interactBoost(InteractFn original, void* gm, void* target, const void* location) {
    if (!original) return false;
    bool result = original(gm, target, location);
    if (g_fastContainers.load(std::memory_order_relaxed) && recentlyClosedContainer() && !result) {
        result = original(gm, target, location);
    }
    return result;
}

bool gameModeInteractDetour(void* gm, void* target, const void* location) {
    return interactBoost(g_interactGame, gm, target, location);
}

bool survivalModeInteractDetour(void* gm, void* target, const void* location) {
    return interactBoost(g_interactSurvival, gm, target, location);
}

bool hookAt(std::uintptr_t addr, void* detour, void** originalOut, const char* name) {
    if (!addr) {
        LOGE("%s not found", name);
        return false;
    }
    if (pl::memory::hook(reinterpret_cast<void*>(addr), detour, originalOut) != 0) {
        LOGE("%s hook failed", name);
        return false;
    }
    LOGI("%s @ %p", name, reinterpret_cast<void*>(addr));
    return true;
}

void installContainerHooksAsync() {
    if (g_containerHooksReady.load(std::memory_order_acquire)) return;
    std::thread([] {
        // Single batch resolve — safe, not on UI thread
        const std::vector<std::string_view> views = {
            bactro::sigs::ContainerScreenControllerOpen,
            bactro::sigs::ContainerScreenControllerDtor,
            bactro::sigs::GameModeUseItemOn,
            bactro::sigs::SurvivalModeUseItemOn,
            bactro::sigs::GameModeInteract,
            bactro::sigs::SurvivalModeInteract,
        };
        auto map = resolveBatch(views);

        auto get = [&](std::string_view p) -> std::uintptr_t {
            auto it = map.find(std::string(p));
            return it != map.end() ? it->second : 0;
        };

        void* o = nullptr;
        int n = 0;
        if (hookAt(get(bactro::sigs::ContainerScreenControllerOpen),
                   reinterpret_cast<void*>(&containerOpenDetour), &o, "ContainerOpen")) {
            g_containerOpenOrig = reinterpret_cast<ScreenFn>(o);
            ++n;
        }
        o = nullptr;
        if (hookAt(get(bactro::sigs::ContainerScreenControllerDtor),
                   reinterpret_cast<void*>(&containerCloseDetour), &o, "ContainerClose")) {
            g_containerCloseOrig = reinterpret_cast<ScreenFn>(o);
            ++n;
        }
        o = nullptr;
        if (hookAt(get(bactro::sigs::GameModeUseItemOn),
                   reinterpret_cast<void*>(&gameModeUseItemOnDetour), &o, "GameModeUseItemOn")) {
            g_useOnGame = reinterpret_cast<UseItemOnFn>(o);
            ++n;
        }
        o = nullptr;
        if (hookAt(get(bactro::sigs::SurvivalModeUseItemOn),
                   reinterpret_cast<void*>(&survivalModeUseItemOnDetour), &o, "SurvivalUseItemOn")) {
            g_useOnSurvival = reinterpret_cast<UseItemOnFn>(o);
            ++n;
        }
        o = nullptr;
        if (hookAt(get(bactro::sigs::GameModeInteract),
                   reinterpret_cast<void*>(&gameModeInteractDetour), &o, "GameModeInteract")) {
            g_interactGame = reinterpret_cast<InteractFn>(o);
            ++n;
        }
        o = nullptr;
        if (hookAt(get(bactro::sigs::SurvivalModeInteract),
                   reinterpret_cast<void*>(&survivalModeInteractDetour), &o, "SurvivalInteract")) {
            g_interactSurvival = reinterpret_cast<InteractFn>(o);
            ++n;
        }

        g_containerHooksReady.store(n > 0, std::memory_order_release);
        LOGI("Fast Containers hooks installed: %d/6", n);
    }).detach();
}

void onPerfToggle(std::string_view, bool enabled) {
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

void onFastToggle(std::string_view, bool enabled) {
    g_fastContainers.store(enabled, std::memory_order_release);
    if (enabled) installContainerHooksAsync();
    LOGI("Fast Containers %s", enabled ? "ON" : "OFF");
}

void onPerfConfig(std::string_view, std::string_view key, std::string_view value) {
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
        b.description("Unlock FPS (VSync off) + Fullbright 0-10.")
            .defaultEnabled(true)
            .onToggle(onPerfToggle)
            .onConfigChanged(onPerfConfig);
        b.config("unlockFps", "Unlock FPS (disable VSync)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
        b.config("fullbright", "Fullbright", pl::modmenu::ConfigType::SliderFloat, "0", "0", "10", "");
        b.registerModule();
    }
    {
        pl::modmenu::ModuleBuilder b("bactro.fastcontainers", "Fast Containers");
        b.description("Faster chest/shulker re-open after close (BedrockTools ABIs, async resolve).")
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
        resolveFullbrightAsync();
        if (g_fastContainers.load()) installContainerHooksAsync();
        g_perfEnabled.store(true, std::memory_order_release);
        LOGI("BactroNative enabled");
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        onPerfToggle("", false);
        g_fastContainers.store(false, std::memory_order_release);
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        onPerfToggle("", false);
        g_fastContainers.store(false, std::memory_order_release);
        return true;
    }
};

PL_REGISTER_MOD(BactroNativeMod, BactroNativeMod::instance())
