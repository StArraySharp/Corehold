#include <iostream>
#include <windows.h>
#include <mmsystem.h>
#include "json.hpp"

typedef UINT(WINAPI* pfnAuxGetNumDevs)();
typedef UINT(WINAPI* pfnJoyGetNumDevs)();
typedef UINT(WINAPI* pfnWaveOutGetNumDevs)();
typedef UINT(WINAPI* pfnMidiOutGetNumDevs)();
typedef BOOL(WINAPI* pfnPlaySoundW)(LPCWSTR, HMODULE, DWORD);
typedef MCIERROR(WINAPI* pfnMciSendStringW)(LPCWSTR, LPWSTR, UINT, HANDLE);
typedef DWORD(WINAPI* pfnTimeGetTime)();

int main() {
    std::cout << "Corehold Test Executable" << std::endl;

    nlohmann::json j;
    j["name"] = "Corehold";
    j["version"] = "1.0";
    std::cout << "JSON: " << j.dump(4) << std::endl;

    std::cout << "\n=== Loading proxy winmm.dll ===" << std::endl;
    HMODULE hProxy = LoadLibraryW(L"winmm.dll");
    if (!hProxy) {
        std::cerr << "Failed to load proxy winmm.dll, error: " << GetLastError() << std::endl;
        std::string a; std::cin >> a;
        return 1;
    }
    std::cout << "Proxy winmm.dll loaded at: " << hProxy << std::endl;
    std::cout << "Waiting for init thread..." << std::endl;
    Sleep(2000);  // 等 DllMain 中的 CreateThread 完成

    auto _auxGetNumDevs     = (pfnAuxGetNumDevs)   GetProcAddress(hProxy, "auxGetNumDevs");
    auto _joyGetNumDevs     = (pfnJoyGetNumDevs)    GetProcAddress(hProxy, "joyGetNumDevs");
    auto _waveOutGetNumDevs = (pfnWaveOutGetNumDevs)GetProcAddress(hProxy, "waveOutGetNumDevs");
    auto _midiOutGetNumDevs = (pfnMidiOutGetNumDevs)GetProcAddress(hProxy, "midiOutGetNumDevs");
    auto _PlaySoundW        = (pfnPlaySoundW)       GetProcAddress(hProxy, "PlaySoundW");
    auto _mciSendStringW    = (pfnMciSendStringW)   GetProcAddress(hProxy, "mciSendStringW");
    auto _timeGetTime       = (pfnTimeGetTime)      GetProcAddress(hProxy, "timeGetTime");

    std::cout << "\n=== Hijacked winmm API Tests ===" << std::endl;

    if (_auxGetNumDevs)
        std::cout << "auxGetNumDevs: " << _auxGetNumDevs() << std::endl;
    else std::cout << "auxGetNumDevs: NOT FOUND" << std::endl;

    if (_joyGetNumDevs)
        std::cout << "joyGetNumDevs: " << _joyGetNumDevs() << std::endl;
    else std::cout << "joyGetNumDevs: NOT FOUND" << std::endl;

    if (_waveOutGetNumDevs)
        std::cout << "waveOutGetNumDevs: " << _waveOutGetNumDevs() << std::endl;
    else std::cout << "waveOutGetNumDevs: NOT FOUND" << std::endl;

    if (_midiOutGetNumDevs)
        std::cout << "midiOutGetNumDevs: " << _midiOutGetNumDevs() << std::endl;
    else std::cout << "midiOutGetNumDevs: NOT FOUND" << std::endl;

    if (_PlaySoundW) {
        BOOL ps = _PlaySoundW(nullptr, nullptr, SND_NODEFAULT);
        std::cout << "PlaySoundW(nullptr): " << (ps ? "OK" : "FAIL") << std::endl;
    } else std::cout << "PlaySoundW: NOT FOUND" << std::endl;

    if (_mciSendStringW) {
        WCHAR buf[256] = {0};
        MCIERROR mciErr = _mciSendStringW(L"sysinfo all quantity", buf, 256, nullptr);
        std::cout << "mciSendStringW: " << (mciErr == 0 ? "OK (" : "FAIL (") << mciErr << ")" << std::endl;
    } else std::cout << "mciSendStringW: NOT FOUND" << std::endl;

    if (_timeGetTime)
        std::cout << "timeGetTime: " << _timeGetTime() << " ms" << std::endl;
    else std::cout << "timeGetTime: NOT FOUND" << std::endl;

    // 等待 DLL 初始化完成（下载+CoreCLR）
    typedef void (WINAPI* pfnWaitReady)(DWORD);
    auto _waitReady = (pfnWaitReady)GetProcAddress(hProxy, "corehold_wait_ready");
    if (_waitReady) {
        std::cout << "\nWaiting for init..." << std::endl;
        _waitReady(300000); // 最多 5 分钟
    }

    FreeLibrary(hProxy);
    std::cout << "\nAll tests passed." << std::endl;
    return 0;
}
