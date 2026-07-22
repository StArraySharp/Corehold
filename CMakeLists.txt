cmake_minimum_required(VERSION 3.15)
project(Corehold LANGUAGES C CXX)

# ============================================================================
# x64 only
# ============================================================================
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project targets x64 only.")
endif()

# ============================================================================
# C++ standard
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Static CRT (/MT /MTd)
# ============================================================================
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ============================================================================
# Dependencies: MinHook (API hooking library)
# ============================================================================
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY  https://github.com/TsudaKageyu/minhook.git
    GIT_TAG         master
)
set(MINHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(minhook)

# ============================================================================
# Sources
# ============================================================================
set(SOURCES
    src/dllmain.c
    src/winmm.c
    src/init.cpp
    src/utils/clrloader.cpp
    src/utils/downloader.cpp
    src/utils/unzipper.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    minhook
    winhttp
    crypt32
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/winmm.def")
else()
    target_sources(${PROJECT_NAME} PRIVATE src/winmm.def)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
)

if(DEFINED OUTPUT_NAME)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${OUTPUT_NAME}")
endif()

# ============================================================================
# Post-build: copy Corehold/ to output directory
# ============================================================================
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Corehold"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Corehold"
    COMMENT "Copying Corehold/ to output dir"
)

# ============================================================================
# Test executable
# ============================================================================
add_executable(corehold_test src/test_main.cpp)
add_dependencies(corehold_test ${PROJECT_NAME})

set_target_properties(corehold_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(corehold_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
