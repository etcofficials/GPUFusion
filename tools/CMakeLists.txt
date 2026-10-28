# GPUFusionProbe: harmless helper launched by GPU Fusion to verify GPU preferences (no Qt).
add_executable(GPUFusionProbe probe/main.cpp)
target_link_libraries(GPUFusionProbe PRIVATE gpufusion::options dxgi d3d11)
# --render uses the workload shaders, compiled into headers when gpufusion_workloads is built.
target_include_directories(GPUFusionProbe PRIVATE "${CMAKE_BINARY_DIR}/generated" "${PROJECT_SOURCE_DIR}/sdk/include")
add_dependencies(GPUFusionProbe gpufusion_workloads)
# Static CRT: the probe must run even if it is copied on its own.
set_property(TARGET GPUFusionProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# GPUFusionLauncher: packaged as release\GPUFusion.exe next to the portable "GPUFusion" folder, so the
# release folder has a double-clickable GPUFusion.exe with icon and version info (no Qt, static CRT).
add_executable(GPUFusionLauncher WIN32 launcher/main.cpp "${CMAKE_BINARY_DIR}/generated/app.rc")
target_link_libraries(GPUFusionLauncher PRIVATE gpufusion::options)
set_property(TARGET GPUFusionLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# GPUFusionSplitDemo: a program that integrates the GPU Fusion SDK (public headers only, no Qt) and
# divides a measurable workload between the GPUs it is offered with the SDK's adaptive scheduler.
add_executable(GPUFusionSplitDemo splitdemo/main.cpp)
target_link_libraries(GPUFusionSplitDemo PRIVATE gpufusion::options dxgi d3d11 psapi)
target_include_directories(GPUFusionSplitDemo PRIVATE "${CMAKE_BINARY_DIR}/generated" "${PROJECT_SOURCE_DIR}/sdk/include")
add_dependencies(GPUFusionSplitDemo gpufusion_workloads)
set_property(TARGET GPUFusionSplitDemo PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# GPUFusionFrameCapture: measures frame times of running programs from outside with Windows' graphics
# event tracing (ETW; needs administrator rights, asked for each time). No Qt, static CRT.
add_executable(GPUFusionFrameCapture framecapture/main.cpp)
target_link_libraries(GPUFusionFrameCapture PRIVATE gpufusion::options advapi32 psapi)
set_property(TARGET GPUFusionFrameCapture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# GPUFusionFrameTest: presents frames with Direct3D 11/12, Direct3D 9 or OpenGL at a known pace and
# reports its own present times: the ground truth the frame-time capture is checked against.
add_executable(GPUFusionFrameTest frametest/main.cpp)
target_link_libraries(GPUFusionFrameTest PRIVATE gpufusion::options d3d11 d3d12 d3d9 dxgi opengl32 winmm)
set_property(TARGET GPUFusionFrameTest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
