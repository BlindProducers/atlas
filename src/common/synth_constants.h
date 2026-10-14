/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "value.h"

#include <string>

namespace vital {

  constexpr int kNumLfos = 12;
  constexpr int kNumOscillators = 8;
  constexpr int kNumArpeggiators = 8;
  constexpr int kNumOscillatorWaveFrames = 257;
  constexpr int kNumEnvelopes = 10;
  constexpr int kNumRandomLfos = 4;
  constexpr int kNumMacros = 16;
  constexpr int kNumFilters = 2;
  constexpr int kNumBuses = 3;
  constexpr int kNumFormants = 4;
  constexpr int kNumChannels = 2;
  constexpr int kMaxPolyphony = 33;
  constexpr int kMaxActivePolyphony = 32;
  constexpr int kLfoDataResolution = 2048;
  constexpr int kMaxModulationConnections = 64;

  constexpr int kOscilloscopeMemorySampleRate = 22000;
  constexpr int kOscilloscopeMemoryResolution = 512;
  constexpr int kAudioMemorySamples = 1 << 15;
  constexpr int kDefaultWindowWidth = 1400;
  constexpr int kDefaultWindowHeight = 820;
  constexpr int kMinWindowWidth = 350;
  constexpr int kMinWindowHeight = 205;

  constexpr int kDefaultKeyboardOffset = 48;
  constexpr wchar_t kDefaultKeyboardOctaveUp = 'x';
  constexpr wchar_t kDefaultKeyboardOctaveDown = 'z';
  const std::wstring kDefaultKeyboard = L"awsedftgyhujkolp;'";

  const std::string kPresetExtension = "atp";
  const std::string kLegacyPresetExtension = "vital";
  const std::string kPresetExtensionsList = "*." + vital::kPresetExtension + ";*." + vital::kLegacyPresetExtension;
  const std::string kWavetableExtension = "att";
  const std::string kLegacyWavetableExtension = "vitaltable";
  const std::string kWavetableExtensionsList = "*." + vital::kWavetableExtension + ";*." + vital::kLegacyWavetableExtension + ";*.wav;*.flac";
  const std::string kSampleExtensionsList = "*.wav;*.flac;*.aif;*.aiff";
  const std::string kSkinExtension = "vitalskin";
  const std::string kLfoExtension = "ats";
  const std::string kLegacyLfoExtension = "vitallfo";
  const std::string kLfoExtensionsList = "*." + vital::kLfoExtension + ";*." + vital::kLegacyLfoExtension;
  const std::string kBankExtension = "atb";
  const std::string kLegacyBankExtension = "vitalbank";
  const std::string kBankExtensionsList = "*." + vital::kBankExtension + ";*." + vital::kLegacyBankExtension;
  const std::string kFxExtension = "atf";
  const std::string kFxBusExtension = "atfx";
  const std::string kIrExtension = "atpir";
  const std::string kIrExtensionsList = "*." + vital::kIrExtension + ";*.wav;*.flac;*.aif;*.aiff";

  namespace constants {
    enum SourceDestination {
      kFilter1,
      kFilter2,
      kDualFilters,
      kEffects,
      kBus1,
      kBus2,
      kDirectOut,
      kBus3,
      kNumSourceDestinations
    };

    // Keep SourceDestination's numeric values frozen for preset compatibility.
    // This table is the user-facing order used by menus and accessible controls.
    inline constexpr int kSourceDestinationPresentationOrder[kNumSourceDestinations] = {
      kFilter1,
      kFilter2,
      kDualFilters,
      kEffects,
      kBus1,
      kBus2,
      kBus3,
      kDirectOut,
    };

    static int sourceDestinationPresentationIndex(int destination) {
      for (int i = 0; i < kNumSourceDestinations; ++i) {
        if (kSourceDestinationPresentationOrder[i] == destination)
          return i;
      }
      return 0;
    }

    static int shiftSourceDestination(int destination, int shift) {
      const int index = sourceDestinationPresentationIndex(destination);
      const int next = (index + shift % kNumSourceDestinations + kNumSourceDestinations) %
                       kNumSourceDestinations;
      return kSourceDestinationPresentationOrder[next];
    }

    static int toggleFilter1(int current_destination, bool on) {
      if (on) {
        if (current_destination == vital::constants::kFilter2)
          return vital::constants::kDualFilters;
        else
          return vital::constants::kFilter1;
      }
      else if (current_destination == vital::constants::kDualFilters)
        return vital::constants::kFilter2;
      else if (current_destination == vital::constants::kFilter1)
        return vital::constants::kEffects;

      return current_destination;
    }

