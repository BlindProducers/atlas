cmake_minimum_required(VERSION 3.22)

set(ATLAS_PROJECT_LANGUAGES C CXX)
set(ATLAS_PLUGIN_FORMATS VST3)

if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS architectures" FORCE)
  list(APPEND ATLAS_PROJECT_LANGUAGES OBJC OBJCXX)
  list(APPEND ATLAS_PLUGIN_FORMATS AU)
endif()

project(Atlas VERSION 2.1.0 LANGUAGES ${ATLAS_PROJECT_LANGUAGES})

if(MSVC)
  # Keep Atlas and all static dependencies on the same self-contained CRT.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE, nlohmann json, concurrentqueue and KISS FFT are git submodules pinned
# to the exact versions used for validation.
set(ATLAS_JUCE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE8" CACHE PATH "JUCE source directory")
foreach(dependency "${ATLAS_JUCE_SOURCE}/CMakeLists.txt"
                   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/include/nlohmann/json.hpp"
                   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/concurrentqueue/concurrentqueue.h"
                   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/kissfft/kissfft.hh")
  if(NOT EXISTS "${dependency}")
    message(FATAL_ERROR "Missing ${dependency}. Fetch the submodules with: git submodule update --init")
  endif()
endforeach()
add_subdirectory("${ATLAS_JUCE_SOURCE}" JUCE)
# Every Atlas target includes nlohmann json as "nlohmann/json.hpp".
include_directories(third_party/nlohmann_json/include)

juce_add_plugin(Atlas
  COMPANY_NAME "Blind Producers"
  BUNDLE_ID "plugins.alessio.atlas"
  PLUGIN_MANUFACTURER_CODE Alss
  PLUGIN_CODE Atls
  FORMATS ${ATLAS_PLUGIN_FORMATS}
  PRODUCT_NAME "Atlas"
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(Atlas)

target_sources(Atlas PRIVATE
  # Spectral effects keep IEEE semantics outside the fast-math synthesis unity build.
  src/synthesis/effects/suppressor/suppressor_engine.cpp
  src/synthesis/effects/spectral_compressor/spectral_compressor_engine.cpp
  # Compile BP Scream and its adapter outside the fast-math synthesis unity build.
  "third_party/scream/Source/DSP/ScreamDSP.cpp"
  src/plugin/trance_gate_editor.cpp
  src/common/tape_noise_library.cpp
  src/synthesis/modules/tape_module.cpp
  src/synthesis/modules/defuse_module.cpp
  src/synthesis/modules/scream_module.cpp
  src/synthesis/filters/scream_filter.cpp
  src/synthesis/modules/spectral_compressor_module.cpp
  src/synthesis/modules/spectral_lock_module.cpp
  src/synthesis/effects/spectral_lock/spectral_lock_engine.cpp
  src/synthesis/effects/spectral_lock/spectral_retune_engine.cpp
  # Resonator keeps strict double-precision pole arithmetic outside fast-math.
  src/synthesis/modules/resonator_module.cpp
  src/synthesis/effects/resonator/resonator_engine.cpp
  # Analogue models evaluate analog filters at BBD clock instants in double
  # precision and keep IEEE semantics outside fast-math.
  src/synthesis/effects/analogue/analog_filter.cpp
  src/synthesis/effects/analogue/bbd.cpp
  src/synthesis/effects/analogue/chorus_engine.cpp
  src/synthesis/modules/analogue_chorus_module.cpp
  src/synthesis/effects/analogue/flanger_engine.cpp
  src/synthesis/modules/analogue_flanger_module.cpp
  src/synthesis/effects/analogue/echo_engine.cpp
  src/synthesis/modules/analogue_echo_module.cpp
  src/synthesis/effects/analogue/rotary_engine.cpp
  src/synthesis/modules/analogue_rotary_module.cpp
  src/synthesis/effects/analogue/spring_engine.cpp
  src/synthesis/modules/analogue_spring_module.cpp
  src/synthesis/effects/analogue/linear_circuit.cpp
  src/synthesis/effects/analogue/wah_engine.cpp
  src/synthesis/modules/analogue_wah_module.cpp
  src/synthesis/effects/spectral_balance/spectral_balance_engine.cpp
  src/synthesis/effects/filter_table/filter_table_kernel.cpp
  src/synthesis/filters/table_filter.cpp
  src/synthesis/modules/spectral_balance_module.cpp
  src/synthesis/modules/suppressor_module.cpp
  src/unity_build/common.cpp
  src/unity_build/synthesis.cpp
  src/unity_build/plugin.cpp)

# Rust's all-pass recurrences use separate multiply/add operations. Do not
# contract these into FMA or apply Atlas's unity fast-math transformations.
set_source_files_properties(src/synthesis/modules/defuse_module.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")

target_include_directories(Atlas PRIVATE
  src/common
  src/common/wavetable
  src/interface/look_and_feel
  src/plugin
  src/synthesis/synth_engine
  src/synthesis/effects
  src/synthesis/filters
  src/synthesis/framework
  src/synthesis/lookups
  src/synthesis/modulators
  src/synthesis/modules
  src/synthesis/producers
  src/synthesis/utilities
  third_party
  third_party/concurrentqueue)

target_compile_definitions(Atlas PRIVATE
  VITAL_ACCESSIBLE_UI=1
  HEADLESS=0
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_DSP_USE_SHARED_FFTW=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

# Vital's DSP unity build is designed around SIMD arithmetic. The original
# production build enables fast floating-point transforms for this code, but
# applying that flag to JUCE itself would weaken JUCE's NaN/Inf handling.
set_source_files_properties(src/unity_build/synthesis.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/fp:fast>;$<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-ffast-math>")

target_link_libraries(Atlas PRIVATE
  juce::juce_audio_utils
  juce::juce_dsp
  juce::juce_gui_extra
  PUBLIC
  juce::juce_recommended_config_flags)

# Copy Tape ciphertext before the final signature, including AU bundles whose
# linker signature alone does not seal resource files. Asset edits trigger relinking.
file(GLOB ATLAS_TAPE_ASSETS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape/*.atape")
set_property(TARGET Atlas_VST3 APPEND PROPERTY LINK_DEPENDS "${ATLAS_TAPE_ASSETS}")
if(APPLE)
  set_property(TARGET Atlas_AU APPEND PROPERTY LINK_DEPENDS "${ATLAS_TAPE_ASSETS}")
  foreach(format AU VST3)
    add_custom_command(TARGET Atlas_${format} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape"
        "$<TARGET_BUNDLE_DIR:Atlas_${format}>/Contents/Resources/Tape"
      COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:Atlas_${format}>"
      VERBATIM)
  endforeach()
elseif(WIN32)
  target_link_libraries(Atlas PRIVATE bcrypt)
  add_custom_command(TARGET Atlas_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape"
      "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/Tape" VERBATIM)
else()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(Atlas PRIVATE OpenSSL::Crypto)
  add_custom_command(TARGET Atlas_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape"
      "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/Tape" VERBATIM)
endif()

option(ATLAS_BUILD_TIMING_TESTS "Build the Atlas test suite (run with ctest)" OFF)
if(ATLAS_BUILD_TIMING_TESTS)
  enable_testing()
  juce_add_console_app(AtlasTimingTests PRODUCT_NAME "Atlas Timing Tests")
  juce_generate_juce_header(AtlasTimingTests)
  target_sources(AtlasTimingTests PRIVATE tests/timing_stability_tests.cpp)
  target_include_directories(AtlasTimingTests PRIVATE
    src/common
    src/common/wavetable
    src/synthesis/effects
    src/synthesis/filters
    src/synthesis/framework
    src/synthesis/lookups
    src/synthesis/modulators
    src/synthesis/modules
    src/synthesis/producers
    src/synthesis/synth_engine
    third_party)
  target_compile_definitions(AtlasTimingTests PRIVATE
    VITAL_ACCESSIBLE_UI=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasTimingTests PRIVATE
    Atlas
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags)
  add_test(NAME atlas_timing_stability COMMAND AtlasTimingTests)

  juce_add_console_app(AtlasWavetableEditTests PRODUCT_NAME "Atlas Wavetable Edit Tests")
  juce_generate_juce_header(AtlasWavetableEditTests)
  target_sources(AtlasWavetableEditTests PRIVATE tests/wavetable_edit_tests.cpp)
  target_include_directories(AtlasWavetableEditTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/lookups
    src/synthesis/producers src/synthesis/modulators src/synthesis/synth_engine third_party)
  target_compile_definitions(AtlasWavetableEditTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasWavetableEditTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_wavetable_edit COMMAND AtlasWavetableEditTests)
  add_test(NAME atlas_envelope_regressions COMMAND AtlasTimingTests --envelopes)
  add_test(NAME atlas_oscillator_levels COMMAND AtlasTimingTests --oscillator-levels)
  add_test(NAME atlas_spectral_oscillator COMMAND AtlasTimingTests --spectral-oscillator)
  add_test(NAME atlas_oscillator_warps COMMAND AtlasTimingTests --oscillator-warps)
  add_test(NAME atlas_oscillator_warp_controls COMMAND AtlasTimingTests --oscillator-warp-controls)
  add_test(NAME atlas_random_amplitude_seeds COMMAND AtlasTimingTests --random-amplitude-seeds)
  add_test(NAME atlas_random_amplitude_seed_controls COMMAND AtlasTimingTests --random-amplitude-seed-controls)
  add_test(NAME atlas_sample_navigation COMMAND AtlasTimingTests --sample-navigation)
  add_test(NAME atlas_reverb_filter COMMAND AtlasTimingTests --reverb-filter)
  add_test(NAME atlas_scream_filter COMMAND AtlasTimingTests --scream-filter)
  add_test(NAME atlas_ring_mod_filter COMMAND AtlasTimingTests --ring-mod-filter)
  add_test(NAME atlas_sample_hold_filter COMMAND AtlasTimingTests --sample-hold-filter)
  add_test(NAME atlas_filter_keytrack COMMAND AtlasTimingTests --filter-keytrack)
  add_test(NAME atlas_filter_routing COMMAND AtlasTimingTests --filter-routing)
  add_test(NAME atlas_release_velocity_glide COMMAND AtlasTimingTests --release-velocity-glide)
  add_test(NAME atlas_analog_oscillator COMMAND AtlasTimingTests --analog-oscillator)
  add_test(NAME atlas_analog_oscillator_controls COMMAND AtlasTimingTests --analog-oscillator-controls)

  juce_add_console_app(AtlasCpuTests PRODUCT_NAME "Atlas CPU Tests")
  juce_generate_juce_header(AtlasCpuTests)
  target_sources(AtlasCpuTests PRIVATE tests/cpu_optimization_tests.cpp)
  target_include_directories(AtlasCpuTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasCpuTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasCpuTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_cpu_optimizations COMMAND AtlasCpuTests "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape")

  juce_add_console_app(AtlasPreparedEffectTests PRODUCT_NAME "Atlas Prepared Effect Tests")
  juce_generate_juce_header(AtlasPreparedEffectTests)
  target_sources(AtlasPreparedEffectTests PRIVATE tests/prepared_effect_tests.cpp)
  target_include_directories(AtlasPreparedEffectTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasPreparedEffectTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasPreparedEffectTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_prepared_effects COMMAND AtlasPreparedEffectTests)
  add_test(NAME atlas_convolution_threading COMMAND AtlasPreparedEffectTests --convolution-threading)
  set_tests_properties(atlas_convolution_threading PROPERTIES TIMEOUT 60)
  add_test(NAME atlas_ir_switching COMMAND AtlasPreparedEffectTests --ir-switching)
  set_tests_properties(atlas_ir_switching PROPERTIES TIMEOUT 60)

  juce_add_console_app(AtlasLatencyTests PRODUCT_NAME "Atlas Latency Tests")
  juce_generate_juce_header(AtlasLatencyTests)
  target_sources(AtlasLatencyTests PRIVATE tests/latency_tests.cpp)
  target_include_directories(AtlasLatencyTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasLatencyTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasLatencyTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_latency COMMAND AtlasLatencyTests)

  juce_add_console_app(AtlasFxAutomationTests PRODUCT_NAME "Atlas FX Automation Tests")
  juce_generate_juce_header(AtlasFxAutomationTests)
  target_sources(AtlasFxAutomationTests PRIVATE tests/fx_automation_tests.cpp)
  target_include_directories(AtlasFxAutomationTests PRIVATE
    src/common
    src/common/wavetable
    src/interface/look_and_feel
    src/plugin
    src/synthesis/effects
    src/synthesis/filters
    src/synthesis/framework
    src/synthesis/lookups
    src/synthesis/modulators
    src/synthesis/modules
    src/synthesis/producers
    src/synthesis/synth_engine
    third_party)
  target_compile_definitions(AtlasFxAutomationTests PRIVATE
    VITAL_ACCESSIBLE_UI=1
    HEADLESS=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DSP_USE_SHARED_FFTW=0
    # The Resonator UI test runs the editor's timer to refresh model-specific rows.
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasFxAutomationTests PRIVATE
    Atlas
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags)
  add_test(NAME atlas_fx_automation COMMAND AtlasFxAutomationTests)

  juce_add_console_app(AtlasMacroTests PRODUCT_NAME "Atlas Macro Tests")
  juce_generate_juce_header(AtlasMacroTests)
  target_sources(AtlasMacroTests PRIVATE tests/macro_modulation_tests.cpp)
  if(APPLE)
    target_sources(AtlasMacroTests PRIVATE tests/cpu_meter_accessibility_mac.mm)
  endif()
  target_include_directories(AtlasMacroTests PRIVATE
    src/common src/common/wavetable src/interface/look_and_feel src/plugin
    src/synthesis/effects src/synthesis/filters src/synthesis/framework
    src/synthesis/lookups src/synthesis/modulators src/synthesis/modules
    src/synthesis/producers src/synthesis/synth_engine third_party)
  target_compile_definitions(AtlasMacroTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ATLAS_FILTER_TABLE_FACTORY="${CMAKE_SOURCE_DIR}/Factory/Wavetables/Filter Wavetables")
  target_link_libraries(AtlasMacroTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  add_test(NAME atlas_macros COMMAND AtlasMacroTests)
  add_test(NAME atlas_editor_accessibility COMMAND AtlasMacroTests --editor-accessibility)
  add_test(NAME atlas_editor_rebuild_focus COMMAND AtlasMacroTests --editor-rebuild-focus)

  juce_add_console_app(AtlasDefuseTests PRODUCT_NAME "Atlas Defuse Tests")
  juce_generate_juce_header(AtlasDefuseTests)
  target_sources(AtlasDefuseTests PRIVATE tests/defuse/defuse_tests.cpp)
  target_include_directories(AtlasDefuseTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasDefuseTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasDefuseTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  add_test(NAME atlas_defuse COMMAND AtlasDefuseTests)
  juce_add_console_app(AtlasDefuseNoteTests PRODUCT_NAME "Atlas Defuse Note Tests")
  juce_generate_juce_header(AtlasDefuseNoteTests)
  target_sources(AtlasDefuseNoteTests PRIVATE tests/defuse/note_tail_tests.cpp)
  target_include_directories(AtlasDefuseNoteTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasDefuseNoteTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasDefuseNoteTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  add_test(NAME atlas_defuse_note_tails COMMAND AtlasDefuseNoteTests)

  juce_add_console_app(AtlasSourceSlotTests PRODUCT_NAME "Atlas Source Slot Tests")
  juce_generate_juce_header(AtlasSourceSlotTests)
  target_sources(AtlasSourceSlotTests PRIVATE tests/source_slot_tests.cpp)
  target_include_directories(AtlasSourceSlotTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasSourceSlotTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasSourceSlotTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  add_test(NAME atlas_source_slots COMMAND AtlasSourceSlotTests --behaviour)
  add_test(NAME atlas_source_slot_cpu COMMAND AtlasSourceSlotTests --cpu)
  set_tests_properties(atlas_source_slot_cpu PROPERTIES LABELS cpu)
  # Bit-exact regression against renders captured before the slot engine
  # (AtlasSourceSlotTests --capture <dir> on the pre-slot build).
  add_test(NAME atlas_source_slot_regression COMMAND AtlasSourceSlotTests
           --compare "${CMAKE_CURRENT_BINARY_DIR}/slot-reference")

  # Optional independent oracle: production builds do not require Rust or
  # the external Diopser checkout. Point this at nih-plug/plugins/diopser, or
  # fetch the opt-in third_party/nih-plug submodule, which is used by default.
  set(ATLAS_DIOPSER_SOURCE "" CACHE PATH "Diopser source directory for exact DSP reference tests")
  if(NOT ATLAS_DIOPSER_SOURCE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nih-plug/plugins/diopser/src/lib.rs")
    set(ATLAS_DIOPSER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nih-plug/plugins/diopser")
  endif()
  if(ATLAS_DIOPSER_SOURCE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ATLAS_DIOPSER_RUSTC rustc REQUIRED)
    set(ATLAS_DEFUSE_REFERENCE "${CMAKE_CURRENT_BINARY_DIR}/defuse-reference${CMAKE_EXECUTABLE_SUFFIX}")
    add_custom_command(OUTPUT "${ATLAS_DEFUSE_REFERENCE}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/defuse/build_reference.py"
        "${ATLAS_DIOPSER_SOURCE}" "${ATLAS_DEFUSE_REFERENCE}" --rustc "${ATLAS_DIOPSER_RUSTC}"
      DEPENDS tests/defuse/build_reference.py
        "${ATLAS_DIOPSER_SOURCE}/src/lib.rs" "${ATLAS_DIOPSER_SOURCE}/src/filter.rs"
        "${ATLAS_DIOPSER_SOURCE}/src/params.rs" "${ATLAS_DIOPSER_SOURCE}/../../src/params/smoothing.rs"
      VERBATIM)
    add_custom_target(AtlasDefuseReference DEPENDS "${ATLAS_DEFUSE_REFERENCE}")
    add_dependencies(AtlasDefuseTests AtlasDefuseReference)
    add_test(NAME atlas_defuse_reference COMMAND AtlasDefuseTests "${ATLAS_DEFUSE_REFERENCE}")
  endif()

  juce_add_console_app(AtlasSpectralGateTests PRODUCT_NAME "Atlas Spectral Gate Tests")
  juce_generate_juce_header(AtlasSpectralGateTests)
  target_sources(AtlasSpectralGateTests PRIVATE tests/spectral_gate_tests.cpp)
  target_include_directories(AtlasSpectralGateTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasSpectralGateTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasSpectralGateTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_spectral_gate COMMAND AtlasSpectralGateTests)

  juce_add_console_app(AtlasSuppressorTests PRODUCT_NAME "Atlas Suppressor Tests")
  juce_generate_juce_header(AtlasSuppressorTests)
  target_sources(AtlasSuppressorTests PRIVATE tests/suppressor/suppressor_tests.cpp)
  target_include_directories(AtlasSuppressorTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasSuppressorTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasSuppressorTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_suppressor COMMAND AtlasSuppressorTests)

  # The original Spectral Lock processor is the oracle for these tests. It
  # comes from the opt-in third_party/aquanode-plugins submodule.
  set(ATLAS_SPECTRAL_LOCK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aquanode-plugins/spectral/spectrallock/src")
  if(EXISTS "${ATLAS_SPECTRAL_LOCK_SOURCE}/PluginProcessor.cpp")
    juce_add_console_app(AtlasSpectralLockTests PRODUCT_NAME "Atlas Spectral Lock Tests")
    juce_generate_juce_header(AtlasSpectralLockTests)
    target_sources(AtlasSpectralLockTests PRIVATE tests/spectral_lock_tests.cpp
      tests/spectral_lock_reference_processor.cpp "${ATLAS_SPECTRAL_LOCK_SOURCE}/PluginEditor.cpp")
    target_include_directories(AtlasSpectralLockTests PRIVATE
      src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
    target_compile_definitions(AtlasSpectralLockTests PRIVATE
      VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
    target_link_libraries(AtlasSpectralLockTests PRIVATE
      Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
    add_test(NAME atlas_spectral_lock COMMAND AtlasSpectralLockTests)
  else()
    message(STATUS "Spectral Lock reference tests skipped: "
                   "git submodule update --init --checkout third_party/aquanode-plugins")
  endif()

  juce_add_console_app(AtlasSpectralCompressorTests PRODUCT_NAME "Atlas Spectral Compressor Tests")
  juce_generate_juce_header(AtlasSpectralCompressorTests)
  target_sources(AtlasSpectralCompressorTests PRIVATE tests/spectral_compressor_tests.cpp)
  target_include_directories(AtlasSpectralCompressorTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasSpectralCompressorTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasSpectralCompressorTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_spectral_compressor COMMAND AtlasSpectralCompressorTests)

  juce_add_console_app(AtlasSpectralBalanceTests PRODUCT_NAME "Atlas Spectral Balance Tests")
  juce_generate_juce_header(AtlasSpectralBalanceTests)
  target_sources(AtlasSpectralBalanceTests PRIVATE tests/spectral_balance_tests.cpp)
  target_include_directories(AtlasSpectralBalanceTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasSpectralBalanceTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasSpectralBalanceTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_spectral_balance COMMAND AtlasSpectralBalanceTests)

  juce_add_console_app(AtlasFilterTableTests PRODUCT_NAME "Atlas Filter Table Tests")
  juce_generate_juce_header(AtlasFilterTableTests)
  target_sources(AtlasFilterTableTests PRIVATE tests/filter_table_tests.cpp)
  target_include_directories(AtlasFilterTableTests PRIVATE
    src/common src/common/wavetable src/interface/look_and_feel src/plugin src/synthesis/framework
    src/synthesis/modules src/synthesis/filters src/synthesis/effects src/synthesis/lookups
    src/synthesis/producers src/synthesis/synth_engine src/synthesis/modulators third_party)
  target_compile_definitions(AtlasFilterTableTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    ATLAS_FILTER_TABLE_FACTORY="${CMAKE_SOURCE_DIR}/Factory/Wavetables/Filter Wavetables")
  target_link_libraries(AtlasFilterTableTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_filter_table COMMAND AtlasFilterTableTests)

  juce_add_console_app(AtlasTapeTests PRODUCT_NAME "Atlas Tape Tests")
  juce_generate_juce_header(AtlasTapeTests)
  target_sources(AtlasTapeTests PRIVATE tests/tape_tests.cpp)
  target_include_directories(AtlasTapeTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/filters src/synthesis/effects third_party)
  target_compile_definitions(AtlasTapeTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasTapeTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_tape COMMAND AtlasTapeTests "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape")

  juce_add_console_app(AtlasTranceGateTests PRODUCT_NAME "Atlas Trance Gate Tests")
  juce_generate_juce_header(AtlasTranceGateTests)
  target_sources(AtlasTranceGateTests PRIVATE tests/trance_gate/module_tests.cpp)
  target_include_directories(AtlasTranceGateTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel src/synthesis/effects
    src/synthesis/modules src/synthesis/framework src/synthesis/lookups src/synthesis/modulators
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine third_party)
  target_compile_definitions(AtlasTranceGateTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasTranceGateTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_trance_gate COMMAND AtlasTranceGateTests)

  juce_add_console_app(AtlasTremoloPanTests PRODUCT_NAME "Atlas Tremolo Pan Tests")
  juce_generate_juce_header(AtlasTremoloPanTests)
  target_sources(AtlasTremoloPanTests PRIVATE tests/tremolo_pan_tests.cpp)
  target_include_directories(AtlasTremoloPanTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasTremoloPanTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasTremoloPanTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_tremolo_pan COMMAND AtlasTremoloPanTests)

  juce_add_console_app(AtlasResonatorTests PRODUCT_NAME "Atlas Resonator Tests")
  juce_generate_juce_header(AtlasResonatorTests)
  target_sources(AtlasResonatorTests PRIVATE tests/resonator_tests.cpp)
  target_include_directories(AtlasResonatorTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel src/synthesis/effects
    src/synthesis/modules src/synthesis/framework src/synthesis/lookups src/synthesis/modulators
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine third_party)
  target_compile_definitions(AtlasResonatorTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasResonatorTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_resonator COMMAND AtlasResonatorTests)

  juce_add_console_app(AtlasAnalogueTests PRODUCT_NAME "Atlas Analogue Tests")
  juce_generate_juce_header(AtlasAnalogueTests)
  target_sources(AtlasAnalogueTests PRIVATE tests/analogue_tests.cpp)
  target_include_directories(AtlasAnalogueTests PRIVATE
    src/common src/common/wavetable src/plugin src/interface/look_and_feel src/synthesis/effects
    src/synthesis/modules src/synthesis/framework src/synthesis/lookups src/synthesis/modulators
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine third_party)
  target_compile_definitions(AtlasAnalogueTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    ATLAS_ANALOGUE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/analogue"
    ATLAS_ANALOGUE_PRESETS="${CMAKE_SOURCE_DIR}/Factory/FX/Analogue")
  target_link_libraries(AtlasAnalogueTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_analogue_bbd COMMAND AtlasAnalogueTests --bbd)
  add_test(NAME atlas_analogue_chorus COMMAND AtlasAnalogueTests --chorus)
  add_test(NAME atlas_analogue_flanger COMMAND AtlasAnalogueTests --flanger)
  add_test(NAME atlas_analogue_echo COMMAND AtlasAnalogueTests --echo)
  add_test(NAME atlas_analogue_presets COMMAND AtlasAnalogueTests --presets)
  add_test(NAME atlas_analogue_rotary COMMAND AtlasAnalogueTests --rotary)
  add_test(NAME atlas_analogue_labels COMMAND AtlasAnalogueTests --labels)
  add_test(NAME atlas_analogue_spring COMMAND AtlasAnalogueTests --spring)
  add_test(NAME atlas_analogue_wah COMMAND AtlasAnalogueTests --wah)
  add_test(NAME atlas_analogue_cpu COMMAND AtlasAnalogueTests --cpu)
  set_tests_properties(atlas_analogue_cpu PROPERTIES LABELS cpu)

  juce_add_console_app(AtlasScreamTests PRODUCT_NAME "Atlas Scream Tests")
  juce_generate_juce_header(AtlasScreamTests)
  target_sources(AtlasScreamTests PRIVATE
    tests/scream_tests.cpp tests/scream_reference_processor.cpp
    "third_party/scream/Source/LFO/LFOData.cpp"
    "third_party/scream/Source/LFO/LFOProcessor.cpp")
  target_include_directories(AtlasScreamTests PRIVATE
    "third_party/scream/Source"
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasScreamTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasScreamTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_scream COMMAND AtlasScreamTests)

  juce_add_console_app(AtlasJsfxTests PRODUCT_NAME "Atlas JSFX Tests")
  juce_generate_juce_header(AtlasJsfxTests)
  target_sources(AtlasJsfxTests PRIVATE tests/jsfx/module_tests.cpp)
  target_include_directories(AtlasJsfxTests PRIVATE
    src/common src/common/wavetable src/synthesis/framework src/synthesis/modules src/synthesis/effects third_party)
  target_compile_definitions(AtlasJsfxTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
  target_link_libraries(AtlasJsfxTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_jsfx_modules COMMAND AtlasJsfxTests)

  # Optional oracle: compile and execute the unmodified supplied JSFX with WDL
  # EEL2. This external test dependency is never linked into the plug-in. The
  # opt-in third_party/WDL submodule is used by default once fetched.
  set(ATLAS_EEL2_DIR "" CACHE PATH "Optional path to WDL/eel2 for JSFX reference tests")
  if(NOT ATLAS_EEL2_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL/WDL/eel2/ns-eel.h")
    set(ATLAS_EEL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL/WDL/eel2")
  endif()
  if(EXISTS "${ATLAS_EEL2_DIR}/ns-eel.h")
    add_library(AtlasEelReference STATIC
      ${ATLAS_EEL2_DIR}/nseel-caltab.c ${ATLAS_EEL2_DIR}/nseel-compiler.c
      ${ATLAS_EEL2_DIR}/nseel-eval.c ${ATLAS_EEL2_DIR}/nseel-lextab.c
      ${ATLAS_EEL2_DIR}/nseel-ram.c ${ATLAS_EEL2_DIR}/nseel-yylex.c ${ATLAS_EEL2_DIR}/nseel-cfunc.c)
    target_compile_definitions(AtlasEelReference PUBLIC EEL_TARGET_PORTABLE)
    target_include_directories(AtlasEelReference PUBLIC ${ATLAS_EEL2_DIR})
    juce_add_console_app(AtlasReverbFilterReferenceTests PRODUCT_NAME "Atlas Reverb Filter Reference Tests")
    juce_generate_juce_header(AtlasReverbFilterReferenceTests)
    target_sources(AtlasReverbFilterReferenceTests PRIVATE tests/reverb_filter/reference_tests.cpp)
    target_include_directories(AtlasReverbFilterReferenceTests PRIVATE
      src/common src/synthesis/filters src/synthesis/framework src/synthesis/modules
      src/synthesis/lookups src/synthesis/utilities src/interface/look_and_feel)
    target_compile_definitions(AtlasReverbFilterReferenceTests PRIVATE
      VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
    target_link_libraries(AtlasReverbFilterReferenceTests PRIVATE AtlasEelReference Atlas
      juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
    add_test(NAME atlas_reverb_filter_reference COMMAND AtlasReverbFilterReferenceTests
      "${CMAKE_SOURCE_DIR}/tests/reverb_filter/reference.jsfx")
    add_executable(AtlasJsfxReferenceTests tests/jsfx/eel_reference_tests.cpp)
    target_include_directories(AtlasJsfxReferenceTests PRIVATE src/synthesis/effects)
    target_link_libraries(AtlasJsfxReferenceTests PRIVATE AtlasEelReference Atlas)
    add_test(NAME atlas_jsfx_reference COMMAND AtlasJsfxReferenceTests "${CMAKE_SOURCE_DIR}/tests/jsfx/fixtures")
    juce_add_console_app(AtlasTranceGateReferenceTests PRODUCT_NAME "Atlas Trance Gate Reference Tests")
    juce_generate_juce_header(AtlasTranceGateReferenceTests)
    target_sources(AtlasTranceGateReferenceTests PRIVATE tests/trance_gate/reference_tests.cpp)
    target_include_directories(AtlasTranceGateReferenceTests PRIVATE
      src/common src/synthesis/modules src/synthesis/framework src/synthesis/lookups)
    target_compile_definitions(AtlasTranceGateReferenceTests PRIVATE
      VITAL_ACCESSIBLE_UI=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0)
    target_link_libraries(AtlasTranceGateReferenceTests PRIVATE AtlasEelReference Atlas
      juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
    add_test(NAME atlas_trance_gate_reference COMMAND AtlasTranceGateReferenceTests
      "${CMAKE_SOURCE_DIR}/tests/trance_gate/BP StepGate.jsfx")
    add_executable(AtlasResonatorReferenceTests tests/resonator/reference_tests.cpp)
    target_include_directories(AtlasResonatorReferenceTests PRIVATE src/synthesis/effects)
    target_link_libraries(AtlasResonatorReferenceTests PRIVATE AtlasEelReference Atlas)
    add_test(NAME atlas_resonator_reference COMMAND AtlasResonatorReferenceTests
      "${CMAKE_SOURCE_DIR}/tests/resonator/Corpus.jsfx")
  endif()

  # A tiny external host used to stress the real VST3 bundle and factory.
  # Unlike AtlasTimingTests, this executable does not link Atlas' shared code;
  # it loads the built plug-in dynamically through JUCE's headless VST3 host.
  juce_add_console_app(AtlasVstFactoryStress PRODUCT_NAME "Atlas VST3 Factory Stress")
  juce_generate_juce_header(AtlasVstFactoryStress)
  target_sources(AtlasVstFactoryStress PRIVATE tests/vst3_factory_stress.cpp)
  target_compile_definitions(AtlasVstFactoryStress PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
  target_link_libraries(AtlasVstFactoryStress PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)
  add_dependencies(AtlasVstFactoryStress Atlas_VST3)
  if(APPLE)
    add_test(NAME atlas_vst3_latency COMMAND AtlasVstFactoryStress
      "$<TARGET_BUNDLE_DIR:Atlas_VST3>" --latency-test)
  elseif(WIN32)
    add_test(NAME atlas_vst3_latency COMMAND AtlasVstFactoryStress
      "$<TARGET_FILE_DIR:Atlas_VST3>/../.." --latency-test)
  endif()

  # Several tests measure timing/CPU or spawn their own threads, so never let
  # ctest run them side by side, even when invoked with -j. Give every test a
  # timeout so a hung test fails instead of holding the machine for ctest's
  # 1500 s default. Tests that already set a TIMEOUT keep it.
  set(ATLAS_TEST_TIMEOUT 600 CACHE STRING "Default per-test timeout in seconds")
  get_property(atlas_tests DIRECTORY PROPERTY TESTS)
  foreach(atlas_test IN LISTS atlas_tests)
    set_tests_properties(${atlas_test} PROPERTIES RUN_SERIAL TRUE)
    get_test_property(${atlas_test} TIMEOUT atlas_test_timeout)
    if(NOT atlas_test_timeout)
      set_tests_properties(${atlas_test} PROPERTIES TIMEOUT ${ATLAS_TEST_TIMEOUT})
    endif()
  endforeach()
endif()
