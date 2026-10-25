find_package(Qt6 REQUIRED COMPONENTS Test)

# Tests run from the build tree: Qt DLLs come from the Qt bin folder on PATH, and
# TEMP/TMP point into the build directory so QTemporaryDir never writes to C:.
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(GPUFUSION_QT_BIN "${_qmake}" DIRECTORY)
# Tests share build/<preset>/bin with GPUFusion.exe, whose deployed plugins folder only has the
# "windows" platform plugin. GUI tests run headless ("offscreen"), so point them at Qt's own plugins.
get_filename_component(GPUFUSION_QT_PLUGINS "${GPUFUSION_QT_BIN}/../plugins" ABSOLUTE)
set(GPUFUSION_TEST_TEMP "${CMAKE_BINARY_DIR}/test-temp")
file(MAKE_DIRECTORY "${GPUFUSION_TEST_TEMP}")

# gpufusion_add_test(<name> <source> [LABELS <labels>...])
function(gpufusion_add_test name source)
    cmake_parse_arguments(ARG "" "" "LABELS" ${ARGN})
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE gpufusion_monitoring Qt6::Test gpufusion::options dxgi d3d11 pdh)
    # Results go to the console and to test-temp/<name>.log, which survives even if the console
    # output is lost (e.g. a process that ends abnormally loses its buffered output).
    add_test(NAME ${name} COMMAND ${name} -o "${GPUFUSION_TEST_TEMP}/${name}.log,txt" -o "-,txt")
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "TEMP=${GPUFUSION_TEST_TEMP};TMP=${GPUFUSION_TEST_TEMP};QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${GPUFUSION_QT_PLUGINS}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GPUFUSION_QT_BIN}"
        LABELS "${ARG_LABELS}"
        TIMEOUT 60)
endfunction()

gpufusion_add_test(tst_settings tst_settings.cpp LABELS unit)
gpufusion_add_test(tst_logging tst_logging.cpp LABELS unit)
gpufusion_add_test(tst_apppaths tst_apppaths.cpp LABELS unit)
gpufusion_add_test(tst_adaptermodel tst_adaptermodel.cpp LABELS unit)
gpufusion_add_test(tst_metricmodel tst_metricmodel.cpp LABELS unit)
gpufusion_add_test(tst_processmodel tst_processmodel.cpp LABELS unit)
gpufusion_add_test(tst_processtable tst_processtable.cpp LABELS unit)
target_link_libraries(tst_processtable PRIVATE gpufusion_ui)
gpufusion_add_test(tst_process_live tst_process_live.cpp LABELS hardware)
gpufusion_add_test(tst_preferences tst_preferences.cpp LABELS unit)
gpufusion_add_test(tst_diagnostics tst_diagnostics.cpp LABELS unit)
target_link_libraries(tst_diagnostics PRIVATE gpufusion_diagnostics)
gpufusion_add_test(tst_export_live tst_export_live.cpp LABELS hardware)
gpufusion_add_test(tst_benchmarkmodel tst_benchmarkmodel.cpp LABELS unit)
target_link_libraries(tst_benchmarkmodel PRIVATE gpufusion_benchmarks)
gpufusion_add_test(tst_workload_live tst_workload_live.cpp LABELS hardware)
target_link_libraries(tst_workload_live PRIVATE gpufusion_benchmarks)
set_tests_properties(tst_workload_live PROPERTIES TIMEOUT 300)
add_dependencies(tst_export_live GPUFusion)
# Writes (and restores) the per-user GPU preference for this build's GPUFusionProbe.exe only.
gpufusion_add_test(tst_preferences_live tst_preferences_live.cpp LABELS hardware registry)
add_dependencies(tst_preferences_live GPUFusionProbe)
# Runs after tst_preferences_live even if it crashed, restoring the probe's original value.
add_test(NAME tst_preferences_live_restore COMMAND tst_preferences_live restoreFromSnapshot)
set_tests_properties(tst_preferences_live_restore PROPERTIES
    ENVIRONMENT "TEMP=${GPUFUSION_TEST_TEMP};TMP=${GPUFUSION_TEST_TEMP}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GPUFUSION_QT_BIN}"
    FIXTURES_CLEANUP probe_preference LABELS "hardware;registry")
