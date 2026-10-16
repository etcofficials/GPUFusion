cmake_minimum_required(VERSION 3.28)

project(GPUFusion
    VERSION 0.1.0
    DESCRIPTION "Windows GPU monitor, GPU preference manager and multi-GPU workload lab"
    LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "GPU Fusion targets Windows only.")
endif()

option(GPUFUSION_BUILD_TESTS "Build the automated tests" ON)
option(GPUFUSION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
# Development builds write logs/settings/benchmarks to the repository's data folder (on G:)
# through a GPUFusion.datadir pointer file next to the executable. Packaged releases do
# not ship that file and use a portable data folder beside the executable instead.
set(GPUFUSION_DEV_DATA_ROOT "${PROJECT_SOURCE_DIR}/data" CACHE PATH "Data root used by development builds")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)

include(cmake/CompilerOptions.cmake)
include(cmake/Deploy.cmake)

configure_file(src/core/Version.h.in "${CMAKE_BINARY_DIR}/generated/core/Version.h" @ONLY)
configure_file(resources/app.rc.in "${CMAKE_BINARY_DIR}/generated/app.rc" @ONLY)

add_subdirectory(src)
add_subdirectory(tools)

if(GPUFUSION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
