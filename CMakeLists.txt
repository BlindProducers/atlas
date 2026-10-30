cmake_minimum_required(VERSION 3.22)

set(ATLAS_PROJECT_LANGUAGES C CXX)
set(ATLAS_PLUGIN_FORMATS VST3)

if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS architectures" FORCE)
  # Without a deployment target the plugins only load on the macOS version
  # that built them. 11.0 is the first macOS for Apple Silicon.
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum macOS version" FORCE)
  endif()
  list(APPEND ATLAS_PROJECT_LANGUAGES OBJC OBJCXX)
  list(APPEND ATLAS_PLUGIN_FORMATS AU)
endif()

project(Atlas VERSION 2.0.0 LANGUAGES ${ATLAS_PROJECT_LANGUAGES})

if(MSVC)
  # Keep Atlas and all static dependencies on the same self-contained CRT.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # MSVC only declares M_PI and the other <cmath> constants with this macro.
  add_compile_definitions(_USE_MATH_DEFINES)
  # JUCE turns on /MP, which starts one compiler per core inside each project.
  # The unity files need several GB each, so cap it like --parallel caps make.
  set(ATLAS_MSVC_COMPILE_JOBS 4 CACHE STRING "Compiler processes per Visual Studio project (/MP)")
  set(CMAKE_VS_GLOBALS "CL_MPCount=${ATLAS_MSVC_COMPILE_JOBS}")
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
                   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/kissfft/kissfft.hh"
                   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/espeak-ng/CMakeLists.txt")
  if(NOT EXISTS "${dependency}")
    message(FATAL_ERROR "Missing ${dependency}. Fetch the submodules with: git submodule update --init")
  endif()
endforeach()

# Atlas's changes to JUCE, applied to the submodule's working tree once (a
# patch that already applies in reverse is in place). third_party/README.atlas.md
# says what each one does.
find_package(Git REQUIRED)
file(GLOB ATLAS_JUCE_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/patches/juce8-*.patch")
foreach(patch ${ATLAS_JUCE_PATCHES})
  execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
                  WORKING_DIRECTORY "${ATLAS_JUCE_SOURCE}" RESULT_VARIABLE already_applied
                  OUTPUT_QUIET ERROR_QUIET)
  if(NOT already_applied EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
                    WORKING_DIRECTORY "${ATLAS_JUCE_SOURCE}" RESULT_VARIABLE patch_result
                    ERROR_VARIABLE patch_error)
    if(NOT patch_result EQUAL 0)
      message(FATAL_ERROR "Could not apply ${patch} to ${ATLAS_JUCE_SOURCE}:\n${patch_error}")
    endif()
    message(STATUS "Applied ${patch}")
  endif()
endforeach()
add_subdirectory("${ATLAS_JUCE_SOURCE}" JUCE)

# espeak-ng (GPL-3.0-or-later) speaks the Speech oscillator's text: its
# static library, the espeak-ng program that compiles its data, and the data
# (phonemes, intonations and every language's dictionary) that the bundles
# carry in Resources/espeak-ng-data. No audio output, MBROLA or async mode.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
set(COMPILE_INTONATIONS ON)
set(ENABLE_TESTS OFF)
set(BUILD_TESTING OFF)
set(USE_MBROLA OFF)
set(USE_LIBSONIC OFF)
set(USE_LIBPCAUDIO OFF)
set(USE_ASYNC OFF)
set(USE_KLATT ON)
set(USE_SPEECHPLAYER ON)
set(ESPEAK_COMPAT OFF)
set(ESPEAK_BUILD_MANPAGES OFF)
add_subdirectory(third_party/espeak-ng espeak-ng EXCLUDE_FROM_ALL)
unset(BUILD_SHARED_LIBS)
# The folder espeak_Initialize takes: it holds espeak-ng-data.
set(ATLAS_ESPEAK_DATA_ROOT "${CMAKE_CURRENT_BINARY_DIR}/espeak-ng")
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

# The OpenWurli port reproduces the Rust's separate multiplies and adds: no
# FMA contraction and no fast-math, or it drifts from the reference renders.
set(ATLAS_OPENWURLI_SOURCES
  src/synthesis/effects/openwurli/wurli_tables.cpp
  src/synthesis/effects/openwurli/wurli_pickup.cpp
  src/synthesis/effects/openwurli/wurli_pickup_tables.cpp
  src/synthesis/effects/openwurli/wurli_mlp.cpp
  src/synthesis/effects/openwurli/wurli_voice.cpp
  src/synthesis/effects/openwurli/wurli_oversampler.cpp
  src/synthesis/effects/openwurli/wurli_preamp.cpp
  src/synthesis/effects/openwurli/wurli_tremolo.cpp
  src/synthesis/effects/openwurli/wurli_tremolo_circuit.cpp
  src/synthesis/effects/openwurli/wurli_power_amp.cpp
  src/synthesis/effects/openwurli/wurli_speaker.cpp
  src/synthesis/effects/openwurli/wurli_amp.cpp)

# The OpenB3 port (setBfree's tonegen, vibrato and whirl) keeps the C code's
# separate multiplies and adds, so the whirl and scanner match it sample for
# sample: no FMA contraction and no fast-math.
set(ATLAS_OPENB3_SOURCES
  src/synthesis/effects/openb3/tonewheel_tables.cpp
  src/synthesis/effects/openb3/tonewheel_organ.cpp
  src/synthesis/effects/openb3/scanner_vibrato.cpp
  src/synthesis/effects/openb3/whirl.cpp)

# The Analogue Compressor (ZL Compressor's engine) works in decibels with
# logarithms of signals that reach zero, so it keeps IEEE semantics: no
# fast-math, and no FMA contraction, so macOS and Windows compute alike.
set(ATLAS_ZL_COMPRESSOR_SOURCES
  src/synthesis/effects/zl_compressor/zl_compressor_engine.cpp
  src/synthesis/modules/analogue_compressor_module.cpp)

# The Maim effect (Maim's controllers around its codecs): the LPC vocoder's
# quantizers and the packet patterns decide the same way on every platform
# only with IEEE semantics, so no fast-math and no FMA contraction.
set(ATLAS_MAIM_SOURCES
  src/synthesis/effects/maim/blade_codec.cpp
  src/synthesis/effects/maim/gsm_codec.cpp
  src/synthesis/effects/maim/lpc_codec.cpp
  src/synthesis/effects/maim/maim_engine.cpp
  src/synthesis/effects/maim/mp3_codec.cpp
  src/synthesis/effects/maim/mp3_manager.cpp
  src/synthesis/effects/maim/opus_codec.cpp
  src/synthesis/effects/maim/resampler.cpp
  src/synthesis/modules/maim_module.cpp)