set_tests_properties(tst_preferences_live PROPERTIES FIXTURES_REQUIRED probe_preference)
gpufusion_add_test(tst_preferences_ui tst_preferences_ui.cpp LABELS hardware registry)
target_link_libraries(tst_preferences_ui PRIVATE gpufusion_ui)
add_dependencies(tst_preferences_ui GPUFusionProbe)
set_tests_properties(tst_preferences_ui PROPERTIES FIXTURES_REQUIRED probe_preference RUN_SERIAL TRUE)
set_tests_properties(tst_preferences_live PROPERTIES RUN_SERIAL TRUE)
# Use the real GPUs and drivers; skip cleanly where hardware or APIs are missing.
gpufusion_add_test(tst_hardware_live tst_hardware_live.cpp LABELS hardware)
gpufusion_add_test(tst_monitoring_live tst_monitoring_live.cpp LABELS hardware)
gpufusion_add_test(tst_benchmarks_ui tst_benchmarks_ui.cpp LABELS hardware)
target_link_libraries(tst_benchmarks_ui PRIVATE gpufusion_ui)
gpufusion_add_test(tst_lab_live tst_lab_live.cpp LABELS hardware)
target_link_libraries(tst_lab_live PRIVATE gpufusion_benchmarks)
set_tests_properties(tst_lab_live PROPERTIES TIMEOUT 300)
set_tests_properties(tst_benchmarks_ui PROPERTIES TIMEOUT 300)
gpufusion_add_test(tst_labmodel tst_labmodel.cpp LABELS unit)
target_link_libraries(tst_labmodel PRIVATE gpufusion_benchmarks)
gpufusion_add_test(tst_gpuselection tst_gpuselection.cpp LABELS unit)
gpufusion_add_test(tst_dispatch tst_dispatch.cpp LABELS unit)
target_link_libraries(tst_dispatch PRIVATE gpufusion_dispatch)
gpufusion_add_test(tst_dispatch_live tst_dispatch_live.cpp LABELS hardware)
target_link_libraries(tst_dispatch_live PRIVATE gpufusion_dispatch)
set_tests_properties(tst_dispatch_live PROPERTIES TIMEOUT 180)
gpufusion_add_test(tst_profiles tst_profiles.cpp LABELS unit)
target_link_libraries(tst_profiles PRIVATE gpufusion_profiles)
# Real per-application verification with the probe as the application; changes (and restores) only
# the probe's own preference, so it shares the cleanup fixture with tst_preferences_live.
gpufusion_add_test(tst_appverify_live tst_appverify_live.cpp LABELS hardware registry)
add_dependencies(tst_appverify_live GPUFusionProbe GPUFusion)
set_tests_properties(tst_appverify_live PROPERTIES FIXTURES_REQUIRED probe_preference RUN_SERIAL TRUE TIMEOUT 180)
# Generic profiles in every mode with the probe as an SDK-integrated program (probe preference only).
gpufusion_add_test(tst_profiles_live tst_profiles_live.cpp LABELS hardware registry)
target_link_libraries(tst_profiles_live PRIVATE gpufusion_profiles)
add_dependencies(tst_profiles_live GPUFusionProbe)
set_tests_properties(tst_profiles_live PROPERTIES FIXTURES_REQUIRED probe_preference RUN_SERIAL TRUE TIMEOUT 240)
gpufusion_add_test(tst_gpumanager_ui tst_gpumanager_ui.cpp LABELS hardware registry)
target_link_libraries(tst_gpumanager_ui PRIVATE gpufusion_ui)
add_dependencies(tst_gpumanager_ui GPUFusionProbe)
set_tests_properties(tst_gpumanager_ui PROPERTIES FIXTURES_REQUIRED probe_preference RUN_SERIAL TRUE TIMEOUT 180)
