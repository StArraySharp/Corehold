#include "clrloader.h"
#include "downloader.h"
#include "unzipper.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <wincrypt.h>
#include <MinHook.h>

#pragma comment(lib, "crypt32.lib")

CoreholdConfig g_config;
static std::string    g_exeDir;

// CoreCLR 运行时状态（DLL_PROCESS_DETACH 时清理）
static HMODULE        g_hCoreCLR = nullptr;
static void*          g_hostHandle = nullptr;
static unsigned int   g_domainId = 0;
static int (*g_shutdownFn)(void*, unsigned int) = nullptr;

static bool g_passiveMode = false;

// coreclr_initialize hook (passive mode)
static int (*original_coreclr_initialize)(const char*, const char*, int, const char**, const char**, void**, unsigned int*) = nullptr;
static HANDLE g_hInitDone = nullptr;

static bool GetCustomPathInfo(std::string& outDllPaths, std::string& outDirPaths) {
    std::string fullPath = g_exeDir + "\\" + g_config.target_assembly_path;
    DWORD attr = GetFileAttributesA(fullPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        outDirPaths = fullPath;
        WIN32_FIND_DATAA fd;
        std::string search = fullPath + "\\*.dll";
        HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!outDllPaths.empty()) outDllPaths += ";";
                outDllPaths += fullPath + "\\" + fd.cFileName;
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    } else {
        outDllPaths = fullPath;
        size_t pos = fullPath.find_last_of("\\/");
        if (pos != std::string::npos)
            outDirPaths = fullPath.substr(0, pos);
    }
    return !outDllPaths.empty() || !outDirPaths.empty();
}

static const char** ModifyCoreCLRProperties(const char** keys, const char** values, int count) {
    std::string dllPaths, dirPaths;
    if (!GetCustomPathInfo(dllPaths, dirPaths)) return nullptr;

    const char** copyValues = new const char*[count];
    for (int j = 0; j < count; ++j)
        copyValues[j] = values[j];

    bool modified = false;
    for (int i = 0; i < count; ++i) {
        if (!keys[i]) continue;

        std::string newValue;
        if (strcmp(keys[i], "TRUSTED_PLATFORM_ASSEMBLIES") == 0) {
            if (dllPaths.empty()) continue;
            newValue = std::string(values[i]) + ";" + dllPaths;
        } else if (strcmp(keys[i], "APP_PATHS") == 0) {
            if (dirPaths.empty()) continue;
            newValue = std::string(values[i]) + ";" + dirPaths;
        } else if (strcmp(keys[i], "NATIVE_DLL_SEARCH_DIRECTORIES") == 0) {
            if (dirPaths.empty()) continue;
            newValue = std::string(values[i]) + ";" + dirPaths;
        } else {
            continue;
        }

        char* copy = new char[newValue.size() + 1];
        strcpy(copy, newValue.c_str());
        copyValues[i] = copy;
        modified = true;
    }

    if (!modified) {
        delete[] copyValues;
        return nullptr;
    }
    return copyValues;
}

static int detour_coreclr_initialize(
    const char* exePath,
    const char* appDomainFriendlyName,
    int propertyCount,
    const char** propertyKeys,
    const char** propertyValues,
    void** hostHandle,
    unsigned int* domainId)
{
    const char** modValues = nullptr;
    if (g_passiveMode && !g_config.target_assembly_path.empty())
        modValues = ModifyCoreCLRProperties(propertyKeys, propertyValues, propertyCount);

    const char** finalValues = modValues ? modValues : propertyValues;
    int rc = original_coreclr_initialize(exePath, appDomainFriendlyName, propertyCount, propertyKeys, finalValues, hostHandle, domainId);

    if (modValues) {
        for (int i = 0; i < propertyCount; ++i)
            if (modValues[i] != propertyValues[i])
                delete[] modValues[i];
        delete[] modValues;
    }
    printf("[CoreCLR][Hook] coreclr_initialize returned %d\n", rc);
    printf("[CoreCLR][Hook] hostHandle = %p\n", *hostHandle);
    printf("[CoreCLR][Hook] domainId    = %u\n", *domainId);
    g_hostHandle = *hostHandle;
    g_domainId = *domainId;
    if (g_hInitDone)
        SetEvent(g_hInitDone);
    return rc;
}

