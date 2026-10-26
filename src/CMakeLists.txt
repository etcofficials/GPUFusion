# ---- core: no widgets; unit-testable ----------------------------------------
add_library(gpufusion_core STATIC
    core/AppPaths.cpp core/AppPaths.h
    core/Logging.cpp core/Logging.h
    core/Settings.cpp core/Settings.h
)
target_include_directories(gpufusion_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(gpufusion_core PUBLIC Qt6::Core PRIVATE gpufusion::options)

# ---- hardware: adapter model (pure) + Windows probes ----------------------------
add_library(gpufusion_hardware STATIC
    hardware/AdapterModel.cpp hardware/AdapterModel.h
    hardware/AdapterDetector.cpp hardware/AdapterDetector.h
    hardware/HardwareService.cpp hardware/HardwareService.h
    platform/windows/DxgiProbe.cpp platform/windows/DxgiProbe.h
    platform/windows/PnpDevices.cpp platform/windows/PnpDevices.h
)
# d3d12.dll is loaded at run time (see DxgiProbe.cpp); only its headers are used here.
target_link_libraries(gpufusion_hardware
    PUBLIC gpufusion_core
    PRIVATE gpufusion::options dxgi d3d11 gdi32 setupapi cfgmgr32 ole32)

# ---- preferences: Windows per-app GPU preference (model, journal, verification) ---
add_library(gpufusion_preferences STATIC
    preferences/PreferenceModel.cpp preferences/PreferenceModel.h
    preferences/DualGpu.cpp preferences/DualGpu.h
    preferences/GpuSelection.cpp preferences/GpuSelection.h
    preferences/PreferenceService.cpp preferences/PreferenceService.h
    platform/windows/PeImports.cpp platform/windows/PeImports.h
    platform/windows/ProbeRunner.cpp platform/windows/ProbeRunner.h
    platform/windows/RegistryPreferenceStore.cpp platform/windows/RegistryPreferenceStore.h
)
target_link_libraries(gpufusion_preferences PUBLIC gpufusion_core PRIVATE gpufusion::options advapi32)

# ---- monitoring: metric model (pure), providers, worker thread -------------------
add_library(gpufusion_monitoring STATIC
    core/RingBuffer.h
    monitoring/MetricModel.cpp monitoring/MetricModel.h
    monitoring/History.cpp monitoring/History.h
    monitoring/MonitorWorker.cpp monitoring/MonitorWorker.h
    monitoring/MonitorService.cpp monitoring/MonitorService.h
    monitoring/ProcessModel.cpp monitoring/ProcessModel.h
    monitoring/UsageObserver.cpp monitoring/UsageObserver.h
    monitoring/UsageVerification.cpp monitoring/UsageVerification.h
    platform/windows/GpuPreferenceReader.cpp platform/windows/GpuPreferenceReader.h
    platform/windows/KmtProcessProvider.cpp platform/windows/KmtProcessProvider.h
    platform/windows/KmtStatsProvider.cpp platform/windows/KmtStatsProvider.h
    platform/windows/NvmlProvider.cpp platform/windows/NvmlProvider.h
    platform/windows/PdhGpuProcessProvider.cpp platform/windows/PdhGpuProcessProvider.h
    platform/windows/ProcessList.cpp platform/windows/ProcessList.h
)
# nvml.dll is loaded at run time (see NvmlProvider.cpp); nothing NVIDIA-specific is linked.
target_link_libraries(gpufusion_monitoring PUBLIC gpufusion_hardware gpufusion_preferences PRIVATE gpufusion::options gdi32 pdh advapi32)

# ---- workloads: reference hash kernel (CPU) + Direct3D 11 implementation --------------
# Shaders are compiled at build time by the SDK's fxc into headers (no run-time compiler needed).
get_filename_component(_sdk_bin "${CMAKE_RC_COMPILER}" DIRECTORY)
find_program(GPUFUSION_FXC NAMES fxc fxc.exe HINTS "${_sdk_bin}" REQUIRED)
set(GPUFUSION_SHADER_DIR "${CMAKE_BINARY_DIR}/generated/shaders")
set(GPUFUSION_SHADER_HEADERS "")
foreach(_shader fullscreen_vs:vs_4_0 hash_ps:ps_4_0)
    string(REPLACE ":" ";" _parts "${_shader}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _profile)
    add_custom_command(
        OUTPUT "${GPUFUSION_SHADER_DIR}/${_name}.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${GPUFUSION_SHADER_DIR}"
        COMMAND "${GPUFUSION_FXC}" /nologo /T ${_profile} /E main /O3 /WX /Fh "${GPUFUSION_SHADER_DIR}/${_name}.h" /Vn g_${_name}
                "${CMAKE_CURRENT_SOURCE_DIR}/workloads/shaders/${_name}.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/workloads/shaders/${_name}.hlsl"
        COMMENT "Compiling shader ${_name} (${_profile})"
        VERBATIM)
    list(APPEND GPUFUSION_SHADER_HEADERS "${GPUFUSION_SHADER_DIR}/${_name}.h")
endforeach()

add_library(gpufusion_workloads STATIC
    workloads/HashKernel.cpp workloads/HashKernel.h
    workloads/D3D11HashDevice.cpp workloads/D3D11HashDevice.h
    workloads/SplitHash.cpp workloads/SplitHash.h
    workloads/SplitPlan.cpp workloads/SplitPlan.h
    workloads/VideoDecode.cpp workloads/VideoDecode.h
    workloads/shaders/fullscreen_vs.hlsl workloads/shaders/hash_ps.hlsl
    ${GPUFUSION_SHADER_HEADERS}
)
set_source_files_properties(workloads/shaders/fullscreen_vs.hlsl workloads/shaders/hash_ps.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_link_libraries(gpufusion_workloads PUBLIC gpufusion_hardware PRIVATE gpufusion::options d3d11 dxgi mfplat mfreadwrite mfuuid ole32)

# ---- benchmarks: model (pure), runner, service ---------------------------------------
add_library(gpufusion_benchmarks STATIC
    benchmarks/BenchmarkModel.cpp benchmarks/BenchmarkModel.h
    benchmarks/BenchmarkRunner.cpp benchmarks/BenchmarkRunner.h
    benchmarks/BenchmarkService.cpp benchmarks/BenchmarkService.h
    benchmarks/LabModel.cpp benchmarks/LabModel.h
    benchmarks/LabRunner.cpp benchmarks/LabRunner.h
)
target_link_libraries(gpufusion_benchmarks PUBLIC gpufusion_monitoring gpufusion_preferences gpufusion_workloads PRIVATE gpufusion::options psapi)

# ---- profiles: generic per-application GPU profiles, discovery, apply/launch/verify -------
add_library(gpufusion_profiles STATIC
    profiles/AppDiscovery.cpp profiles/AppDiscovery.h
    profiles/AppProfile.cpp profiles/AppProfile.h
    profiles/KnownOptions.cpp profiles/KnownOptions.h
    profiles/LiveCheck.cpp profiles/LiveCheck.h
    profiles/ProfileActions.cpp profiles/ProfileActions.h
    profiles/ProfileStore.cpp profiles/ProfileStore.h
    profiles/RunningInstances.cpp profiles/RunningInstances.h
    profiles/Shortcuts.cpp profiles/Shortcuts.h
)
target_link_libraries(gpufusion_profiles PUBLIC gpufusion_monitoring gpufusion_preferences PRIVATE gpufusion::options ole32 shell32 uuid user32)

# ---- dispatch: independent jobs spread over several GPUs (class C), measured comparison ---
add_library(gpufusion_dispatch STATIC
    dispatch/JobDispatcher.cpp dispatch/JobDispatcher.h
)
target_link_libraries(gpufusion_dispatch PUBLIC gpufusion_benchmarks PRIVATE gpufusion::options)

# ---- diagnostics: report model (pure) + read-only platform checks --------------------
add_library(gpufusion_diagnostics STATIC
    diagnostics/DiagnosticReport.cpp diagnostics/DiagnosticReport.h
    platform/windows/OpenClInfo.cpp platform/windows/OpenClInfo.h
)
target_link_libraries(gpufusion_diagnostics PUBLIC gpufusion_monitoring gpufusion_preferences PRIVATE gpufusion::options advapi32)

# ---- ui: Qt Widgets ------------------------------------------------------------
add_library(gpufusion_ui STATIC
    ui/AppContext.cpp ui/AppContext.h
    ui/DiagnosticsCollector.cpp ui/DiagnosticsCollector.h
    ui/MainWindow.cpp ui/MainWindow.h
    ui/Theme.cpp ui/Theme.h
    ui/UsageCheck.cpp ui/UsageCheck.h
    ui/models/ProcessTableModel.cpp ui/models/ProcessTableModel.h
    ui/pages/ApplicationsPage.cpp ui/pages/ApplicationsPage.h
    ui/pages/BenchmarksPage.cpp ui/pages/BenchmarksPage.h
    ui/pages/DashboardPage.cpp ui/pages/DashboardPage.h
    ui/pages/DiagnosticsPage.cpp ui/pages/DiagnosticsPage.h
    ui/pages/GpuManagerPage.cpp ui/pages/GpuManagerPage.h
    ui/pages/GpusPage.cpp ui/pages/GpusPage.h
    ui/pages/PlaceholderPage.cpp ui/pages/PlaceholderPage.h
    ui/pages/PreferencesPage.cpp ui/pages/PreferencesPage.h
    ui/pages/SettingsPage.cpp ui/pages/SettingsPage.h
    ui/pages/WorkloadLabPage.cpp ui/pages/WorkloadLabPage.h
    ui/widgets/AdapterCard.cpp ui/widgets/AdapterCard.h
    ui/widgets/BarChart.cpp ui/widgets/BarChart.h
    ui/widgets/Card.cpp ui/widgets/Card.h
    ui/widgets/HistoryGraph.cpp ui/widgets/HistoryGraph.h
    ui/widgets/JobBatchPanel.cpp ui/widgets/JobBatchPanel.h
    ui/widgets/LiveText.cpp ui/widgets/LiveText.h
    ui/widgets/MonitorCard.cpp ui/widgets/MonitorCard.h
)
target_link_libraries(gpufusion_ui PUBLIC gpufusion_core gpufusion_hardware gpufusion_monitoring gpufusion_diagnostics gpufusion_benchmarks gpufusion_profiles gpufusion_dispatch Qt6::Widgets PRIVATE gpufusion::options)

# ---- application -----------------------------------------------------------------
add_executable(GPUFusion WIN32
    main.cpp
    "${CMAKE_BINARY_DIR}/generated/app.rc"
)
target_link_libraries(GPUFusion PRIVATE gpufusion_ui gpufusion::options)
set_target_properties(GPUFusion PROPERTIES OUTPUT_NAME GPUFusion)

# Development builds keep their data in the repository's data folder (see top-level CMakeLists).
file(WRITE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GPUFusion.datadir"
     "# Development build: data location. Not shipped in release packages.\n${GPUFUSION_DEV_DATA_ROOT}\n")

gpufusion_deploy_runtime(GPUFusion)
