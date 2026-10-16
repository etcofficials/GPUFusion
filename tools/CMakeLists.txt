# GPUFusionProbe: harmless helper launched by GPU Fusion to verify GPU preferences (no Qt).
add_executable(GPUFusionProbe probe/main.cpp)
target_link_libraries(GPUFusionProbe PRIVATE gpufusion::options dxgi d3d11)
# Static CRT: the probe must run even if it is copied on its own.
set_property(TARGET GPUFusionProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
