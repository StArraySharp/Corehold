#include <cstdio>
#include <windows.h>
#include "utils/clrloader.h"
#include "config.h"
#include "init.h"

extern CoreholdConfig g_config;

static HANDLE g_hReadyEvent = nullptr;
static HANDLE g_hShutdownEvent = nullptr;

static DWORD WINAPI InitThread(LPVOID) {
    load_json_config();

    if (g_config.console_enabled) {
        AllocConsole();
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
        FILE* dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        setvbuf(stdout, nullptr, _IONBF, 0);
    }

    load_coreclr_functions();

    SetEvent(g_hReadyEvent);

    WaitForSingleObject(g_hShutdownEvent, INFINITE);
    coreclr_shutdown();
    return 0;
}

HANDLE g_hInitThread = nullptr;

BOOL InitOnAttach(HMODULE hModule) {
    try_passive_hook();
    g_hReadyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_hShutdownEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_hInitThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    return TRUE;
}

BOOL InitOnDetach() {
    if (g_hShutdownEvent) {
        SetEvent(g_hShutdownEvent);
        WaitForSingleObject(g_hInitThread, 5000);
        CloseHandle(g_hInitThread);
        CloseHandle(g_hShutdownEvent);
        CloseHandle(g_hReadyEvent);
        g_hInitThread = nullptr;
        g_hShutdownEvent = nullptr;
        g_hReadyEvent = nullptr;
    }
    coreclr_passive_cleanup();
    if (g_config.console_enabled)
        FreeConsole();
    return TRUE;
}

void corehold_wait_ready(DWORD timeoutMs) {
    if (g_hReadyEvent)
        WaitForSingleObject(g_hReadyEvent, timeoutMs);
}
