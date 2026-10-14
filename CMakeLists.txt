cmake_minimum_required(VERSION 3.22)
project(adam_audio_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ADAM_BUILD_TESTS "Build the native test suite" ON)
option(ADAM_BUILD_RENDERER "Build the offline renderer" ON)
option(ADAM_BUILD_AU "Build the JUCE Audio Unit (fetches JUCE)" OFF)

# --- shared DSP + core synth (the one implementation every host links) ------
add_library(adam_core_synth STATIC
  instruments/core-synth/params.cpp
  instruments/core-synth/core_synth.cpp
  instruments/core-synth/c_api.cpp)
target_include_directories(adam_core_synth PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include
  ${CMAKE_CURRENT_SOURCE_DIR}/instruments/core-synth)
if(NOT MSVC)
  target_compile_options(adam_core_synth PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(EMSCRIPTEN)
  # --- browser host: standalone WASM, no JS glue, loaded by the AudioWorklet
  add_executable(adam_synth_wasm hosts/wasm/entry.cpp)
  target_link_libraries(adam_synth_wasm PRIVATE adam_core_synth)
  set_target_properties(adam_synth_wasm PROPERTIES OUTPUT_NAME "adam_synth" SUFFIX ".wasm")
  set(ADAM_WASM_EXPORTS
    _malloc _free
    _adam_synth_create _adam_synth_destroy _adam_synth_reset
    _adam_synth_note_on _adam_synth_note_off _adam_synth_sustain
    _adam_synth_all_notes_off _adam_synth_all_sound_off
    _adam_synth_set_param _adam_synth_get_param
    _adam_synth_apply_preset _adam_param_count _adam_param_to_physical _adam_param_to_normalized _adam_params_json
    _adam_synth_snap_params _adam_synth_render _adam_synth_out_left _adam_synth_out_right _adam_synth_read_meter
    _adam_seq_load _adam_seq_play _adam_seq_set_bpm _adam_seq_set_loop _adam_seq_rewind
    _adam_seq_beat _adam_seq_length _adam_seq_pattern_bpm _adam_seq_playing
    _adam_text_buffer _adam_analysis_input _adam_analyze)
  list(JOIN ADAM_WASM_EXPORTS "," ADAM_WASM_EXPORTS_CSV)
  target_link_options(adam_synth_wasm PRIVATE
    -sSTANDALONE_WASM=1 --no-entry
    -sEXPORTED_FUNCTIONS=[${ADAM_WASM_EXPORTS_CSV}]
    -sINITIAL_MEMORY=8MB -sSTACK_SIZE=256KB -sALLOW_MEMORY_GROWTH=0
    -sFILESYSTEM=0 -fno-exceptions)
  target_compile_options(adam_core_synth PRIVATE -fno-exceptions)
  return()
endif()

# --- native hosts -----------------------------------------------------------
if(ADAM_BUILD_RENDERER)
  add_executable(adam-render hosts/offline-renderer/main.cpp)
  target_link_libraries(adam-render PRIVATE adam_core_synth)
endif()

if(ADAM_BUILD_TESTS)
  enable_testing()
  add_executable(adam-tests tests/test_main.cpp)
  target_link_libraries(adam-tests PRIVATE adam_core_synth)
  target_compile_definitions(adam-tests PRIVATE
    ADAM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  add_test(NAME adam-tests COMMAND adam-tests)
endif()

if(ADAM_BUILD_AU)
  add_subdirectory(hosts/juce-au)
endif()
