#include "log.h"
#include "discovery.h"
#include "panel.h"
#include "net.h"
#include "savemgr.h"
#include "engine.h"
#include "version.h"

#include <windows.h>

static bool g_started = false;

static DWORD WINAPI PanelWatcher(LPVOID) {
    // Wait until the world is discovered, then open the login panel.
    while (!kmmo::Discovery::Get().Ready()) {
        Sleep(500);
    }
    kmmo::PanelStart(KMMO_SERVER_HOST, kmmo::net::kProtoPort);
    return 0;
}

extern "C" __declspec(dllexport) void dllStartPlugin() {
    if (g_started) return;
    g_started = true;

    kmmo::log::Init();
    kmmo::log::Info("KenshiMMO plugin v%s loaded (dllStartPlugin)",
                    KENSHI_MMO_VERSION);
    kmmo::engine::Init();
    kmmo::savemgr::Init();
    kmmo::Discovery::Get().Start();
    CreateThread(nullptr, 0, PanelWatcher, nullptr, 0, nullptr);
}

// RE_Kenshi loads mod plugins by the C++-mangled entry (?startPlugin@@YAXXZ).
void startPlugin() {
    dllStartPlugin();
}

extern "C" __declspec(dllexport) void dllStopPlugin() {
    if (!g_started) return;
    g_started = false;

    kmmo::PanelStop();
    kmmo::Discovery::Get().Stop();
    kmmo::savemgr::Shutdown();
    kmmo::log::Info("KenshiMMO plugin stopped");
    kmmo::log::Close();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_started) {
            kmmo::PanelStop();
            kmmo::Discovery::Get().Stop();
            kmmo::savemgr::Shutdown();
            kmmo::log::Close();
        }
    }
    return TRUE;
}