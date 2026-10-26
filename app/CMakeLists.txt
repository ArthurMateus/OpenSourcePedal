# JUCE plugin target: Standalone + VST3 (ADR-0006 D3, D6). Artefacts land in
# build/<preset>/app/OpenSourcePedal_artefacts/<Config>/{Standalone,VST3}/ (AGENTS.md section 3).
juce_add_plugin(OpenSourcePedal
    PRODUCT_NAME "OpenSourcePedal"
    COMPANY_NAME "OpenSourcePedal"
    PLUGIN_MANUFACTURER_CODE Ospd
    PLUGIN_CODE Osp1
    FORMATS Standalone VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

set(osp_app_sources
    src/PluginProcessor.cpp)
target_sources(OpenSourcePedal PRIVATE ${osp_app_sources})

target_compile_definitions(OpenSourcePedal
    PUBLIC
        JUCE_ASIO=$<BOOL:${OSP_ENABLE_ASIO}>
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_DISPLAY_SPLASH_SCREEN=0
    PRIVATE
        WIN32_LEAN_AND_MEAN)

target_link_libraries(OpenSourcePedal
    PRIVATE
        osp_engine
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

# Our sources get /W4 (+ /WX in CI); JUCE's module sources compiled into this target keep their own level.
osp_apply_settings(OpenSourcePedal WARNINGS_ON ${osp_app_sources})
foreach(wrapper OpenSourcePedal_Standalone OpenSourcePedal_VST3)
    if(TARGET ${wrapper})
        osp_apply_settings(${wrapper} WARNINGS_ON ${osp_app_sources})
    endif()
endforeach()