# The Particles effect runs Mutable Instruments' granular processor (MIT,
# src/synthesis/effects/particles/clouds, unmodified) and the stmlib DSP it
# needs. Like Braids it is firmware code: TEST is stmlib's host-build switch
# (portable C in place of ARM assembly), and its 16.16 phase arithmetic wants
# -fwrapv. Its floats round alike on arm64 and x64 only without fast-math or
# FMA contraction, which the engine test's bit-exact comparison relies on.
# _USE_MATH_DEFINES gives MSVC the M_PI stmlib's filters use.
set(ATLAS_PARTICLES_SOURCES
  src/synthesis/effects/particles/particles_engine.cpp
  src/synthesis/effects/particles/clouds/resources.cc
  src/synthesis/effects/particles/clouds/dsp/correlator.cc
  src/synthesis/effects/particles/clouds/dsp/granular_processor.cc
  src/synthesis/effects/particles/clouds/dsp/mu_law.cc
  src/synthesis/effects/particles/clouds/dsp/pvoc/frame_transformation.cc
  src/synthesis/effects/particles/clouds/dsp/pvoc/phase_vocoder.cc
  src/synthesis/effects/particles/clouds/dsp/pvoc/stft.cc
  src/synthesis/effects/stmlib/stmlib/dsp/atan.cc
  src/synthesis/effects/stmlib/stmlib/dsp/units.cc)

# Saturator, Overdrive, Dynamic Tube and Multiband Dynamics: Alessio Scala's
# JSFX ported line for line in double precision, which the reference test
# compares with the scripts run through EEL2. No fast-math, so the arithmetic
# stays as written, and no FMA contraction, so macOS and Windows agree.
set(ATLAS_DRIVE_DYNAMICS_SOURCES
  src/synthesis/effects/drive_dynamics/saturator.cpp
  src/synthesis/effects/drive_dynamics/dynamic_tube.cpp
  src/synthesis/effects/drive_dynamics/overdrive.cpp
  src/synthesis/effects/drive_dynamics/multiband_dynamics.cpp)

# The Frequency Shifter's Mode 2: Alessio Scala's Serum 2 Bode JSFX, ported the
# same way, statement for statement, so that AtlasBodeReferenceTests finds every
# output sample identical to the script's.
set(ATLAS_BODE_SOURCES src/synthesis/effects/bode/bode_shifter.cpp)

# PFFFT (third_party/pffft) is every FFT in Atlas on every platform: the
# oscillators' (src/common/fourier_transform.h) and, through
# third_party/patches/juce8-pffft-fft-engine.patch, every juce::dsp::FFT and
# juce::dsp::Convolution. Windows and macOS then transform alike, and Windows
# no longer runs JUCE's scalar fallback. The definitions are public so every
# target built against Atlas compiles JUCE's FFT and fourier_transform.h the
# same way (they must all agree).
option(ATLAS_USE_PFFFT "Use PFFFT for every FFT" ON)
if(ATLAS_USE_PFFFT)
  target_sources(Atlas PRIVATE third_party/pffft/pffft.c)
  target_include_directories(Atlas PUBLIC third_party/pffft)
  target_compile_definitions(Atlas PUBLIC ATLAS_USE_PFFFT=1 JUCE_DSP_USE_PFFFT=1)
else()
  target_compile_definitions(Atlas PUBLIC ATLAS_USE_PFFFT=0)
endif()

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
  src/synthesis/filters/scream_tanh.cpp
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
  # Glitch Delay (adapted from Astralay, MIT): long glides in double precision
  # and non-finite checks by bit pattern, kept outside fast-math.
  src/synthesis/effects/glitch_delay/formant_shifter.cpp
  src/synthesis/effects/glitch_delay/glitch_chain.cpp
  src/synthesis/effects/glitch_delay/glitch_delay_engine.cpp
  src/synthesis/effects/glitch_delay/note_values.cpp
  src/synthesis/effects/glitch_delay/real_fft.cpp
  src/synthesis/effects/glitch_delay/smear.cpp
  src/synthesis/effects/glitch_delay/tap.cpp
  src/synthesis/modules/glitch_delay_module.cpp
  # Mutable Instruments Braids (MIT): fixed-point firmware code built on its
  # own so stmlib's macros stay out of the unity builds.
  src/synthesis/effects/braids/braids_engine.cpp
  src/synthesis/effects/braids/braids/analog_oscillator.cc
  src/synthesis/effects/braids/braids/digital_oscillator.cc
  src/synthesis/effects/braids/braids/macro_oscillator.cc
  src/synthesis/effects/braids/braids/resources.cc
  src/synthesis/effects/stmlib/stmlib/utils/random.cc
  # The Particles effect: Mutable Instruments' granular processor (MIT) with
  # stmlib, built on its own like Braids.
  ${ATLAS_PARTICLES_SOURCES}
  # OpenWurli (GPL-3.0): the Wurlitzer 200A model ported from Rust, in double
  # precision and checked sample for sample against the original.
  ${ATLAS_OPENWURLI_SOURCES}
  src/synthesis/producers/openwurli_source.cpp
  src/synthesis/modules/openwurli_module.cpp
  src/synthesis/modules/openwurli_amp_module.cpp
  # OpenB3 (setBfree's Beatrix engine, GPL v2 or later; OpenB3 AGPL v3 or
  # later): the tonewheel organ, the scanner vibrato and the whirl.
  ${ATLAS_OPENB3_SOURCES}
  src/synthesis/producers/openb3_source.cpp
  src/synthesis/modules/openb3_module.cpp
  src/synthesis/modules/scanner_vibrato_module.cpp
  # Saturator, Overdrive, Dynamic Tube and Multiband Dynamics (Alessio Scala's JSFX).
  ${ATLAS_DRIVE_DYNAMICS_SOURCES}
  src/synthesis/modules/drive_dynamics_modules.cpp
  # The Frequency Shifter's Mode 2 (Alessio Scala's Serum 2 Bode JSFX).
  ${ATLAS_BODE_SOURCES}
  # ZL Compressor by zsliu98 (AGPL v3): the Analogue Compressor effect.
  ${ATLAS_ZL_COMPRESSOR_SOURCES}
  # Maim by Arden Butterfield (GPL v3): the Maim effect.
  ${ATLAS_MAIM_SOURCES}
  # The community library's API token in the system's credential store. On
  # its own so that Security.h (Carbon's Component) and <windows.h> never
  # meet JUCE's names in the unity files.
  src/common/credential_store.cpp
  src/unity_build/common.cpp
  src/unity_build/synthesis.cpp
  src/unity_build/plugin.cpp)