    static int toggleFilter2(int current_destination, bool on) {
      if (on) {
        if (current_destination == vital::constants::kFilter1)
          return vital::constants::kDualFilters;
        else
          return vital::constants::kFilter2;
      }
      else if (current_destination == vital::constants::kDualFilters)
        return vital::constants::kFilter1;
      else if (current_destination == vital::constants::kFilter2)
        return vital::constants::kEffects;

      return current_destination;
    }

    enum Effect {
      kChorus,
      kCompressor,
      kDelay,
      kDistortion,
      kEq,
      kFilterFx,
      kFlanger,
      kPhaser,
      kReverb,
      kUtility,
      kPhaseShift,
      kFrequencyShifter,
      kLimiter,
      kDimensionExpander,
      kWaveShaper,
      kConvolutionReverb,
      kMidSideEq,
      kSpectralGate,
      kErosion,
      kFloatyDelay,
      kSpectralCompressor,
      kScream,
      kTremoloPan,
      kTape,
      kDefuse,
      kSuppressor,
      kTranceGate,
      kSpectralLock,
      kResonator,
      kAnalogueChorus,
      kAnalogueFlanger,
      kAnalogueEcho,
      kAnalogueRotary,
      kAnalogueSpring,
      kAnalogueWah,
      kSpectralBalance,
      kGlitchDelay,
      kOpenWurliAmp,
      kScannerVibrato,
      kNumEffects
    };

    constexpr int kNumReorderableEffects = kUtility;
    constexpr int kMaxEffectSlots = 20;
    constexpr int kNumDestinations = kNumSourceDestinations + kNumEffects;

    enum BusDestination {
      kBusDestinationMain,
      kBusDestinationBus1,
      kBusDestinationBus2,
      kBusDestinationBus3,
      kBusDestinationDirectOut,
      kNumBusDestinations
    };

    enum FilterModel {
      kAnalog,
      kDirty,
      kLadder,
      kDigital,
      kDiode,
      kFormant,
      kComb,
      kPhase,
      kReverbFilter,
      kScreamFilter,
      kRingModFilter,
      kSampleHoldFilter,
      kFilterTable,
      kNumFilterModels
    };

    enum RetriggerStyle {
      kFree,
      kRetrigger,
      kSyncToPlayHead,
      kNumRetriggerStyles,
    };

    constexpr int kNumSyncedFrequencyRatios = 13;
    constexpr vital::mono_float kSyncedFrequencyRatios[kNumSyncedFrequencyRatios] = {
      0.0f,
      1.0f / 128.0f,
      1.0f / 64.0f,
      1.0f / 32.0f,
      1.0f / 16.0f,
      1.0f / 8.0f,
      1.0f / 4.0f,
      1.0f / 2.0f,
      1.0f,
      2.0f,
      4.0f,
      8.0f,
      16.0f
    };

    const poly_float kLeftOne(1.0f, 0.0f);
    const poly_float kRightOne(0.0f, 1.0f);
    const poly_float kFirstVoiceOne(1.0f, 1.0f, 0.0f, 0.0f);
    const poly_float kSecondVoiceOne(0.0f, 0.0f, 1.0f, 1.0f);
    const poly_float kStereoSplit = kLeftOne - kRightOne;
    const poly_float kPolySqrt2 = kSqrt2;
    const poly_mask kFullMask = poly_float::equal(0.0f, 0.0f);
    const poly_mask kLeftMask = poly_float::equal(kLeftOne, 1.0f);
    const poly_mask kRightMask = poly_float::equal(kRightOne, 1.0f);
    const poly_mask kFirstMask = poly_float::equal(kFirstVoiceOne, 1.0f);
    const poly_mask kSecondMask = poly_float::equal(kSecondVoiceOne, 1.0f);

    const cr::Value kValueZero(0.0f);
    const cr::Value kValueOne(1.0f);
    const cr::Value kValueTwo(2.0f);
    const cr::Value kValueHalf(0.5f);
    const cr::Value kValueFifth(0.2f);
    const cr::Value kValueTenth(0.1f);
    const cr::Value kValuePi(kPi);
    const cr::Value kValue2Pi(2.0f * kPi);
    const cr::Value kValueSqrt2(kSqrt2);
    const cr::Value kValueNegOne(-1.0f);
  } // namespace constants
} // namespace vital
