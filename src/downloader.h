//
// HTTP 下载器
//
#pragma once
#include <string>

// 下载 URL → 本地文件，返回 true 成功
bool HttpDownload(const std::wstring& url, const std::wstring& filePath);
