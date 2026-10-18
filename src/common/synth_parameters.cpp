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

#include "synth_parameters.h"
#include "source_types.h"
#include "braids_engine.h"
#include "speech_voices.h"

#include "compressor.h"
#include "distortion.h"
#include "digital_svf.h"
#include "synth_constants.h"
#include "random_lfo.h"
#include "noise_source.h"
#include "granular_source.h"
#include "spectral_source.h"
#include "synth_lfo.h"
#include "synth_oscillator.h"
#include "synth_strings.h"
#include "tape_controls.h"
#include "voice_handler.h"
#include "wavetable.h"
#include "utils.h"

#include <algorithm>
#include <cfloat>

namespace vital {

  bool compareValueDetails(const ValueDetails* a, const ValueDetails* b) {
    if (a->version_added != b->version_added)
      return a->version_added < b->version_added;
    
    return a->name.compare(b->name) < 0;
  }

  using namespace constants;
  static const std::string kIdDelimiter = "_";
  static const std::string kEnvIdPrefix = "env";
  static const std::string kLfoIdPrefix = "lfo";
  static const std::string kRandomIdPrefix = "random";
  static const std::string kOscIdPrefix = "osc";
  static const std::string kFilterIdPrefix = "filter";
  static const std::string kModulationIdPrefix = "modulation";
  static const std::string kNameDelimiter = " ";
  static const std::string kEnvNamePrefix = "Envelope";
  static const std::string kLfoNamePrefix = "LFO";
  static const std::string kRandomNamePrefix = "Random LFO";
  static const std::string kOscNamePrefix = "Oscillator";
  static const std::string kFilterNamePrefix = "Filter";
  static const std::string kModulationNamePrefix = "Modulation";