# The keychain on macOS, Credential Manager on Windows. juce_core links both
# already (Security in its module header, advapi32 by #pragma); named here so
# the credential store does not lean on that.
if(APPLE)
  target_link_libraries(Atlas PRIVATE "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
  target_link_libraries(Atlas PRIVATE advapi32)
endif()

# Rust's all-pass recurrences use separate multiply/add operations. Do not
# contract these into FMA or apply Atlas's unity fast-math transformations.
# On MSVC /fp:precise does both (it stopped contracting in VS 2022 17.0) and
# matches clang's -ffp-contract=off -fno-fast-math. /fp:strict would add
# floating-point environment access, which none of this code uses and which
# costs OpenWurli and OpenB3 about 4 % for the same output.
set_source_files_properties(src/synthesis/modules/defuse_module.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")
set_source_files_properties(${ATLAS_OPENWURLI_SOURCES} src/synthesis/producers/openwurli_source.cpp
  src/synthesis/modules/openwurli_module.cpp src/synthesis/modules/openwurli_amp_module.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")
set_source_files_properties(${ATLAS_OPENB3_SOURCES} src/synthesis/producers/openb3_source.cpp
  src/synthesis/modules/openb3_module.cpp src/synthesis/modules/scanner_vibrato_module.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")
set_source_files_properties(${ATLAS_ZL_COMPRESSOR_SOURCES} ${ATLAS_MAIM_SOURCES}
  ${ATLAS_DRIVE_DYNAMICS_SOURCES} src/synthesis/modules/drive_dynamics_modules.cpp ${ATLAS_BODE_SOURCES} PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")
# The Scream filter's feedback loop amplifies any rounding difference, so its
# SVFs and waveshaper round alike on arm64 and x64.
set_source_files_properties(src/synthesis/filters/scream_filter.cpp src/synthesis/filters/scream_tanh.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")

# Braids is written for the firmware's two's-complement integer arithmetic.
# TEST is stmlib's host-build switch (no ARM RAM sections).
set(ATLAS_BRAIDS_SOURCES
  src/synthesis/effects/braids/braids_engine.cpp
  src/synthesis/effects/braids/braids/analog_oscillator.cc
  src/synthesis/effects/braids/braids/digital_oscillator.cc
  src/synthesis/effects/braids/braids/macro_oscillator.cc
  src/synthesis/effects/braids/braids/resources.cc
  src/synthesis/effects/stmlib/stmlib/utils/random.cc)
set_source_files_properties(${ATLAS_BRAIDS_SOURCES} PROPERTIES
  COMPILE_DEFINITIONS TEST
  COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fwrapv;-w>")

set_source_files_properties(${ATLAS_PARTICLES_SOURCES} PROPERTIES
  COMPILE_DEFINITIONS "TEST;$<$<CXX_COMPILER_ID:MSVC>:_USE_MATH_DEFINES>"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/synthesis/effects/particles"
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fwrapv;-w;-ffp-contract=off;-fno-fast-math>")
# The engine also reaches JUCE through zeroed_buffer.h; without JUCE's using
# directive juce::Random and stmlib::Random stay apart.
set_property(SOURCE src/synthesis/effects/particles/particles_engine.cpp APPEND PROPERTY
  COMPILE_DEFINITIONS DONT_SET_USING_JUCE_NAMESPACE=1)

# The Maim effect's codecs. Maim's circuit-bent LAME (libmp3lame 3.99.5 with
# mpglib's decoder, LGPL 2.1 or later) and BladeEnc (LGPL 2.1 or later) are
# kept in third_party/maim; libgsm (its own permissive licence) in
# third_party/gsm; Opus (BSD-3) is a submodule built by its own CMake. The C
# libraries build without warnings and without FMA contraction, so the
# encoders decide the same way on macOS and Windows. Atlas reaches them only
# through third_party/maim/include, keeping their config.h and common.h off
# its include path.
add_library(atlas_maim_lame STATIC
  third_party/maim/lame/libmp3lame/VbrTag.c
  third_party/maim/lame/libmp3lame/bitstream.c
  third_party/maim/lame/libmp3lame/encoder.c
  third_party/maim/lame/libmp3lame/fft.c
  third_party/maim/lame/libmp3lame/gain_analysis.c
  third_party/maim/lame/libmp3lame/id3tag.c
  third_party/maim/lame/libmp3lame/lame.c
  third_party/maim/lame/libmp3lame/mpglib_interface.c
  third_party/maim/lame/libmp3lame/newmdct.c
  third_party/maim/lame/libmp3lame/presets.c
  third_party/maim/lame/libmp3lame/psymodel.c
  third_party/maim/lame/libmp3lame/quantize.c
  third_party/maim/lame/libmp3lame/quantize_pvt.c
  third_party/maim/lame/libmp3lame/reservoir.c
  third_party/maim/lame/libmp3lame/set_get.c
  third_party/maim/lame/libmp3lame/tables.c
  third_party/maim/lame/libmp3lame/takehiro.c
  third_party/maim/lame/libmp3lame/util.c
  third_party/maim/lame/libmp3lame/vbrquantize.c
  third_party/maim/lame/libmp3lame/version.c
  third_party/maim/lame/mpglib/common.c
  third_party/maim/lame/mpglib/dct64_i386.c
  third_party/maim/lame/mpglib/decode_i386.c
  third_party/maim/lame/mpglib/interface.c
  third_party/maim/lame/mpglib/layer1.c
  third_party/maim/lame/mpglib/layer2.c
  third_party/maim/lame/mpglib/layer3.c
  third_party/maim/lame/mpglib/tabinit.c)
target_include_directories(atlas_maim_lame PRIVATE
  third_party/maim/lame/atlas_config
  third_party/maim/lame/include
  third_party/maim/lame/libmp3lame
  third_party/maim/lame/mpglib)
target_compile_definitions(atlas_maim_lame PRIVATE HAVE_CONFIG_H=1)

add_library(atlas_maim_blade STATIC
  third_party/maim/blade/bladeenc/blade.c
  third_party/maim/blade/bladeenc/codec.c
  third_party/maim/blade/bladeenc/common.c
  third_party/maim/blade/bladeenc/encode.c
  third_party/maim/blade/bladeenc/formatbitstream2.c
  third_party/maim/blade/bladeenc/l3bitstream.c
  third_party/maim/blade/bladeenc/l3psy.c
  third_party/maim/blade/bladeenc/loop.c
  third_party/maim/blade/bladeenc/mdct.c
  third_party/maim/blade/bladeenc/reservoir.c
  third_party/maim/blade/bladeenc/subs.c
  third_party/maim/blade/bladeenc/tables.c)

# libgsm without its toast command-line tool. SASR: >> is an arithmetic
# shift on every compiler Atlas builds with.
add_library(atlas_gsm STATIC
  third_party/gsm/src/add.c
  third_party/gsm/src/code.c
  third_party/gsm/src/debug.c
  third_party/gsm/src/decode.c
  third_party/gsm/src/gsm_create.c
  third_party/gsm/src/gsm_decode.c
  third_party/gsm/src/gsm_destroy.c
  third_party/gsm/src/gsm_encode.c
  third_party/gsm/src/gsm_explode.c
  third_party/gsm/src/gsm_implode.c
  third_party/gsm/src/gsm_option.c
  third_party/gsm/src/gsm_print.c
  third_party/gsm/src/long_term.c
  third_party/gsm/src/lpc.c
  third_party/gsm/src/preprocess.c
  third_party/gsm/src/rpe.c
  third_party/gsm/src/short_term.c
  third_party/gsm/src/table.c)
target_include_directories(atlas_gsm PRIVATE third_party/gsm/inc)
target_compile_definitions(atlas_gsm PRIVATE SASR=1 NeedFunctionPrototypes=1)

foreach(library atlas_maim_lame atlas_maim_blade atlas_gsm)
  set_property(TARGET ${library} PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_compile_options(${library} PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/w;/fp:precise>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w;-ffp-contract=off;-fno-fast-math>)
endforeach()

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
if(MSVC)
  # Opus picks the C runtime itself and takes the DLL one unless told
  # otherwise; Atlas and its other static libraries use the static one, and
  # mixing them makes the plug-in link two runtimes (LNK4098).
  set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
endif()
# Opus reads the target CPU from CMAKE_SYSTEM_PROCESSOR, which Visual Studio
# generators fill with the host's: an x64 build on a Windows on ARM machine
# would get Opus's ARM configuration and none of its SSE paths. The platform
# the generator was given names the target.
if(CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
  set(CMAKE_SYSTEM_PROCESSOR AMD64)
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "Win32")
  set(CMAKE_SYSTEM_PROCESSOR x86)
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
  set(CMAKE_SYSTEM_PROCESSOR ARM64)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  # Every arm64 CPU has NEON, and Opus has no runtime check for it on macOS;
  # presuming it, as Opus does for aarch64 Linux, compiles its NEON paths in.
  set(OPUS_PRESUME_NEON ON CACHE BOOL "" FORCE)
else()
  set(OPUS_PRESUME_NEON OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/opus opus EXCLUDE_FROM_ALL)
set_property(TARGET opus PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(Atlas PRIVATE
  src/common
  src/common/multisample
  src/common/speech
  src/common/wavetable
  src/interface/look_and_feel
  src/plugin
  src/synthesis/synth_engine
  src/synthesis/effects
  src/synthesis/effects/braids
  src/synthesis/effects/openwurli
  src/synthesis/effects/stmlib
  src/synthesis/filters
  src/synthesis/framework
  src/synthesis/lookups
  src/synthesis/modulators
  src/synthesis/modules
  src/synthesis/producers
  src/synthesis/utilities
  third_party
  third_party/concurrentqueue
  third_party/maim/include)

# The beta the release workflow publishes this version as (ATLAS_PRERELEASE in
# .github/workflows/release.yml, "rc.1"), which Settings shows after the
# version; empty for a final release. Bumping it there configures again.
set(ATLAS_PRERELEASE "")
set(ATLAS_RELEASE_WORKFLOW "${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/release.yml")
if(EXISTS "${ATLAS_RELEASE_WORKFLOW}")
  file(STRINGS "${ATLAS_RELEASE_WORKFLOW}" ATLAS_PRERELEASE_LINE REGEX "^[ ]+ATLAS_PRERELEASE:")
  if(ATLAS_PRERELEASE_LINE)
    string(REGEX REPLACE "^[ ]+ATLAS_PRERELEASE:[ ]*\"?([^\" ]*)\"?.*$" "\\1" ATLAS_PRERELEASE "${ATLAS_PRERELEASE_LINE}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ATLAS_RELEASE_WORKFLOW}")
endif()
message(STATUS "Atlas ${PROJECT_VERSION} ${ATLAS_PRERELEASE}")

target_compile_definitions(Atlas PRIVATE
  ATLAS_PRERELEASE="${ATLAS_PRERELEASE}"
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
  atlas_espeak
  atlas_maim_lame
  atlas_maim_blade
  atlas_gsm
  opus
  PUBLIC
  juce::juce_recommended_config_flags)
# Links espeak-ng without its usage requirements: those put its compat shims
# for math.h, stdio.h, string.h, wchar.h and others ahead of the system
# headers in every Atlas and JUCE translation unit (MSVC warns of
# _USE_MATH_DEFINES being redefined in each one). LINK_ONLY drops them only
# on the way through a link interface, hence the interface target.
# espeak_speech.cpp needs only the public header, and the static library's
# symbols must not be declared dllimport on Windows.
add_library(atlas_espeak INTERFACE)
target_link_libraries(atlas_espeak INTERFACE $<LINK_ONLY:espeak-ng>)
target_include_directories(atlas_espeak INTERFACE third_party/espeak-ng/src/include)
target_compile_definitions(atlas_espeak INTERFACE LIBESPEAK_NG_EXPORT=1)
# espeak-ng's compiled data, which the bundles copy below.
add_dependencies(Atlas data)

# Copy the Tape effect's noise recordings before the final signature, including
# AU bundles whose linker signature alone does not seal resource files. Asset
# edits trigger relinking, and the bundle's folder is emptied first so that a
# recording removed from Factory does not linger in a bundle built earlier.
file(GLOB ATLAS_TAPE_ASSETS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape/*.wav")
set_property(TARGET Atlas_VST3 APPEND PROPERTY LINK_DEPENDS "${ATLAS_TAPE_ASSETS}")
if(APPLE)
  set_property(TARGET Atlas_AU APPEND PROPERTY LINK_DEPENDS "${ATLAS_TAPE_ASSETS}")
  foreach(format AU VST3)
    add_custom_command(TARGET Atlas_${format} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_DIR:Atlas_${format}>/Contents/Resources/Tape"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape"
        "$<TARGET_BUNDLE_DIR:Atlas_${format}>/Contents/Resources/Tape"
      COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_DIR:Atlas_${format}>/Contents/Resources/espeak-ng-data"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${ATLAS_ESPEAK_DATA_ROOT}/espeak-ng-data"
        "$<TARGET_BUNDLE_DIR:Atlas_${format}>/Contents/Resources/espeak-ng-data"
      COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:Atlas_${format}>"
      VERBATIM)
  endforeach()
elseif(WIN32)
  # Announcements go to NVDA through its controller interface. MIDL generates
  # the RPC client stub from NVDA's interface definition with the flags NVDA's
  # own client build uses (see third_party/nvda_controller/README.atlas.md).
  # MIDL comes with the Windows SDK the generator selected (or a developer
  # prompt set up), in the SDK's bin/<version>/<host> folder.
  set(ATLAS_WINDOWS_SDK_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
  if(NOT ATLAS_WINDOWS_SDK_VERSION)
    string(REGEX REPLACE "[\\/]$" "" ATLAS_WINDOWS_SDK_VERSION "$ENV{WindowsSDKVersion}")
  endif()
  set(ATLAS_PROGRAM_FILES_X86 "ProgramFiles(x86)")
  set(ATLAS_MIDL_HINTS)
  foreach(root "$ENV{WindowsSdkDir}" "${CMAKE_WINDOWS_KITS_10_DIR}"
               "$ENV{${ATLAS_PROGRAM_FILES_X86}}/Windows Kits/10" "$ENV{ProgramFiles}/Windows Kits/10")
    if(root)
      foreach(host x64 arm64 x86)
        list(APPEND ATLAS_MIDL_HINTS "${root}/bin/${ATLAS_WINDOWS_SDK_VERSION}/${host}")
      endforeach()
    endif()
  endforeach()
  find_program(ATLAS_MIDL midl HINTS ${ATLAS_MIDL_HINTS} REQUIRED)
  if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64")
    set(ATLAS_MIDL_ENV arm64)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ATLAS_MIDL_ENV x64)
  else()
    set(ATLAS_MIDL_ENV win32)
  endif()
  get_filename_component(ATLAS_CL_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  set(ATLAS_NVDA_DIR "${CMAKE_SOURCE_DIR}/third_party/nvda_controller")
  set(ATLAS_NVDA_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/nvda_controller")
  file(MAKE_DIRECTORY "${ATLAS_NVDA_STUB_DIR}")
  add_custom_command(
    OUTPUT "${ATLAS_NVDA_STUB_DIR}/nvdaController.h" "${ATLAS_NVDA_STUB_DIR}/nvdaController_c.c"
    COMMAND ${CMAKE_COMMAND} "-DMIDL=${ATLAS_MIDL}" "-DCL_DIR=${ATLAS_CL_DIR}" "-DMIDL_ENV=${ATLAS_MIDL_ENV}"
      "-DSOURCE_DIR=${ATLAS_NVDA_DIR}" "-DOUTPUT_DIR=${ATLAS_NVDA_STUB_DIR}"
      -P "${ATLAS_NVDA_DIR}/generate_stub.cmake"
    DEPENDS "${ATLAS_NVDA_DIR}/nvdaController.idl" "${ATLAS_NVDA_DIR}/nvdaController.acf"
      "${ATLAS_NVDA_DIR}/generate_stub.cmake"
    COMMENT "Generating the NVDA controller client stub"
    VERBATIM)
  # Zeroed buffers come straight from VirtualAlloc; kept out of the unity
  # files so <windows.h> never reaches the DSP code.
  target_sources(Atlas PRIVATE
    src/plugin/windows_screen_reader.cpp
    src/synthesis/framework/zeroed_buffer.cpp
    "${ATLAS_NVDA_STUB_DIR}/nvdaController_c.c")
  target_include_directories(Atlas PRIVATE "${ATLAS_NVDA_STUB_DIR}")
  target_link_libraries(Atlas PRIVATE rpcrt4)

  add_custom_command(TARGET Atlas_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/Tape"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape"
      "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/Tape"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/espeak-ng-data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ATLAS_ESPEAK_DATA_ROOT}/espeak-ng-data"
      "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/espeak-ng-data" VERBATIM)
else()
  add_custom_command(TARGET Atlas_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/Tape"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Factory/Samples/Tape"
      "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/Tape"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/espeak-ng-data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ATLAS_ESPEAK_DATA_ROOT}/espeak-ng-data"
      "$<TARGET_FILE_DIR:Atlas_VST3>/../Resources/espeak-ng-data" VERBATIM)
endif()

# AtlasBench hosts Atlas builds as VST3s and times them against each other
# (bench/README.md). It does not link Atlas, so it can time any build.
option(ATLAS_BUILD_BENCH "Build AtlasBench, which compares the CPU load of Atlas builds" OFF)
if(ATLAS_BUILD_BENCH)
  juce_add_console_app(AtlasBench PRODUCT_NAME "AtlasBench")
  target_sources(AtlasBench PRIVATE bench/atlas_bench.cpp)
  target_compile_definitions(AtlasBench PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(AtlasBench PRIVATE
    juce::juce_audio_processors juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
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
    src/interface/look_and_feel
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
  add_test(NAME atlas_distortion_filter COMMAND AtlasTimingTests --distortion-filter)
  add_test(NAME atlas_filter_keytrack COMMAND AtlasTimingTests --filter-keytrack)
  add_test(NAME atlas_filter_routing COMMAND AtlasTimingTests --filter-routing)
  add_test(NAME atlas_release_velocity_glide COMMAND AtlasTimingTests --release-velocity-glide)
  add_test(NAME atlas_analog_oscillator COMMAND AtlasTimingTests --analog-oscillator)
  add_test(NAME atlas_fourier_transform COMMAND AtlasTimingTests --fourier-transform)
  add_test(NAME atlas_maim COMMAND AtlasTimingTests --maim)
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
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasPreparedEffectTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  add_test(NAME atlas_prepared_effects COMMAND AtlasPreparedEffectTests)
  add_test(NAME atlas_convolution_threading COMMAND AtlasPreparedEffectTests --convolution-threading)
  set_tests_properties(atlas_convolution_threading PROPERTIES TIMEOUT 60)
  add_test(NAME atlas_ir_switching COMMAND AtlasPreparedEffectTests --ir-switching)
  add_test(NAME atlas_drive_dynamics_modules COMMAND AtlasPreparedEffectTests --drive-dynamics
    "${CMAKE_SOURCE_DIR}/Factory/FX/Dynamics")
  add_test(NAME atlas_effect_sleep COMMAND AtlasPreparedEffectTests --effect-sleep)
  add_test(NAME atlas_voice_output_sharing COMMAND AtlasPreparedEffectTests --voice-output-sharing)
  set_tests_properties(atlas_ir_switching PROPERTIES TIMEOUT 60)
  # Reports plugin and effect chain construction time and memory, and the
  # cost of processing with light effects; `--construction chain` measures one chain.
  add_test(NAME atlas_effect_construction COMMAND AtlasPreparedEffectTests --construction)
  set_tests_properties(atlas_effect_construction PROPERTIES LABELS cpu)
  # Saving, restoring and saving again gives the same state.
  add_test(NAME atlas_state_round_trip COMMAND AtlasPreparedEffectTests --state-round-trip)
  # Preset loads, state restores and FX bus loads and saves never make the
  # host's audio thread wait on Atlas's callback lock.
  add_test(NAME atlas_callback_lock_wait COMMAND AtlasPreparedEffectTests --lock-wait)

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
  elseif(WIN32)
    # Reads a text field through UI Automation as NVDA does (--text-field-reading),
    # and listens for its focus events as NVDA does (--focus-events).
    target_sources(AtlasMacroTests PRIVATE tests/uia_text_reading_windows.cpp tests/uia_focus_events_windows.cpp)
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
  add_test(NAME atlas_editor_speech COMMAND AtlasMacroTests --editor-speech)
  add_test(NAME atlas_editor_rebuild_focus COMMAND AtlasMacroTests --editor-rebuild-focus)
  add_test(NAME atlas_editor_focus COMMAND AtlasMacroTests --editor-focus)
  # Undo and redo of values, presets, the default patch, effects, routes and LFO edits.
  add_test(NAME atlas_editor_undo COMMAND AtlasMacroTests --editor-undo)
  # The Community collection: the library's pages parse, downloads are checked
  # against its checksums, and the browser lists and fetches from a saved copy.
  add_test(NAME atlas_community_library COMMAND AtlasMacroTests --community-library)
  # The library layout (library, bank, kind, category): paths, the banks a
  # library lists, the one-time migration from the older layout, stray banks
  # adopted into Third Party, and bank archives written and imported.
  add_test(NAME atlas_library_layout COMMAND AtlasMacroTests --library-layout)
  # The system's credential store (keychain, Credential Manager): a secret
  # saved, read, replaced and removed under a service of the tests' own, with
  # keychain dialogs off. Skipped where there is no store or it is locked.
  add_test(NAME atlas_credential_store COMMAND AtlasMacroTests --credential-store)
  add_test(NAME atlas_oscillator_modulation COMMAND AtlasMacroTests --oscillator-modulation)
  add_test(NAME atlas_arpeggiator COMMAND AtlasMacroTests --arpeggiator)
  # Voice allocation with arpeggiator tails, MIDI learn, state restore and
  # what the host is told (the 2026-10-14 audit).
  add_test(NAME atlas_release_hardening COMMAND AtlasMacroTests --release-hardening)
  # A window gone idle still paints every repaint at once: JUCE's vblanks stop
  # while nothing waits to be drawn (third_party/patches/juce8-*-idle-vblank.patch).
  add_test(NAME atlas_window_repaint COMMAND AtlasMacroTests --window-repaint)
  # NVDA reads the character Backspace deletes in a text field, through UI
  # Automation (third_party/patches/juce8-uia-text-reading.patch).
  add_test(NAME atlas_text_field_reading COMMAND AtlasMacroTests --text-field-reading)
  # Each Tab raises one UI Automation focus event, the new control's
  # (third_party/patches/juce8-uia-focus-loss.patch).
  add_test(NAME atlas_focus_events COMMAND AtlasMacroTests --focus-events)
  # Settings > Modulation amount slider: the amount prompt's slider steps and
  # reads as the destination's own control and plays each amount at once;
  # Return keeps it, anything else puts back the amount the route had.
  add_test(NAME atlas_modulation_amount_slider COMMAND AtlasMacroTests --modulation-amount-slider)

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

  juce_add_console_app(AtlasMultiSampleTests PRODUCT_NAME "Atlas Multi-sample Tests")
  juce_generate_juce_header(AtlasMultiSampleTests)
  target_sources(AtlasMultiSampleTests PRIVATE tests/multisample_tests.cpp)
  target_include_directories(AtlasMultiSampleTests PRIVATE
    src/common src/common/multisample src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasMultiSampleTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasMultiSampleTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  foreach(multisample_test sfz-parser sf2-parser decentsampler-parser decentsampler-load decentsampler-controls
          pingpong-loop loop-past-end dynamic-envelope decode pitch select envelope-mode release-trigger choke cc-trigger container
          host-state bank sf2-preset-switch sf2-encoding editor)
    add_test(NAME atlas_multisample_${multisample_test} COMMAND AtlasMultiSampleTests --${multisample_test})
  endforeach()

  juce_add_console_app(AtlasBraidsTests PRODUCT_NAME "Atlas Braids Tests")
  juce_generate_juce_header(AtlasBraidsTests)
  target_sources(AtlasBraidsTests PRIVATE tests/braids_tests.cpp)
  target_include_directories(AtlasBraidsTests PRIVATE
    src/common src/common/multisample src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects src/synthesis/effects/braids src/synthesis/effects/stmlib
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasBraidsTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasBraidsTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  foreach(braids_test models pitch strike random-phase slot-modulation resampler parameters editor)
    add_test(NAME atlas_braids_${braids_test} COMMAND AtlasBraidsTests --${braids_test})
  endforeach()

  juce_add_console_app(AtlasParticlesTests PRODUCT_NAME "Atlas Particles Tests")
  juce_generate_juce_header(AtlasParticlesTests)
  target_sources(AtlasParticlesTests PRIVATE tests/particles_tests.cpp)
  target_include_directories(AtlasParticlesTests PRIVATE
    src/common src/common/multisample src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasParticlesTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasParticlesTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  foreach(particles_test engine latency modes trigger parameters editor)
    add_test(NAME atlas_particles_${particles_test} COMMAND AtlasParticlesTests --${particles_test})
  endforeach()

  juce_add_console_app(AtlasSpeechTests PRODUCT_NAME "Atlas Speech Tests")
  juce_generate_juce_header(AtlasSpeechTests)
  target_sources(AtlasSpeechTests PRIVATE tests/speech_tests.cpp)
  target_include_directories(AtlasSpeechTests PRIVATE
    src/common src/common/multisample src/common/speech src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasSpeechTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1 "ATLAS_TEST_ESPEAK_DATA=\"${ATLAS_ESPEAK_DATA_ROOT}\"")
  target_link_libraries(AtlasSpeechTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  foreach(speech_test syllables phrase pitch tuning slot-modulation split spoken voice-controls envelope voice-kill legato sequence transport parameters editor)
    add_test(NAME atlas_speech_${speech_test} COMMAND AtlasSpeechTests --${speech_test})
  endforeach()

  juce_add_console_app(AtlasOpenWurliTests PRODUCT_NAME "Atlas OpenWurli Tests")
  juce_generate_juce_header(AtlasOpenWurliTests)
  target_sources(AtlasOpenWurliTests PRIVATE tests/openwurli_tests.cpp)
  target_include_directories(AtlasOpenWurliTests PRIVATE
    src/common src/common/multisample src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects src/synthesis/effects/openwurli
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities tests/openwurli third_party)
  target_compile_definitions(AtlasOpenWurliTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasOpenWurliTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  foreach(openwurli_test oscillator pitch envelope parameters voicing amp editor self)
    add_test(NAME atlas_openwurli_${openwurli_test} COMMAND AtlasOpenWurliTests --${openwurli_test})
  endforeach()

  # Optional oracle: the original OpenWurli (Rust) renders the reference. It is
  # built with rustc alone from the opt-in third_party/openwurli and
  # third_party/melange submodules; production builds need neither.
  set(ATLAS_OPENWURLI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openwurli" CACHE PATH
      "OpenWurli checkout for the reference tests")
  set(ATLAS_MELANGE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/melange" CACHE PATH
      "Melange checkout (the revision OpenWurli pins) for the reference tests")
  if(EXISTS "${ATLAS_OPENWURLI_SOURCE}/crates/openwurli-dsp/src/lib.rs" AND
     EXISTS "${ATLAS_MELANGE_SOURCE}/crates/melange-primitives/src/lib.rs")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ATLAS_OPENWURLI_RUSTC rustc REQUIRED)
    file(GLOB ATLAS_OPENWURLI_RUST_SOURCES CONFIGURE_DEPENDS
      "${ATLAS_OPENWURLI_SOURCE}/crates/openwurli-dsp/src/*.rs"
      "${ATLAS_OPENWURLI_SOURCE}/crates/openwurli-dsp/src/*/*.rs"
      "${ATLAS_MELANGE_SOURCE}/crates/melange-primitives/src/*.rs")
    set(ATLAS_OPENWURLI_REFERENCE "${CMAKE_CURRENT_BINARY_DIR}/openwurli-reference${CMAKE_EXECUTABLE_SUFFIX}")
    add_custom_command(OUTPUT "${ATLAS_OPENWURLI_REFERENCE}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/openwurli/build_reference.py"
        "${ATLAS_OPENWURLI_SOURCE}" "${ATLAS_MELANGE_SOURCE}" "${ATLAS_OPENWURLI_REFERENCE}"
        --rustc "${ATLAS_OPENWURLI_RUSTC}"
      DEPENDS tests/openwurli/build_reference.py tests/openwurli/reference.rs ${ATLAS_OPENWURLI_RUST_SOURCES}
      VERBATIM)
    add_custom_target(AtlasOpenWurliReference DEPENDS "${ATLAS_OPENWURLI_REFERENCE}")
    add_dependencies(AtlasOpenWurliTests AtlasOpenWurliReference)
    foreach(openwurli_test voice-reference amp-reference oscillator-reference engine-reference)
      add_test(NAME atlas_openwurli_${openwurli_test}
               COMMAND AtlasOpenWurliTests --${openwurli_test} "${ATLAS_OPENWURLI_REFERENCE}")
    endforeach()
  else()
    message(STATUS "OpenWurli reference tests skipped: "
                   "git submodule update --init --checkout third_party/openwurli third_party/melange")
  endif()

  juce_add_console_app(AtlasOpenB3Tests PRODUCT_NAME "Atlas OpenB3 Tests")
  juce_generate_juce_header(AtlasOpenB3Tests)
  target_sources(AtlasOpenB3Tests PRIVATE tests/openb3_tests.cpp)
  target_include_directories(AtlasOpenB3Tests PRIVATE
    src/common src/common/multisample src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects src/synthesis/effects/openwurli
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasOpenB3Tests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasOpenB3Tests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  foreach(openb3_test self oscillator drawbars percussion envelope coherence parameters scanner whirl editor)
    add_test(NAME atlas_openb3_${openb3_test} COMMAND AtlasOpenB3Tests --${openb3_test})
  endforeach()

  # Optional oracle: OpenB3's own Beatrix engine (C, from the opt-in
  # third_party/OpenB3 submodule) renders the reference. Production builds do
  # not need it.
  set(ATLAS_OPENB3_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpenB3" CACHE PATH
      "OpenB3 checkout for the reference tests")
  set(ATLAS_BEATRIX "${ATLAS_OPENB3_SOURCE}/BeatrixCPP/Source")
  if(EXISTS "${ATLAS_BEATRIX}/tonegen/tonegen.c")
    add_executable(AtlasOpenB3Reference
      tests/openb3/reference.c
      "${ATLAS_BEATRIX}/tonegen/tonegen.c"
      "${ATLAS_BEATRIX}/vibrato/vibrato.c"
      "${ATLAS_BEATRIX}/whirl/whirl.c"
      "${ATLAS_BEATRIX}/whirl/eqcomp.c"
      "${ATLAS_BEATRIX}/global_definitions.c")
    target_include_directories(AtlasOpenB3Reference PRIVATE
      "${ATLAS_BEATRIX}" "${ATLAS_BEATRIX}/tonegen" "${ATLAS_BEATRIX}/vibrato" "${ATLAS_BEATRIX}/whirl"
      "${ATLAS_BEATRIX}/midi" "${ATLAS_BEATRIX}/config" "${ATLAS_BEATRIX}/overdrive" "${ATLAS_BEATRIX}/program"
      "${ATLAS_BEATRIX}/reverb" "${ATLAS_BEATRIX}/state" "${ATLAS_BEATRIX}/memstream"
      "${ATLAS_BEATRIX}/convolution")
    # As the Atlas port: separate multiplies and adds, IEEE semantics.
    target_compile_options(AtlasOpenB3Reference PRIVATE
      "$<$<C_COMPILER_ID:MSVC>:/fp:strict;/w>" "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math;-w>")
    if(NOT MSVC)
      target_link_libraries(AtlasOpenB3Reference PRIVATE m)
    endif()
    add_dependencies(AtlasOpenB3Tests AtlasOpenB3Reference)
    foreach(openb3_test wiring-reference gains-reference tonegen-reference scanner-reference whirl-reference
                        rotary-reference)
      add_test(NAME atlas_openb3_${openb3_test}
               COMMAND AtlasOpenB3Tests --${openb3_test} "$<TARGET_FILE:AtlasOpenB3Reference>")
    endforeach()
  else()
    message(STATUS "OpenB3 reference tests skipped: git submodule update --init --checkout third_party/OpenB3")
  endif()

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

  juce_add_console_app(AtlasGlitchDelayTests PRODUCT_NAME "Atlas Glitch Delay Tests")
  juce_generate_juce_header(AtlasGlitchDelayTests)
  # The engine suites come from Astralay with its DSP (MIT); they register
  # themselves as juce::UnitTests and run under --dsp.
  target_sources(AtlasGlitchDelayTests PRIVATE
    tests/glitch_delay_tests.cpp
    tests/glitch_delay/dsp_tests.cpp
    tests/glitch_delay/formant_scheduling_tests.cpp
    tests/glitch_delay/formant_tests.cpp
    tests/glitch_delay/freeze_sustain_tests.cpp
    tests/glitch_delay/glitch_tests.cpp
    tests/glitch_delay/reset_tests.cpp
    tests/glitch_delay/smear_tests.cpp
    tests/glitch_delay/tape_stop_tests.cpp)
  target_include_directories(AtlasGlitchDelayTests PRIVATE
    src/common src/common/multisample src/common/wavetable src/plugin src/interface/look_and_feel
    src/synthesis/framework src/synthesis/modules src/synthesis/effects
    src/synthesis/filters src/synthesis/producers src/synthesis/synth_engine
    src/synthesis/lookups src/synthesis/modulators src/synthesis/utilities third_party)
  target_compile_definitions(AtlasGlitchDelayTests PRIVATE
    VITAL_ACCESSIBLE_UI=1 HEADLESS=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DSP_USE_SHARED_FFTW=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(AtlasGlitchDelayTests PRIVATE
    Atlas juce::juce_audio_utils juce::juce_dsp juce::juce_gui_extra juce::juce_recommended_config_flags)
  foreach(glitch_delay_test dsp parameters module reproducible chain state editor)
    add_test(NAME atlas_glitch_delay_${glitch_delay_test} COMMAND AtlasGlitchDelayTests --${glitch_delay_test})
  endforeach()

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
  add_test(NAME atlas_analogue_compressor COMMAND AtlasAnalogueTests --compressor)
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
    add_executable(AtlasDriveDynamicsReferenceTests tests/drive_dynamics/reference_tests.cpp
      ${ATLAS_DRIVE_DYNAMICS_SOURCES})
    target_include_directories(AtlasDriveDynamicsReferenceTests PRIVATE src/synthesis/effects/drive_dynamics)
    set_source_files_properties(tests/drive_dynamics/reference_tests.cpp PROPERTIES
      COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")
    target_link_libraries(AtlasDriveDynamicsReferenceTests PRIVATE AtlasEelReference)
    add_test(NAME atlas_drive_dynamics_reference COMMAND AtlasDriveDynamicsReferenceTests
      "${CMAKE_SOURCE_DIR}/tests/drive_dynamics")
    # The Frequency Shifter's Mode 2 against the Serum 2 Bode JSFX, which is not
    # in the repository: the test is added once ATLAS_BODE_JSFX names the script.
    set(ATLAS_BODE_JSFX "" CACHE FILEPATH "Path to Serum2Bode.jsfx for the Frequency Shifter Mode 2 reference test")
    add_executable(AtlasBodeReferenceTests tests/bode/reference_tests.cpp ${ATLAS_BODE_SOURCES})
    target_include_directories(AtlasBodeReferenceTests PRIVATE src/synthesis/effects/bode)
    set_source_files_properties(tests/bode/reference_tests.cpp PROPERTIES
      COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off;-fno-fast-math>")
    target_link_libraries(AtlasBodeReferenceTests PRIVATE AtlasEelReference)
    if(EXISTS "${ATLAS_BODE_JSFX}")
      add_test(NAME atlas_bode_reference COMMAND AtlasBodeReferenceTests "${ATLAS_BODE_JSFX}")
    endif()
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

  # A program's main thread gets 8 MB of stack on macOS but 1 MB on Windows,
  # and some tests build large fixtures (a whole SynthPlugin, spectral
  # sources) on the stack. MSVC's frames run larger than Clang's, so give the
  # test programs 16 MB there; untouched stack costs nothing.
  if(MSVC)
    get_property(atlas_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(atlas_target IN LISTS atlas_targets)
      if(atlas_target MATCHES "^Atlas.*(Tests|Stress)$")
        target_link_options(${atlas_target} PRIVATE /STACK:16777216)
      endif()
    endforeach()
  endif()
endif()
