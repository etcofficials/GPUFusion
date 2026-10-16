# Shared compiler settings, applied through the INTERFACE target gpufusion::options.

add_library(gpufusion_options INTERFACE)
add_library(gpufusion::options ALIAS gpufusion_options)

target_compile_features(gpufusion_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(gpufusion_options INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /MP
        /w44062     # enum value not handled in switch
        /w44265     # virtual functions without virtual destructor
    )
    target_compile_definitions(gpufusion_options INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        _WIN32_WINNT=0x0A00          # Windows 10
        QT_NO_KEYWORDS               # avoid clashes between Qt's emit/slots and Windows headers
    )
    if(GPUFUSION_WARNINGS_AS_ERRORS)
        target_compile_options(gpufusion_options INTERFACE /WX)
    endif()
    # Release keeps PDBs for crash analysis; they are not shipped.
    target_compile_options(gpufusion_options INTERFACE /Zi $<$<CONFIG:Debug>:/Od>)
    target_link_options(gpufusion_options INTERFACE /DEBUG $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
endif()
