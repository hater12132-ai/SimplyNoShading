#include "bactro/Signatures.hpp"
#include "bactro/TargetHud.hpp"
#include "bactro/HealthCache.hpp"
#include "bactro/Status.hpp"
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

// File heartbeat — Termux often cannot read Minecraft logcat on non-root Android.
// Check with: cat /sdcard/Android/media/org.levimc.launcher/bactro_status.txt
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
    bactro::targethud::onFrame();
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
        writeStatus("NormalTick hook FAIL (TargetHUD cannot draw without it)");
        return false;
    }
    g_tickOriginal = reinterpret_cast<NormalTickFn>(o);
    g_tickHooked = true;
    LOGI("NormalTick hooked");
    writeStatus("NormalTick hooked");
    return true;
}

// ---- NetworkPeerReceive (CompressedNetworkPeer @ 0xc6cf920 on 1.26.51.1) ----
// IMPORTANT: only parse when DataStatus indicates real data.
// Bedrock DataStatus is typically: 0 = Ok/HasData, 1 = NoData, 2 = BrokenData.
// Parsing on every non-empty std::string was wrong: on NoData the string can keep
// leftover bytes (often last *outbound* packet), which produced client→server IDs
// (30/33/36) and never UpdateAttributes (29).
using NetworkPeerReceiveFn = int (*)(void* self, std::string& data, int a2, int a3);
NetworkPeerReceiveFn g_netRecvOriginal = nullptr;
bool g_netRecvHooked = false;

