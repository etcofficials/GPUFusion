# GPU Fusion for Godot (GDExtension). Built on its own, because it needs godot-cpp:
#   cmake -S sdk/godot -B build/godot -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DGODOT_CPP_DIR=<godot-cpp 4.5 checkout> -DGPUFUSION_GENERATED_DIR=<GPU Fusion build>/generated
# (scripts/build-godot.ps1 does this.) The result, gpufusion_godot.dll, goes to demo/bin/.
cmake_minimum_required(VERSION 3.28)
project(gpufusion_godot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the extension DLL needs no Visual C++ runtime installed next to Godot.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GODOT_CPP_DIR "" CACHE PATH "godot-cpp checkout (branch 4.5)")
set(GPUFUSION_GENERATED_DIR "" CACHE PATH "GPU Fusion's build/<preset>/generated (compiled SDK shaders)")
if(NOT EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt" OR NOT EXISTS "${GPUFUSION_GENERATED_DIR}/shaders/blur_ps.h")
    message(FATAL_ERROR "Set GODOT_CPP_DIR and GPUFUSION_GENERATED_DIR (see the top of this file).")
endif()

# Only the Godot classes the extension uses are generated: much faster to build.
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/build_profile.json" CACHE PATH "" FORCE)
set(GODOTCPP_TARGET template_debug CACHE STRING "" FORCE) # loads in the editor and in debug exports
add_subdirectory("${GODOT_CPP_DIR}" godot-cpp EXCLUDE_FROM_ALL)

add_library(gpufusion_godot SHARED src/gpufusion_godot.cpp src/engine.cpp src/engine.h)
target_include_directories(gpufusion_godot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include" "${GPUFUSION_GENERATED_DIR}")
target_link_libraries(gpufusion_godot PRIVATE godot::cpp d3d11 dxgi)
target_compile_definitions(gpufusion_godot PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(gpufusion_godot PROPERTIES OUTPUT_NAME gpufusion_godot PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/demo/bin" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/demo/bin")
