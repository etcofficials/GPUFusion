# Build-time script (cmake -P): copies the MSVC C/C++ runtime DLLs that match the
# current configuration next to the executable. Invoked by gpufusion_deploy_runtime().
#
# Inputs: CONFIG, DEST, CRT_RELEASE_DIR, CRT_DEBUG_DIR, UCRTBASED

if(CONFIG STREQUAL "Debug")
    set(_names msvcp140d.dll msvcp140_1d.dll msvcp140_2d.dll msvcp140d_atomic_wait.dll msvcp140d_codecvt_ids.dll
               vcruntime140d.dll vcruntime140_1d.dll)
    set(_dir "${CRT_DEBUG_DIR}")
    set(_extra "${UCRTBASED}")
else()
    # ucrtbase.dll is part of Windows 10; only the VC++ runtime is deployed app-local.
    set(_names msvcp140.dll msvcp140_1.dll msvcp140_2.dll msvcp140_atomic_wait.dll msvcp140_codecvt_ids.dll
               vcruntime140.dll vcruntime140_1.dll)
    set(_dir "${CRT_RELEASE_DIR}")
    set(_extra "")
endif()

set(_files "")
foreach(_n IN LISTS _names)
    if(_dir AND EXISTS "${_dir}/${_n}")
        list(APPEND _files "${_dir}/${_n}")
    endif()
endforeach()
if(_extra AND EXISTS "${_extra}")
    list(APPEND _files "${_extra}")
endif()

if(NOT _files)
    message(WARNING "DeployCrt: no runtime DLLs found for ${CONFIG} (dir='${_dir}')")
    return()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_files} "${DEST}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "DeployCrt: copying runtime DLLs to ${DEST} failed (${_rc})")
endif()
