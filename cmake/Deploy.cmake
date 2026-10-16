# Runtime deployment for the build tree.
#
# gpufusion_deploy_runtime(<target>) adds POST_BUILD steps that make the directory
# containing <target> runnable from Explorer or a plain shell with no environment setup:
#   1. Qt DLLs + required plugins via windeployqt, matched to the configuration.
#   2. MSVC C/C++ runtime next to the executable (app-local), located from the
#      toolchain that is compiling (portable layout under G:\DevTools or an
#      official Visual Studio layout).

function(_gpufusion_find_crt_dirs out_release out_debug out_ucrtbased)
    # cl.exe lives in <toolset>/bin/Host<h>/<t>/; the redist lives beside the toolset
    # version directory (portable: <root>/Redist/<ver>, official: VC/Redist/MSVC/<ver>).
    get_filename_component(_tools_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_toolset "${_tools_bin}/../../.." ABSOLUTE)
    get_filename_component(_toolset_parent "${_toolset}/.." ABSOLUTE)
    file(GLOB _redist_dirs LIST_DIRECTORIES true
        "${_toolset_parent}/Redist/*"
        "${_toolset_parent}/../../Redist/MSVC/*")

    set(_found_rel "")
    set(_found_dbg "")
    foreach(_dir IN LISTS _redist_dirs)
        file(GLOB _crt "${_dir}/x64/Microsoft.VC14*.CRT")
        file(GLOB _dbg "${_dir}/debug_nonredist/x64/Microsoft.VC14*.DebugCRT")
        if(_crt AND NOT _found_rel)
            list(GET _crt 0 _found_rel)
        endif()
        if(_dbg AND NOT _found_dbg)
            list(GET _dbg 0 _found_dbg)
        endif()
    endforeach()

    # ucrtbased.dll ships with the Windows SDK next to rc.exe: bin/<ver>/x64/ucrt/
    get_filename_component(_rc_dir "${CMAKE_RC_COMPILER}" DIRECTORY)
    set(_ucrtbased "${_rc_dir}/ucrt/ucrtbased.dll")
    if(NOT EXISTS "${_ucrtbased}")
        set(_ucrtbased "")
    endif()

    set(${out_release} "${_found_rel}" PARENT_SCOPE)
    set(${out_debug} "${_found_dbg}" PARENT_SCOPE)
    set(${out_ucrtbased} "${_ucrtbased}" PARENT_SCOPE)
endfunction()

function(gpufusion_deploy_runtime target)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    find_program(GPUFUSION_WINDEPLOYQT NAMES windeployqt windeployqt.exe HINTS "${_qt_bin}" NO_DEFAULT_PATH)
    if(NOT GPUFUSION_WINDEPLOYQT)
        message(FATAL_ERROR "windeployqt not found in ${_qt_bin}")
    endif()
    # Only what a Widgets app needs: platform plugin, styles, ico/png image formats.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${GPUFUSION_WINDEPLOYQT}"
                $<IF:$<CONFIG:Debug>,--debug,--release>
                --no-translations
                --no-system-d3d-compiler
                --no-system-dxc-compiler
                --no-opengl-sw
                --no-compiler-runtime
                --no-network
                --no-svg
                --skip-plugin-types generic,networkinformation,tls,iconengines
                --exclude-plugins qgif,qjpeg
                --verbose 0
                "$<TARGET_FILE:${target}>"
        COMMENT "Deploying Qt runtime for ${target}"
        VERBATIM)

    _gpufusion_find_crt_dirs(_crt_release _crt_debug _ucrtbased)
    if(NOT _crt_release OR NOT _crt_debug OR NOT _ucrtbased)
        message(WARNING "MSVC runtime redistributables not fully located "
                        "(release='${_crt_release}' debug='${_crt_debug}' ucrtbased='${_ucrtbased}'); "
                        "the build tree may need the CRT on PATH to run.")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                -DCONFIG=$<CONFIG>
                -DDEST=$<TARGET_FILE_DIR:${target}>
                "-DCRT_RELEASE_DIR=${_crt_release}"
                "-DCRT_DEBUG_DIR=${_crt_debug}"
                "-DUCRTBASED=${_ucrtbased}"
                -P "${PROJECT_SOURCE_DIR}/cmake/DeployCrt.cmake"
        COMMENT "Deploying MSVC runtime for ${target}"
        VERBATIM)
endfunction()
