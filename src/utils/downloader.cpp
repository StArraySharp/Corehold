#include "downloader.h"
#include <cstdio>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

bool HttpDownload(const std::wstring& url, const std::wstring& filePath) {
    // 确保目录存在
    std::wstring dirPath = filePath;
    size_t lastSep = dirPath.find_last_of(L"\\/");
    if (lastSep != std::wstring::npos) {
        std::wstring dir = dirPath.substr(0, lastSep);
        if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
            CreateDirectoryW(dir.c_str(), nullptr);
    }

    // 解析 URL
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    WCHAR host[512] = {}, path[4096] = {}, extra[512] = {};
    uc.lpszHostName = host;     uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;      uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra;   uc.dwExtraInfoLength = 512;

    if (!WinHttpCrackUrl(url.c_str(), 0, ICU_ESCAPE, &uc)) return false;

    // Session
    HINTERNET hSession = WinHttpOpen(L"Corehold/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    // 禁用自动重定向
    DWORD noRedirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &noRedirect, sizeof(noRedirect));

    // 设置超时 (resolve, connect, send, receive)
    WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 30000);

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // request 级别也禁用
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &noRedirect, sizeof(noRedirect));

    if (!WinHttpSendRequest(hRequest,
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        printf("[Download] WinHttpSendRequest: %lu\n", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        printf("[Download] WinHttpReceiveResponse: %lu\n", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // 检查状态码
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

    // 重定向 → 递归
    if (status == 301 || status == 302 || status == 307 || status == 308) {
        DWORD bufSize = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &bufSize, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bufSize > 0) {
            WCHAR* locBuf = (WCHAR*)malloc(bufSize);
            bool ok = false;
            if (locBuf && WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                              WINHTTP_HEADER_NAME_BY_INDEX,
                                              locBuf, &bufSize, WINHTTP_NO_HEADER_INDEX)) {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                ok = HttpDownload(locBuf, filePath);
            }
            free(locBuf);
            return ok;
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (status != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // 读取 Content-Length
    DWORD contentSize = 0;
    sz = sizeof(contentSize);
    DWORD idx = 0;
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &contentSize, &sz, &idx);
    if (contentSize > 0)
        printf("[Download] Content-Length: %lu bytes (%.1f MB)\n", contentSize, contentSize / 1048576.0f);

    // 开始接收数据
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD total = 0;
    BYTE buf[65536];
    DWORD lastMb = 0;
    int zeroAvailCount = 0;

    while (1) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            printf("[Download] QueryDataAvailable failed, got %lu bytes\n", total);
            break;
        }
        if (avail == 0) {
            // CDN 可能分批传输，等一等再试
            if (contentSize > 0 && total >= contentSize) break; // 已下载完毕
            if (++zeroAvailCount > 10) break; // 连续 10 次空，放弃
            Sleep(100);
            continue;
        }
        zeroAvailCount = 0;

        DWORD read = 0;
        DWORD chunk = avail > sizeof(buf) ? sizeof(buf) : avail;
        if (!WinHttpReadData(hRequest, buf, chunk, &read)) {
            printf("[Download] ReadData failed: %lu, got %lu bytes\n", GetLastError(), total);
            break;
        }
        if (read == 0) break;

        DWORD written;
        WriteFile(hFile, buf, read, &written, nullptr);
        total += read;

        DWORD mb = total / (1024 * 1024);
        if (mb != lastMb) {
            lastMb = mb;
            if (contentSize > 0)
                printf("[Download] %lu/%lu MB (%lu%%)\r",
                       mb, contentSize / 1048576, (DWORD)((double)total / contentSize * 100));
            else
                printf("[Download] %lu MB\r", mb);
        }

        if (contentSize > 0 && total >= contentSize) break;
    }
    printf("\n");

    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    printf("[Download] Done, %lu bytes (%.1f MB)\n", total, total / 1048576.0f);

    if (contentSize > 0 && total < contentSize) {
        printf("[Download] WARNING: incomplete download! Expected %lu, got %lu\n",
               contentSize, total);
    }

    return total > 0;
}
