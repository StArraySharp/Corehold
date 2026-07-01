#include <cstdio>
#include <windows.h>
#include "NsHiJack.h"
#include "clrloader.h"

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
        HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        const char* msg = "NsHiJack 已加载\r\n";
        DWORD written;
        WriteConsoleA(hStdOut, msg, lstrlenA(msg), &written, nullptr);
    }

    load_coreclr_functions();

    // 通知宿主：初始化完成
    SetEvent(g_hReadyEvent);

    // 阻塞，等 DLL_PROCESS_DETACH 信号再 shutdown
    WaitForSingleObject(g_hShutdownEvent, INFINITE);
    coreclr_shutdown();
    return 0;
}

HANDLE g_hInitThread = nullptr;

DLLEXPORT void corehold_wait_ready(DWORD timeoutMs) {
    if (g_hReadyEvent)
        WaitForSingleObject(g_hReadyEvent, timeoutMs);
}

BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD ul_reason_for_call,
                      LPVOID lpReserved
) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH: {
            if (!NsInitDll())
                return false;

            g_hReadyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            g_hShutdownEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            DisableThreadLibraryCalls(hModule);
            g_hInitThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        }
        break;
        case DLL_PROCESS_DETACH:
            if (g_hShutdownEvent) {
                SetEvent(g_hShutdownEvent);
                WaitForSingleObject(g_hInitThread, 5000);
                CloseHandle(g_hInitThread);
                CloseHandle(g_hShutdownEvent);
                CloseHandle(g_hReadyEvent);
            }
            if (g_config.console_enabled)
                FreeConsole();
            break;
        default:
            return TRUE;
    }
    return TRUE;
}
