#include <stdio.h>
#include <windows.h>
#include "winmm.h"
#include "init.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        if (Load() && Init())
        {
            InitOnAttach(hModule);
        }
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        InitOnDetach();
        Free();
    }
    return TRUE;
}
