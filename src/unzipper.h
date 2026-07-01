//
// ZIP 解压器（Windows 10+ 内置 tar）
//
#pragma once
#include <string>

// 解压 zipFile → destDir，返回 true 成功
bool UnzipToDir(const std::wstring& zipFile, const std::wstring& destDir);
