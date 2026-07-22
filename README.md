# Corehold

通过 winmm.dll 代理劫持，在任意 x64 Windows 进程中启动 .NET CoreCLR 运行时并执行托管程序集。设计理念类似 [Doorstop](https://github.com/NeighTools/UnityDoorstop)，但面向 CoreCLR 而非 Mono。

## 工作原理

1. 将 `winmm.dll`（代理 DLL）和 `Corehold/` 文件夹放置到目标可执行文件同目录
2. 目标进程加载 `winmm.dll` 时，代理 DLL 自动初始化
3. 读取 `Corehold/corehold.json` 配置
4. 根据进程是否已携 CoreCLR 选择两种模式：

   **正常模式** — 进程无 CoreCLR：
   
   a. 若不存在则自动下载 .NET 运行时（NuGet 包 `.nupkg`）
   b. SHA256 校验完整性，`tar` 解压到 `runtime/`
   c. 加载 `coreclr.dll`，初始化 CoreCLR

   **被动模式** — 进程已内置 CoreCLR（如 Unity IL2CPP）：
   
   a. 预加载进程目录下的 `coreclr.dll`
   b. 挂钩 `coreclr_initialize`，在属性中追加自定义 DLL 路径到 `TRUSTED_PLATFORM_ASSEMBLIES`、`APP_PATHS`、`NATIVE_DLL_SEARCH_DIRECTORIES`
   c. 等待宿主初始化完成，获取 `hostHandle`/`domainId`

5. 通过 `coreclr_create_delegate` 调用托管程序集中的入口方法
6. 通过 `coreclr_set_error_writer` 将 CoreCLR 错误输出重定向到控制台

## 入口方法签名

托管 DLL 的入口方法必须匹配以下签名：

```csharp
[UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
public static int Main(int argc, nint argv)
```

- `argc` — 命令行参数个数（来自 `entrypoint_string_args`）
- `argv` — 指向字符串指针数组的指针
- 返回 `int`

## 环境变量

| 变量 | 说明 |
|---|---|
| `COREHOLD_TARGET_DIR` | 入口托管 DLL 所在目录。由 Corehold 在调用入口方法前设置，托管代码可通过 `Environment.GetEnvironmentVariable("COREHOLD_TARGET_DIR")` 获取。 |

## 配置

```json
{
    "enabled": true,
    "console_enabled": true,
    "runtime_path": "Corehold/runtime/",
    "coreclr_path": "Corehold/runtime/coreclr.dll",
    "target_assembly_path": "Corehold/managed/Mod.dll",
    "entry_point_method": "MyMod.Loader.Main",
    "entrypoint_string_args": ["arg1", "arg2"],
    "runtime_download_url": "https://globalcdn.nuget.org/packages/microsoft.netcore.app.runtime.win-x64.10.0.9.nupkg",
    "runtime_download_sha256": "b681046aef7cc0ffa9fec3ad865f033c7165300bd295c4a8902f24150a3fa414"
}
```

| 字段 | 说明 |
|---|---|
| `enabled` | 是否启用 |
| `console_enabled` | 是否创建控制台窗口（`true` 时 SetConsoleOutputCP 65001） |
| `runtime_path` | .NET 运行时核心 DLL 目录 |
| `coreclr_path` | coreclr.dll 相对路径 |
| `target_assembly_path` | 托管 DLL 文件路径或目录 |
| `entry_point_method` | 入口方法，格式 `Namespace.Type.Method` |
| `entrypoint_string_args` | JSON 字符串数组，传入入口方法 |
| `runtime_download_url` | 运行时下载直链（NuGet 包） |
| `runtime_download_sha256` | 运行时 SHA256 校验值（空则跳过） |

## 目录结构

```
TargetApp/
  winmm.dll                    ← 代理 DLL
  Corehold/
    corehold.json              ← 配置文件
    managed/                   ← 托管程序集
      Mod.dll
    runtime/                   ← .NET 运行时（自动下载）
      coreclr.dll
      System.Private.CoreLib.dll
      ...
```

## 构建

需要 Visual Studio 2022 Build Tools + CMake。

产物在CMake构建目录：

- `winmm.dll` — 代理 DLL
- `corehold_test.exe` — 测试可执行文件

## 与 Doorstop 对比

| | Doorstop | Corehold |
|---|---|---|
| 目标运行时 | Mono | CoreCLR (.NET 10) |
| 代理 DLL | `winhttp.dll` | `winmm.dll` |
| 运行时获取 | 预装 | 自动下载 + SHA256 校验 / 复用宿主 CoreCLR |
| 配置格式 | INI | JSON |
| 平台 | 跨平台 | Windows x64 |

## 许可

MIT