  const ValueDetails ValueDetailsLookup::parameter_list[] = {
    { "bypass", 0x000702, 0.0, 1.0, 0.0, 0.0, 60.0,
      ValueDetails::kIndexed, false, "", "Bypass", nullptr },
    { "beats_per_minute", 0x000000, 0.333333333, 5.0, 2.0, 0.0, 60.0,
      ValueDetails::kLinear, false, "", "Beats Per Minute", nullptr },
    { "arp_1_on", 0x020000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Switch", strings::kOffOnNames },
    { "arp_1_time_mode", 0x020000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Time Mode", strings::kArpTimeModeNames },
    { "arp_1_sync_rate", 0x020000, 0.0, 22.0, 10.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Sync Rate", strings::kArpSyncRateNames },
    { "arp_1_free_rate", 0x020000, 0.05, 100.0, 5.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Arpeggiator 1 Free Rate", nullptr },
    { "arp_1_style", 0x020000, 0.0, 16.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Style", strings::kArpStyleNames },
    { "arp_1_gate", 0x020000, 1.0, 100.0, 80.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 1 Gate", nullptr },
    { "arp_1_random_gate", 0x020000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 1 Random Gate", nullptr },
    { "arp_1_repeats", 0x020000, 1.0, 16.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Repeats", nullptr },
    { "arp_1_offset", 0x020000, 0.0, 31.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Offset", nullptr },
    { "arp_1_distance", 0x020000, -24.0, 24.0, 12.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " semitones", "Arpeggiator 1 Distance", nullptr },
    { "arp_1_steps", 0x020000, 1.0, 8.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Steps", nullptr },
    { "arp_1_velocity", 0x020000, 1.0, 127.0, 100.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Velocity", nullptr },
    { "arp_1_velocity_decay", 0x020000, 0.0, 100.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 1 Velocity Decay", nullptr },
    { "arp_1_random_velocity", 0x020000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 1 Random Velocity", nullptr },
    { "arp_1_hold", 0x020000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Hold", strings::kOffOnNames },
    { "arp_1_retrigger", 0x020000, 0.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Retrigger", strings::kArpRetriggerNames },
    { "arp_1_channel", 0x020000, 0.0, 16.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Output Channel", strings::kArpChannelNames },
    { "arp_1_destination_osc_1", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 1", strings::kOffOnNames },
    { "arp_1_destination_osc_2", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 2", strings::kOffOnNames },
    { "arp_1_destination_osc_3", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 3", strings::kOffOnNames },
    { "arp_1_destination_osc_4", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 4", strings::kOffOnNames },
    { "arp_1_destination_sample", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Sample", strings::kOffOnNames },
    { "arp_1_destination_granular", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Granular", strings::kOffOnNames },
    { "arp_1_destination_spectral", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Spectral", strings::kOffOnNames },
    { "arp_1_destination_noise", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Noise", strings::kOffOnNames },
    { "arp_2_on", 0x020000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Switch", strings::kOffOnNames },
    { "arp_2_time_mode", 0x020000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Time Mode", strings::kArpTimeModeNames },
    { "arp_2_sync_rate", 0x020000, 0.0, 22.0, 10.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Sync Rate", strings::kArpSyncRateNames },
    { "arp_2_free_rate", 0x020000, 0.05, 100.0, 5.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Arpeggiator 2 Free Rate", nullptr },
    { "arp_2_style", 0x020000, 0.0, 16.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Style", strings::kArpStyleNames },
    { "arp_2_gate", 0x020000, 1.0, 100.0, 80.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 2 Gate", nullptr },
    { "arp_2_random_gate", 0x020000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 2 Random Gate", nullptr },
    { "arp_2_repeats", 0x020000, 1.0, 16.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Repeats", nullptr },
    { "arp_2_offset", 0x020000, 0.0, 31.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Offset", nullptr },
    { "arp_2_distance", 0x020000, -24.0, 24.0, 12.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " semitones", "Arpeggiator 2 Distance", nullptr },
    { "arp_2_steps", 0x020000, 1.0, 8.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Steps", nullptr },
    { "arp_2_velocity", 0x020000, 1.0, 127.0, 100.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Velocity", nullptr },
    { "arp_2_velocity_decay", 0x020000, 0.0, 100.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 2 Velocity Decay", nullptr },
    { "arp_2_random_velocity", 0x020000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Arpeggiator 2 Random Velocity", nullptr },
    { "arp_2_hold", 0x020000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Hold", strings::kOffOnNames },
    { "arp_2_retrigger", 0x020000, 0.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Retrigger", strings::kArpRetriggerNames },
    { "arp_2_channel", 0x020000, 0.0, 16.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Output Channel", strings::kArpChannelNames },
    { "arp_2_destination_osc_1", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 1", strings::kOffOnNames },
    { "arp_2_destination_osc_2", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 2", strings::kOffOnNames },
    { "arp_2_destination_osc_3", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 3", strings::kOffOnNames },
    { "arp_2_destination_osc_4", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 4", strings::kOffOnNames },
    { "arp_2_destination_sample", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Sample", strings::kOffOnNames },
    { "arp_2_destination_granular", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Granular", strings::kOffOnNames },
    { "arp_2_destination_spectral", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Spectral", strings::kOffOnNames },
    { "arp_2_destination_noise", 0x020000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Noise", strings::kOffOnNames },
    { "delay_dry_wet", 0x000000, 0.0, 1.0, 0.3334, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Delay Mix", nullptr },
    { "delay_feedback", 0x000000, -1.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Delay Feedback", nullptr },
    { "delay_frequency", 0x000000, -2.0, 9.0, 2.0, 0.0, 1.0,
      ValueDetails::kExponential, true, " secs", "Delay Frequency", nullptr },
    { "delay_aux_frequency", 0x000507, -2.0, 9.0, 2.0, 0.0, 1.0,
      ValueDetails::kExponential, true, " secs", "Delay Frequency 2", nullptr },
    { "delay_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Delay Switch", strings::kOffOnNames },
    { "delay_style", 0x000000, 0.0, 4.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Delay Style", strings::kDelayStyleNames },
    { "delay_filter_cutoff", 0x000000, 8.0, 136.0, 60.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Delay Filter Cutoff", nullptr },
    { "delay_filter_spread", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Delay Filter Spread", nullptr },
    { "delay_sync", 0x000000, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Delay Sync", strings::kFrequencySyncNames },
    { "delay_tempo", 0x000000, 4.0, 12.0, 9.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Delay Tempo", strings::kSyncedFrequencyNames },
    { "delay_aux_sync", 0x000507, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Delay Sync 2", strings::kFrequencySyncNames },
    { "delay_aux_tempo", 0x000507, 4.0, 12.0, 9.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Delay Tempo 2", strings::kSyncedFrequencyNames },
    { "distortion_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Distortion Switch", strings::kOffOnNames },
    { "distortion_type", 0x000000, 0.0, 5.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Distortion Type", strings::kDistortionTypeNames },
    { "distortion_drive", 0x000000, Distortion::kMinDrive, Distortion::kMaxDrive, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Distortion Drive", nullptr },
    { "distortion_mix", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Distortion Mix", nullptr },
    { "distortion_filter_order", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Distortion Filter Order", strings::kDistortionFilterOrderNames },
    { "distortion_filter_cutoff", 0x000000, 8.0, 136.0, 80.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Distortion Filter Cutoff", nullptr },
    { "distortion_filter_resonance", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Distortion Filter Resonance", nullptr },
    { "distortion_filter_blend", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Distortion Filter Blend", nullptr },
    { "frequency_shifter_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Frequency Shifter Switch", strings::kOffOnNames },
    { "frequency_shifter_shift", 0x000000, -5000.0, 5000.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Frequency Shifter Shift", nullptr },
    { "frequency_shifter_feedback", 0x000000, -0.95, 0.95, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Frequency Shifter Feedback", nullptr },
    { "frequency_shifter_delay", 0x020100, 0.0, 50.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " ms", "Frequency Shifter Delay", nullptr },
    { "frequency_shifter_dry_wet", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Frequency Shifter Mix", nullptr },
    { "legato", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Legato", strings::kOffOnNames },
    { "limiter_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Limiter Switch", strings::kOffOnNames },
    { "limiter_gain", 0x000000, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Limiter Gain", nullptr },
    { "limiter_ceiling", 0x000000, -24.0, 0.0, -0.1, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Limiter Ceiling", nullptr },
    { "limiter_release", 0x000000, 10.0, 1000.0, 100.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " ms", "Limiter Release", nullptr },
    { "limiter_dry_wet", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Limiter Mix", nullptr },
    { "utility_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Utility Switch", strings::kOffOnNames },
    { "utility_input_gain", 0x000000, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Utility Input Gain", nullptr },
    { "utility_width", 0x000000, 0.0, 2.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Utility Width", nullptr },
    { "utility_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Utility Low Cut", nullptr },
    { "utility_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Utility High Cut", nullptr },
    { "utility_filter_slope", 0x000000, 0.0, 7.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Utility Filter Slope", strings::kCutFilterSlopeNames },
    { "utility_output_gain", 0x000000, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Utility Output Gain", nullptr },
    { "phase_shift_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Phase Shift Switch", strings::kOffOnNames },
    { "phase_shift_amount", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Phase Shift Amount", nullptr },
    { "phase_shift_tone", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Phase Shift Tone", nullptr },
    { "phase_shift_dry_wet", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Phase Shift Mix", nullptr },
    { "dimension_expander_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Dimension Expander Switch", strings::kOffOnNames },
    { "dimension_expander_amount", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Dimension Expander Amount", nullptr },
    { "dimension_expander_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Dimension Expander Low Cut", nullptr },
    { "dimension_expander_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Dimension Expander High Cut", nullptr },
    { "waveshaper_on", 0x020100, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Waveshaper Switch", strings::kOffOnNames },
    { "waveshaper_shape", 0x020100, 0.0, 6.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Waveshaper Shape", strings::kWaveShaperShapeNames },
    { "waveshaper_drive", 0x020100, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Waveshaper Drive", nullptr },
    { "waveshaper_tension", 0x020100, -100.0, 100.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Waveshaper Tension", nullptr },
    { "waveshaper_warp_type", 0x020100, 0.0, 6.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Waveshaper Warp Type", strings::kWaveShaperWarpTypeNames },
    { "waveshaper_warp_amount", 0x020100, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Waveshaper Warp Amount", nullptr },
    { "waveshaper_mode", 0x020100, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Waveshaper Mode", strings::kWaveShaperModeNames },
    { "waveshaper_remove_dc", 0x020100, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Waveshaper Remove DC", strings::kOffOnNames },
    { "waveshaper_dry_wet", 0x020100, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Waveshaper Mix", nullptr },
    { "waveshaper_output_gain", 0x020100, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Waveshaper Output Gain", nullptr },
    { "convolution_reverb_on", 0x020100, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Convolution Reverb Switch", strings::kOffOnNames },
    { "convolution_reverb_dry_wet", 0x020100, 0.0, 1.0, 0.35, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb Mix", nullptr },
    { "convolution_reverb_ir_gain", 0x020100, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Convolution Reverb IR Gain", nullptr },
    { "convolution_reverb_width", 0x020100, 0.0, 2.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb Width", nullptr },
    { "convolution_reverb_low_cutoff", 0x020100, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Convolution Reverb Low Cut", nullptr },
    { "convolution_reverb_high_cutoff", 0x020100, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Convolution Reverb High Cut", nullptr },
    { "convolution_reverb_filter_slope", 0x020100, 0.0, 7.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Convolution Reverb Filter Slope", strings::kConvolutionSlopeNames },
    { "convolution_reverb_ir_start", 0x020100, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb IR Start", nullptr },
    { "convolution_reverb_ir_end", 0x020100, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb IR End", nullptr },
    { "convolution_reverb_ir_size", 0x020100, 0.1, 4.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb IR Size", nullptr },
    { "convolution_reverb_ir_decay", 0x020100, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb IR Decay", nullptr },
    { "convolution_reverb_ir_dampening", 0x020100, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb IR Dampening", nullptr },
    { "convolution_reverb_ir_reverse", 0x020100, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Convolution Reverb IR Reverse", strings::kOffOnNames },
    { "convolution_reverb_predelay_sync", 0x020118, 0.0, 3.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Convolution Reverb Predelay Sync", strings::kConvolutionPredelaySyncNames },
    { "convolution_reverb_predelay", 0x020118, 0.0, 0.5, 0.0, 0.0, 1000.0,
      ValueDetails::kLinear, false, " ms", "Convolution Reverb Predelay", nullptr },
    { "convolution_reverb_predelay_tempo", 0x020118, 6.0, 12.0, 10.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Convolution Reverb Predelay Tempo", strings::kSyncedFrequencyNames },
    { "convolution_reverb_fade_in", 0x020118, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb Fade In", nullptr },
    { "convolution_reverb_fade_out", 0x020118, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb Fade Out", nullptr },
    { "convolution_reverb_tone", 0x020118, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Convolution Reverb Tone", nullptr },
    { "mid_side_eq_on", 0x020100, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Mid Side EQ Switch", strings::kOffOnNames },
    // Use a later parameter version so sorting preserves all existing host IDs.
    { "defuse_on", 0x020108, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Defuse Switch", strings::kOffOnNames },
    { "defuse_stages", 0x020108, 0.0, 512.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Defuse Stages", nullptr },
    { "defuse_frequency", 0x020108, 20.0, 20000.0, 200.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Defuse Frequency", nullptr },
    { "defuse_resonance", 0x020108, 0.01, 30.0, 0.5, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Defuse Resonance", nullptr },
    { "defuse_spread", 0x020108, -5.0, 5.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " octaves", "Defuse Spread", nullptr },
    { "defuse_spread_style", 0x020108, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Defuse Spread Style", strings::kDefuseSpreadStyleNames },
    { "defuse_automation_precision", 0x020108, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Defuse Automation Precision", nullptr },

#include "tape_parameters.inc"

    // Tremolo Pan timing follows the shared Atlas tempo chooser.
#include "trance_gate_parameters.inc"
    { "tremolo_pan_on", 0x020106, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Tremolo Pan Switch", strings::kOffOnNames },
    { "tremolo_pan_sync", 0x020106, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Tremolo Pan Time Mode", strings::kTremoloPanSyncNames },
    { "tremolo_pan_frequency", 0x020106, 0.01, 20.0, 2.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Tremolo Pan Free Rate", nullptr },
    { "tremolo_pan_tempo", 0x020106, 0.0, 12.0, 8.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Tremolo Pan Sync Rate", strings::kSyncedFrequencyNames },
    { "tremolo_pan_shape", 0x020106, 0.0, 4.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Tremolo Pan Shape", strings::kTremoloPanShapeNames },
    { "tremolo_pan_stereo_phase", 0x020106, 0.0, 1.0, 0.5, 0.0, 360.0,
      ValueDetails::kLinear, false, " degrees", "Tremolo Pan Stereo Phase", nullptr },
    { "tremolo_pan_offset", 0x020106, 0.0, 1.0, 0.0, 0.0, 360.0,
      ValueDetails::kLinear, false, " degrees", "Tremolo Pan Offset", nullptr },
    { "tremolo_pan_depth", 0x020106, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Tremolo Pan Depth", nullptr },
    { "tremolo_pan_smoothing", 0x020106, 0.0, 20.0, 2.0, 0.0, 0.001,
      ValueDetails::kLinear, false, " secs", "Tremolo Pan Smoothing", nullptr },
    { "tremolo_pan_mix", 0x020106, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Tremolo Pan Mix", nullptr },

    { "scream_on", 0x020105, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Scream Switch", strings::kOffOnNames },
    { "scream_cutoff", 0x020105, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "Hz", "Scream Cutoff", nullptr },
    { "scream_scream", 0x020105, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Scream Scream", nullptr },
    { "scream_resonance", 0x020105, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Scream Resonance", nullptr },
    { "scream_input_gain", 0x020105, 0.0, 1.0, 0.75, -72.0, 96.0,
      ValueDetails::kLinear, false, "dB", "Scream Input Gain", nullptr },
    { "scream_wet", 0x020105, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Scream Wet", nullptr },
    { "scream_output_gain", 0x020105, 0.0, 1.0, 0.75, -24.0, 24.0,
      ValueDetails::kLinear, false, "dB", "Scream Output Gain", nullptr },
    { "scream_gate_release", 0x020105, 5.0, 500.0, 30.0, 0.0, 0.001,
      ValueDetails::kLinear, false, "secs", "Scream Gate Release", nullptr },
    { "scream_autogain", 0x020105, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Scream Autogain", strings::kOffOnNames },
    { "scream_keytracking", 0x020105, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Scream MIDI Keytracking", strings::kOffOnNames },

    { "spectral_lock_on", 0x02010b, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Lock On", strings::kOffOnNames },
    { "spectral_lock_amount", 0x02010b, 0.0, 2.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Amount", nullptr },
    { "spectral_lock_isolate", 0x02010b, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Isolate", nullptr },
    { "spectral_lock_shimmer", 0x02010b, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Shimmer", nullptr },
    { "spectral_lock_bend", 0x02010b, -1.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Spectral Lock Bend", nullptr },
    { "spectral_lock_bend_range", 0x02010b, 1.0, 24.0, 2.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "st", "Spectral Lock Bend Range", nullptr },
    { "spectral_lock_range_low", 0x02010b, 60.0, 4800.0, 65.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "Hz", "Spectral Lock Range Low", nullptr },
    { "spectral_lock_range_high", 0x02010b, 60.0, 4800.0, 2200.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "Hz", "Spectral Lock Range High", nullptr },
    { "spectral_lock_mute_low", 0x02010b, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Lock Mute Low", strings::kOffOnNames },
    { "spectral_lock_mute_high", 0x02010b, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Lock Mute High", strings::kOffOnNames },
    { "spectral_lock_spray", 0x02010b, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Spray", nullptr },
    { "spectral_lock_tilt", 0x02010b, -1.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Spectral Lock Tilt", nullptr },
    { "spectral_lock_pitch", 0x02010b, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "st", "Spectral Lock Pitch", nullptr },
    { "spectral_lock_root", 0x02010b, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Lock Root", strings::kSpectralLockRootNames },
    { "spectral_lock_scale", 0x02010b, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Lock Scale", strings::kSpectralLockScaleNames },
    { "spectral_lock_mix", 0x02010b, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Mix", nullptr },
    { "spectral_lock_width", 0x020115, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Width", nullptr },
    { "spectral_lock_follow", 0x020115, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Follow", nullptr },
    { "spectral_lock_mode", 0x020116, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Lock Mode", strings::kSpectralLockModeNames },
    { "spectral_lock_transients", 0x020116, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Lock Transients", nullptr },
    { "spectral_lock_reach", 0x020116, 1.0, 12.0, 3.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "st", "Spectral Lock Reach", nullptr },

    { "effect_chain_slot_28", 0x02010b, 0.0, constants::kNumEffects - 1, constants::kSpectralLock, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Slot 28", nullptr },

    // Resonator: a later version keeps every existing host parameter ID in place.
    // Tune, Decay and the rates are stored as log2 values for even keyboard steps.
    { "resonator_on", 0x020112, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator Switch", strings::kOffOnNames },
    { "resonator_model", 0x020112, 0.0, 6.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator Model", strings::kResonatorModelNames },
    { "resonator_quality", 0x020112, 0.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator Quality", strings::kResonatorQualityNames },
    { "resonator_decay", 0x020112, -5.643856, 4.906891, 0.263034, 0.0, 1.0,
      ValueDetails::kExponential, false, " secs", "Resonator Decay", nullptr },
    { "resonator_material", 0x020112, -1.0, 1.0, -0.2, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Material", nullptr },
    { "resonator_brightness", 0x020112, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Brightness", nullptr },
    { "resonator_inharmonics", 0x020112, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Inharmonics", nullptr },
    { "resonator_ratio", 0x020112, 0.25, 4.0, 1.3, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Resonator Ratio", nullptr },
    { "resonator_radius", 0x020112, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Radius", nullptr },
    { "resonator_opening", 0x020112, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Opening", nullptr },
    { "resonator_hit", 0x020112, 0.0, 1.0, 0.35, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Hit Position", nullptr },
    { "resonator_listen_left", 0x020112, 0.0, 1.0, 0.25, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Listen Left", nullptr },
    { "resonator_listen_right", 0x020112, 0.0, 1.0, 0.35, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Listen Right", nullptr },
    { "resonator_midi_pitch", 0x020112, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator MIDI Pitch", strings::kOffOnNames },
    { "resonator_tune", 0x020112, 4.321928, 12.287712, 7.03136, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Resonator Tune", nullptr },
    { "resonator_transpose", 0x020112, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "st", "Resonator Transpose", nullptr },
    { "resonator_fine", 0x020112, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, " cents", "Resonator Fine Tune", nullptr },
    { "resonator_spread", 0x020112, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, " cents", "Resonator Spread", nullptr },
    { "resonator_glide", 0x020112, 0.0, 500.0, 10.0, 0.0, 0.001,
      ValueDetails::kLinear, false, " secs", "Resonator Glide", nullptr },
    { "resonator_note_priority", 0x020112, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator Note Priority", strings::kResonatorNotePriorityNames },
    { "resonator_off_decay", 0x020112, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Off Decay", nullptr },
    { "resonator_lfo_amount", 0x020112, 0.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "st", "Resonator LFO Amount", nullptr },
    { "resonator_lfo_shape", 0x020112, 0.0, 6.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator LFO Shape", strings::kResonatorLfoShapeNames },
    { "resonator_lfo_sync", 0x020112, 0.0, 3.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator LFO Time Mode", strings::kTremoloPanSyncNames },
    { "resonator_lfo_frequency", 0x020112, -6.643856, 4.321928, -1.0, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Resonator LFO Free Rate", nullptr },
    { "resonator_lfo_tempo", 0x020112, 0.0, 12.0, 8.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator LFO Sync Rate", strings::kSyncedFrequencyNames },
    { "resonator_lfo_stereo_phase", 0x020112, 0.0, 1.0, 0.0, 0.0, 360.0,
      ValueDetails::kLinear, false, " degrees", "Resonator LFO Stereo Phase", nullptr },
    { "resonator_lfo_spin", 0x020112, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator LFO Spin", nullptr },
    { "resonator_lfo_offset", 0x020112, 0.0, 1.0, 0.0, 0.0, 360.0,
      ValueDetails::kLinear, false, " degrees", "Resonator LFO Offset", nullptr },
    { "resonator_filter", 0x020112, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator Filter", strings::kOffOnNames },
    { "resonator_filter_frequency", 0x020112, 4.321928, 14.135709, 10.550747, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Resonator Filter Frequency", nullptr },
    { "resonator_filter_bandwidth", 0x020112, 0.1, 6.0, 3.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " octaves", "Resonator Filter Bandwidth", nullptr },
    { "resonator_bleed", 0x020112, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Bleed", nullptr },
    { "resonator_width", 0x020112, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Width", nullptr },
    { "resonator_excitation", 0x020112, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Resonator Excitation", nullptr },
    { "resonator_wet_gain", 0x020112, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Resonator Wet Gain", nullptr },
    { "resonator_limiter", 0x020112, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Resonator Limiter", strings::kOffOnNames },
    { "resonator_output_gain", 0x020112, -24.0, 12.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Resonator Output Gain", nullptr },
    { "resonator_mix", 0x020112, 0.0, 1.0, 0.35, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonator Mix", nullptr },

    { "effect_chain_slot_29", 0x020112, 0.0, constants::kNumEffects - 1, constants::kResonator, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Slot 29", nullptr },

#include "analogue_parameters.inc"

    // Spectral Balance: one Amount knob moves the spectrum toward a target
    // balance. Target picks the curve, Bias tilts it; Range edges are log2 Hz.
    { "spectral_balance_on", 0x020117, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Balance Switch", strings::kOffOnNames },
    { "spectral_balance_amount", 0x020117, 0.0, 1.0, 0.25, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Balance Amount", nullptr },
    { "spectral_balance_target", 0x020117, 0.0, 5.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Balance Target", strings::kSpectralBalanceTargetNames },
    { "spectral_balance_bias", 0x020117, -3.0, 3.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB/oct", "Spectral Balance Bias", nullptr },
    { "spectral_balance_speed", 0x020117, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Balance Speed", nullptr },
    { "spectral_balance_range_low", 0x020117, 4.321928, 10.965784, 4.906891, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Spectral Balance Range Low", nullptr },
    { "spectral_balance_range_high", 0x020117, 9.965784, 14.287712, 13.965784, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Spectral Balance Range High", nullptr },
    { "spectral_balance_tame_recover", 0x020117, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Balance Tame Recover", nullptr },
    { "spectral_balance_mix", 0x020117, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Balance Mix", nullptr },
    { "effect_chain_slot_36", 0x020117, 0.0, constants::kNumEffects - 1, constants::kSpectralBalance, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Slot 36", nullptr },

#include "glitch_delay_parameters.inc"

    // OpenWurli Amp: the Wurlitzer 200A's shared amplifier from OpenWurli, for
    // the reeds of the OpenWurli oscillator type.
    { "openwurli_amp_on", 0x02011c, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenWurli Amp Switch", strings::kOffOnNames },
    { "openwurli_amp_volume", 0x02011c, 0.0, 1.0, 0.8, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenWurli Amp Volume", nullptr },
    { "openwurli_amp_tremolo_depth", 0x02011c, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenWurli Amp Tremolo Depth", nullptr },
    { "openwurli_amp_speaker", 0x02011c, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenWurli Amp Speaker Character", nullptr },
    { "effect_chain_slot_38", 0x02011c, 0.0, constants::kNumEffects - 1, constants::kOpenWurliAmp, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Slot 38", nullptr },

    // Scanner Vibrato: OpenB3's tonewheel organ vibrato and chorus. C3 is the
    // setting most organists leave it on; 7.25 Hz is setBfree's scanner.hz.
    { "scanner_vibrato_on", 0x02011d, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Scanner Vibrato Switch", strings::kOffOnNames },
    { "scanner_vibrato_mode", 0x02011d, 0.0, 5.0, 5.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Scanner Vibrato Mode", strings::kScannerVibratoModeNames },
    { "scanner_vibrato_rate", 0x02011d, 4.0, 22.0, 7.25, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Scanner Vibrato Rate", nullptr },
    { "effect_chain_slot_39", 0x02011d, 0.0, constants::kNumEffects - 1, constants::kScannerVibrato, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Slot 39", nullptr },

    { "spectral_compressor_on", 0x020104, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Compressor Switch", strings::kOffOnNames },
    { "spectral_compressor_compression", 0x020104, 0.0, 100.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Spectral Compressor Compression", nullptr },
    { "spectral_compressor_downward", 0x020104, 0.0, 100.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Spectral Compressor Downward", nullptr },
    { "spectral_compressor_upward", 0x020104, 0.0, 100.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Spectral Compressor Upward", nullptr },
    { "spectral_compressor_target", 0x020104, -64.0, 0.0, -16.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "dBFS", "Spectral Compressor Target", nullptr },
    { "spectral_compressor_attack", 0x020104, 0.0, 100.0, 25.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Spectral Compressor Attack", nullptr },
    { "spectral_compressor_release", 0x020104, 0.0, 100.0, 25.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Spectral Compressor Release", nullptr },
    { "spectral_compressor_smooth", 0x020104, 0.0, 100.0, 50.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Spectral Compressor Smooth", nullptr },
    { "spectral_compressor_curve", 0x020104, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Compressor Curve", strings::kSpectralCompressorCurveNames },
    { "spectral_compressor_tilt", 0x020104, -6.0, 0.0, -3.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "dB", "Spectral Compressor Tilt", nullptr },
    { "spectral_compressor_makeup", 0x020104, -6.0, 18.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "dB", "Spectral Compressor Makeup", nullptr },
    { "spectral_compressor_mix", 0x020104, 0.0, 100.0, 100.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Spectral Compressor Mix", nullptr },

    { "spectral_gate_on", 0x020101, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Gate Switch", strings::kOffOnNames },
    { "spectral_gate_cutoff", 0x020101, 0.0, 1.0, 0.6, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Gate Cutoff", nullptr },
    { "spectral_gate_balance", 0x020101, 0.0, 1.0, 0.7, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Gate Strong Balance", nullptr },
    { "spectral_gate_tilt_enabled", 0x020101, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Gate Enable Tilt", strings::kOffOnNames },
    { "spectral_gate_tilt", 0x020101, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Gate Tilt", nullptr },
    { "spectral_gate_mix", 0x020101, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Gate Mix", nullptr },
    // Append new controls by version so existing host parameter IDs stay stable.
    { "dimension_expander_delay", 0x020103, 0.0, 40.0, 20.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " ms", "Dimension Expander Delay", nullptr },
    { "spectral_gate_fft_size", 0x020103, 0.0, 5.0, 3.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " samples", "Spectral Gate FFT Size", strings::kSpectralGateFftSizeNames },
    { "sample_filter_slope", 0x020103, 0.0, 7.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Filter Slope", strings::kCutFilterSlopeNames },
    { "dimension_expander_mode", 0x020102, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Dimension Expander Mode", strings::kDimensionModeNames },
    { "dimension_expander_quorax_size", 0x020102, 0.0, 100.0, 35.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Dimension Expander Size", nullptr },
    { "dimension_expander_quorax_mix", 0x020102, 0.0, 100.0, 35.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Dimension Expander Mix", nullptr },
    { "dimension_expander_quorax_output", 0x020102, -12.0, 12.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Dimension Expander Output", nullptr },
    { "dimension_expander_quorax_motion", 0x020102, 0.02, 2.0, 0.4, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Dimension Expander Motion Rate", nullptr },
    { "dimension_expander_quorax_low_cut", 0x020102, 0.0, 2000.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Dimension Expander Wet Low Cut", nullptr },
    { "dimension_expander_quorax_high_cut", 0x020102, 500.0, 20000.0, 20000.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Dimension Expander Wet High Cut", nullptr },
    { "dimension_expander_quorax_slope", 0x020102, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Dimension Expander Wet Filter Slope", strings::kQuoraxSlopeNames },
    { "erosion_on", 0x020102, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Erosion Switch", strings::kOffOnNames },
    { "erosion_mode", 0x020102, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Erosion Mode", strings::kErosionModeNames },
    { "erosion_frequency", 0x020102, 20.0, 20000.0, 5000.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Erosion Frequency", nullptr },
    { "erosion_width", 0x020102, 0.1, 5.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Erosion Width", nullptr },
    { "erosion_amount", 0x020102, 0.0, 200.0, 50.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Erosion Amount", nullptr },
    { "erosion_stereo", 0x020102, 0.0, 100.0, 100.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Erosion Stereo", nullptr },
    { "floaty_delay_on", 0x020102, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Floaty Delay Switch", strings::kOffOnNames },
    { "floaty_delay_time", 0x020102, 0.0, 4000.0, 250.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " ms", "Floaty Delay Time", nullptr },
    { "floaty_delay_mix", 0x020102, 0.0, 100.0, 40.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Floaty Delay Mix", nullptr },
    { "floaty_delay_feedback", 0x020102, 0.0, 80.0, 30.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Floaty Delay Feedback", nullptr },
    { "floaty_delay_warp_rate", 0x020102, 0.0, 10.0, 0.1, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Floaty Delay Warp Rate", nullptr },
    { "floaty_delay_warp_amount", 0x020102, 0.0, 4.0, 0.5, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Floaty Delay Warp Amount", nullptr },
    { "floaty_delay_cutoff", 0x020102, 0.0, 100.0, 80.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Floaty Delay Filter Cutoff", nullptr },
    { "floaty_delay_resonance", 0x020102, 0.0, 150.0, 80.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Floaty Delay Filter Resonance", nullptr },
    { "floaty_delay_playback_rate", 0x020102, -2.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Floaty Delay Playback Rate", nullptr },
    { "floaty_delay_sync", 0x020102, 0.0, 64.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Floaty Delay Tempo", strings::kFloatySyncNames },
    { "floaty_delay_stereo_offset", 0x020102, 70.0, 100.0, 98.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Floaty Delay L/R Offset", nullptr },
    { "floaty_delay_clamp", 0x020102, -12.0, 12.0, 6.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Floaty Delay Pre-Filter Clamp", nullptr },
    { "macro_control_1", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 1", nullptr },
    { "macro_control_2", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 2", nullptr },
    { "macro_control_3", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 3", nullptr },
    { "macro_control_4", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 4", nullptr },
    { "macro_control_5", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 5", nullptr },
    { "macro_control_6", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 6", nullptr },
    { "macro_control_7", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 7", nullptr },
    { "macro_control_8", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 8", nullptr },
    { "macro_control_9", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 9", nullptr },
    { "macro_control_10", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 10", nullptr },
    { "macro_control_11", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 11", nullptr },
    { "macro_control_12", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 12", nullptr },
    { "macro_control_13", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 13", nullptr },
    { "macro_control_14", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 14", nullptr },
    { "macro_control_15", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 15", nullptr },
    { "macro_control_16", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Macro 16", nullptr },
    { "macro_bipolar_1", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 1 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_2", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 2 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_3", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 3 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_4", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 4 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_5", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 5 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_6", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 6 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_7", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 7 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_8", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 8 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_9", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 9 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_10", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 10 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_11", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 11 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_12", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 12 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_13", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 13 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_14", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 14 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_15", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 15 Bipolar", strings::kOffOnNames },
    { "macro_bipolar_16", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Macro 16 Bipolar", strings::kOffOnNames },
    { "pitch_bend_range", 0x000000, 0.0, 48.0, 2.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " semitones", "Pitch Bend Range", nullptr },
    { "polyphony", 0x000000, 1.0, kMaxPolyphony - 1, 8.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " voices", "Polyphony", nullptr },
    { "voice_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, " cents", "Voice Tune", nullptr },
    { "voice_transpose", 0x000604, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Voice Transpose", nullptr },
    { "voice_amplitude", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Voice Amplitude", nullptr },
    { "release_velocity_mode", 0x02010c, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Release velocity mode", strings::kReleaseVelocityModeNames },
    { "release_velocity_target", 0x02010e, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Release velocity target", strings::kReleaseVelocityTargetNames },
    { "release_velocity_glide_time", 0x02010c, 0.0, 10.0, 0.25, 0.0, 1.0,
      ValueDetails::kLinear, false, " secs", "Release velocity glide time", nullptr },
    { "release_velocity_glide_curve", 0x02010c, -8.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Release velocity glide curve", nullptr },
    { "stereo_routing", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Stereo Routing", nullptr },
    { "stereo_mode", 0x000605, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Stereo Mode", strings::kStereoModeNames },
    { "portamento_time", 0x000000, -10.0, 4.0, -10.0, 0.0, 1.0,
      ValueDetails::kExponential, false, " secs", "Portamento Time", nullptr },
    { "portamento_slope", 0x000000, -8.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Portamento Slope", nullptr },
    { "portamento_force", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Portamento Force", strings::kOffOnNames },
    { "portamento_scale", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Portamento Scale", strings::kOffOnNames },
    { "portamento_glide_zones", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Glide Zone Crossing", strings::kOffOnNames },
    { "zone_crossfade", 0x000000, 0.0, 0.1, 0.0, 0.0, 1000.0,
      ValueDetails::kLinear, false, " ms", "Zone Crossfade", nullptr },
    { "reverb_pre_low_cutoff", 0x000000, 0.0, 128.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Reverb Pre Low Cutoff", nullptr },
    { "reverb_pre_high_cutoff", 0x000000, 0.0, 128.0, 110.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Reverb Pre High Cutoff", nullptr },
    { "reverb_low_shelf_cutoff", 0x000000, 0.0, 128.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Reverb Low Cutoff", nullptr },
    { "reverb_low_shelf_gain", 0x000000, -6.0, 0.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Reverb Low Gain", nullptr },
    { "reverb_high_shelf_cutoff", 0x000000, 0.0, 128.0, 90.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Reverb High Cutoff", nullptr },
    { "reverb_high_shelf_gain", 0x000000, -6.0, 0.0, -1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Reverb High Gain", nullptr },
    { "reverb_dry_wet", 0x000000, 0.0, 1.0, 0.25, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Reverb Mix", nullptr },
    { "reverb_delay", 0x000609, 0.0, 0.3, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " secs", "Reverb Delay", nullptr },
    { "reverb_decay_time", 0x000000, -6.0, 6.0, 0.0, 0.0, 1.0,
      ValueDetails::kExponential, false, " secs", "Reverb Decay Time", nullptr },
    { "reverb_size", 0x000506, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Reverb Size", nullptr },
    { "reverb_chorus_amount", 0x000000, 0.0, 1.0, 0.223607, 0.0, 100.0,
      ValueDetails::kQuadratic, false, "%", "Reverb Chorus Amount", nullptr },
    { "reverb_chorus_frequency", 0x000000, -8.0, 3.0, -2.0, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Reverb Chorus Frequency", nullptr },
    { "reverb_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Reverb Switch", strings::kOffOnNames },
    { "sub_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sub Switch", strings::kOffOnNames },
    { "sub_direct_out", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sub Direct Out", nullptr },
    { "sub_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sub Transpose", nullptr },
    { "sub_transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sub Transpose Quantize", nullptr },
    { "sub_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Sub Tune", nullptr },
    { "sub_level", 0x000000, 0.0, 1.0, 0.70710678119, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "", "Sub Level", nullptr },
    { "sub_pan", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sub Pan", nullptr },
    { "sub_waveform", 0x000000, 0.0, PredefinedWaveFrames::kNumShapes - 1, 2.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sub Osc Waveform", nullptr },
    // Fixed-source parameters (sample_*, granular_*, spectral_*, noise_*) and the
    // arp destinations named after them are legacy aliases: their controls live in
    // oscillator slots 5-8 (osc_5_sample_*, ...). They stay listed, in place, so
    // the host parameter numbering of everything sorted after them is unchanged;
    // SynthPlugin maps each alias to its slot control. Do not add new ones.
    { "sample_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Switch", strings::kOffOnNames },
    { "sample_solo", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Solo", strings::kOffOnNames },
    { "sample_random_phase", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Random Phase", strings::kOffOnNames },
    { "sample_keytrack", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Keytrack", strings::kOffOnNames },
    { "sample_root_key", 0x000000, 0.0, 127.0, 60.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Root Key", nullptr },
    { "sample_loop", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Loop", strings::kOffOnNames },
    { "sample_bounce", 0x000603, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Bounce", strings::kOffOnNames },
    { "sample_playback_mode", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Playback Mode", strings::kSamplePlaybackModeNames },
    { "sample_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Start", nullptr },
    { "sample_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample End", nullptr },
    { "sample_loop_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Loop Start", nullptr },
    { "sample_loop_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Loop End", nullptr },
    { "sample_loop_crossfade", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Loop Crossfade", nullptr },
    { "sample_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Sample Low Cutoff", nullptr },
    { "sample_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Sample High Cutoff", nullptr },
    { "sample_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose", nullptr },
    { "sample_transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize", nullptr },
    { "sample_transpose_quantize_key", 0x000000, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize Key", strings::kTransposeQuantizeKeyNames },
    { "sample_transpose_quantize_scale", 0x000000, 0.0, 14.0, 14.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize Scale", strings::kTransposeQuantizeScaleNames },
    { "sample_transpose_quantize_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize Mode", strings::kTransposeQuantizeModeNames },
    { "sample_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Sample Tune", nullptr },
    { "sample_level", 0x000000, 0.0, 1.0, 0.70710678119, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "", "Sample Level", nullptr },
    { "sample_destination", 0x000500, 0.0, constants::kNumDestinations - 1, 3.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Destination", strings::kDestinationNames },
    { "granular_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Switch", strings::kOffOnNames },
    { "granular_solo", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Solo", strings::kOffOnNames },
    { "granular_keytrack", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Keytrack", strings::kOffOnNames },
    { "granular_root_key", 0x000000, 0.0, 127.0, 60.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Root Key", nullptr },
    { "granular_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Mode", strings::kGranularModeNames },
    { "granular_midi_density", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular MIDI Density", strings::kOffOnNames },
    { "granular_grain_count", 0x000000, 1.0, 64.0, 16.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Grain Count", nullptr },
    { "granular_density", 0x000000, 1.0, 120.0, 24.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " grains/s", "Granular Density", nullptr },
    { "granular_grain_size", 0x000000, 5.0, 500.0, 90.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " ms", "Granular Grain Size", nullptr },
    { "granular_speed", 0x000000, 0.05, 4.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x", "Granular Speed", nullptr },
    { "granular_position", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Position", nullptr },
    { "granular_position_mod", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Position Mod", nullptr },
    { "granular_position_mod_rate", 0x000000, 0.01, 20.0, 0.25, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Granular Position Mod Rate", nullptr },
    { "granular_random_position", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Random Position", nullptr },
    { "granular_random_volume", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Random Volume", nullptr },
    { "granular_random_pan", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Random Pan", nullptr },
    { "granular_random_pitch", 0x000000, 0.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " st", "Granular Random Pitch", nullptr },
    { "granular_interval", 0x000000, -24.0, 24.0, 12.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " st", "Granular Interval", nullptr },
    { "granular_interval_chance", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Interval Chance", nullptr },
    { "granular_direction", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Direction", strings::kGranularDirectionNames },
    { "granular_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Start", nullptr },
    { "granular_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular End", nullptr },
    { "granular_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Granular Low Cutoff", nullptr },
    { "granular_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Granular High Cutoff", nullptr },
    { "granular_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose", nullptr },
    { "granular_transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize", nullptr },
    { "granular_transpose_quantize_key", 0x000000, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize Key", strings::kTransposeQuantizeKeyNames },
    { "granular_transpose_quantize_scale", 0x000000, 0.0, 14.0, 14.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize Scale", strings::kTransposeQuantizeScaleNames },
    { "granular_transpose_quantize_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize Mode", strings::kTransposeQuantizeModeNames },
    { "granular_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Granular Tune", nullptr },
    { "granular_level", 0x000000, 0.0, 1.0, 0.70710678119, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "", "Granular Level", nullptr },
    { "granular_pan", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Pan", nullptr },
    // Granular grid and clarity controls. Append by version to preserve
    // every existing host parameter ID; defaults reproduce earlier timing.
    { "granular_window_shape", 0x020111, 0.0, GranularSource::kNumWindowShapes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Window Shape", strings::kGranularWindowNames },
    { "granular_timing_jitter", 0x020111, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Timing Jitter", nullptr },
    { "granular_timing_mode", 0x020111, 0.0, GranularSource::kNumTimingModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Timing", strings::kGranularTimingNames },
    { "granular_grid_rate", 0x020111, 0.0, GranularSource::kNumGridRates - 1, 4.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Grid Rate", strings::kGranularGridRateNames },
    { "granular_size_mode", 0x020111, 0.0, GranularSource::kNumSizeModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Size Mode", strings::kGranularSizeModeNames },
    { "granular_grid_length", 0x020111, 0.05, 4.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x step", "Granular Grid Length", nullptr },
    { "granular_position_grid", 0x020111, 0.0, GranularSource::kNumPositionGrids - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Position Grid", strings::kGranularPositionGridNames },
    { "granular_destination", 0x000500, 0.0, constants::kNumDestinations - 1, 3.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Destination", strings::kDestinationNames },
    { "granular_key_zone_start", 0x000000, 0.0, 127.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Key Zone Start", nullptr },
    { "granular_key_zone_end", 0x000000, 0.0, 127.0, 127.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Key Zone End", nullptr },
    { "granular_velocity_zone_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Velocity Zone Start", nullptr },
    { "granular_velocity_zone_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Velocity Zone End", nullptr },
    { "spectral_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Switch", strings::kOffOnNames },
    { "spectral_solo", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Solo", strings::kOffOnNames },
    { "spectral_keytrack", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Keytrack", strings::kOffOnNames },
    { "spectral_root_key", 0x000000, 0.0, 127.0, 60.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Root Key", nullptr },
    { "spectral_play_mode", 0x000000, 0.0, SpectralSource::kNumPlayModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Play Mode", strings::kSpectralPlayModeNames },
    { "spectral_position", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Position", nullptr },
    { "spectral_speed", 0x000000, -2.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x", "Spectral Speed", nullptr },
    { "spectral_loop_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Loop Start", nullptr },
    { "spectral_loop_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Loop End", nullptr },
    { "spectral_bin_low", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Bin Low", nullptr },
    { "spectral_bin_high", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Bin High", nullptr },
    { "spectral_shift", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Shift", nullptr },
    { "spectral_stretch", 0x000000, 0.25, 4.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x", "Spectral Stretch", nullptr },
    { "spectral_blur", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Blur", nullptr },
    { "spectral_tilt", 0x000000, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB/oct", "Spectral Tilt", nullptr },
    { "spectral_phase_randomize", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Phase Randomize", nullptr },
    { "spectral_freeze", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Freeze", strings::kOffOnNames },
    { "spectral_keep_fundamental", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Keep Fundamental", strings::kOffOnNames },
    { "spectral_phase_mode", 0x000000, 0.0, SpectralSource::kNumPhaseModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Phase Mode", strings::kSpectralPhaseModeNames },
    { "spectral_preserve_transients", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Preserve Transients", strings::kOffOnNames },
    { "spectral_effect", 0x000000, 0.0, SpectralSource::kNumSpectralEffects - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Effect", strings::kSpectralEffectNames },
    { "spectral_effect_amount", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Effect Amount", nullptr },
    { "spectral_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose", nullptr },
    { "spectral_transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize", nullptr },
    { "spectral_transpose_quantize_key", 0x000000, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize Key", strings::kTransposeQuantizeKeyNames },
    { "spectral_transpose_quantize_scale", 0x000000, 0.0, 14.0, 14.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize Scale", strings::kTransposeQuantizeScaleNames },
    { "spectral_transpose_quantize_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize Mode", strings::kTransposeQuantizeModeNames },
    { "spectral_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Spectral Tune", nullptr },
    { "spectral_level", 0x000000, 0.0, 1.0, 0.70710678119, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "", "Spectral Level", nullptr },
    { "spectral_pan", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Pan", nullptr },
    { "spectral_destination", 0x000500, 0.0, constants::kNumDestinations - 1, 3.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Destination", strings::kDestinationNames },
    { "spectral_key_zone_start", 0x000000, 0.0, 127.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Key Zone Start", nullptr },
    { "spectral_key_zone_end", 0x000000, 0.0, 127.0, 127.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Key Zone End", nullptr },
    { "spectral_velocity_zone_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Velocity Zone Start", nullptr },
    { "spectral_velocity_zone_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Velocity Zone End", nullptr },
    { "noise_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Switch", strings::kOffOnNames },
    { "noise_solo", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Solo", strings::kOffOnNames },
    { "noise_type", 0x000000, 0.0, NoiseSource::kNumNoiseTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Type", strings::kNoiseTypeNames },
    { "noise_level", 0x000000, 0.0, 1.0, 0.70710678119, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "", "Noise Level", nullptr },
    { "noise_pan", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Noise Pan", nullptr },
    { "noise_width", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Noise Width", nullptr },
    { "noise_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Noise Low Cutoff", nullptr },
    { "noise_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Noise High Cutoff", nullptr },
    { "noise_filter_slope", 0x000000, 0.0, 7.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Filter Slope", strings::kCutFilterSlopeNames },
    { "noise_destination", 0x000500, 0.0, constants::kNumDestinations - 1, 3.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Destination", strings::kDestinationNames },
    { "noise_key_zone_start", 0x000000, 0.0, 127.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Key Zone Start", nullptr },
    { "noise_key_zone_end", 0x000000, 0.0, 127.0, 127.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Key Zone End", nullptr },
    { "noise_velocity_zone_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Noise Velocity Zone Start", nullptr },
    { "noise_velocity_zone_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Noise Velocity Zone End", nullptr },
    { "bus_1_destination", 0x000801, 0.0, constants::kNumBusDestinations - 1,
      constants::kBusDestinationMain, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Bus 1 Destination", strings::kBusDestinationNames },
    { "bus_2_destination", 0x000801, 0.0, constants::kNumBusDestinations - 1,
      constants::kBusDestinationMain, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Bus 2 Destination", strings::kBusDestinationNames },
    { "bus_3_destination", 0x000801, 0.0, constants::kNumBusDestinations - 1,
      constants::kBusDestinationMain, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Bus 3 Destination", strings::kBusDestinationNames },
    { "filter_1_destination", 0x000709, 0.0, constants::kNumSourceDestinations - 1, constants::kEffects, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Filter 1 Destination", strings::kDestinationNames },
    { "filter_2_destination", 0x000709, 0.0, constants::kNumSourceDestinations - 1, constants::kEffects, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Filter 2 Destination", strings::kDestinationNames },
    { "sample_pan", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Pan", nullptr },
    { "sample_key_zone_start", 0x000000, 0.0, 127.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Key Zone Start", nullptr },
    { "sample_key_zone_end", 0x000000, 0.0, 127.0, 127.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Key Zone End", nullptr },
    { "sample_velocity_zone_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Velocity Zone Start", nullptr },
    { "sample_velocity_zone_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Velocity Zone End", nullptr },
    { "velocity_track", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Velocity Track", nullptr },
    { "volume", 0x000000, 0.0, 7399.4404, 5473.0404, -80, 1.0,
      ValueDetails::kSquareRoot, false, "dB", "Volume", nullptr },
    { "phaser_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Phaser Switch", strings::kOffOnNames },
    { "phaser_dry_wet", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Phaser Mix", nullptr },
    { "phaser_feedback", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Phaser Feedback", nullptr },
    { "phaser_frequency", 0x000000, -5.0, 2.0, -3.0, 0.0, 1.0,
      ValueDetails::kExponential, true, " secs", "Phaser Frequency", nullptr },
    { "phaser_sync", 0x000000, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Phaser Sync",strings::kFrequencySyncNames },
    { "phaser_tempo", 0x000000, 0.0, 10.0, 3.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Phaser Tempo", strings::kSyncedFrequencyNames },
    { "phaser_center", 0x000000, 8.0, 136.0, 80.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Phaser Center", nullptr },
    { "phaser_blend", 0x000509, 0.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Phaser Blend", nullptr },
    { "phaser_mod_depth", 0x000000, 0.0, 48.0, 24.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Phaser Mod Depth", nullptr },
    { "phaser_phase_offset", 0x000000, 0, 1.0, 0.33333333, 0.0, kDegreesPerCycle,
      ValueDetails::kLinear, false, "", "Phaser Phase Offset", nullptr },
    { "flanger_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Flanger Switch", strings::kOffOnNames },
    { "flanger_dry_wet", 0x000000, 0.0, 0.5, 0.5, 0.0, 200.0,
      ValueDetails::kLinear, false, "%", "Flanger Mix", nullptr },
    { "flanger_feedback", 0x000000, -1.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Flanger Feedback", nullptr },
    { "flanger_frequency", 0x000000, -5.0, 2.0, 2.0, 0.0, 1.0,
      ValueDetails::kExponential, true, " secs", "Flanger Frequency", nullptr },
    { "flanger_sync", 0x000000, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Flanger Sync", strings::kFrequencySyncNames },
    { "flanger_tempo", 0x000000, 0.0, 10.0, 4.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Flanger Tempo", strings::kSyncedFrequencyNames },
    { "flanger_center", 0x000505, 8.0, 136.0, 64.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Flanger Center", nullptr },
    { "flanger_mod_depth", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Flanger Mod Depth", nullptr },
    { "flanger_phase_offset", 0x000000, 0, 1.0, 0.33333333, 0.0, kDegreesPerCycle,
      ValueDetails::kLinear, false, "", "Flanger Phase Offset", nullptr },
    { "chorus_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Chorus Switch", strings::kOffOnNames },
    { "chorus_dry_wet", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Chorus Mix", nullptr },
    { "chorus_feedback", 0x000000, -0.95, 0.95, 0.4, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Chorus Feedback", nullptr },
    { "chorus_cutoff", 0x000000, 8.0, 136.0, 60.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Chorus Filter Cutoff", nullptr },
    { "chorus_spread", 0x000607, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Chorus Filter Spread", nullptr },
    { "chorus_voices", 0x000508, 1.0, 4.0, 4.0, 0.0, 4.0,
      ValueDetails::kIndexed, false, "", "Chorus Voices", nullptr },
    { "chorus_frequency", 0x000000, -6.0, 3.0, -3.0, 0.0, 1.0,
      ValueDetails::kExponential, true, " secs", "Chorus Frequency", nullptr },
    { "chorus_sync", 0x000000, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Chorus Sync", strings::kFrequencySyncNames },
    { "chorus_tempo", 0x000000, 0.0, 10.0, 4.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Chorus Tempo", strings::kSyncedFrequencyNames },
    { "chorus_mod_depth", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Chorus Mod Depth", nullptr },
    { "chorus_delay_1", 0x000000, -10.0, -5.64386, -9.0, 0.0, 1000.0,
      ValueDetails::kExponential, false, "ms", "Chorus Delay 1", nullptr },
    { "chorus_delay_2", 0x000000, -10.0, -5.64386, -7.0, 0.0, 1000.0,
      ValueDetails::kExponential, false, " ms", "Chorus Delay 2", nullptr },
    { "compressor_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Compressor Switch", strings::kOffOnNames },
    { "compressor_low_upper_threshold", 0x000000, -80.0, 0.0, -28.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Low Upper Threshold", nullptr },
    { "compressor_band_upper_threshold", 0x000000, -80.0, 0.0, -25.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Band Upper Threshold", nullptr },
    { "compressor_high_upper_threshold", 0x000000, -80.0, 0.0, -30.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "High Upper Threshold", nullptr },
    { "compressor_low_lower_threshold", 0x000000, -80.0, 0.0, -35.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Low Lower Threshold", nullptr },
    { "compressor_band_lower_threshold", 0x000000, -80.0, 0.0, -36.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Band Lower Threshold", nullptr },
    { "compressor_high_lower_threshold", 0x000000, -80.0, 0.0, -35.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "High Lower Threshold", nullptr },
    { "compressor_low_upper_ratio", 0x000000, 0.0, 1.0, 0.9, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Low Upper Ratio", nullptr },
    { "compressor_band_upper_ratio", 0x000000, 0.0, 1.0, 0.857, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Band Upper Ratio", nullptr },
    { "compressor_high_upper_ratio", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "High Upper Ratio", nullptr },
    { "compressor_low_lower_ratio", 0x000000, -1.0, 1.0, 0.8, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Low Lower Ratio", nullptr },
    { "compressor_band_lower_ratio", 0x000000, -1.0, 1.0, 0.8, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Band Lower Ratio", nullptr },
    { "compressor_high_lower_ratio", 0x000000, -1.0, 1.0, 0.8, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "High Lower Ratio", nullptr },
    { "compressor_low_gain", 0x000000, -30.0, 30.0, 16.3, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Compressor Low Gain", nullptr },
    { "compressor_band_gain", 0x000000, -30.0, 30.0, 11.7, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Compressor Band Gain", nullptr },
    { "compressor_high_gain", 0x000000, -30.0, 30.0, 16.3, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Compressor High Gain", nullptr },
    { "compressor_attack", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Compressor Attack", nullptr },
    { "compressor_release", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Compressor Release", nullptr },
    { "compressor_enabled_bands", 0x000000, 0.0, vital::MultibandCompressor::kNumBandOptions - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Compressor Enabled Bands", strings::kCompressorBandNames },
    { "compressor_mix", 0x000602, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Compressor Mix", nullptr },
    { "compressor_low_band_unused", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Compressor Unused", nullptr },
    { "eq_on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "EQ Switch", strings::kOffOnNames },
    { "eq_low_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "EQ Low Mode", strings::kEqLowModeNames },
    { "eq_low_cutoff", 0x000000, 8.0, 136.0, 40.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "EQ Low Cutoff", nullptr },
    { "eq_low_gain", 0x000000, DigitalSvf::kMinGain, DigitalSvf::kMaxGain, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "EQ Low Gain", nullptr },
    { "eq_low_resonance", 0x000000, 0.0, 1.0, 0.3163, 0.0, 100.0,
      ValueDetails::kQuadratic, false, "%", "EQ Low Resonance", nullptr },
    { "eq_band_mode", 0x000506, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "EQ Band 1 Mode", strings::kEqBandModeNames },
    { "eq_band_cutoff", 0x000000, 8.0, 136.0, 80.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "EQ Band 1 Cutoff", nullptr },
    { "eq_band_gain", 0x000000, DigitalSvf::kMinGain, DigitalSvf::kMaxGain, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "EQ Band 1 Gain", nullptr },
    { "eq_band_resonance", 0x000000, 0.0, 1.0, 0.4473, 0.0, 100.0,
      ValueDetails::kQuadratic, false, "", "EQ Band 1 Resonance", nullptr },
    { "eq_band_2_mode", 0x000506, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "EQ Band 2 Mode", strings::kEqBandModeNames },
    { "eq_band_2_cutoff", 0x000000, 8.0, 136.0, 68.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "EQ Band 2 Cutoff", nullptr },
    { "eq_band_2_gain", 0x000000, DigitalSvf::kMinGain, DigitalSvf::kMaxGain, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "EQ Band 2 Gain", nullptr },
    { "eq_band_2_resonance", 0x000000, 0.0, 1.0, 0.4473, 0.0, 100.0,
      ValueDetails::kQuadratic, false, "", "EQ Band 2 Resonance", nullptr },
    { "eq_band_3_mode", 0x000506, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "EQ Band 3 Mode", strings::kEqBandModeNames },
    { "eq_band_3_cutoff", 0x000000, 8.0, 136.0, 92.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "EQ Band 3 Cutoff", nullptr },
    { "eq_band_3_gain", 0x000000, DigitalSvf::kMinGain, DigitalSvf::kMaxGain, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "EQ Band 3 Gain", nullptr },
    { "eq_band_3_resonance", 0x000000, 0.0, 1.0, 0.4473, 0.0, 100.0,
      ValueDetails::kQuadratic, false, "", "EQ Band 3 Resonance", nullptr },
    { "eq_band_4_mode", 0x000506, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "EQ Band 4 Mode", strings::kEqBandModeNames },
    { "eq_band_4_cutoff", 0x000000, 8.0, 136.0, 104.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "EQ Band 4 Cutoff", nullptr },
    { "eq_band_4_gain", 0x000000, DigitalSvf::kMinGain, DigitalSvf::kMaxGain, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "EQ Band 4 Gain", nullptr },
    { "eq_band_4_resonance", 0x000000, 0.0, 1.0, 0.4473, 0.0, 100.0,
      ValueDetails::kQuadratic, false, "", "EQ Band 4 Resonance", nullptr },
    { "eq_high_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "EQ High Mode", strings::kEqHighModeNames },
    { "eq_high_cutoff", 0x000000, 8.0, 136.0, 100.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "EQ High Cutoff", nullptr },
    { "eq_high_gain", 0x000000, DigitalSvf::kMinGain, DigitalSvf::kMaxGain, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "EQ High Gain", nullptr },
    { "eq_high_resonance", 0x000000, 0.0, 1.0, 0.3163, 0.0, 100.0,
      ValueDetails::kQuadratic, false, "", "EQ High Resonance", nullptr },
    { "effect_chain_order", 0x000000, 0.0,
      static_cast<mono_float>(vital::utils::factorial(vital::constants::kNumReorderableEffects) - 1),
      0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Order", nullptr },
    { "effect_chain_slot_1", 0x000000, 0.0, constants::kNumEffects - 1, constants::kChorus, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 1", strings::kEffectOrder },
    { "effect_chain_slot_2", 0x000000, 0.0, constants::kNumEffects - 1, constants::kCompressor, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 2", strings::kEffectOrder },
    { "effect_chain_slot_3", 0x000000, 0.0, constants::kNumEffects - 1, constants::kDelay, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 3", strings::kEffectOrder },
    { "effect_chain_slot_4", 0x000000, 0.0, constants::kNumEffects - 1, constants::kDistortion, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 4", strings::kEffectOrder },
    { "effect_chain_slot_5", 0x000000, 0.0, constants::kNumEffects - 1, constants::kEq, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 5", strings::kEffectOrder },
    { "effect_chain_slot_6", 0x000000, 0.0, constants::kNumEffects - 1, constants::kFilterFx, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 6", strings::kEffectOrder },
    { "effect_chain_slot_7", 0x000000, 0.0, constants::kNumEffects - 1, constants::kFlanger, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 7", strings::kEffectOrder },
    { "effect_chain_slot_8", 0x000000, 0.0, constants::kNumEffects - 1, constants::kPhaser, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 8", strings::kEffectOrder },
    { "effect_chain_slot_9", 0x000000, 0.0, constants::kNumEffects - 1, constants::kReverb, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 9", strings::kEffectOrder },
    { "effect_chain_slot_10", 0x000000, 0.0, constants::kNumEffects - 1, constants::kUtility, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 10", strings::kEffectOrder },
    { "effect_chain_slot_11", 0x000000, 0.0, constants::kNumEffects - 1, constants::kPhaseShift, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 11", strings::kEffectOrder },
    { "effect_chain_slot_12", 0x000000, 0.0, constants::kNumEffects - 1, constants::kFrequencyShifter, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 12", strings::kEffectOrder },
    { "effect_chain_slot_13", 0x000000, 0.0, constants::kNumEffects - 1, constants::kLimiter, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 13", strings::kEffectOrder },
    { "effect_chain_slot_14", 0x000000, 0.0, constants::kNumEffects - 1, constants::kDimensionExpander, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 14", strings::kEffectOrder },
    { "effect_chain_slot_15", 0x020100, 0.0, constants::kNumEffects - 1, constants::kWaveShaper, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 15", strings::kEffectOrder },
    { "effect_chain_slot_16", 0x020100, 0.0, constants::kNumEffects - 1, constants::kConvolutionReverb, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 16", strings::kEffectOrder },
    { "effect_chain_slot_17", 0x020100, 0.0, constants::kNumEffects - 1, constants::kMidSideEq, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 17", strings::kEffectOrder },
    { "effect_chain_slot_18", 0x020101, 0.0, constants::kNumEffects - 1, constants::kSpectralGate, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 18", strings::kEffectOrder },
    { "effect_chain_slot_19", 0x020102, 0.0, constants::kNumEffects - 1, constants::kErosion, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 19", strings::kEffectOrder },
    { "effect_chain_slot_20", 0x020102, 0.0, constants::kNumEffects - 1, constants::kFloatyDelay, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 20", strings::kEffectOrder },
    { "effect_chain_slot_21", 0x020104, 0.0, constants::kNumEffects - 1, constants::kSpectralCompressor, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 21", strings::kEffectOrder },
    { "effect_chain_slot_22", 0x020105, 0.0, constants::kNumEffects - 1, constants::kScream, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 22", strings::kEffectOrder },
    { "effect_chain_slot_23", 0x020106, 0.0, constants::kNumEffects - 1, constants::kTremoloPan, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 23", strings::kEffectOrder },
    { "effect_chain_slot_24", 0x020107, 0.0, constants::kNumEffects - 1, constants::kTape, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 24", strings::kEffectOrder },
    { "effect_chain_slot_25", 0x020108, 0.0, constants::kNumEffects - 1, constants::kDefuse, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Position 25", strings::kEffectOrder },
    { "post_effect_order", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Post Effect Order", strings::kPostEffectOrderNames },
    { "voice_priority", 0x000000, 0.0, VoiceHandler::kNumVoicePriorities - 1, VoiceHandler::kRoundRobin, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Voice Priority", strings::kVoicePriorityNames },
    { "voice_override", 0x000700, 0.0, VoiceHandler::kNumVoiceOverrides - 1, VoiceHandler::kKill, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Voice Override", strings::kVoiceOverrideNames },
    { "oversampling", 0x000000, 0.0, 3.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Oversampling", strings::kOversamplingNames },
    { "pitch_wheel", 0x000400, -1.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Pitch Wheel", nullptr },
    { "mod_wheel", 0x000400, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Mod Wheel", nullptr },
    { "mpe_enabled", 0x000501, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "MPE Enabled", strings::kOffOnNames },
    { "view_spectrogram", 0x000803, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "View Spectrogram", nullptr },
    { "effect_chain_slot_27", 0x02010a, 0.0, constants::kNumEffects - 1, constants::kTranceGate, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Slot 27", strings::kEffectOrder },
    { "effect_chain_slot_26", 0x020109, 0.0, constants::kNumEffects - 1, constants::kSuppressor, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Effect Chain Slot 26", strings::kEffectOrder },
    { "suppressor_on", 0x020109, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Suppressor Switch", strings::kOffOnNames },
    { "suppressor_amount", 0x020109, 0.0, 100.0, 25.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Suppressor Amount", nullptr },
    { "suppressor_smoothing", 0x020109, 0.0, 100.0, 100.f * std::pow(66.f / 200.f, 1.25f), 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Suppressor Smoothing", nullptr },
    { "suppressor_release", 0x020109, 1.0, 8.0, 1.f + 7.f * std::pow(65.5f / 200.f, 2.f), 0.0, 1.0,
      ValueDetails::kLinear, false, " factor", "Suppressor Release", nullptr },
    { "suppressor_frequency", 0x020109, 14.0, 370.0, 192.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Suppressor Curve Frequency", nullptr },
    { "suppressor_width", 0x020109, 0.0, 1000.0, 128.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Suppressor Curve Width", nullptr },
    { "suppressor_slope", 0x020109, 0.0, 100.0, 100.f * 50.f / 256.f, 0.0, 1.0,
      ValueDetails::kLinear, false, "%", "Suppressor Curve Slope", nullptr },
    { "suppressor_delta", 0x020109, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Suppressor Delta", strings::kOffOnNames },

    // Oscillator slots 5-8 arpeggiator destinations (see the legacy aliases above).
    { "arp_1_destination_osc_5", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 5", strings::kOffOnNames },
    { "arp_1_destination_osc_6", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 6", strings::kOffOnNames },
    { "arp_1_destination_osc_7", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 7", strings::kOffOnNames },
    { "arp_1_destination_osc_8", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Destination Oscillator 8", strings::kOffOnNames },
    { "arp_2_destination_osc_5", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 5", strings::kOffOnNames },
    { "arp_2_destination_osc_6", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 6", strings::kOffOnNames },
    { "arp_2_destination_osc_7", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 7", strings::kOffOnNames },
    { "arp_2_destination_osc_8", 0x020114, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Destination Oscillator 8", strings::kOffOnNames },

    // How often Beat retrigger restarts each arpeggiator, appended by version
    // to preserve every existing host parameter ID.
    { "arp_1_retrigger_division", 0x02011e, 0.0, 8.0, 4.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Retrigger Division", strings::kArpRetriggerDivisionNames },
    { "arp_2_retrigger_division", 0x02011e, 0.0, 8.0, 4.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Retrigger Division", strings::kArpRetriggerDivisionNames },
    // Arpeggiator play order, appended by version to preserve every existing host parameter ID.
    { "arp_1_play_order", 0x02011f, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Play Order", strings::kArpPlayOrderNames },
    { "arp_2_play_order", 0x02011f, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Play Order", strings::kArpPlayOrderNames },
    // How many times each arpeggiator pattern plays, appended by version to
    // preserve every existing host parameter ID. Infinite matches older patches.
    { "arp_1_loop", 0x020120, 0.0, 16.0, 16.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 1 Loop", strings::kArpLoopNames },
    { "arp_2_loop", 0x020120, 0.0, 16.0, 16.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Arpeggiator 2 Loop", strings::kArpLoopNames },

    // Octave each arpeggiator plays in relative to the held keys, appended by
    // version to preserve every existing host parameter ID. 0 matches older patches.
    { "arp_1_octave", 0x020122, -4.0, 4.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " octaves", "Arpeggiator 1 Octave", nullptr },
    { "arp_2_octave", 0x020122, -4.0, 4.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " octaves", "Arpeggiator 2 Octave", nullptr },
  };

  const ValueDetails ValueDetailsLookup::env_parameter_list[] = {
    { "tempo_sync", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Tempo Sync", strings::kOffOnNames },
    { "delay", 0x000503, 0.0, 1.4142135624, 0.0, 0.0, 1.0,
      ValueDetails::kQuartic, false, " secs", "Delay", nullptr },
    { "attack", 0x000000, 0.0, 2.37842, 0.1495, 0.0, 1.0,
      ValueDetails::kQuartic, false, " secs", "Attack", nullptr },
    { "hold", 0x000504, 0.0, 1.4142135624, 0.0, 0.0, 1.0,
      ValueDetails::kQuartic, false, " secs", "Hold", nullptr },
    { "decay", 0x000000, 0.0, 2.37842, 1.0, 0.0, 1.0,
      ValueDetails::kQuartic, false, " secs", "Decay", nullptr },
    { "release", 0x000000, 0.0, 2.37842, 0.5476, 0.0, 1.0,
      ValueDetails::kQuartic, false, " secs", "Release", nullptr },
    { "attack_power", 0x000000, -20.0, 20.0, 0.0f, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Attack Power", nullptr },
    { "decay_power", 0x000000, -20.0, 20.0, -2.0f, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Decay Power", nullptr },
    { "release_power", 0x000000, -20.0, 20.0, -2.0f, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Release Power", nullptr },
    { "sustain", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Sustain", nullptr },
  };

  const ValueDetails ValueDetailsLookup::lfo_parameter_list[] = {
    { "phase", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Phase", nullptr },
    { "sync_type", 0x000000, 0.0, SynthLfo::kNumSyncTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sync Type", strings::kSyncNames },
    { "frequency", 0x000000, -7.0, 9.0, 1.0, 0.0, 1.0,
      ValueDetails::kExponential, true, " secs", "Frequency", nullptr },
    { "rate_x10", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "10x Rate", strings::kOffOnNames },
    { "sync", 0x000000, 0.0, SynthLfo::kNumSyncOptions - 1, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sync", strings::kFrequencySyncNames },
    { "tempo", 0x000000, 0.0, 12.0, 7.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Tempo", strings::kSyncedFrequencyNames },
    { "fade_time", 0x000000, 0.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " secs", "Fade In", nullptr },
    { "smooth_mode", 0x000801, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Smooth Mode", strings::kOffOnNames },
    { "smooth_time", 0x000801, -10.0, 4.0, -7.5, 0.0, 1.0,
      ValueDetails::kExponential, false, " secs", "Smooth Time", nullptr },
    { "deform", 0x020000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Deform", nullptr },
    { "swing", 0x020000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Swing", nullptr },
    { "delay_time", 0x000000, 0.0, 4.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " secs", "Delay", nullptr },
    { "stereo", 0x000406, -0.5, 0.5, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Stereo", nullptr },
    { "keytrack_transpose", 0x000704, -60.0, 36.0, -12.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Transpose", nullptr },
    { "keytrack_tune", 0x000704, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Tune", nullptr },
  };

  const ValueDetails ValueDetailsLookup::random_lfo_parameter_list[] = {
    { "style", 0x000401, 0.0, RandomLfo::kNumStyles - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Style", strings::kRandomNames },
    { "frequency", 0x000401, -7.0, 9.0, 1.0, 0.0, 1.0,
      ValueDetails::kExponential, true, " secs", "Frequency", nullptr },
    { "rate_x10", 0x000401, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "10x Rate", strings::kOffOnNames },
    { "sync", 0x000401, 0.0, SynthLfo::kNumSyncOptions - 1, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sync", strings::kFrequencySyncNames },
    { "tempo", 0x000401, 0.0, 12.0, 8.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Tempo", strings::kSyncedFrequencyNames },
    { "stereo", 0x000401, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Stereo", strings::kOffOnNames },
    { "sync_type", 0x000600, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sync Type", strings::kOffOnNames },
    { "keytrack_transpose", 0x000704, -60.0, 36.0, -12.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Transpose", nullptr },
    { "keytrack_tune", 0x000704, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Tune", nullptr },
  };

  const ValueDetails ValueDetailsLookup::filter_parameter_list[] = {
    { "mix", 0x000000, 0.0f, 1.0f, 1.0f, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Mix", nullptr },
    { "cutoff", 0x000000, 8.0, 136.0, 60.0, -60.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Cutoff", nullptr },
    { "resonance", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Resonance", nullptr },
    { "drive", 0x000000, 0.0, 20.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "dB", "Drive", nullptr },
    { "blend", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Blend", nullptr },
    { "style", 0x000000, 0.0, 9.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Style", strings::kFilterStyleNames },
    { "model", 0x000000, 0.0, kNumFilterModels - 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Model", strings::kFilterModelNames },
    { "on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Switch", strings::kOffOnNames },
    { "blend_transpose", 0x000000, 0.0, 84.0, 42.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Comb Blend Offset", nullptr },
    { "keytrack", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Key Track", nullptr },
    { "formant_x", 0x000000, 0.0, 1.0, 0.5, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Formant X", nullptr },
    { "formant_y", 0x000000, 0.0, 1.0, 0.5, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Formant Y", nullptr },
    { "formant_transpose", 0x000000, -12.0, 12.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Formant Transpose", nullptr },
    { "formant_resonance", 0x000000, 0.3, 1.0, 0.85, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Formant Resonance", nullptr },
    { "formant_spread", 0x000707, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Formant Spread", nullptr },
    { "osc1_input", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OSC 1 Input", strings::kOffOnNames },
    { "osc2_input", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OSC 2 Input", strings::kOffOnNames },
    { "osc3_input", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OSC 3 Input", strings::kOffOnNames },
    { "osc4_input", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OSC 4 Input", strings::kOffOnNames },
    { "sample_input", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "SAMPLE Input", strings::kOffOnNames },
    { "filter_input", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "FILTER Input", strings::kOffOnNames },
    { "reverb_cutoff", 0x02010f, 3.0, 14.28771237954945, 6.643856189774724, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Reverb Cutoff", nullptr },
    { "reverb_damp", 0x02010f, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Damp", nullptr },
    { "reverb_output", 0x02010f, -24.0, 12.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Output", nullptr },
    { "reverb_bass_preserve", 0x02010f, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Bass Preserve", nullptr },
    { "scream_amount", 0x02010f, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Scream", nullptr },
    { "scream_input_gain", 0x02010f, -72.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Input Gain", nullptr },
    { "scream_output_gain", 0x02010f, -24.0, 0.0, -6.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Output", nullptr },
    { "scream_autogain", 0x02010f, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Autogain", strings::kOffOnNames },
    { "table_frame", 0x020118, 0.0, 256.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Wavetable Frame", nullptr },
    { "distortion_drive", 0x020125, Distortion::kMinDrive, Distortion::kMaxDrive, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "Distortion Drive", nullptr },
    { "distortion_filter_order", 0x020125, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Distortion Filter Order", strings::kDistortionFilterOrderNames },
  };

  const ValueDetails ValueDetailsLookup::osc_parameter_list[] = {
    { "on", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Switch", strings::kOffOnNames },
    { "solo", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Solo", strings::kOffOnNames },
    { "transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Transpose", nullptr },
    { "transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Transpose Quantize", nullptr },
    { "transpose_quantize_key", 0x000000, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Transpose Quantize Key", strings::kTransposeQuantizeKeyNames },
    { "transpose_quantize_scale", 0x000000, 0.0, 14.0, 14.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Transpose Quantize Scale", strings::kTransposeQuantizeScaleNames },
    { "transpose_quantize_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Transpose Quantize Mode", strings::kTransposeQuantizeModeNames },
    { "key_zone_start", 0x000000, 0.0, 127.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Key Zone Start", nullptr },
    { "key_zone_end", 0x000000, 0.0, 127.0, 127.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Key Zone End", nullptr },
    { "velocity_zone_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Velocity Zone Start", nullptr },
    { "velocity_zone_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Velocity Zone End", nullptr },
    { "tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Tune", nullptr },
    { "fixed_frequency_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Fixed Frequency Mode", strings::kOffOnNames },
    { "fixed_frequency", 0x000000, 1.0, 20000.0, 440.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Fixed Frequency", nullptr },
    { "pan", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Pan", nullptr },
    { "stack_style", 0x000000, 0.0, SynthOscillator::kNumUnisonStackTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Stack Style", strings::kUnisonStackNames },
    { "unison_detune", 0x000000, 0.0, 10.0, 4.472135955, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "%", "Unison Detune", nullptr },
    { "unison_voices", 0x000000, 1.0, 16.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "v", "Unison Voices", nullptr },
    { "unison_blend", 0x000000, 0.0, 1.0, 0.8, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Blend", nullptr },
    { "detune_power", 0x000000, -5.0, 5.0, 1.5, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Detune Power", nullptr },
    { "detune_range", 0x000000, 0.0, 48.0, 2.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Detune Range", nullptr },
    { "level", 0x000000, 0.0, 1.0, 0.70710678119, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "", "Level", nullptr },
    { "midi_track", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Midi Track", strings::kOffOnNames },
    { "smooth_interpolation", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Smooth Interpolation", strings::kOffOnNames },
    { "spectral_unison", 0x000500, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Unison", strings::kOffOnNames },
    { "wave_frame", 0x000000, 0.0, kNumOscillatorWaveFrames - 1, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Wave Frame", nullptr },
    { "frame_spread", 0x000000, -kNumOscillatorWaveFrames / 2, kNumOscillatorWaveFrames / 2, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Unison Frame Spread", nullptr },
    { "stereo_spread", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Stereo Spread", nullptr },
    { "phase", 0x000000, 0.0, 1.0, 0.0, 0.0, 360.0,
      ValueDetails::kLinear, false, "", "Phase", nullptr },
    { "distortion_phase", 0x000000, 0.0, 1.0, 0.5, 0.0, 360.0,
      ValueDetails::kLinear, false, "", "Distortion Phase", nullptr },
    { "random_phase", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Phase Randomization", nullptr },
    { "distortion_type", 0x000000, 0.0, SynthOscillator::kNumDistortionTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Distortion Type", strings::kPhaseDistortionNames },
    { "distortion_amount", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Distortion Amount", nullptr },
    // Append by version to preserve every existing host parameter ID.
    { "secondary_distortion_type", 0x02010d, 0.0, SynthOscillator::kNumDistortionTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Secondary Distortion Mode", strings::kPhaseDistortionNames },
    { "secondary_distortion_amount", 0x02010d, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Secondary Distortion Amount", nullptr },
    { "secondary_spectral_morph_type", 0x02010d, 0.0, SynthOscillator::kNumSpectralMorphTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Secondary Frequency Morph Mode", strings::kSpectralMorphNames },
    { "secondary_spectral_morph_amount", 0x02010d, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Secondary Frequency Morph Amount", nullptr },
    { "spectral_morph_seed", 0x020110, 0.0, RandomValues::kMaxSeed, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Random Amplitudes Seed", nullptr },
    { "secondary_spectral_morph_seed", 0x020110, 0.0, RandomValues::kMaxSeed, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Secondary Random Amplitudes Seed", nullptr },
    { "secondary_distortion_spread", 0x020115, -0.5, 0.5, 0.0, 0.0, 200.0,
      ValueDetails::kLinear, false, "%", "Secondary Distortion Spread", nullptr },
    { "secondary_spectral_morph_spread", 0x020115, -0.5, 0.5, 0.0, 0.0, 200.0,
      ValueDetails::kLinear, false, "%", "Secondary Frequency Morph Spread", nullptr },
    { "distortion_spread", 0x000000, -0.5, 0.5, 0.0, 0.0, 200.0,
      ValueDetails::kLinear, false, "%", "Distortion Spread", nullptr },
    { "spectral_morph_type", 0x000407, 0.0, SynthOscillator::kNumSpectralMorphTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Frequency Morph Type", strings::kSpectralMorphNames },
    { "spectral_morph_amount", 0x000407, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Frequency Morph Amount", nullptr },
    { "spectral_morph_spread", 0x000407, -0.5, 0.5, 0.0, 0.0, 200.0,
      ValueDetails::kLinear, false, "%", "Frequency Morph Spread", nullptr },
    { "destination", 0x000500, 0.0, constants::kNumDestinations - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Destination", strings::kDestinationNames },
    { "view_2d", 0x000402, 0.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "View 2D", nullptr },
    // Analog oscillator type, appended by version to preserve every existing host parameter ID.
    { "type", 0x020113, 0.0, source_types::kNumTypes - 1, source_types::kWavetable, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Type", strings::kOscillatorTypeNames },
    { "analog_waveform", 0x020113, 0.0, analog::kNumWaveforms - 1, analog::kSaw, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Analog Waveform", strings::kAnalogWaveformNames },
    { "analog_shape", 0x020113, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Analog Shape", nullptr },
    { "sub_level", 0x020113, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kQuadratic, false, "", "Sub Level", nullptr },
    { "sub_octave", 0x020113, 0.0, analog::kNumSubOctaves - 1, analog::kSubOctaveDown1, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sub Octave", strings::kSubOctaveNames },
    { "sub_waveform", 0x020113, 0.0, analog::kNumSubWaveforms - 1, analog::kSubSquare, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sub Waveform", strings::kSubWaveformNames },
    // Analog aging, appended by version. Every amount defaults to 0, which leaves
    // the oscillator exactly as before.
    { "analog_drift", 0x020124, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Analog Drift", nullptr },
    { "analog_voice_spread", 0x020124, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Analog Voice Spread", nullptr },
    { "analog_wear", 0x020124, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Analog Wear", nullptr },
    { "analog_noise", 0x020124, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Analog Noise", nullptr },
    { "analog_hum", 0x020124, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Analog Hum", nullptr },
    { "analog_hum_frequency", 0x020124, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Analog Hum Frequency", strings::kHumFrequencyNames },
    { "analog_sag", 0x020124, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Analog Supply Sag", nullptr },
  };

  // Sample oscillator type: registered once per oscillator slot as osc_N_sample_*.
  const ValueDetails ValueDetailsLookup::sample_slot_parameter_list[] = {
    { "sample_filter_slope", 0x020103, 0.0, 7.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Filter Slope", strings::kCutFilterSlopeNames },
    { "sample_random_phase", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Random Phase", strings::kOffOnNames },
    { "sample_keytrack", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Keytrack", strings::kOffOnNames },
    { "sample_root_key", 0x000000, 0.0, 127.0, 60.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Root Key", nullptr },
    { "sample_loop", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Loop", strings::kOffOnNames },
    { "sample_bounce", 0x000603, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Bounce", strings::kOffOnNames },
    { "sample_playback_mode", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Playback Mode", strings::kSamplePlaybackModeNames },
    { "sample_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Start", nullptr },
    { "sample_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample End", nullptr },
    { "sample_loop_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Loop Start", nullptr },
    { "sample_loop_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Loop End", nullptr },
    { "sample_loop_crossfade", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Sample Loop Crossfade", nullptr },
    { "sample_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Sample Low Cutoff", nullptr },
    { "sample_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Sample High Cutoff", nullptr },
    { "sample_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose", nullptr },
    { "sample_transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize", nullptr },
    { "sample_transpose_quantize_key", 0x000000, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize Key", strings::kTransposeQuantizeKeyNames },
    { "sample_transpose_quantize_scale", 0x000000, 0.0, 14.0, 14.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize Scale", strings::kTransposeQuantizeScaleNames },
    { "sample_transpose_quantize_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Sample Transpose Quantize Mode", strings::kTransposeQuantizeModeNames },
    { "sample_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Sample Tune", nullptr },
  };

  // Granular oscillator type: registered once per oscillator slot as osc_N_granular_*.
  const ValueDetails ValueDetailsLookup::granular_slot_parameter_list[] = {
    { "granular_keytrack", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Keytrack", strings::kOffOnNames },
    { "granular_root_key", 0x000000, 0.0, 127.0, 60.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Root Key", nullptr },
    { "granular_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Mode", strings::kGranularModeNames },
    { "granular_midi_density", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular MIDI Density", strings::kOffOnNames },
    { "granular_grain_count", 0x000000, 1.0, 64.0, 16.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Grain Count", nullptr },
    { "granular_density", 0x000000, 1.0, 120.0, 24.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " grains/s", "Granular Density", nullptr },
    { "granular_grain_size", 0x000000, 5.0, 500.0, 90.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " ms", "Granular Grain Size", nullptr },
    { "granular_speed", 0x000000, 0.05, 4.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x", "Granular Speed", nullptr },
    { "granular_position", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Position", nullptr },
    { "granular_position_mod", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Position Mod", nullptr },
    { "granular_position_mod_rate", 0x000000, 0.01, 20.0, 0.25, 0.0, 1.0,
      ValueDetails::kLinear, false, " Hz", "Granular Position Mod Rate", nullptr },
    { "granular_random_position", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Random Position", nullptr },
    { "granular_random_volume", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Random Volume", nullptr },
    { "granular_random_pan", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Random Pan", nullptr },
    { "granular_random_pitch", 0x000000, 0.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " st", "Granular Random Pitch", nullptr },
    { "granular_interval", 0x000000, -24.0, 24.0, 12.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, " st", "Granular Interval", nullptr },
    { "granular_interval_chance", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Interval Chance", nullptr },
    { "granular_direction", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Direction", strings::kGranularDirectionNames },
    { "granular_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Start", nullptr },
    { "granular_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular End", nullptr },
    { "granular_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Granular Low Cutoff", nullptr },
    { "granular_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Granular High Cutoff", nullptr },
    { "granular_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose", nullptr },
    { "granular_transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize", nullptr },
    { "granular_transpose_quantize_key", 0x000000, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize Key", strings::kTransposeQuantizeKeyNames },
    { "granular_transpose_quantize_scale", 0x000000, 0.0, 14.0, 14.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize Scale", strings::kTransposeQuantizeScaleNames },
    { "granular_transpose_quantize_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Transpose Quantize Mode", strings::kTransposeQuantizeModeNames },
    { "granular_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Granular Tune", nullptr },
    { "granular_window_shape", 0x020111, 0.0, GranularSource::kNumWindowShapes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Window Shape", strings::kGranularWindowNames },
    { "granular_timing_jitter", 0x020111, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Granular Timing Jitter", nullptr },
    { "granular_timing_mode", 0x020111, 0.0, GranularSource::kNumTimingModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Timing", strings::kGranularTimingNames },
    { "granular_grid_rate", 0x020111, 0.0, GranularSource::kNumGridRates - 1, 4.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Grid Rate", strings::kGranularGridRateNames },
    { "granular_size_mode", 0x020111, 0.0, GranularSource::kNumSizeModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Size Mode", strings::kGranularSizeModeNames },
    { "granular_grid_length", 0x020111, 0.05, 4.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x step", "Granular Grid Length", nullptr },
    { "granular_position_grid", 0x020111, 0.0, GranularSource::kNumPositionGrids - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Granular Position Grid", strings::kGranularPositionGridNames },
  };

  // Spectral oscillator type: registered once per oscillator slot as osc_N_spectral_*.
  const ValueDetails ValueDetailsLookup::spectral_slot_parameter_list[] = {
    { "spectral_keytrack", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Keytrack", strings::kOffOnNames },
    { "spectral_root_key", 0x000000, 0.0, 127.0, 60.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Root Key", nullptr },
    { "spectral_play_mode", 0x000000, 0.0, SpectralSource::kNumPlayModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Play Mode", strings::kSpectralPlayModeNames },
    { "spectral_position", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Position", nullptr },
    { "spectral_speed", 0x000000, -2.0, 2.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x", "Spectral Speed", nullptr },
    { "spectral_loop_start", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Loop Start", nullptr },
    { "spectral_loop_end", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Loop End", nullptr },
    { "spectral_bin_low", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Bin Low", nullptr },
    { "spectral_bin_high", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Bin High", nullptr },
    { "spectral_shift", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Shift", nullptr },
    { "spectral_stretch", 0x000000, 0.25, 4.0, 1.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "x", "Spectral Stretch", nullptr },
    { "spectral_blur", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Blur", nullptr },
    { "spectral_tilt", 0x000000, -24.0, 24.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB/oct", "Spectral Tilt", nullptr },
    { "spectral_phase_randomize", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Phase Randomize", nullptr },
    { "spectral_freeze", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Freeze", strings::kOffOnNames },
    { "spectral_keep_fundamental", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Keep Fundamental", strings::kOffOnNames },
    { "spectral_phase_mode", 0x000000, 0.0, SpectralSource::kNumPhaseModes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Phase Mode", strings::kSpectralPhaseModeNames },
    { "spectral_preserve_transients", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Preserve Transients", strings::kOffOnNames },
    { "spectral_effect", 0x000000, 0.0, SpectralSource::kNumSpectralEffects - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Effect", strings::kSpectralEffectNames },
    { "spectral_effect_amount", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Spectral Effect Amount", nullptr },
    { "spectral_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose", nullptr },
    { "spectral_transpose_quantize", 0x000000, 0.0, 8191.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize", nullptr },
    { "spectral_transpose_quantize_key", 0x000000, 0.0, 11.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize Key", strings::kTransposeQuantizeKeyNames },
    { "spectral_transpose_quantize_scale", 0x000000, 0.0, 14.0, 14.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize Scale", strings::kTransposeQuantizeScaleNames },
    { "spectral_transpose_quantize_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Spectral Transpose Quantize Mode", strings::kTransposeQuantizeModeNames },
    { "spectral_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Spectral Tune", nullptr },
  };

  // Noise oscillator type: registered once per oscillator slot as osc_N_noise_*.
  const ValueDetails ValueDetailsLookup::noise_slot_parameter_list[] = {
    { "noise_type", 0x000000, 0.0, NoiseSource::kNumNoiseTypes - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Type", strings::kNoiseTypeNames },
    { "noise_width", 0x000000, 0.0, 1.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Noise Width", nullptr },
    { "noise_low_cutoff", 0x000000, 8.0, 136.0, 8.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Noise Low Cutoff", nullptr },
    { "noise_high_cutoff", 0x000000, 8.0, 136.0, 136.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Noise High Cutoff", nullptr },
    { "noise_filter_slope", 0x000000, 0.0, 7.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Noise Filter Slope", strings::kCutFilterSlopeNames },
  };

  // Multi-sample oscillator type (SFZ / SF2): registered once per slot as osc_N_multisample_*.
  // The instrument itself is preset state, not a parameter.
  const ValueDetails ValueDetailsLookup::multisample_slot_parameter_list[] = {
    { "multisample_amp_env", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Multi-sample Amplitude Envelope", strings::kMultiSampleAmpEnvNames },
    { "multisample_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Multi-sample Transpose", nullptr },
    { "multisample_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Multi-sample Tune", nullptr },
  };

  // Braids oscillator type (Mutable Instruments macro-oscillator): registered
  // once per slot as osc_N_braids_*. Timbre and Color mean something different
  // in each mode; the editor names them after the mode.
  const ValueDetails ValueDetailsLookup::braids_slot_parameter_list[] = {
    { "braids_model", 0x000000, 0.0, braids_engine::kNumModels - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Braids Synthesis Mode", strings::kBraidsModelNames },
    { "braids_timbre", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Braids Timbre", nullptr },
    { "braids_color", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Braids Color", nullptr },
    { "braids_resolution", 0x000000, 0.0, braids_engine::kNumResolutions - 1, braids_engine::kFullResolution,
      0.0, 1.0, ValueDetails::kIndexed, false, "", "Braids Bit Depth", strings::kBraidsResolutionNames },
    { "braids_sample_rate", 0x000000, 0.0, braids_engine::kNumSampleRates - 1, braids_engine::kFullSampleRate,
      0.0, 1.0, ValueDetails::kIndexed, false, "", "Braids Sample Rate", strings::kBraidsSampleRateNames },
    { "braids_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Braids Transpose", nullptr },
    { "braids_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Braids Tune", nullptr },
    // Off by default: every note from silence starts its cycle at zero.
    { "braids_random_phase", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Braids Phase Randomization", nullptr },
  };

  // OpenWurli oscillator type (the Wurlitzer 200A reed model of OpenWurli):
  // registered once per slot as osc_N_wurli_*. The amplifier half of the
  // instrument is the OpenWurli Amp effect.
  const ValueDetails ValueDetailsLookup::wurli_slot_parameter_list[] = {
    // Reed: the reed's own decay and damper shape each note and hold the voice open.
    { "wurli_amp_env", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenWurli Amplitude Envelope", strings::kOpenWurliAmpEnvNames },
    // Off by default, as in OpenWurli.
    { "wurli_mlp", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenWurli MLP Corrections", strings::kOffOnNames },
    // Voicing, read when a key is struck. Every default is OpenWurli's calibration.
    // Bark scales the reeds' swing at the pickup by 2^(2 x bark - 1), half to double.
    { "wurli_bark", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenWurli Bark", nullptr },
    // Air drag on the reed: 45% to 224% is its plausible band, 0% none.
    { "wurli_damping", 0x000000, 0.0, 2.5, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenWurli Air Damping", nullptr },
    // Per-reed tuning spread, +-3 cents at 0% to +-14 cents at 100%.
    { "wurli_condition", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenWurli Condition", nullptr },
    // Level change at the top key (C7), tapering to none at middle C.
    { "wurli_treble_voicing", 0x000000, -12.0, 12.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " dB", "OpenWurli Treble Voicing", nullptr },
    { "wurli_register_balance", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenWurli Register Balance", strings::kOffOnNames },
    { "wurli_pickup_low_cut", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenWurli Pickup Low Cut", strings::kOpenWurliPickupLowCutNames },
    { "wurli_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenWurli Transpose", nullptr },
    { "wurli_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "OpenWurli Tune", nullptr },
  };

  // OpenB3 oscillator type (OpenB3's tonewheel organ, setBfree's tonegen.c):
  // registered once per slot as osc_N_b3_*. The scanner vibrato and the
  // rotary speaker are the Scanner Vibrato effect and Analogue Rotary's
  // OpenB3 Whirl model. Every default is OpenB3's.
  const ValueDetails ValueDetailsLookup::b3_slot_parameter_list[] = {
    // Organ: the key contacts shape each note and hold the voice open.
    { "b3_amp_env", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Amplitude Envelope", strings::kOpenB3AmpEnvNames },
    // The upper manual's drawbars, OpenB3's 88 6000 000.
    { "b3_drawbar_16", 0x000000, 0.0, 8.0, 8.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 16 Foot", nullptr },
    { "b3_drawbar_5_1_3", 0x000000, 0.0, 8.0, 8.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 5 1/3 Foot", nullptr },
    { "b3_drawbar_8", 0x000000, 0.0, 8.0, 6.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 8 Foot", nullptr },
    { "b3_drawbar_4", 0x000000, 0.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 4 Foot", nullptr },
    { "b3_drawbar_2_2_3", 0x000000, 0.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 2 2/3 Foot", nullptr },
    { "b3_drawbar_2", 0x000000, 0.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 2 Foot", nullptr },
    { "b3_drawbar_1_3_5", 0x000000, 0.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 1 3/5 Foot", nullptr },
    { "b3_drawbar_1_1_3", 0x000000, 0.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 1 1/3 Foot", nullptr },
    { "b3_drawbar_1", 0x000000, 0.0, 8.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Drawbar 1 Foot", nullptr },
    // Percussion: off, Normal, Fast, Third, as setBfree starts.
    { "b3_percussion", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Percussion", strings::kOffOnNames },
    { "b3_percussion_volume", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Percussion Volume", strings::kOpenB3PercussionVolumeNames },
    { "b3_percussion_decay", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Percussion Decay", strings::kOpenB3PercussionDecayNames },
    { "b3_percussion_harmonic", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Percussion Harmonic", strings::kOpenB3PercussionHarmonicNames },
    // The contacts as a key closes and opens: 0% a clean fade, 50% setBfree's
    // click model (osc.attack.click.level 0.5, osc.release.click.level 0.25),
    // 100% worn contacts chattering for up to about 10 ms.
    { "b3_key_click", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenB3 Key Click", nullptr },
    { "b3_release_click", 0x000000, 0.0, 1.0, 0.5, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "OpenB3 Release Click", nullptr },
    { "b3_temperament", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Tuning", strings::kOpenB3TemperamentNames },
    { "b3_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "OpenB3 Transpose", nullptr },
    { "b3_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "OpenB3 Tune", nullptr },
  };

  // Speech oscillator type (espeak-ng): registered once per slot as
  // osc_N_speech_*. The text it sings is slot state, not a parameter; these
  // choose the voice espeak-ng renders it with.
  const ValueDetails ValueDetailsLookup::speech_slot_parameter_list[] = {
    { "speech_language", 0x000000, 0.0, speech::kNumLanguages - 1, speech::kDefaultLanguage, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Speech Language", strings::kSpeechLanguageNames },
    { "speech_variant", 0x000000, 0.0, speech::kNumVariants - 1, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Speech Variant", strings::kSpeechVariantNames },
    // Voice: each syllable shapes its note and sings to its last consonant
    // after release. Envelope 1: the slot follows Env 1.
    { "speech_amp_env", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Speech Amplitude Envelope", strings::kSpeechAmpEnvNames },
    { "speech_transpose", 0x000000, -48.0, 48.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Speech Transpose", nullptr },
    { "speech_tune", 0x000000, -1.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "", "Speech Tune", nullptr },
    // Sung: a monotone the notes give the melody, the vowel held while the
    // key is down. Spoken: espeak-ng's own intonation, played through.
    { "speech_mode", 0x000000, 0.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Speech Mode", strings::kSpeechModeNames },
    // What each note takes: the next syllable, word or the whole text.
    { "speech_split", 0x000000, 0.0, 2.0, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Speech Split", strings::kSpeechSplitNames },
    // Moves the voice's formants, in semitones; the pitch stays. Changing the
    // variant sets it back to 0.
    { "speech_formant", 0x000000, -12.0, 12.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, " semitones", "Speech Formant", nullptr },
    // 0% the voice as espeak-ng spoke it; 100% a whisper. Changing the
    // variant sets it back to 0%.
    { "speech_breath", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Speech Breathiness", nullptr },
    // How fast consonants and unheld vowels play, 0% to 400%; 100% is 65% of
    // espeak-ng's speaking speed (SpeechSource::kBaseSpeed).
    { "speech_speed", 0x000000, 0.0, 4.0, 1.0, 0.0, 100.0,
      ValueDetails::kLinear, false, "%", "Speech Speed", nullptr },
    // A vibrato that fades in after each note starts: depth in semitones,
    // rate as log2 Hz (1 to 12 Hz).
    { "speech_vibrato", 0x000000, 0.0, 1.0, 0.0, 0.0, 100.0,
      ValueDetails::kLinear, false, " cents", "Speech Vibrato Depth", nullptr },
    { "speech_vibrato_rate", 0x000000, 0.0, 3.5849625, 2.4594316, 0.0, 1.0,
      ValueDetails::kExponential, false, " Hz", "Speech Vibrato Rate", nullptr },
  };

  const ValueDetails ValueDetailsLookup::mod_parameter_list[] = {
    { "amount", 0x000000, -1.0, 1.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Amount", nullptr },
    { "power", 0x000000, -10.0, 10.0, 0.0, 0.0, 1.0,
      ValueDetails::kLinear, false, "", "Power", nullptr },
    { "bipolar", 0x000000, 0.0, 1.0f, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Bipolar", strings::kOffOnNames },
    { "stereo", 0x000000, 0.0, 1.0f, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Stereo", strings::kOffOnNames },
    { "bypass", 0x000000, 0.0, 1.0f, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Bypass", strings::kOffOnNames },
    { "random_mode", 0x000000, 0.0, 1.0f, 0.0, 0.0, 1.0,
      ValueDetails::kIndexed, false, "", "Random Correlation", strings::kRandomCorrelationNames }
  };

  ValueDetailsLookup::ValueDetailsLookup() {
    static constexpr int kNumOscillatorsOld = 2;
    static constexpr int kNumEnvelopesOld = 6;
    static constexpr int kNumLfosOld = 8;
    static constexpr int kNewOscillatorVersion = 0x000500;
    static constexpr int kNumOscillatorsBeforeSlots = 4;
    static constexpr int kSourceSlotVersion = 0x020114;
    static constexpr int kNewExpandedModulatorVersion = 0x020000;
    static constexpr int kOldMaxModulations = 32;
    static constexpr int kNewModulationVersion = 0x000601;

    int num_parameters = sizeof(parameter_list) / sizeof(ValueDetails);
    for (int i = 0; i < num_parameters; ++i) {
      details_lookup_[parameter_list[i].name] = parameter_list[i];
      details_list_.push_back(&parameter_list[i]);

      VITAL_ASSERT(parameter_list[i].default_value <= parameter_list[i].max);
      VITAL_ASSERT(parameter_list[i].default_value >= parameter_list[i].min);
    }

    // The Mid Side EQ hosts two full equalizers, so its parameters are the
    // regular EQ band parameters cloned for the mid and side channels.
    static const std::vector<ValueDetails> mid_side_eq_parameters = [&] {
      std::vector<ValueDetails> result;
      static const std::pair<const char*, const char*> channels[] = {
        { "mid_side_eq_mid_", "Mid Side EQ Mid " },
        { "mid_side_eq_side_", "Mid Side EQ Side " }
      };
      for (int i = 0; i < num_parameters; ++i) {
        const std::string& base_name = parameter_list[i].name;
        if (base_name.rfind("eq_", 0) != 0 || base_name == "eq_on")
          continue;

        for (const auto& channel : channels) {
          ValueDetails details = parameter_list[i];
          details.name = channel.first + base_name;
          details.version_added = 0x020100;
          std::string display = details.display_name;
          if (display.rfind("EQ ", 0) == 0)
            display = display.substr(3);
          details.display_name = channel.second + display;
          result.push_back(details);
        }
      }
      return result;
    }();

    for (const ValueDetails& details : mid_side_eq_parameters) {
      details_lookup_[details.name] = details;
      details_list_.push_back(&details_lookup_[details.name]);
    }

    auto is_bus_effect_parameter = [](const std::string& name) {
      if (name == "effect_chain_order" || name == "post_effect_order" ||
          name.rfind("effect_chain_slot_", 0) == 0)
        return true;

      static const std::string effect_prefixes[] = {
        "chorus_", "compressor_", "delay_", "distortion_", "eq_", "flanger_",
        "phaser_", "reverb_", "utility_", "phase_shift_", "limiter_", "frequency_shifter_",
        "dimension_expander_", "waveshaper_", "convolution_reverb_", "mid_side_eq_", "spectral_gate_",
        "erosion_", "floaty_delay_", "spectral_compressor_", "scream_", "tremolo_pan_", "tape_", "defuse_", "suppressor_", "trance_gate_", "spectral_lock_", "resonator_",
        "analoguechorus_", "analogueflanger_", "analogueecho_",
        "analoguerotary_", "analoguespring_", "analoguewah_", "spectral_balance_", "glitch_delay_",
        "openwurli_amp_", "scanner_vibrato_"
      };
      for (const auto& prefix : effect_prefixes) {
        if (name.rfind(prefix, 0) == 0)
          return true;
      }
      return false;
    };

    for (int bus = 0; bus < kNumBuses; ++bus) {
      const std::string id_start = "bus_" + std::to_string(bus + 1) + "_";
      const std::string name_start = "Bus " + std::to_string(bus + 1) + " ";
      for (int i = 0; i < num_parameters; ++i) {
        if (!is_bus_effect_parameter(parameter_list[i].name))
          continue;

        ValueDetails details = parameter_list[i];
        details.name = id_start + details.name;
        details.local_description = details.display_name;
        details.display_name = name_start + details.display_name;
        details_lookup_[details.name] = details;
        details_list_.push_back(&details_lookup_[details.name]);
      }

      for (const ValueDetails& base_details : mid_side_eq_parameters) {
        ValueDetails details = base_details;
        details.name = id_start + details.name;
        details.local_description = details.display_name;
        details.display_name = name_start + details.display_name;
        details_lookup_[details.name] = details;
        details_list_.push_back(&details_lookup_[details.name]);
      }
    }

    auto add_internal_effect_slot_details = [&](const std::string& chain_prefix, const std::string& chain_name) {
      for (int slot = 0; slot < constants::kMaxEffectSlots; ++slot) {
        const std::string slot_id = chain_prefix + "fx_slot_" + std::to_string(slot + 1) + "_";
        const std::string slot_name = chain_name + "FX Slot " + std::to_string(slot + 1) + " ";

        ValueDetails type_details = { slot_id + "type", 0x000000, 0.0, constants::kNumEffects, 0.0, 0.0, 1.0,
                                      ValueDetails::kIndexed, false, "", slot_name + "Type",
                                      strings::kEffectSlotTypeNames };
        details_lookup_[type_details.name] = type_details;

        ValueDetails enabled_details = { slot_id + "enabled", 0x000000, 0.0, 1.0, 1.0, 0.0, 1.0,
                                         ValueDetails::kIndexed, false, "", slot_name + "Enabled",
                                         strings::kOffOnNames };
        details_lookup_[enabled_details.name] = enabled_details;

        for (int i = 0; i < num_parameters; ++i) {
          if (!is_bus_effect_parameter(parameter_list[i].name) ||
              parameter_list[i].name == "effect_chain_order" ||
              parameter_list[i].name == "post_effect_order" ||
              parameter_list[i].name.rfind("effect_chain_slot_", 0) == 0)
            continue;

          ValueDetails details = parameter_list[i];
          details.name = slot_id + details.name;
          details.local_description = details.display_name;
          details.display_name = slot_name + details.display_name;
          details_lookup_[details.name] = details;
        }

        const int num_filter_parameters = sizeof(filter_parameter_list) / sizeof(ValueDetails);
        for (int i = 0; i < num_filter_parameters; ++i) {
          ValueDetails details = filter_parameter_list[i];
          details.name = slot_id + "filter_fx_" + details.name;
          details.local_description = details.display_name;
          details.display_name = slot_name + "Filter " + details.display_name;
          details_lookup_[details.name] = details;
        }

        for (const ValueDetails& base_details : mid_side_eq_parameters) {
          ValueDetails details = base_details;
          details.name = slot_id + details.name;
          details.local_description = details.display_name;
          details.display_name = slot_name + details.display_name;
          details_lookup_[details.name] = details;
        }
      }
    };

    add_internal_effect_slot_details("", "");
    for (int bus = 0; bus < kNumBuses; ++bus) {
      add_internal_effect_slot_details("bus_" + std::to_string(bus + 1) + "_",
                                       "Bus " + std::to_string(bus + 1) + " ");
    }

    // Envelope 1 alone has an analog mode, appended by version.
    static const ValueDetails env_1_analog_parameters[] = {
      { "analog", 0x020124, 0.0, 1.0, 0.0, 0.0, 1.0,
        ValueDetails::kIndexed, false, "", "Analog", strings::kOffOnNames },
      { "analog_age", 0x020124, 0.0, 1.0, 0.0, 0.0, 100.0,
        ValueDetails::kLinear, false, "%", "Analog Age", nullptr },
    };
    addParameterGroup(env_1_analog_parameters, sizeof(env_1_analog_parameters) / sizeof(ValueDetails), 0,
                      kEnvIdPrefix, kEnvNamePrefix);

    int num_env_parameters = sizeof(env_parameter_list) / sizeof(ValueDetails);
    for (int env = 0; env < std::min(kNumEnvelopesOld, kNumEnvelopes); ++env)
      addParameterGroup(env_parameter_list, num_env_parameters, env, kEnvIdPrefix, kEnvNamePrefix);
    for (int env = kNumEnvelopesOld; env < kNumEnvelopes; ++env)
      addParameterGroup(env_parameter_list, num_env_parameters, env, kEnvIdPrefix, kEnvNamePrefix,
                        kNewExpandedModulatorVersion);

    int num_lfo_parameters = sizeof(lfo_parameter_list) / sizeof(ValueDetails);
    for (int lfo = 0; lfo < std::min(kNumLfosOld, kNumLfos); ++lfo)
      addParameterGroup(lfo_parameter_list, num_lfo_parameters, lfo, kLfoIdPrefix, kLfoNamePrefix);
    for (int lfo = kNumLfosOld; lfo < kNumLfos; ++lfo)
      addParameterGroup(lfo_parameter_list, num_lfo_parameters, lfo, kLfoIdPrefix, kLfoNamePrefix,
                        kNewExpandedModulatorVersion);

    int num_random_lfo_parameters = sizeof(random_lfo_parameter_list) / sizeof(ValueDetails);
    for (int lfo = 0; lfo < kNumRandomLfos; ++lfo)
      addParameterGroup(random_lfo_parameter_list, num_random_lfo_parameters, lfo, kRandomIdPrefix, kRandomNamePrefix);

    int num_osc_parameters = sizeof(osc_parameter_list) / sizeof(ValueDetails);
    for (int osc = 0; osc < kNumOscillatorsOld; ++osc)
      addParameterGroup(osc_parameter_list, num_osc_parameters, osc, kOscIdPrefix, kOscNamePrefix);
    for (int osc = kNumOscillatorsOld; osc < kNumOscillators; ++osc) {
      addParameterGroup(osc_parameter_list, num_osc_parameters, osc, kOscIdPrefix, kOscNamePrefix,
                        osc < kNumOscillatorsBeforeSlots ? kNewOscillatorVersion : kSourceSlotVersion);
    }
    // Every oscillator slot owns every oscillator type's parameters.
    struct SlotTypeList { const ValueDetails* list; int count; };
    const SlotTypeList slot_type_lists[] = {
      { sample_slot_parameter_list, static_cast<int>(sizeof(sample_slot_parameter_list) / sizeof(ValueDetails)) },
      { granular_slot_parameter_list, static_cast<int>(sizeof(granular_slot_parameter_list) / sizeof(ValueDetails)) },
      { spectral_slot_parameter_list, static_cast<int>(sizeof(spectral_slot_parameter_list) / sizeof(ValueDetails)) },
      { noise_slot_parameter_list, static_cast<int>(sizeof(noise_slot_parameter_list) / sizeof(ValueDetails)) },
    };
    for (int osc = 0; osc < kNumOscillators; ++osc) {
      for (const auto& type_list : slot_type_lists)
        addParameterGroup(type_list.list, type_list.count, osc, kOscIdPrefix, kOscNamePrefix, kSourceSlotVersion);
    }
    // Added after the slot types: a later version keeps existing host parameter ids.
    static constexpr int kMultiSampleVersion = 0x020119;
    const int num_multisample_parameters = sizeof(multisample_slot_parameter_list) / sizeof(ValueDetails);
    for (int osc = 0; osc < kNumOscillators; ++osc) {
      addParameterGroup(multisample_slot_parameter_list, num_multisample_parameters, osc, kOscIdPrefix,
                        kOscNamePrefix, kMultiSampleVersion);
    }
    static constexpr int kBraidsVersion = 0x02011a;
    const int num_braids_parameters = sizeof(braids_slot_parameter_list) / sizeof(ValueDetails);
    for (int osc = 0; osc < kNumOscillators; ++osc) {
      addParameterGroup(braids_slot_parameter_list, num_braids_parameters, osc, kOscIdPrefix,
                        kOscNamePrefix, kBraidsVersion);
    }
    static constexpr int kOpenWurliVersion = 0x02011c;
    const int num_wurli_parameters = sizeof(wurli_slot_parameter_list) / sizeof(ValueDetails);
    for (int osc = 0; osc < kNumOscillators; ++osc) {
      addParameterGroup(wurli_slot_parameter_list, num_wurli_parameters, osc, kOscIdPrefix,
                        kOscNamePrefix, kOpenWurliVersion);
    }
    static constexpr int kOpenB3Version = 0x02011d;
    const int num_b3_parameters = sizeof(b3_slot_parameter_list) / sizeof(ValueDetails);
    for (int osc = 0; osc < kNumOscillators; ++osc) {
      addParameterGroup(b3_slot_parameter_list, num_b3_parameters, osc, kOscIdPrefix,
                        kOscNamePrefix, kOpenB3Version);
    }
    static constexpr int kSpeechVersion = 0x020126;
    const int num_speech_parameters = sizeof(speech_slot_parameter_list) / sizeof(ValueDetails);
    for (int osc = 0; osc < kNumOscillators; ++osc) {
      addParameterGroup(speech_slot_parameter_list, num_speech_parameters, osc, kOscIdPrefix,
                        kOscNamePrefix, kSpeechVersion);
    }

    int num_filter_parameters = sizeof(filter_parameter_list) / sizeof(ValueDetails);
    for (int filter = 0; filter < kNumFilters; ++filter)
      addParameterGroup(filter_parameter_list, num_filter_parameters, filter, kFilterIdPrefix, kFilterNamePrefix);

    addParameterGroup(filter_parameter_list, num_filter_parameters, "fx", kFilterIdPrefix, kFilterNamePrefix);
    for (int bus = 0; bus < kNumBuses; ++bus) {
      addParameterGroup(filter_parameter_list, num_filter_parameters, "fx",
                        "bus_" + std::to_string(bus + 1) + "_" + kFilterIdPrefix,
                        "Bus " + std::to_string(bus + 1) + " " + kFilterNamePrefix);
    }

    int num_mod_parameters = sizeof(mod_parameter_list) / sizeof(ValueDetails);
    for (int modulation = 0; modulation < kOldMaxModulations; ++modulation) {
      addParameterGroup(mod_parameter_list, num_mod_parameters, modulation,
                        kModulationIdPrefix, kModulationNamePrefix);
    }
    for (int modulation = kOldMaxModulations; modulation < kMaxModulationConnections; ++modulation) {
      addParameterGroup(mod_parameter_list, num_mod_parameters, modulation,
        kModulationIdPrefix, kModulationNamePrefix, kNewModulationVersion);
    }

    details_lookup_["osc_1_on"].default_value = 1.0f;
    details_lookup_["osc_2_destination"].default_value = 1.0f;
    details_lookup_["osc_3_destination"].default_value = 3.0f;
    details_lookup_["osc_4_destination"].default_value = 3.0f;
    // Every slot defaults to Wavetable. Slots 5-8, which replaced the Sample, Granular,
    // Spectral and Noise sources, keep their effects destination. Presets that predate
    // this and lack a slot's type load those sources (LoadSave::migrateSlotTypeLevels).
    for (int i = 0; i < source_types::kNumLegacySources; ++i) {
      const std::string prefix = source_types::slotPrefix(source_types::kFirstLegacySourceSlot + i);
      details_lookup_[prefix + "destination"].default_value = 3.0f;
    }
    details_lookup_["filter_1_osc1_input"].default_value = 1.0f;
    details_lookup_["filter_2_osc2_input"].default_value = 1.0f;

    std::sort(details_list_.begin(), details_list_.end(), compareValueDetails);
  }

  void ValueDetailsLookup::addParameterGroup(const ValueDetails* list, int num_parameters, int index,
                                             std::string id_prefix, std::string name_prefix, int version) {
    std::string string_num = std::to_string(index + 1);
    addParameterGroup(list, num_parameters, string_num, id_prefix, name_prefix, version);
  }

  void ValueDetailsLookup::addParameterGroup(const ValueDetails* list, int num_parameters, std::string id,
                                             std::string id_prefix, std::string name_prefix, int version) {
    std::string id_start = id_prefix + kIdDelimiter + id + kIdDelimiter;
    std::string name_start = name_prefix + kNameDelimiter + id + kNameDelimiter;

    for (int i = 0; i < num_parameters; ++i) {
      ValueDetails details = list[i];
      if (version > details.version_added)
        details.version_added = version;

      details.name = id_start + details.name;
      details.local_description = details.display_name;
      details.display_name = name_start + details.display_name;
      details_lookup_[details.name] = details;
      details_list_.push_back(&details_lookup_[details.name]);
    }
  }

  ValueDetailsLookup Parameters::lookup_;

} // namespace vital

static_assert(std::size(strings::kTapeNoiseNames) == vital::tape::kNumNoiseSounds,
              "strings::kTapeNoiseNames names every Tape noise recording");
