# Compiler and linker settings for OUR targets (ADR-0006 D6). Third-party targets keep their own warning levels.
include_guard(GLOBAL)

# osp_apply_settings(<target>): warnings, conformance, floating point and Release optimisation flags.
function(osp_apply_settings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /fp:precise
            $<$<CONFIG:Release>:/O2 /Ob2 /GL>)
        if(OSP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
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
