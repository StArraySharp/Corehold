//
// Created by StArray on 2026/7/1.
//

#ifndef WINMM_X64_CONFIG_H
#define WINMM_X64_CONFIG_H

#include <string>
#include <vector>
#include <fstream>
#include <windows.h>
#include "json.hpp"

struct CoreholdConfig {
    bool        enabled                = true;
    bool        console_enabled        = true;
    std::string runtime_path           = "Corehold/runtime/";
    std::string coreclr_path           = "Corehold/runtime/coreclr.dll";
    std::string target_assembly_path   = "";
    std::string entry_point_method     = "Namespace.Type.Method";
    std::vector<std::string> entrypoint_string_args;
    std::string runtime_download_url   = "https://globalcdn.nuget.org/packages/microsoft.netcore.app.runtime.win-x64.10.0.9.nupkg?packageVersion=10.0.9";
    std::string runtime_download_sha256 = "";

    static CoreholdConfig Load(const std::string& configPath) {
        CoreholdConfig cfg;

        std::ifstream ifs(configPath);
        if (!ifs.is_open()) {
            printf("[Config] Cannot open: %s\n", configPath.c_str());
            return cfg;
        }

        nlohmann::json j;
        try {
            ifs >> j;

            if (j.contains("enabled"))                cfg.enabled                = j["enabled"];
            if (j.contains("console_enabled"))        cfg.console_enabled        = j["console_enabled"];
            if (j.contains("runtime_path"))           cfg.runtime_path           = j["runtime_path"];
            if (j.contains("coreclr_path"))           cfg.coreclr_path           = j["coreclr_path"];
            if (j.contains("target_assembly_path"))   cfg.target_assembly_path   = j["target_assembly_path"];
            if (j.contains("entry_point_method"))     cfg.entry_point_method     = j["entry_point_method"];
            if (j.contains("entrypoint_string_args") && j["entrypoint_string_args"].is_array()) {
                for (auto& item : j["entrypoint_string_args"])
                    cfg.entrypoint_string_args.push_back(item.get<std::string>());
            }
            if (j.contains("runtime_download_sha256"))
                cfg.runtime_download_sha256 = j["runtime_download_sha256"];
            if (j.contains("runtime_download_url"))
                cfg.runtime_download_url = j["runtime_download_url"];

        } catch (const std::exception& e) {
            printf("[Config] JSON parse error: %s\n", e.what());
        }

        return cfg;
    }
};

#endif //WINMM_X64_CONFIG_H