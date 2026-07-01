#include "unzipper.h"
#include <cstdio>
#include <windows.h>

bool UnzipToDir(const std::wstring& zipFile, const std::wstring& destDir) {
    CreateDirectoryW(destDir.c_str(), nullptr);

    // Windows 10+ 内置 tar，直接解压 .nupkg（即 .zip）
    std::wstring cmd = L"tar -xf \"" + zipFile + L"\" -C \"" + destDir + L"\"";
    printf("[Unzip] %ls\n", cmd.c_str());

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr,
                             FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok) {
        WaitForSingleObject(pi.hProcess, 60000); // 最多等 60s
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        printf("[Unzip] exit code: %lu\n", exitCode);
        return exitCode == 0;
    }

    printf("[Unzip] CreateProcess failed: %lu\n", GetLastError());
    return false;
}
