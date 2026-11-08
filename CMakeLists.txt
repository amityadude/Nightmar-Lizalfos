cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(nightmare_lizalfos VERSION 2.0.0 LANGUAGES CXX)

# The Dusklight version to build against: a release tag (e.g. "v2.0.0") or a commit SHA.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.2")
endif ()
# Fetches the Dusklight sources into ./dusklight and provides DUSKLIGHT_DIR.
# Use -DDUSKLIGHT_DIR= to point at an existing Dusklight checkout.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(nightmare_lizalfos
        FEATURES game              # game headers (e_dn_class, daPy_py_c, ...); no fmt, logging uses the C API
        SOURCES src/mod.cpp src/lizalfos_nm.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        RES_DIR res            # mod resources, including icon.png and banner.png
        OVERLAY_DIR overlay    # game file overlays; remove if unused
        TEXTURES_DIR textures  # texture replacements; remove if unused
)

# MSVC reports __cplusplus as 199711L unless told otherwise, and Dusklight's dolphin/types.h then
# defines `nullptr` as 0, which breaks any standard-library header included afterwards
# (error C2440 in <utility> / <xmemory>). This flag makes MSVC report the real value.
if (MSVC)
    target_compile_options(nightmare_lizalfos PRIVATE /Zc:__cplusplus)
endif ()

# Optional: the engine-independent decision logic has its own tests (no Dusklight needed to run
# them). `cmake -DNIGHTMARE_LIZALFOS_TESTS=ON ..` adds the `test_nm_logic` target.
option(NIGHTMARE_LIZALFOS_TESTS "Build the nm_logic unit tests" OFF)
if (NIGHTMARE_LIZALFOS_TESTS)
    add_executable(test_nm_logic tests/test_nm_logic.cpp)
    target_include_directories(test_nm_logic PRIVATE src)
    target_compile_features(test_nm_logic PRIVATE cxx_std_17)
    enable_testing()
    add_test(NAME nm_logic COMMAND test_nm_logic)
endif ()