int networkPeerReceiveDetour(void* self, std::string& data, int a2, int a3) {
    const int status = g_netRecvOriginal ? g_netRecvOriginal(self, data, a2, a3) : 1;

    // Status histogram (first few distinct values) for diagnosis
    {
        static int s_statusLog = 0;
        static int s_seen[8] = {};
        if (status >= 0 && status < 8) s_seen[status]++;
        if (s_statusLog < 12) {
            char b[96];
            std::snprintf(b, sizeof(b), "net: status=%d size=%zu (HasData only if status==0)",
                          status, data.size());
            writeStatus(b);
            ++s_statusLog;
        } else if (s_statusLog == 12) {
            char b[128];
            std::snprintf(b, sizeof(b),
                          "net: status hist 0=%d 1=%d 2=%d 3=%d 4=%d (parse only status==0)",
                          s_seen[0], s_seen[1], s_seen[2], s_seen[3], s_seen[4]);
            writeStatus(b);
            ++s_statusLog;
        }
    }

    // Only feed the parser when the peer reports HasData/Ok.
    if (status == 0 && !data.empty() && data.size() < (1u << 22)) {
        bactro::health::onRawGamePacket(
            reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }
    return status;
}

bool installNetworkPeerReceiveHook() {
    if (g_netRecvHooked) return true;
    void* o = nullptr;
    if (!bactro::memory::hook(SignatureId::NetworkPeerReceive,
                              reinterpret_cast<void*>(&networkPeerReceiveDetour), &o)) {
        LOGE("NetworkPeerReceive hook failed");
        writeStatus("NetworkPeerReceive HOOK FAIL");
        return false;
    }
    g_netRecvOriginal = reinterpret_cast<NetworkPeerReceiveFn>(o);
    g_netRecvHooked = true;
    LOGI("NetworkPeerReceive hooked (CompressedNetworkPeer) status==0 only");
    writeStatus("NetworkPeerReceive OK (parse only DataStatus==0)");
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

bool tryHook(SignatureId id, void* detour, void** originalOut, const char* name, int& n) {
    const auto addr = bactro::memory::resolve(id);
    char line[192];
    if (!addr) {
        std::snprintf(line, sizeof(line), "FAIL %s: signature not found", name);
        writeStatus(line);
        LOGE("%s", line);
        return false;
    }
    void* o = nullptr;
    if (!bactro::memory::hook(id, detour, &o)) {
        // Resolved but hook failed — often BedrockTools already hooked the same site
        std::snprintf(line, sizeof(line), "FAIL %s: hook @%p (already hooked by another mod?)", name,
                      reinterpret_cast<void*>(addr));
        writeStatus(line);
        LOGE("%s", line);
        return false;
    }
    if (originalOut) *originalOut = o;
    ++n;
    std::snprintf(line, sizeof(line), "OK   %s @%p", name, reinterpret_cast<void*>(addr));
    writeStatus(line);
    LOGI("%s", line);
    return true;
}

void installGameHooksFromResolved() {
    // 1.26.51.1 crash on load was caused by SurvivalUseItemOn / GameModeInteract /
    // ContainerClose detours (wrong target or ABI). Only install the proven-safe pair:
    //   ContainerOpen  (state) + GameModeUseItemOn (firstEvent retries).
    // Always-retry mode is used so we don't need close tracking.
    int n = 0;
    void* o = nullptr;

    o = nullptr;
    if (tryHook(SignatureId::ContainerScreenControllerOpen,
                reinterpret_cast<void*>(&containerOpenDetour), &o, "ContainerOpen", n))
        g_containerOpenOrig = reinterpret_cast<ScreenFn>(o);

    o = nullptr;
    if (tryHook(SignatureId::GameModeUseItemOn,
                reinterpret_cast<void*>(&gameModeUseItemOnDetour), &o, "GameModeUseItemOn", n))
        g_useOnGame = reinterpret_cast<UseItemOnFn>(o);

    // Intentionally NOT hooked (caused load crash on 1.26.51.1):
    //   ContainerClose, SurvivalUseItemOn, GameModeInteract, SurvivalInteract
    writeStatus("skip ContainerClose SurvivalUseItemOn GameModeInteract SurvivalInteract (crash-safe)");

    // Always allow fast open retries without close tracking
    g_readyForNextOpen.store(true, std::memory_order_release);
    g_containerOpen.store(false, std::memory_order_release);
    writeStatus("always-retry mode ON");

    LOGI("Fast Containers hooks: %d (safe set)", n);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "hooks=%d/2-safe", n);
    writeStatus(buf);
}

void resolveEverythingAsync() {
    std::thread([] {
        LOGI("resolveAll starting (background)...");
        writeStatus("resolveAll starting...");
        const bool ok = bactro::memory::resolveAll("libminecraftpe.so");
        g_sigsReady.store(ok, std::memory_order_release);
        LOGI("resolveAll done ok=%d", ok ? 1 : 0);
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "resolveAll done ok=%d", ok ? 1 : 0);
            writeStatus(buf);
        }

        const auto fb = bactro::memory::resolve(SignatureId::Fullbright);
        if (fb) {
            g_fullbrightTarget = reinterpret_cast<void*>(fb);
            std::memcpy(g_fullbrightOriginal, g_fullbrightTarget, 12);
            LOGI("Fullbright @ %p", g_fullbrightTarget);
            writeStatus("fullbright target found");
            syncFullbright();
        } else {
            LOGE("Fullbright missing");
            writeStatus("fullbright MISSING");
        }

        {
            const auto nr = bactro::memory::resolve(SignatureId::NetworkPeerReceive);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "NetworkPeerReceive @ %p", reinterpret_cast<void*>(nr));
            LOGI("%s", buf);
            writeStatus(buf);
        }

        if (g_fastContainers.load(std::memory_order_relaxed))
            installGameHooksFromResolved();
        if (g_perfEnabled.load(std::memory_order_relaxed))
            installTickHook();
        // TargetHUD needs NormalTick for draw; ensure tick hook even if perf off
        installTickHook();
        // Packet HP for TargetHUD (UpdateAttributes via CompressedNetworkPeer)
        installNetworkPeerReceiveHook();
        bactro::targethud::onSignaturesReady();
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
    bactro::targethud::registerModule();
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
        LOGI("load %s %s", bactro::Name.data(), bactro::Version.data());
        writeStatusReplace(std::string("load ") + std::string(bactro::Name) + " " +
                           std::string(bactro::Version) + "\n");
        return true;
    }

    bool enable(pl::mod::ModContext&) {
        registerMenus();
        installSwapIntervalHook();
        writeStatus(g_swapIntervalHooked ? "eglSwapInterval OK" : "eglSwapInterval FAIL");
        resolveEverythingAsync();
        g_perfEnabled.store(true, std::memory_order_release);
        LOGI("BactroNative enabled");
        writeStatus("enabled");
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
