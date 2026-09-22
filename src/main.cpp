#include "bactro/Signatures.hpp"
#include "bactro/HandShader.hpp"
#include "bactro/MotionBlur.hpp"
#include "bactro/ItemGlint.hpp"
#include "bactro/Status.hpp"
#include "Version.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BactroNative", __VA_ARGS__)

namespace {

using bactro::memory::SignatureId;

constexpr const char* kStatusPath =
    "/storage/emulated/0/Android/media/org.levimc.launcher/bactro_status.txt";
constexpr const char* kStatusPathAlt =
    "/sdcard/Android/media/org.levimc.launcher/bactro_status.txt";

void writeStatus(const char* line) {
    for (const char* path : {kStatusPath, kStatusPathAlt}) {
        std::ofstream out(path, std::ios::app);
        if (!out) continue;
        out << line << '\n';
        out.close();
        return;
    }
}

void writeStatusReplace(const std::string& body) {
    for (const char* path : {kStatusPath, kStatusPathAlt}) {
        std::ofstream out(path, std::ios::trunc);
        if (!out) continue;
        out << body;
        out.close();
        return;
    }
}

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

bool patchMemory(void* addr, const void* src, size_t n) {
    if (!addr || !src || n == 0) return false;
    const auto page = reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF);
    if (mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    std::memcpy(addr, src, n);
    __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr) + n);
    return true;
}

void applyFullbrightPatch(bool enable) {
    if (!g_fullbrightTarget) return;
    if (enable && !g_fullbrightPatched) {
        if (patchMemory(g_fullbrightTarget, g_fullbrightOriginal, 12)) {
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

EGLBoolean swapIntervalDetour(EGLDisplay d, EGLint interval) {
    if (g_unlockFps.load(std::memory_order_relaxed) && g_perfEnabled.load(std::memory_order_relaxed))
        interval = 0;
    return g_swapIntervalOriginal ? g_swapIntervalOriginal(d, interval) : eglSwapInterval(d, interval);
}

void normalTickDetour(void* self) {
    if (g_tickOriginal) g_tickOriginal(self);
    if (g_unlockFps.load(std::memory_order_relaxed) && g_perfEnabled.load(std::memory_order_relaxed)) {
        EGLDisplay d = eglGetCurrentDisplay();
        if (d != EGL_NO_DISPLAY) {
            if (g_swapIntervalOriginal) g_swapIntervalOriginal(d, 0);
            else eglSwapInterval(d, 0);
        }
    }
    bactro::handshader::onFrame();
}

bool installSwapIntervalHook() {
    if (g_swapIntervalHooked) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl) {
        LOGE("libEGL missing");
        writeStatus("eglSwapInterval FAIL no lib");
        return false;
    }
    void* sym = dlsym(egl, "eglSwapInterval");
    if (!sym) {
        writeStatus("eglSwapInterval FAIL no sym");
        return false;
    }
    void* o = nullptr;
    if (pl::memory::hook(sym, reinterpret_cast<void*>(&swapIntervalDetour), &o) != 0) {
        LOGE("eglSwapInterval hook failed");
        writeStatus("eglSwapInterval HOOK FAIL");
        // Still force once
        EGLDisplay d = eglGetCurrentDisplay();
        if (d != EGL_NO_DISPLAY) eglSwapInterval(d, 0);
        return false;
    }
    g_swapIntervalOriginal = reinterpret_cast<EglSwapIntervalFn>(o);
    g_swapIntervalHooked = true;
    EGLDisplay d = eglGetCurrentDisplay();
    if (d != EGL_NO_DISPLAY) swapIntervalDetour(d, 0);
    LOGI("eglSwapInterval hooked");
    writeStatus("eglSwapInterval OK");
    return true;
}

bool installTickHook() {
    if (g_tickHooked) return true;
    void* o = nullptr;
    if (!bactro::memory::hook(SignatureId::NormalTick, reinterpret_cast<void*>(&normalTickDetour), &o)) {
        writeStatus("NormalTick HOOK FAIL");
        return false;
    }
    g_tickOriginal = reinterpret_cast<NormalTickFn>(o);
    g_tickHooked = true;
    writeStatus("NormalTick hooked");
    return true;
}

void resolveEverythingAsync() {
    std::thread([] {
        writeStatus("resolveAll starting...");
        const bool ok = bactro::memory::resolveAll("libminecraftpe.so");
        g_sigsReady.store(ok, std::memory_order_release);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "resolveAll done ok=%d", ok ? 1 : 0);
        writeStatus(buf);

        const auto fb = bactro::memory::resolve(SignatureId::Fullbright);
        if (fb) {
            g_fullbrightTarget = reinterpret_cast<void*>(fb);
            std::memcpy(g_fullbrightOriginal, g_fullbrightTarget, 12);
            writeStatus("fullbright target found");
            syncFullbright();
        } else {
            writeStatus("fullbright MISSING");
        }

        installTickHook();
        bactro::handshader::onSignaturesReady();
        bactro::motionblur::onSignaturesReady();
        bactro::itemglint::onSignaturesReady();
        writeStatus("async init finished");
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

void registerMenus() {
    {
        pl::modmenu::ModuleBuilder b("bactro.performance", "Performance");
        b.description("VSync unlock + Fullbright.")
            .defaultEnabled(true)
            .onToggle(onPerfToggle)
            .onConfigChanged(onPerfConfig);
        b.config("unlockFps", "Unlock FPS (disable VSync)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
        b.config("fullbright", "Fullbright", pl::modmenu::ConfigType::SliderFloat, "0", "0", "10", "");
        b.registerModule();
    }
    bactro::handshader::registerModule();
    bactro::motionblur::registerModule();
    bactro::itemglint::registerModule();
}

} // namespace

namespace bactro {
void statusLine(const char* line) { writeStatus(line); }
} // namespace bactro

class BactroNativeMod {
public:
    static BactroNativeMod& instance() {
        static BactroNativeMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext&) {
        writeStatusReplace(std::string("load ") + std::string(bactro::Name) + " " +
                           std::string(bactro::Version) + "\n");
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        registerMenus();
        installSwapIntervalHook();
        resolveEverythingAsync();
        g_perfEnabled.store(true, std::memory_order_release);
        LOGI("BactroNative enabled (Hand Shader)");
        writeStatus("enabled");
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        onPerfToggle("", false);
        bactro::handshader::shutdown();
        bactro::motionblur::shutdown();
        bactro::itemglint::shutdown();
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        onPerfToggle("", false);
        bactro::handshader::shutdown();
        bactro::motionblur::shutdown();
        bactro::itemglint::shutdown();
        return true;
    }
};

PL_REGISTER_MOD(BactroNativeMod, BactroNativeMod::instance())
