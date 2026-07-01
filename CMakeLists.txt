cmake_minimum_required(VERSION 3.15)
project(Corehold LANGUAGES CXX ASM_MASM)

# 仅 x64
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project targets x64 only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态链接 CRT（对应原 vcxproj 的 /MT /MTd）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 下载 nlohmann/json.hpp 单头文件
set(JSON_HPP "${CMAKE_CURRENT_SOURCE_DIR}/src/json.hpp")
if(NOT EXISTS "${JSON_HPP}")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nlohmann/json/refs/heads/develop/single_include/nlohmann/json.hpp"
        "${JSON_HPP}"
        SHOW_PROGRESS
        STATUS JSON_DOWNLOAD_STATUS
    )
    list(GET JSON_DOWNLOAD_STATUS 0 JSON_DOWNLOAD_CODE)
    if(NOT JSON_DOWNLOAD_CODE EQUAL 0)
        list(GET JSON_DOWNLOAD_STATUS 1 JSON_DOWNLOAD_MSG)
        file(REMOVE "${JSON_HPP}")
        message(FATAL_ERROR "Failed to download json.hpp: ${JSON_DOWNLOAD_MSG}")
    endif()
endif()

# 源文件
set(SOURCES
    src/dllmain.cpp
    src/clrloader.cpp
    src/downloader.cpp
    src/unzipper.cpp
    src/NsHiJack.asm
    src/NsHiJack.h
    src/resource.h
    src/winmm.rc
    src/winmm.def
)

# 构建为 DLL
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# 预处理器定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    _WIN64
    WINMM_EXPORTS
    USING_CMAKE
    $<$<CONFIG:Release>:DLL_EXPORT>
)

# 链接器选项与依赖库
target_link_options(${PROJECT_NAME} PRIVATE
    /IGNORE:4003
)
target_link_libraries(${PROJECT_NAME} PRIVATE
    winhttp
    crypt32
    ole32
    oleaut32
    shell32
)

# 模块定义文件
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/winmm.def"
)

# 源文件目录作为 include 路径
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 将 Corehold/ 文件夹复制到输出目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Corehold"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Corehold"
    COMMENT "Copying Corehold/ to output dir"
)

# 测试可执行文件 — 链接系统 winmm.lib（LoadLibrary 加载代理 DLL）
add_executable(corehold_test src/test_main.cpp)
add_dependencies(corehold_test ${PROJECT_NAME})
set_target_properties(corehold_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_include_directories(corehold_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)

