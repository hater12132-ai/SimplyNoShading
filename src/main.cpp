#include "bactro/Signatures.hpp"
#include "Version.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BactroNative", __VA_ARGS__)

namespace {

using bactro::memory::SignatureId;

// -------- Performance --------
// IMPORTANT: do NOT hook eglSwapBuffers — that made LeviLauncher's FPS counter show 0.
// Only force eglSwapInterval(0) and optionally re-apply from NormalTick.
std::atomic_bool g_perfEnabled{true};
std::atomic_bool g_unlockFps{true};
std::atomic<float> g_fullbright{0.0f};

using EglSwapIntervalFn = EGLBoolean (*)(EGLDisplay, EGLint);
EglSwapIntervalFn g_swapIntervalOriginal = nullptr;
bool g_swapIntervalHooked = false;

using NormalTickFn = void (*)(void*);
NormalTickFn g_tickOriginal = nullptr;
bool g_tickHooked = false;

void* g_fullbrightTarget = nullptr;
uint8_t g_fullbrightOriginal[12]{};
bool g_fullbrightPatched = false;
std::atomic_bool g_sigsReady{false};

// -------- Fast Containers --------
std::atomic_bool g_fastContainers{true};
std::atomic_bool g_containerOpen{false};
std::atomic_bool g_readyForNextOpen{true};
std::atomic<int64_t> g_lastContainerCloseNs{0};

struct InteractionResultValue {
    std::uint8_t value{};
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

int64_t monoNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

bool wantsFastOpen() {
    if (!g_fastContainers.load(std::memory_order_relaxed)) return false;
    if (g_containerOpen.load(std::memory_order_acquire)) return false;
    return g_readyForNextOpen.load(std::memory_order_acquire);
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
            0x40, 0x8F, 0xA8, 0x52, 0x00, 0x00, 0x27, 0x1E, 0xC0, 0x03, 0x5F, 0xD6
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
    if (!g_sigsReady.load(std::memory_order_acquire)) return;
    applyFullbrightPatch(g_fullbright.load(std::memory_order_relaxed) >= 9.5f);
}

EGLBoolean swapIntervalDetour(EGLDisplay display, EGLint interval) {
    if (g_perfEnabled.load(std::memory_order_relaxed) &&
        g_unlockFps.load(std::memory_order_relaxed)) {
        interval = 0;
    }
    return g_swapIntervalOriginal ? g_swapIntervalOriginal(display, interval) : EGL_FALSE;
}

void normalTickDetour(void* self) {
    if (g_tickOriginal) g_tickOriginal(self);
    if (g_perfEnabled.load(std::memory_order_relaxed) &&
        g_unlockFps.load(std::memory_order_relaxed)) {
        EGLDisplay d = eglGetCurrentDisplay();
        if (d != EGL_NO_DISPLAY) {
            if (g_swapIntervalOriginal) g_swapIntervalOriginal(d, 0);
            else eglSwapInterval(d, 0);
        }
    }
}

bool installSwapIntervalHook() {
    if (g_swapIntervalHooked) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl) {
        LOGE("dlopen libEGL failed");
        return false;
    }
    void* sym = dlsym(egl, "eglSwapInterval");
    if (!sym) {
        LOGE("eglSwapInterval missing");
        return false;
    }
    void* orig = nullptr;
    if (pl::memory::hook(sym, reinterpret_cast<void*>(&swapIntervalDetour), &orig) != 0) {
        LOGE("eglSwapInterval hook failed");
        return false;
    }
    g_swapIntervalOriginal = reinterpret_cast<EglSwapIntervalFn>(orig);
    g_swapIntervalHooked = true;
    EGLDisplay d = eglGetCurrentDisplay();
    if (d != EGL_NO_DISPLAY && g_swapIntervalOriginal) g_swapIntervalOriginal(d, 0);
    LOGI("eglSwapInterval hooked (Levi FPS counter safe)");
    return true;
}

bool installTickHook() {
    if (g_tickHooked) return true;
    void* o = nullptr;
    if (!bactro::memory::hook(SignatureId::NormalTick, reinterpret_cast<void*>(&normalTickDetour), &o)) {
        LOGE("NormalTick hook failed (optional)");
        return false;
    }
    g_tickOriginal = reinterpret_cast<NormalTickFn>(o);
    g_tickHooked = true;
    LOGI("NormalTick hooked");
    return true;
}

// ---- Fast Containers ----

void* containerOpenDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    g_containerOpen.store(true, std::memory_order_release);
    g_readyForNextOpen.store(false, std::memory_order_release);
    return g_containerOpenOrig ? g_containerOpenOrig(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
}

void* containerCloseDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    void* r = g_containerCloseOrig ? g_containerCloseOrig(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
    g_containerOpen.store(false, std::memory_order_release);
    g_readyForNextOpen.store(true, std::memory_order_release);
    g_lastContainerCloseNs.store(monoNs(), std::memory_order_release);
    return r;
}

InteractionResultValue useOnDetour(UseItemOnFn original, void* gm, void* item, const void* pos,
                                   std::uint8_t face, const void* hit, const void* block, bool firstEvent) {
    if (!original) return {};
    if (wantsFastOpen()) {
        InteractionResultValue result{};
        for (int i = 0; i < 3; ++i) {
            result = original(gm, item, pos, face, hit, block, true);
            if (result.value != 0) break;
        }
        return result;
    }
    return original(gm, item, pos, face, hit, block, firstEvent);
}

InteractionResultValue gameModeUseItemOnDetour(void* gm, void* item, const void* pos, std::uint8_t face,
                                               const void* hit, const void* block, bool firstEvent) {
    return useOnDetour(g_useOnGame, gm, item, pos, face, hit, block, firstEvent);
}

InteractionResultValue survivalModeUseItemOnDetour(void* gm, void* item, const void* pos, std::uint8_t face,
                                                   const void* hit, const void* block, bool firstEvent) {
    return useOnDetour(g_useOnSurvival, gm, item, pos, face, hit, block, firstEvent);
}

bool interactDetour(InteractFn original, void* gm, void* target, const void* location) {
    if (!original) return false;
    bool result = original(gm, target, location);
    if (wantsFastOpen() && !result) result = original(gm, target, location);
    return result;
}

bool gameModeInteractDetour(void* gm, void* target, const void* location) {
    return interactDetour(g_interactGame, gm, target, location);
}

bool survivalModeInteractDetour(void* gm, void* target, const void* location) {
    return interactDetour(g_interactSurvival, gm, target, location);
}

void installGameHooksFromResolved() {
    int n = 0;
    void* o = nullptr;
    if (bactro::memory::hook(SignatureId::ContainerScreenControllerOpen,
                             reinterpret_cast<void*>(&containerOpenDetour), &o)) {
        g_containerOpenOrig = reinterpret_cast<ScreenFn>(o);
        ++n;
        LOGI("hook ContainerOpen");
    }
    o = nullptr;
    if (bactro::memory::hook(SignatureId::ContainerScreenControllerDtor,
                             reinterpret_cast<void*>(&containerCloseDetour), &o)) {
        g_containerCloseOrig = reinterpret_cast<ScreenFn>(o);
        ++n;
        LOGI("hook ContainerClose");
    }
    o = nullptr;
    if (bactro::memory::hook(SignatureId::GameModeUseItemOn,
                             reinterpret_cast<void*>(&gameModeUseItemOnDetour), &o)) {
        g_useOnGame = reinterpret_cast<UseItemOnFn>(o);
        ++n;
        LOGI("hook GameModeUseItemOn");
    }
    o = nullptr;
    if (bactro::memory::hook(SignatureId::SurvivalModeUseItemOn,
                             reinterpret_cast<void*>(&survivalModeUseItemOnDetour), &o)) {
        g_useOnSurvival = reinterpret_cast<UseItemOnFn>(o);
        ++n;
        LOGI("hook SurvivalUseItemOn");
    }
    o = nullptr;
    if (bactro::memory::hook(SignatureId::GameModeInteract,
                             reinterpret_cast<void*>(&gameModeInteractDetour), &o)) {
        g_interactGame = reinterpret_cast<InteractFn>(o);
        ++n;
        LOGI("hook GameModeInteract");
    }
    o = nullptr;
    if (bactro::memory::hook(SignatureId::SurvivalModeInteract,
                             reinterpret_cast<void*>(&survivalModeInteractDetour), &o)) {
        g_interactSurvival = reinterpret_cast<InteractFn>(o);
        ++n;
        LOGI("hook SurvivalInteract");
    }
    LOGI("Fast Containers hooks: %d/6", n);
}

void resolveEverythingAsync() {
    std::thread([] {
        LOGI("resolveAll starting (background)...");
        const bool ok = bactro::memory::resolveAll("libminecraftpe.so");
        g_sigsReady.store(ok, std::memory_order_release);
        LOGI("resolveAll done ok=%d", ok ? 1 : 0);

        const auto fb = bactro::memory::resolve(SignatureId::Fullbright);
        if (fb) {
            g_fullbrightTarget = reinterpret_cast<void*>(fb);
            std::memcpy(g_fullbrightOriginal, g_fullbrightTarget, 12);
            LOGI("Fullbright @ %p", g_fullbrightTarget);
            syncFullbright();
        } else {
            LOGE("Fullbright missing");
        }

        if (g_fastContainers.load(std::memory_order_relaxed))
            installGameHooksFromResolved();
        if (g_perfEnabled.load(std::memory_order_relaxed))
            installTickHook();
    }).detach();
}

void onPerfToggle(std::string_view, bool enabled) {
    g_perfEnabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        applyFullbrightPatch(false);
        EGLDisplay d = eglGetCurrentDisplay();
        if (d != EGL_NO_DISPLAY) {
            if (g_swapIntervalOriginal) g_swapIntervalOriginal(d, 1);
            else eglSwapInterval(d, 1);
        }
    } else {
        installSwapIntervalHook();
        if (g_sigsReady.load()) {
            installTickHook();
            syncFullbright();
        } else {
            resolveEverythingAsync();
        }
    }
}

