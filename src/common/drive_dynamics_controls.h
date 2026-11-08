// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

// The controls of Saturator, Dynamic Tube, Overdrive and Multiband Dynamics
// (src/synthesis/effects/drive_dynamics) and Destroy (src/synthesis/effects/destroy),
// in the order the editor shows them.
// Every value is stored in its script's own units and range, so the modules
// hand it over unchanged. A modulated control is a modulation destination; the
// rest (menus and switches) are base controls. step is the arrow-key step in
// display units.
namespace vital::drive_dynamics {
  struct ControlSpec {
    const char* suffix;
    float minimum, maximum;
    float step;
    bool modulated;
  };

  enum SaturatorControl {
    kSaturatorType, kSaturatorDrive, kSaturatorBassThreshold, kSaturatorWsDrive, kSaturatorWsLinear,
    kSaturatorWsCurve, kSaturatorWsDamp, kSaturatorWsPeriod, kSaturatorWsDepth, kSaturatorColor,
    kSaturatorColorLow, kSaturatorColorFrequency, kSaturatorColorWidth, kSaturatorColorHigh,
    kSaturatorPostClip, kSaturatorOutput, kSaturatorDcFilter, kSaturatorHiQuality, kSaturatorMix,
    kNumSaturatorControls
  };
  inline constexpr ControlSpec kSaturatorControls[kNumSaturatorControls] = {
    { "type", 0.0f, 7.0f, 1.0f, false },
    { "drive", -36.0f, 36.0f, 0.5f, true },
    { "bass_threshold", -50.0f, 0.0f, 0.5f, true },
    { "ws_drive", 0.0f, 100.0f, 1.0f, true },
    { "ws_linear", 0.0f, 100.0f, 1.0f, true },
    { "ws_curve", 0.0f, 100.0f, 1.0f, true },
    { "ws_damp", 0.0f, 100.0f, 1.0f, true },
    { "ws_period", 0.0f, 100.0f, 1.0f, true },
    { "ws_depth", 0.0f, 100.0f, 1.0f, true },
    { "color", 0.0f, 1.0f, 1.0f, false },
    { "color_low", -36.0f, 36.0f, 0.5f, true },
    { "color_frequency", 30.0f, 18500.0f, 10.0f, true },
    { "color_width", 0.0f, 100.0f, 1.0f, true },
    { "color_high", -24.0f, 24.0f, 0.5f, true },
    { "post_clip", 0.0f, 2.0f, 1.0f, false },
    { "output", -36.0f, 0.0f, 0.5f, true },
    { "dc_filter", 0.0f, 1.0f, 1.0f, false },
    { "hi_quality", 0.0f, 1.0f, 1.0f, false },
    { "mix", 0.0f, 100.0f, 1.0f, true },
  };
  // The Type menu's curves, in the script's order.
  enum SaturatorType { kAnalogClip, kSoftSine, kBassShaper, kMediumCurve, kHardCurve, kSinoidFold,
                       kDigitalClip, kWaveshaper };

  enum DynamicTubeControl {
    kDynamicTubeType, kDynamicTubeDrive, kDynamicTubeBias, kDynamicTubeEnvelope, kDynamicTubeAttack,
    kDynamicTubeRelease, kDynamicTubeTone, kDynamicTubeOutput, kDynamicTubeHiQuality, kDynamicTubeMix,
    kNumDynamicTubeControls
  };
  inline constexpr ControlSpec kDynamicTubeControls[kNumDynamicTubeControls] = {
    { "tube_type", 0.0f, 2.0f, 1.0f, false },
    { "drive", -15.0f, 15.0f, 0.5f, true },
    { "bias", 0.0f, 100.0f, 1.0f, true },
    { "envelope", -3.0f, 3.0f, 0.05f, true },
    { "attack", 1.0f, 400.0f, 1.0f, true },
    { "release", 1.0f, 1500.0f, 1.0f, true },
    { "tone", -1.0f, 1.0f, 1.0f, true },
    { "output", -15.0f, 15.0f, 0.5f, true },
    { "hi_quality", 0.0f, 1.0f, 1.0f, false },
    { "mix", 0.0f, 100.0f, 1.0f, true },
  };

  enum OverdriveControl {
    kOverdriveFrequency, kOverdriveWidth, kOverdriveDrive, kOverdriveTone, kOverdriveDynamics, kOverdriveMix,
    kNumOverdriveControls
  };
  inline constexpr ControlSpec kOverdriveControls[kNumOverdriveControls] = {
    { "frequency", 50.0f, 20000.0f, 10.0f, true },
    { "width", 0.5f, 9.0f, 0.1f, true },
    { "drive", 0.0f, 100.0f, 1.0f, true },
    { "tone", 0.0f, 100.0f, 1.0f, true },
    { "dynamics", 0.0f, 100.0f, 1.0f, true },
    { "mix", 0.0f, 100.0f, 1.0f, true },
  };

