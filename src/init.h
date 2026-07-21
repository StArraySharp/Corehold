#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

extern HANDLE g_hInitThread;

BOOL InitOnAttach(HMODULE hModule);
BOOL InitOnDetach();

__declspec(dllexport) void corehold_wait_ready(DWORD timeoutMs);

#ifdef __cplusplus
}
#endif
