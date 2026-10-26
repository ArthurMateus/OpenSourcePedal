# Compiler and linker settings for OUR targets (ADR-0006 D6). Third-party targets keep their own warning levels.
include_guard(GLOBAL)

# osp_apply_settings(<target> [WARNINGS_ON <sources...>]): warnings, conformance, floating point and Release
# optimisation flags. With WARNINGS_ON, /W4 and /WX apply only to those sources: a JUCE plugin target also compiles
# JUCE's module sources, which keep their own warning level (ADR-0006 D1).
function(osp_apply_settings target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "WARNINGS_ON")
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /permissive- /utf-8 /Zc:__cplusplus /EHsc /fp:precise
            $<$<CONFIG:Release>:/O2 /Ob2 /GL>)
        # Flags JUCE module sources may not tolerate: only on our own code.
        set(own_flags /W4 /Zc:preprocessor)
        if(OSP_WARNINGS_AS_ERRORS)
            list(APPEND own_flags /WX)
        endif()
        if(arg_WARNINGS_ON)
            set_source_files_properties(${arg_WARNINGS_ON} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "${own_flags}")
        else()
            target_compile_options(${target} PRIVATE ${own_flags})
        endif()
        if(OSP_ENABLE_AVX2)
            target_compile_options(${target} PRIVATE /arch:AVX2)
        endif()
        target_compile_definitions(${target} PRIVATE NOMINMAX)

        get_target_property(type ${target} TYPE)
        if(type STREQUAL "STATIC_LIBRARY")
            set_property(TARGET ${target} APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:Release>:/LTCG>)
        elseif(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
            target_link_options(${target} PRIVATE /DEBUG $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
        endif()
    else()
        # Non-MSVC builds exist only for the optional Linux/Clang sanitizer job (ADR-0006 D5, linux-clang).
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${OSP_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()

    if(OSP_RT_CHECKER)
        target_compile_definitions(${target} PRIVATE OSP_RT_CHECKER=1)
    endif()
endfunction()
