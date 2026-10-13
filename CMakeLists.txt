cmake_minimum_required(VERSION 3.22)
project(atom_audio_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ATOM_BUILD_TESTS "Build the native test suite" ON)
option(ATOM_BUILD_RENDERER "Build the offline renderer" ON)
option(ATOM_BUILD_AU "Build the JUCE Audio Unit (fetches JUCE)" OFF)

# --- shared DSP + core synth (the one implementation every host links) ------
add_library(atom_core_synth STATIC
  instruments/core-synth/params.cpp
  instruments/core-synth/core_synth.cpp
  instruments/core-synth/c_api.cpp)
target_include_directories(atom_core_synth PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include
  ${CMAKE_CURRENT_SOURCE_DIR}/instruments/core-synth)
if(NOT MSVC)
  target_compile_options(atom_core_synth PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(EMSCRIPTEN)
  # --- browser host: standalone WASM, no JS glue, loaded by the AudioWorklet
  add_executable(atom_synth_wasm hosts/wasm/entry.cpp)
  target_link_libraries(atom_synth_wasm PRIVATE atom_core_synth)
  set_target_properties(atom_synth_wasm PROPERTIES OUTPUT_NAME "atom_synth" SUFFIX ".wasm")
  set(ATOM_WASM_EXPORTS
    _malloc _free
    _atom_synth_create _atom_synth_destroy _atom_synth_reset
    _atom_synth_note_on _atom_synth_note_off _atom_synth_sustain
    _atom_synth_all_notes_off _atom_synth_all_sound_off
    _atom_synth_set_param _atom_synth_get_param
    _atom_synth_apply_preset _atom_param_count _atom_param_to_physical _atom_param_to_normalized _atom_params_json
    _atom_synth_render _atom_synth_out_left _atom_synth_out_right _atom_synth_read_meter
    _atom_seq_load _atom_seq_play _atom_seq_set_bpm _atom_seq_set_loop _atom_seq_rewind
    _atom_seq_beat _atom_seq_length _atom_seq_pattern_bpm _atom_seq_playing
    _atom_text_buffer)
  list(JOIN ATOM_WASM_EXPORTS "," ATOM_WASM_EXPORTS_CSV)
  target_link_options(atom_synth_wasm PRIVATE
    -sSTANDALONE_WASM=1 --no-entry
    -sEXPORTED_FUNCTIONS=[${ATOM_WASM_EXPORTS_CSV}]
    -sINITIAL_MEMORY=8MB -sSTACK_SIZE=256KB -sALLOW_MEMORY_GROWTH=0
    -sFILESYSTEM=0 -fno-exceptions)
  target_compile_options(atom_core_synth PRIVATE -fno-exceptions)
  return()
endif()

# --- native hosts -----------------------------------------------------------
if(ATOM_BUILD_RENDERER)
  add_executable(atom-render hosts/offline-renderer/main.cpp)
  target_link_libraries(atom-render PRIVATE atom_core_synth)
endif()

if(ATOM_BUILD_TESTS)
  enable_testing()
  add_executable(atom-tests tests/test_main.cpp)
  target_link_libraries(atom-tests PRIVATE atom_core_synth)
  target_compile_definitions(atom-tests PRIVATE
    ATOM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  add_test(NAME atom-tests COMMAND atom-tests)
endif()

if(ATOM_BUILD_AU)
  add_subdirectory(hosts/juce-au)
endif()
