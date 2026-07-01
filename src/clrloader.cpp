#include "clrloader.h"
#include "downloader.h"
#include "unzipper.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

CoreholdConfig g_config;
static std::string    g_exeDir;

// CoreCLR 运行时状态（DLL_PROCESS_DETACH 时清理）
static HMODULE        g_hCoreCLR = nullptr;
static void*          g_hostHandle = nullptr;
static unsigned int   g_domainId = 0;
static int (*g_shutdownFn)(void*, unsigned int) = nullptr;

void coreclr_shutdown() {
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

void load_coreclr_functions() {
    if (!g_config.enabled) return;

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
    auto _create_delegate = (int(*)(void*, unsigned int, const char*, const char*, const char*, void**))
        GetProcAddress(hCoreCLR, "coreclr_create_delegate");

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
    std::string customDll; // 单个 DLL 文件名
    std::string entryAssembly = "System.Private.CoreLib";

    if (!g_config.target_assembly_path.empty()) {
        std::string fullPath = g_exeDir + "\\" + g_config.target_assembly_path;
        while (!fullPath.empty() && (fullPath.back() == '\\' || fullPath.back() == '/'))
            fullPath.pop_back();

        // 判断是文件还是目录
        DWORD attr = GetFileAttributesA(fullPath.c_str());
        bool isFile = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));

        if (isFile) {
            // 单个 DLL 文件
            size_t sep = fullPath.find_last_of("\\/");
            appDir = fullPath.substr(0, sep);
            customDll = fullPath.substr(sep + 1);
            // 去掉 .dll 后缀作为程序集名
            entryAssembly = customDll;
            if (entryAssembly.size() > 4)
                if (_stricmp(entryAssembly.c_str() + entryAssembly.size() - 4, ".dll") == 0)
                    entryAssembly = entryAssembly.substr(0, entryAssembly.size() - 4);

            // 加到 TPA
            if (!tpaList.empty()) tpaList += ";";
            tpaList += fullPath;
        } else {
            // 目录 → 扫描所有 DLL
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

            // 取第一个非 System.* 的 DLL 作入口程序集
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
    // NATIVE_DLL_SEARCH_DIRECTORIES: 两者都有
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

    // 解析入口方法（格式: Namespace.Type.Method）
    std::string fullMethod = g_config.entry_point_method;
    size_t dot1 = fullMethod.rfind('.');
    size_t dot2 = (dot1 != std::string::npos) ? fullMethod.rfind('.', dot1 - 1) : std::string::npos;

    std::string typeName, methodName;
    if (dot2 != std::string::npos) {
        typeName = fullMethod.substr(0, dot1);      // Namespace.Type
        methodName = fullMethod.substr(dot1 + 1);   // Method
    } else {
        typeName = fullMethod.substr(0, dot1);
        methodName = fullMethod.substr(dot1 + 1);
    }

    printf("[CoreCLR] entry type: %s, method: %s, assembly: %s\n",
           typeName.c_str(), methodName.c_str(), entryAssembly.c_str());

    // 统一签名: int Method(int argc, nint argv)  — cdecl
    using entry_fn = int (__cdecl *)(int argc, void* argv);
    entry_fn fn = nullptr;
    rc = _create_delegate(hostHandle, domainId,
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

    if (rc < 0) {
        printf("[CoreCLR] coreclr_create_delegate failed: 0x%08X\n", rc);
    }

    // 由 coreclr_shutdown() 在同线程清理
}