void onFastToggle(std::string_view, bool enabled) {
    g_fastContainers.store(enabled, std::memory_order_release);
    if (enabled && g_sigsReady.load()) installGameHooksFromResolved();
    if (enabled && !g_sigsReady.load()) resolveEverythingAsync();
    LOGI("Fast Containers %s", enabled ? "ON" : "OFF");
}

void onPerfConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "unlockFps") {
            g_unlockFps.store(value == "true" || value == "1", std::memory_order_relaxed);
            EGLDisplay d = eglGetCurrentDisplay();
            if (d != EGL_NO_DISPLAY) {
                const EGLint iv = g_unlockFps.load() ? 0 : 1;
                if (g_swapIntervalOriginal) g_swapIntervalOriginal(d, iv);
                else eglSwapInterval(d, iv);
            }
        } else if (key == "fullbright") {
            g_fullbright.store(std::stof(std::string(value)), std::memory_order_relaxed);
            syncFullbright();
        }
    } catch (...) {
    }
}

void onFastConfig(std::string_view, std::string_view, std::string_view) {}

void registerMenus() {
    {
        pl::modmenu::ModuleBuilder b("bactro.performance", "Performance");
        b.description("VSync unlock via eglSwapInterval only (keeps Levi FPS counter working) + Fullbright.")
            .defaultEnabled(true)
            .onToggle(onPerfToggle)
            .onConfigChanged(onPerfConfig);
        b.config("unlockFps", "Unlock FPS (disable VSync)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
        b.config("fullbright", "Fullbright", pl::modmenu::ConfigType::SliderFloat, "0", "0", "10", "");
        b.registerModule();
    }
    {
        pl::modmenu::ModuleBuilder b("bactro.fastcontainers", "Fast Containers");
        b.description("After server opens a chest/shulker, close and open the next with no client wait.")
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
        installSwapIntervalHook();
        resolveEverythingAsync();
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
