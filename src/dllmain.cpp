#include <windows.h>
#include "tracker.h"
#include "overlay.h"

HMODULE g_hModule = nullptr;

static HANDLE g_trackerThread = nullptr;
static HANDLE g_overlayThread = nullptr;

static DWORD WINAPI TrackerThread(LPVOID) {
    Sleep(5000);
    RunTracker();
    return 0;
}

static DWORD WINAPI OverlayThread(LPVOID) {
    Sleep(6000); // Overlay starts slightly after tracker
    StartOverlay();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        g_trackerThread = CreateThread(nullptr, 0, TrackerThread, nullptr, 0, nullptr);
        g_overlayThread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        if (lpvReserved == nullptr) {
            g_trackerRunning = false;
            StopOverlay();
            if (g_trackerThread) WaitForSingleObject(g_trackerThread, 3000);
        }
        break;
    }
    return TRUE;
}