  // Multiband Dynamics: the global controls, then ten for each band.
  enum MultibandControl {
    kMultibandLowBand, kMultibandHighBand, kMultibandLowSplit, kMultibandHighSplit,
    kMultibandFirstBandControl,
    kMultibandDetection = kMultibandFirstBandControl + 30, kMultibandSoftKnee, kMultibandTime, kMultibandOutput,
    kMultibandAmount, kNumMultibandControls
  };
  enum MultibandBandControl {
    kBandActive, kBandSolo, kBandInput, kBandAboveThreshold, kBandAboveRatio, kBandBelowThreshold,
    kBandBelowRatio, kBandAttack, kBandRelease, kBandOutput, kNumBandControls
  };
  inline constexpr int kNumMultibandBands = 3;
  inline constexpr const char* kMultibandBandNames[kNumMultibandBands] = { "low", "mid", "high" };
  inline constexpr int multibandControl(int band, int control) {
    return kMultibandFirstBandControl + band * kNumBandControls + control;
  }
  inline constexpr ControlSpec kMultibandGlobalControls[] = {
    { "low_band", 0.0f, 1.0f, 1.0f, false },
    { "high_band", 0.0f, 1.0f, 1.0f, false },
    { "low_split", 30.0f, 3000.0f, 10.0f, true },
    { "high_split", 300.0f, 15000.0f, 10.0f, true },
  };
  // Each band's controls; the suffix follows the band's name ("low_attack").
  inline constexpr ControlSpec kMultibandBandControls[kNumBandControls] = {
    { "active", 0.0f, 1.0f, 1.0f, false },
    { "solo", 0.0f, 1.0f, 1.0f, false },
    { "input", -24.0f, 24.0f, 0.5f, true },
    { "above_threshold", -80.0f, 0.0f, 0.5f, true },
    { "above_ratio", -1.0f, 1.0f, 0.01f, true },
    { "below_threshold", -80.0f, 0.0f, 0.5f, true },
    { "below_ratio", -3.0f, 1.0f, 0.01f, true },
    { "attack", 0.1f, 5000.0f, 1.0f, true },
    { "release", 0.1f, 5000.0f, 1.0f, true },
    { "output", -24.0f, 24.0f, 0.5f, true },
  };
  inline constexpr ControlSpec kMultibandTailControls[] = {
    { "detection", 0.0f, 1.0f, 1.0f, false },
    { "soft_knee", 0.0f, 1.0f, 1.0f, false },
    { "time", 10.0f, 1000.0f, 1.0f, true },
    { "output", -24.0f, 24.0f, 0.5f, true },
    { "amount", 0.0f, 100.0f, 1.0f, true },
  };

  inline const ControlSpec& multibandSpec(int control) {
    if (control < kMultibandFirstBandControl) return kMultibandGlobalControls[control];
    if (control >= kMultibandDetection) return kMultibandTailControls[control - kMultibandDetection];
    return kMultibandBandControls[(control - kMultibandFirstBandControl) % kNumBandControls];
  }
  inline std::string multibandSuffix(int control) {
    if (control < kMultibandFirstBandControl || control >= kMultibandDetection)
      return multibandSpec(control).suffix;
    const int band = (control - kMultibandFirstBandControl) / kNumBandControls;
    return std::string(kMultibandBandNames[band]) + "_" + multibandSpec(control).suffix;
  }

  // Destroy (the Serum 2 Overdrive JSFX): Filter Frequency and Level hold the
  // script's 0 to 1 sliders and read in Hz and dB, as its own display does.
  enum DestroyControl {
    kDestroyDrive, kDestroyStack, kDestroyFilter, kDestroyFilterFrequency, kDestroyFilterQ, kDestroyFilterMorph,
    kDestroyMix, kDestroyLevel, kNumDestroyControls
  };
  inline constexpr ControlSpec kDestroyControls[kNumDestroyControls] = {
    { "drive", 0.0f, 100.0f, 1.0f, true },
    { "stack", 1.0f, 16.0f, 1.0f, false },
    { "filter", 0.0f, 2.0f, 1.0f, false },
    { "filter_frequency", 0.0f, 1.0f, 0.01f, true },
    { "filter_q", 0.05f, 7.6f, 0.05f, true },
    { "filter_morph", 0.0f, 100.0f, 1.0f, true },
    { "mix", 0.0f, 100.0f, 1.0f, true },
    { "level", 0.0f, 1.0f, 0.01f, true },
  };
  // Whether a parameter is Destroy's control with this suffix, in a slot or a bus.
  inline bool destroyParameterIs(const std::string& name, const std::string& suffix) {
    const std::string id = "destroy_" + suffix;
    return name == id || (name.size() > id.size() &&
        name.compare(name.size() - id.size() - 1, id.size() + 1, "_" + id) == 0);
  }
  // The filter's cutoff in Hz, as the script shows it: 8.1758 Hz times 2697 to the slider.
  inline double destroyFrequencyHz(double value) { return 8.1758 * std::pow(2697.0, value); }
  inline double destroyFrequencyValue(double hz) {
    return std::log(std::max(hz, 1.0e-6) / 8.1758) / std::log(2697.0);
  }
  // The output level in dB: (2 Level)^2 in amplitude. The script shows
  // minus infinity at Level 0.0005 and below, where destroyLevelSilent holds.
  inline bool destroyLevelSilent(double value) { return value <= 0.0005; }
  inline double destroyLevelDb(double value) { return 40.0 * std::log10(2.0 * std::max(value, 0.0005)); }
  inline double destroyLevelValue(double db) { return 0.5 * std::pow(10.0, db / 40.0); }

  // A band's ratio as the script's own display reads it: output dB per input
  // dB above (Above Ratio, 1 + value) or below (Below Ratio, 1 - value) the
  // threshold, shown "1 : x"; returns x, or 0 for infinity.
  inline double multibandRatioDenominator(double value, bool above) {
    const double slope = above ? 1 + value : 1 - value;
    return slope < 0.0005 ? 0.0 : 1 / slope;
  }
}
