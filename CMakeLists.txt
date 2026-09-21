cmake_minimum_required(VERSION 3.26)

project(MaterialXCodegen LANGUAGES CXX)

include(FetchContent)

# ============================================================================
# Vulkan SDK
# ============================================================================

if (NOT IS_DIRECTORY $ENV{VULKAN_SDK})
    message(FATAL_ERROR "Vulkan SDK not detected. Set the VULKAN_SDK environment variable.")
endif()

message(STATUS "Found Vulkan SDK in $ENV{VULKAN_SDK}")

# Slang headers come from the Vulkan SDK.
include_directories(SYSTEM $ENV{VULKAN_SDK}/include)

# Depending on SDK/platform, the library can be named slang or slang-compiler.
find_library(
    Slang_LIBRARY
    NAMES slang slang-compiler
    HINTS "$ENV{VULKAN_SDK}/lib"
    REQUIRED
)

message(STATUS "Found Slang library: ${Slang_LIBRARY}")

# ============================================================================
# MaterialX
# ============================================================================

set(MATERIALX_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_JS OFF CACHE BOOL "" FORCE)

set(MATERIALX_BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_GRAPH_EDITOR OFF CACHE BOOL "" FORCE)

set(MATERIALX_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_BENCHMARK_TESTS OFF CACHE BOOL "" FORCE)

set(MATERIALX_BUILD_RENDER OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_RENDER_PLATFORMS OFF CACHE BOOL "" FORCE)

set(MATERIALX_BUILD_GEN_GLSL ON CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_GEN_SLANG ON CACHE BOOL "" FORCE)

set(MATERIALX_BUILD_GEN_OSL OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_GEN_MDL OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_GEN_MSL OFF CACHE BOOL "" FORCE)

set(MATERIALX_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MATERIALX_BUILD_MONOLITHIC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    MaterialX
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/MaterialX.git
    GIT_TAG v1.39.5
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(MaterialX)

# ============================================================================
# Application
# ============================================================================

add_executable(
    MaterialXCodegen
    main.cc
)

target_compile_features(
    MaterialXCodegen
    PRIVATE
        cxx_std_20
)

target_link_libraries(
    MaterialXCodegen
    PRIVATE
        MaterialXCore
        MaterialXFormat
        MaterialXGenShader
        MaterialXGenHw
        MaterialXGenGlsl
        MaterialXGenSlang

        ${Slang_LIBRARY}
)

target_compile_definitions(
    MaterialXCodegen
    PRIVATE
        MATERIALX_SOURCE_ROOT="${materialx_SOURCE_DIR}"
)

if (WIN32)
    target_compile_definitions(
        MaterialXCodegen
        PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
    )
endif()