void coreclr_shutdown() {
    if (g_passiveMode) {
        printf("[CoreCLR][Passive] Skipping shutdown (host owns the runtime)\n");
        if (g_hCoreCLR) {
            FreeLibrary(g_hCoreCLR);
            g_hCoreCLR = nullptr;
        }
        return;
    }
    printf("[CoreCLR] Shutting down (shutdownFn=%p, host=%p)...\n", g_shutdownFn, g_hostHandle);
    if (g_shutdownFn && g_hostHandle) {
        g_shutdownFn(g_hostHandle, g_domainId);
        g_hostHandle = nullptr;
        g_domainId = 0;
        g_shutdownFn = nullptr;
    }
    if (g_hCoreCLR) {
        FreeLibrary(g_hCoreCLR);
        g_hCoreCLR = nullptr;
    }
    printf("[CoreCLR] Shutdown complete\n");
}

void load_json_config() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    std::string dir(exePath);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos);
    g_exeDir = dir;

    std::string configPath = dir + "\\Corehold\\corehold.json";
    g_config = CoreholdConfig::Load(configPath);

    printf("[Corehold] Dir: %s\n  Config: enabled=%d, entry=%s\n",
           dir.c_str(), g_config.enabled, g_config.entry_point_method.c_str());

}

static void MoveFilesToDir(const std::string& srcDir, const std::string& dstDir) {
    WIN32_FIND_DATAA fd;
    std::string search = srcDir + "\\*.*";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        std::string src = srcDir + "\\" + fd.cFileName;
        std::string dst = dstDir + "\\" + fd.cFileName;
        MoveFileExA(src.c_str(), dst.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

static void RemoveDirTree(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    std::string search = dir + "\\*";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        RemoveDirectoryA(dir.c_str());
        return;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            RemoveDirTree(full);
        else
            DeleteFileA(full.c_str());
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    RemoveDirectoryA(dir.c_str());
}

static bool VerifySha256(const std::string& filePath, const std::string& expectedHex) {
    if (expectedHex.empty()) {
        printf("[Corehold] SHA256 not configured, skipping verification\n");
        return true;
    }

    HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    bool ok = false;

    if (CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            BYTE buf[65536];
            DWORD read;
            while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
                CryptHashData(hHash, buf, read, 0);
            }

            BYTE hash[32];
            DWORD hashLen = sizeof(hash);
            if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
                char hex[65] = {};
                for (int i = 0; i < 32; ++i)
                    sprintf_s(hex + i * 2, 3, "%02x", hash[i]);
                ok = (expectedHex == hex);
                if (!ok)
                    printf("[Corehold] SHA256 mismatch!\n  expected: %s\n  actual:   %s\n",
                           expectedHex.c_str(), hex);
                else
                    printf("[Corehold] SHA256 verified\n");
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }

    CloseHandle(hFile);
    return ok;
}

static void DownloadRuntime() {
    std::string downloadUrl = g_config.runtime_download_url;

    if (downloadUrl.empty()) {
        printf("[Corehold] FATAL: No runtime_download_url configured\n");
        exit(1);
    }

    // 已存在则跳过
    if (g_passiveMode) {
        printf("[Corehold] Passive mode: CoreCLR directory exists, skip download\n");
        return;
    }

    std::string coreclrFull = g_exeDir + "\\" + g_config.coreclr_path;
    if (GetFileAttributesA(coreclrFull.c_str()) != INVALID_FILE_ATTRIBUTES) {
        printf("[Corehold] Runtime already exists, skip download\n");
        return;
    }

    printf("[Corehold] Downloading: %s ...\n", downloadUrl.c_str());

    // 下载 .nupkg 到临时位置（SHA256 失败时重试，最多 3 次）
    std::string nupkgPath = g_exeDir + "\\Corehold\\_runtime.nupkg";
    std::wstring url(downloadUrl.begin(), downloadUrl.end());
    std::wstring file(nupkgPath.begin(), nupkgPath.end());

    for (int retry = 0; retry < 3; ++retry) {
        DeleteFileA(nupkgPath.c_str());

        if (!HttpDownload(url, file)) {
            printf("[Corehold] Download failed (attempt %d/3)\n", retry + 1);
            continue;
        }

        if (!VerifySha256(nupkgPath, g_config.runtime_download_sha256)) {
            printf("[Corehold] SHA256 check failed, retrying... (attempt %d/3)\n", retry + 1);
            continue;
        }

        // 下载 + 校验成功
        break;
    }

    // 最终检查文件是否存在
    if (GetFileAttributesA(nupkgPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("[Corehold] FATAL: All download attempts failed\n");
        exit(1);
    }

    printf("[Corehold] Download complete, extracting...\n");

    // 解压到 runtime_path
    std::string destDir = g_exeDir + "\\" + g_config.runtime_path;
    // 去掉末尾反斜杠
    while (!destDir.empty() && (destDir.back() == '\\' || destDir.back() == '/'))
        destDir.pop_back();

    std::wstring wNupkg(nupkgPath.begin(), nupkgPath.end());
    std::wstring wDest(destDir.begin(), destDir.end());

    if (!UnzipToDir(wNupkg, wDest)) {
        printf("[Corehold] FATAL: Extraction failed!\n");
        DeleteFileA(nupkgPath.c_str());
        exit(1);
    }

    // 删除 .nupkg
    DeleteFileA(nupkgPath.c_str());

    // 解压后目录结构 (NuGet 包)：
    //   runtimes/win-x64/native/    → 原生 DLL (coreclr.dll, clrjit.dll...)
    //   runtimes/win-x64/lib/net10.0/ → 托管程序集 (System.Private.CoreLib.dll...)

    // 搬移 native/
    std::string nativeDir = destDir + "\\runtimes\\win-x64\\native";
    MoveFilesToDir(nativeDir, destDir);
    RemoveDirTree(nativeDir);

    // 搬移托管程序集 lib/net10.0/
    std::string libDir = destDir + "\\runtimes\\win-x64\\lib\\net10.0";
    MoveFilesToDir(libDir, destDir);
    RemoveDirTree(libDir);

    // 清理空目录
    RemoveDirTree(destDir + "\\runtimes\\win-x64\\lib");
    RemoveDirTree(destDir + "\\runtimes\\win-x64");
    RemoveDirTree(destDir + "\\runtimes");

    // 清理 NuGet 元数据（非运行时文件）
    for (auto& f : {"data", "package", "tools", "_rels"}) {
        std::string path = destDir + "\\" + f;
        WIN32_FIND_DATAA fd;
        std::string s = path + "\\*.*";
        HANDLE h = FindFirstFileA(s.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                    continue;
                DeleteFileA((path + "\\" + fd.cFileName).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirTree(path);
    }

    // 清理包文件 (.nuspec, .xml, .txt 等)
    WIN32_FIND_DATAA fd2;
    std::string s2 = destDir + "\\*.*";
    HANDLE h2 = FindFirstFileA(s2.c_str(), &fd2);
    if (h2 != INVALID_HANDLE_VALUE) {
        do {
            if (fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (strstr(fd2.cFileName, ".dll")) continue;
            if (strstr(fd2.cFileName, ".json")) continue;
            DeleteFileA((destDir + "\\" + fd2.cFileName).c_str());
        } while (FindNextFileA(h2, &fd2));
        FindClose(h2);
    }

    printf("[Corehold] Runtime ready.\n");
}

bool try_passive_hook() {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos);

    std::string paths[] = {
        dir + "\\CoreCLR\\native\\coreclr.dll",
        dir + "\\coreclr.dll"
    };
    bool found = false;
    for (auto& p : paths) {
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            found = true;
            break;
        }
    }
    if (!found) return false;

    AllocConsole();
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);

    load_json_config();

    g_passiveMode = true;
    printf("[CoreCLR][Passive] Preemptively loading coreclr.dll ...\n");

    HMODULE hCoreCLR = nullptr;
    for (auto& p : paths) {
        std::wstring w(p.begin(), p.end());
        hCoreCLR = LoadLibraryExW(w.c_str(), nullptr, 0);
        if (hCoreCLR) {
            printf("[CoreCLR][Passive] Loaded from: %s\n", p.c_str());
            break;
        }
    }
    if (!hCoreCLR) {
        printf("[CoreCLR][Passive] FATAL: Failed to load coreclr.dll from any location\n");
        return true;
    }
    printf("[CoreCLR][Passive] coreclr.dll loaded at %p\n", hCoreCLR);
    g_hCoreCLR = hCoreCLR;

    using ErrorWriterFn = void (*)(const char*);
    using SetErrorWriterFn = ErrorWriterFn (*)(ErrorWriterFn);
    auto setErrorWriter = (SetErrorWriterFn)GetProcAddress(hCoreCLR, "coreclr_set_error_writer");
    if (setErrorWriter)
        setErrorWriter([](const char* msg) { printf("[CoreCLR][Error] %s\n", msg); });

    auto initializeAddr = (PVOID)GetProcAddress(hCoreCLR, "coreclr_initialize");
    if (!initializeAddr) {
        printf("[CoreCLR][Passive] FATAL: coreclr_initialize not found\n");
        return true;
    }

    if (MH_Initialize() != MH_OK) {
        printf("[CoreCLR][Passive] FATAL: MinHook init failed\n");
        return true;
    }

    g_hInitDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    MH_STATUS mh = MH_CreateHook((LPVOID)initializeAddr, (LPVOID)detour_coreclr_initialize,
                                 (LPVOID*)&original_coreclr_initialize);
    if (mh != MH_OK) {
        printf("[CoreCLR][Passive] FATAL: MH_CreateHook(initialize) failed: %s\n", MH_StatusToString(mh));
        return true;
    }
    mh = MH_EnableHook(initializeAddr);
    if (mh != MH_OK) {
        printf("[CoreCLR][Passive] FATAL: MH_EnableHook(initialize) failed: %s\n", MH_StatusToString(mh));
        return true;
    }

    printf("[CoreCLR][Passive] coreclr_initialize hooked\n");
    return true;
}

void coreclr_passive_cleanup() {
    if (g_passiveMode) {
        if (g_hCoreCLR) {
            FreeLibrary(g_hCoreCLR);
            g_hCoreCLR = nullptr;
        }
        FreeConsole();
    }
}

void load_coreclr_functions() {
    if (!g_config.enabled) return;

    using CreateDelegateFn = int (*)(void*, unsigned int, const char*, const char*, const char*, void**);

    auto run_entry = [&](CreateDelegateFn _create_delegate) {
        if (!g_config.target_assembly_path.empty()) {
            std::string fp = g_exeDir + "\\" + g_config.target_assembly_path;
            DWORD attr = GetFileAttributesA(fp.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                std::string dir;
                if (attr & FILE_ATTRIBUTE_DIRECTORY) dir = fp;
                else { size_t p = fp.find_last_of("\\/"); if (p != std::string::npos) dir = fp.substr(0, p); }
                if (!dir.empty()) {
                    SetEnvironmentVariableA("COREHOLD_TARGET_DIR", dir.c_str());
                    printf("[CoreCLR] COREHOLD_TARGET_DIR=%s\n", dir.c_str());
                }
            }
        }
        std::string entryAssembly = "System.Private.CoreLib";

        if (!g_config.target_assembly_path.empty()) {
            std::string fullPath = g_exeDir + "\\" + g_config.target_assembly_path;
            while (!fullPath.empty() && (fullPath.back() == '\\' || fullPath.back() == '/'))
                fullPath.pop_back();

            DWORD attr = GetFileAttributesA(fullPath.c_str());
            bool isFile = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));

            if (isFile) {
                entryAssembly = fullPath.substr(fullPath.find_last_of("\\/") + 1);
                if (entryAssembly.size() > 4)
                    if (_stricmp(entryAssembly.c_str() + entryAssembly.size() - 4, ".dll") == 0)
                        entryAssembly = entryAssembly.substr(0, entryAssembly.size() - 4);
            } else {
                WIN32_FIND_DATAA fd;
                HANDLE hFind = FindFirstFileA((fullPath + "\\*.dll").c_str(), &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            std::string name = fd.cFileName;
                            if (name.rfind("System.", 0) != 0) {
                                entryAssembly = name;
                                break;
                            }
                        }
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                }
            }
        }
        if (entryAssembly.size() > 4)
            if (_stricmp(entryAssembly.c_str() + entryAssembly.size() - 4, ".dll") == 0)
                entryAssembly = entryAssembly.substr(0, entryAssembly.size() - 4);

        std::string fullMethod = g_config.entry_point_method;
        size_t dot1 = fullMethod.rfind('.');
        size_t dot2 = (dot1 != std::string::npos) ? fullMethod.rfind('.', dot1 - 1) : std::string::npos;

        std::string typeName, methodName;
        if (dot2 != std::string::npos) {
            typeName = fullMethod.substr(0, dot1);
            methodName = fullMethod.substr(dot1 + 1);
        } else {
            typeName = fullMethod.substr(0, dot1);
            methodName = fullMethod.substr(dot1 + 1);
        }

        printf("[CoreCLR] entry type: %s, method: %s, assembly: %s\n",
               typeName.c_str(), methodName.c_str(), entryAssembly.c_str());

        using entry_fn = int (__cdecl *)(int argc, void* argv);
        entry_fn fn = nullptr;
        int rc = _create_delegate(g_hostHandle, g_domainId,
                                  entryAssembly.c_str(), typeName.c_str(), methodName.c_str(),
                                  (void**)&fn);

        if (rc >= 0 && fn) {
            int argc = (int)g_config.entrypoint_string_args.size();
            void* argv = nullptr;
            if (argc > 0) {
                argv = malloc(argc * sizeof(const char*));
                for (int i = 0; i < argc; ++i)
                    ((const char**)argv)[i] = g_config.entrypoint_string_args[i].c_str();
            }
            int ret = fn(argc, argv);
            printf("[CoreCLR] %s.%s() = %d\n", typeName.c_str(), methodName.c_str(), ret);
            free(argv);
        }

        if (rc < 0)
            printf("[CoreCLR] coreclr_create_delegate failed: 0x%08X\n", rc);
    };

    if (g_passiveMode) {
        printf("[CoreCLR][Passive] Waiting for coreclr_initialize from host ...\n");
        WaitForSingleObject(g_hInitDone, INFINITE);
        printf("[CoreCLR][Passive] hostHandle=%p, domainId=%u\n", g_hostHandle, g_domainId);

        auto _create_delegate = (CreateDelegateFn)GetProcAddress(g_hCoreCLR, "coreclr_create_delegate");
        g_shutdownFn = (int(*)(void*, unsigned int))GetProcAddress(g_hCoreCLR, "coreclr_shutdown");
        if (!g_shutdownFn)
            g_shutdownFn = (int(*)(void*, unsigned int))GetProcAddress(g_hCoreCLR, "coreclr_shutdown_2");
        if (!_create_delegate) {
            printf("[CoreCLR][Passive] FATAL: coreclr_create_delegate not found\n");
            return;
        }
        run_entry(_create_delegate);
        return;
    }

    DownloadRuntime();  // 失败则 exit(1)

    // 构建 coreclr.dll 全路径
    std::string coreclrFull = g_exeDir + "\\" + g_config.coreclr_path;
    std::wstring wCoreclr(coreclrFull.begin(), coreclrFull.end());

    printf("[CoreCLR] Loading %s ...\n", coreclrFull.c_str());
    HMODULE hCoreCLR = LoadLibraryExW(wCoreclr.c_str(), nullptr, 0);
    if (!hCoreCLR) {
        printf("[CoreCLR] FATAL: Failed to load coreclr.dll: %lu\n", GetLastError());
        exit(1);
    }
    g_hCoreCLR = hCoreCLR;

    // 获取 CoreCLR 导出函数
    auto _initialize = (int(*)(const char*, const char*, int, const char**, const char**, void**, unsigned int*))
        GetProcAddress(hCoreCLR, "coreclr_initialize");
    auto _create_delegate = (CreateDelegateFn)GetProcAddress(hCoreCLR, "coreclr_create_delegate");

    // coreclr_shutdown: .NET 8+ 主入口，coreclr_shutdown_2: .NET 5-7
    g_shutdownFn = (int(*)(void*, unsigned int))GetProcAddress(hCoreCLR, "coreclr_shutdown");
    if (!g_shutdownFn)
        g_shutdownFn = (int(*)(void*, unsigned int))GetProcAddress(hCoreCLR, "coreclr_shutdown_2");
    printf("[CoreCLR] shutdown function: %p\n", g_shutdownFn);

    if (!_initialize || !_create_delegate) {
        printf("[CoreCLR] FATAL: Failed to find CoreCLR exports\n");
        exit(1);
    }

    // 构建 TPA 列表（runtime_path 下的核心 .dll）
    std::string runtimeDir = g_exeDir + "\\";
    runtimeDir += g_config.runtime_path;
    while (!runtimeDir.empty() && (runtimeDir.back() == '\\' || runtimeDir.back() == '/'))
        runtimeDir.pop_back();

    std::string tpaList;
    WIN32_FIND_DATAA fd;
    std::string search = runtimeDir + "\\*.dll";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!tpaList.empty()) tpaList += ";";
            tpaList += runtimeDir + "\\" + fd.cFileName;
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    // target_assembly_path: 自定义 DLL 文件 或 目录
    std::string appDir;
    std::string entryAssembly = "System.Private.CoreLib";

    if (!g_config.target_assembly_path.empty()) {
        std::string fullPath = g_exeDir + "\\" + g_config.target_assembly_path;
        while (!fullPath.empty() && (fullPath.back() == '\\' || fullPath.back() == '/'))
            fullPath.pop_back();

        DWORD attr = GetFileAttributesA(fullPath.c_str());
        bool isFile = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));

        if (isFile) {
            size_t sep = fullPath.find_last_of("\\/");
            appDir = fullPath.substr(0, sep);
            std::string customDll = fullPath.substr(sep + 1);
            entryAssembly = customDll;
            if (entryAssembly.size() > 4)
                if (_stricmp(entryAssembly.c_str() + entryAssembly.size() - 4, ".dll") == 0)
                    entryAssembly = entryAssembly.substr(0, entryAssembly.size() - 4);

            if (!tpaList.empty()) tpaList += ";";
            tpaList += fullPath;
        } else {
            appDir = fullPath;
            search = appDir + "\\*.dll";
            hFind = FindFirstFileA(search.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!tpaList.empty()) tpaList += ";";
                    tpaList += appDir + "\\" + fd.cFileName;
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }

            hFind = FindFirstFileA((appDir + "\\*.dll").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        std::string name = fd.cFileName;
                        if (name.rfind("System.", 0) != 0) {
                            entryAssembly = name;
                            break;
                        }
                    }
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        }
    }

    // APP_PATHS: 自定义 DLL 目录优先，runtime 后备
    std::string appPaths = (!appDir.empty() ? appDir : runtimeDir);
    std::string nativeDirs = runtimeDir + ";" + g_exeDir;
    if (!appDir.empty())
        nativeDirs += ";" + appDir;

    const char* propKeys[] = {
        "APP_PATHS",
        "TRUSTED_PLATFORM_ASSEMBLIES",
        "APP_CONTEXT_BASE_DIRECTORY",
        "NATIVE_DLL_SEARCH_DIRECTORIES"
    };
    const char* propValues[] = {
        appPaths.c_str(),
        tpaList.c_str(),
        runtimeDir.c_str(),
        nativeDirs.c_str()
    };

    void* hostHandle = nullptr;
    unsigned int domainId = 0;

    int rc = _initialize("Corehold", "CoreholdDomain",
                         4, propKeys, propValues,
                         &hostHandle, &domainId);
    if (rc < 0) {
        printf("[CoreCLR] FATAL: coreclr_initialize failed: 0x%08X\n", rc);
        exit(1);
    }
    g_hostHandle = hostHandle;
    g_domainId = domainId;
    printf("[CoreCLR] Initialized, domainId=%u\n", domainId);

    run_entry(_create_delegate);
    // 由 coreclr_shutdown() 在同线程清理
}