# JUCE Audio Unit pipeline: adam_add_audio_unit() turns an engine + its
# parameter descriptor into an AUv2 instrument (plus a Standalone app for quick
# checks), copied into ~/Library/Audio/Plug-Ins/Components after the build.
include(FetchContent)
FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 9.0.4          # pinned: bump deliberately, then re-run auval
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

# adam_add_audio_unit(<target>
#   PRODUCT_NAME "Adam Core Synth"   # what Logic lists
#   PLUGIN_CODE  AdCs                # 4 chars, unique per instrument, NEVER change
#   SOURCE       <descriptor .cpp>   # defines createPluginFilter()
#   ENGINE_LIB   <engine target>)
function(adam_add_audio_unit target)
  cmake_parse_arguments(A "" "PRODUCT_NAME;PLUGIN_CODE;SOURCE;ENGINE_LIB" "" ${ARGN})
  juce_add_plugin(${target}
    COMPANY_NAME "Adam"
    COMPANY_WEBSITE "https://github.com/samellgass2/adam-audio-engine"
    PRODUCT_NAME "${A_PRODUCT_NAME}"
    BUNDLE_ID "com.adam.audio.${target}"
    PLUGIN_MANUFACTURER_CODE Adam   # never change: Logic keys sessions on it
    PLUGIN_CODE ${A_PLUGIN_CODE}
    VERSION ${PROJECT_VERSION}
    FORMATS AU Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD TRUE)
  target_sources(${target} PRIVATE ${A_SOURCE})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  target_compile_definitions(${target} PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(${target}
    PRIVATE ${A_ENGINE_LIB} juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
endfunction()

adam_add_audio_unit(AdamCoreSynth
  PRODUCT_NAME "Adam Core Synth"
  PLUGIN_CODE AdCs
  SOURCE core_synth_plugin.cpp
  ENGINE_LIB adam_core_synth)

adam_add_audio_unit(AdamDrums
  PRODUCT_NAME "Adam Drums"
  PLUGIN_CODE AdDr
  SOURCE drum_kit_plugin.cpp
  ENGINE_LIB adam_core_synth)

# adam-au-host: renders through an installed AU via the real plugin API (the
# AU<->engine parity check; see tools/compare_wav.py).
juce_add_console_app(adam-au-host PRODUCT_NAME "adam-au-host")
target_sources(adam-au-host PRIVATE au_host.cpp)
target_compile_definitions(adam-au-host PRIVATE
  JUCE_PLUGINHOST_AU=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(adam-au-host
  PRIVATE adam_core_synth juce::juce_audio_processors juce::juce_audio_formats "-framework CoreAudioKit"
  PUBLIC juce::juce_recommended_config_flags)
