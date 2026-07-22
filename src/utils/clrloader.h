//
// Created by StArray on 2026/7/1.
//

#ifndef WINMM_X64_CLRLOADER_H
#define WINMM_X64_CLRLOADER_H
#define DLLEXPORT extern "C" __declspec(dllexport)

#include "../config.h"

extern CoreholdConfig g_config;

DLLEXPORT void load_coreclr_functions();
DLLEXPORT __cdecl void load_json_config();
void coreclr_shutdown();
bool try_passive_hook();
void coreclr_passive_cleanup();



#endif //WINMM_X64_CLRLOADER_H