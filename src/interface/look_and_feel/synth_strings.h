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

#include <string>
#include "synth_constants.h"

namespace strings {
  const std::string kReleaseVelocityModeNames[] = { "Instant", "Glide" };
  const std::string kReleaseVelocityTargetNames[] = { "MIDI release velocity", "Full amount" };
  const std::string kOffOnNames[] = {
    "Off",
    "On"
  };

  const std::string kRandomCorrelationNames[] = {
    "Shared",
    "Independent"
  };

  const std::string kSamplePlaybackModeNames[] = {
    "Retrigger per note",
    "Continuous global",
    "Continuous per note"
  };

  const std::string kGranularModeNames[] = {
    "Play-through",
    "Manual"
  };

  const std::string kGranularWindowNames[] = {
    "Smooth (Hann)",
    "Soft Plateau",
    "Triangle",
    "Gaussian",
    "Percussive",
    "Reverse Percussive"
  };

  const std::string kGranularTimingNames[] = {
    "Free (grains per second)",
    "Tempo Grid",
    "Tempo Grid Dotted",
    "Tempo Grid Triplet"
  };

  const std::string kGranularGridRateNames[] = {
    "1/1",
    "1/2",
    "1/4",
    "1/8",
    "1/16",
    "1/32",
    "1/64",
    "1/128"
  };

  const std::string kGranularSizeModeNames[] = {
    "Milliseconds",
    "Grid Steps"
  };

  const std::string kGranularPositionGridNames[] = {
    "Off",
    "2 Slices",
    "4 Slices",
    "8 Slices",
    "16 Slices",
    "32 Slices",
    "64 Slices",
    "128 Slices"
  };

  const std::string kGranularDirectionNames[] = {
    "Forward",
    "Reverse",
    "Random"
  };

  const std::string kSpectralPlayModeNames[] = {
    "Manual",
    "Forward",
    "Reverse",
    "Loop",
    "Bounce"
  };

  const std::string kSpectralPhaseModeNames[] = {
    "Vocoder",
    "Locked",
    "Peak Locked"
  };

  const std::string kSpectralEffectNames[] = {
    "None",
    "Gate",
    "Compress",
    "Expand",
    "Comb",
    "Detune",
    "Spread",
    "Shaped Tone"
  };

  const std::string kPostEffectOrderNames[] = {
    "Frequency Shifter then Limiter",
    "Limiter then Frequency Shifter"
  };

  const std::string kTransposeQuantizeKeyNames[] = {
    "C",
    "C Sharp",
    "D",
    "E Flat",
    "E",
    "F",
    "F Sharp",
    "G",
    "A Flat",
    "A",
    "B Flat",
    "B"
  };

  const std::string kTransposeQuantizeScaleNames[] = {
    "Off",
    "Chromatic",
    "Major",
    "Natural Minor",
    "Harmonic Minor",
    "Melodic Minor",
    "Major Pentatonic",
    "Minor Pentatonic",
    "Blues",
    "Dorian",
    "Phrygian",
    "Lydian",
    "Mixolydian",
    "Locrian",
    "Custom"
  };

  const std::string kTransposeQuantizeModeNames[] = {
    "Local",
    "Global"
  };

  const std::string kWavetableDimensionNames[] = {
    "3D",
    "2D",
    "SP"
  };

  const std::string kOversamplingNames[] = {
    "1x",
    "2x",
    "4x",
    "8x"
  };

  // Floaty stores integer thirty-second notes; keep all original durations.
  const std::string kFloatySyncNames[] = {
    "Free", "1/32", "1/16", "1/16d", "1/8", "5/32", "1/8d", "7/32",
    "1/4", "9/32", "5/16", "11/32", "1/4d", "13/32", "7/16", "15/32",
    "1/2", "17/32", "9/16", "19/32", "5/8", "21/32", "11/16", "23/32",
    "1/2d", "25/32", "13/16", "27/32", "7/8", "29/32", "15/16", "31/32",
    "1/1", "33/32", "17/16", "35/32", "9/8", "37/32", "19/16", "39/32",
    "5/4", "41/32", "21/16", "43/32", "11/8", "45/32", "23/16", "47/32",
    "1/1d", "49/32", "25/16", "51/32", "13/8", "53/32", "27/16", "55/32",
    "7/4", "57/32", "29/16", "59/32", "15/8", "61/32", "31/16", "63/32",
    "2/1",
  };

  const std::string kDelayStyleNames[] = {
    "Mono",
    "Stereo",
    "Ping Pong R-L",
    "Mid Ping Pong",
    "Ping Pong L-R",
  };

  const std::string kCompressorBandNames[] = {
    "Multiband",
    "Low Band",
    "High Band",
    "Single Band"
  };

  const std::string kCompressorBandShortNames[] = {
    "Multiband",
    "Low Band",
    "High Band",
    "Single"
  };

  const std::string kPresetStyleNames[] = {
    "Bass",
    "Lead",
    "Keys",
    "Pad",
    "Percussion",
    "Sequence",
    "Experiment",
    "SFX",
    "Template",
  };

  const std::string kOscillatorTypeNames[] = {
    "Wavetable",
    "Analog",
    "Sample",
    "Granular",
    "Spectral",
    "Noise",
    "Multi-sample",
    "Braids",
    "OpenWurli",
    "OpenB3",
    "Speech",
  };

  // The Speech oscillator's espeak-ng languages and voice variants, in the
  // order of src/common/speech_voices.inc (presets store the positions).
  const std::string kSpeechLanguageNames[] = {
#define SPEECH_LANGUAGE(name, identifier) name,
#define SPEECH_VARIANT(name, identifier)
#include "speech_voices.inc"
#undef SPEECH_LANGUAGE
#undef SPEECH_VARIANT
  };

  const std::string kSpeechVariantNames[] = {
    "Default",
#define SPEECH_LANGUAGE(name, identifier)
#define SPEECH_VARIANT(name, identifier) name,
#include "speech_voices.inc"
#undef SPEECH_LANGUAGE
#undef SPEECH_VARIANT
  };

  const std::string kMultiSampleAmpEnvNames[] = {
    "Instrument",
    "Envelope 1",
  };

  const std::string kOpenWurliAmpEnvNames[] = {
    "Reed",
    "Envelope 1",
  };

  const std::string kOpenB3AmpEnvNames[] = {
    "Organ",
    "Envelope 1",
  };

  const std::string kSpeechAmpEnvNames[] = {
    "Voice",
    "Envelope 1",
  };

  const std::string kSpeechModeNames[] = {
    "Sung",
    "Spoken",
  };

  const std::string kSpeechSplitNames[] = {
    "Syllable",
    "Word",
    "Phrase",
  };

  const std::string kOpenB3PercussionVolumeNames[] = { "Normal", "Soft" };
  const std::string kOpenB3PercussionDecayNames[] = { "Fast", "Slow" };
  const std::string kOpenB3PercussionHarmonicNames[] = { "Second", "Third" };
  // osc.temperament: the tone generator's gear ratios for 60 or 50 Hz mains, or equal temperament.
  const std::string kOpenB3TemperamentNames[] = { "Gear 60 Hz", "Gear 50 Hz", "Equal" };

  // Where C-2 returns, which sets the pickup's corner (OpenWurli's pickup.rs fork).
  const std::string kOpenWurliPickupLowCutNames[] = {
    "880 Hz",
    "1436 Hz",
  };

  // Braids synthesis modes, in firmware order (braids/settings.h).
  const std::string kBraidsModelNames[] = {
    "CS-80 sawtooth",
    "Triangle, saw and square morph",
    "Saw and square mix",
    "Sine and triangle folder",
    "Buzz",
    "Square with sub oscillator",
    "Saw with sub oscillator",
    "Square hard sync",
    "Saw hard sync",
    "Triple saw",
    "Triple square",
    "Triple triangle",
    "Triple sine",
    "Triple ring modulation",
    "Saw swarm",
    "Saw comb filter",
    "Toy",
    "Low pass digital filter",
    "Peaking digital filter",
    "Band pass digital filter",
    "High pass digital filter",
    "VOSIM formant",
    "Vowel",
    "Vowel FOF",
    "Additive harmonics",
    "FM",
    "Feedback FM",
    "Chaotic feedback FM",
    "Plucked string",
    "Bowed string",
    "Blown reed",
    "Flute",
    "Struck bell",
    "Struck drum",
    "Kick drum",
    "Cymbal",
    "Snare drum",
    "Wavetables",
    "Wave map",
    "Wave line",
    "Paraphonic wavetable chords",
    "Filtered noise",
    "Twin peaks noise",
    "Clocked noise",
    "Granular cloud",
    "Particle noise",
    "Digital modulation",
    "Question mark",
  };

  // What Timbre does in each Braids mode.
  const std::string kBraidsTimbreNames[] = {
    "Notch width",
    "Waveshape",
    "Pulse width",
    "Wavefolding",
    "Harmonics",
    "Pulse width",
    "Waveshape",
    "Sync ratio",
    "Sync ratio",
    "Second oscillator interval",
    "Second oscillator interval",
    "Second oscillator interval",
    "Second oscillator interval",
    "Second oscillator pitch",
    "Detune",
    "Comb delay",
    "Sample rate reduction",
    "Cutoff",
    "Cutoff",
    "Cutoff",
    "Cutoff",
    "First formant",
    "Vowel",
    "Vowel",
    "Peak harmonic",
    "Modulation index",
    "Modulation index",
    "Modulation index",
    "Decay",
    "Bow friction",
    "Breath pressure",
    "Breath pressure",
    "Decay",
    "Decay",
    "Decay",
    "Cutoff",
    "Tone",
    "Wave position",
    "Horizontal position",
    "Wave position",
    "Wave position",
    "Resonance",
    "Resonance",
    "Cycle length",
    "Grain density",
    "Density",
    "Symbol rate",
    "Message speed",
  };

  // What Color does in each Braids mode.
  const std::string kBraidsColorNames[] = {
    "Notch shape",
    "Brightness and fuzz",
    "Saw to square",
    "Sine to triangle",
    "Detune",
    "Sub level and octave",
    "Sub level and octave",
    "Oscillator mix",
    "Oscillator mix",
    "Third oscillator interval",
    "Third oscillator interval",
    "Third oscillator interval",
    "Third oscillator interval",
    "Third oscillator pitch",
    "High pass",
    "Comb feedback",
    "Bit flipping",
    "Waveshape",
    "Waveshape",
    "Waveshape",
    "Waveshape",
    "Second formant",
    "Formant shift",
    "Voice",
    "Spectral width",
    "Frequency ratio",
    "Frequency ratio",
    "Frequency ratio",
    "Plucking position",
    "Bow position",
    "Instrument geometry",
    "Jet delay",
    "Inharmonicity",
    "Brightness",
    "Tone",
    "Noise mix",
    "Snappiness",
    "Wavetable",
    "Vertical position",
    "Interpolation",
    "Chord",
    "Low pass to high pass",
    "Second peak frequency",
    "Quantization",
    "Pitch dispersion",
    "Frequency dispersion",
    "Data",
    "Noise and distortion",
  };

  const std::string kBraidsResolutionNames[] = {
    "2 bit",
    "3 bit",
    "4 bit",
    "6 bit",
    "8 bit",
    "12 bit",
    "16 bit",
  };

  const std::string kBraidsSampleRateNames[] = {
    "4 kHz",
    "8 kHz",
    "16 kHz",
    "24 kHz",
    "32 kHz",
    "48 kHz",
    "96 kHz",
  };

  const std::string kAnalogWaveformNames[] = {
    "Sine",
    "Triangle",
    "Saw",
    "Square",
    "White Noise",
    "Pitched Noise",
  };

  const std::string kSubOctaveNames[] = {
    "1 Octave Down",
    "2 Octaves Down",
  };

  const std::string kHumFrequencyNames[] = {
    "50 Hz",
    "60 Hz",
  };

  const std::string kSubWaveformNames[] = {
    "Sine",
    "Triangle",
    "Square",
  };

  const std::string kUnisonStackNames[] = {
    "Unison",
    "Center Drop 12",
    "Center Drop 24",
    "Octave",
    "2x Octave",
    "Power Chord",
    "2x Power Chord",
    "Major Chord",
    "Minor Chord",
    "Harmonics",
    "Odd Harmonics",
  };

  // The filter style parameter ranges 0 to 9 for host compatibility; the
  // models use the first six, and the rest read as numbered styles.
  const std::string kFilterStyleNames[] = {
    "12dB",
    "24dB",
    "Notch Blend",
    "Notch Spread",
    "B/P/N",
    "Shelving",
    "Style 7",
    "Style 8",
    "Style 9",
    "Style 10"
  };

  const std::string kDiodeStyleNames[] = {
    "Low Shelf",
    "Low Cut"
  };

  const std::string kCombStyleNames[] = {
    "Low High Comb",
    "Low High Flange+",
    "Low High Flange-",
    "Band Spread Comb",
    "Band Spread Flange+",
    "Band Spread Flange-"
  };

  const std::string kFrequencySyncNames[] = {
    "Seconds",
    "Tempo",
    "Tempo Dotted",
    "Tempo Triplets",
    "Keytrack"
  };

  const std::string kDistortionTypeShortNames[] = {
    "Soft Clip",
    "Hard Clip",
    "Lin Fold",
    "Sine Fold",
    "Bit Crush",
    "Down Samp",
  };

  const std::string kDistortionTypeNames[] = {
    "Soft Clip",
    "Hard Clip",
    "Linear Fold",
    "Sine Fold",
    "Bit Crush",
    "Down Sample",
  };

  const std::string kDistortionFilterOrderNames[] = {
    "None",
    "Pre",
    "Post",
  };


  const std::string kFilterModelNames[] = {
    "Analog",
    "Dirty",
    "Ladder",
    "Digital",
    "Diode",
    "Formant",
    "Comb",
    "Phaser",
    "Reverb",
    "Scream",
    "Ring Mod",
    "Sample & Hold",
    "Wavetable",
    "Distortion",
  };

  // Wavetable filter phase modes: Filter FX offers all four, the voice filters
  // Minimum and Raw (the zero-latency ones).
  const std::string kFilterTableStyleNames[] = { "Minimum", "Linear", "Original", "Raw" };
  const std::string kFilterTableVoiceStyleNames[] = { "Minimum", "Raw" };

  const std::string kRingModStyleNames[] = {
    "Sine",
    "Square",
    "Saw",
    "Triangle",
  };

  const std::string kSampleHoldStyleNames[] = {
    "Hold",
    "Smooth",
    "Inverted",
  };

  const std::string kPredefinedWaveformNames[] = {
    "Sin",
    "Saturated Sin",
    "Triangle",
    "Square",
    "Pulse",
    "Saw",
  };

  const std::string kEffectOrder[] = {
    "chorus",
    "compressor",
    "delay",
    "distortion",
    "eq",
    "filter_fx",
    "flanger",
    "phaser",
    "reverb",
    "utility",
    "phase_shift",
    "frequency_shifter",
    "limiter",
    "dimension_expander",
    "waveshaper",
    "convolution_reverb",
    "mid_side_eq",
    "spectral_gate",
    "erosion",
    "floaty_delay",
    "spectral_compressor",
    "scream",
    "tremolo_pan",
    "tape",
    "defuse",
    "suppressor",
    "trance_gate",
    "spectral_lock",
    "resonator",
    "analoguechorus",
    "analogueflanger",
    "analogueecho",
    "analoguerotary",
    "analoguespring",
    "analoguewah",
    "spectral_balance",
    "glitch_delay",
    "openwurli_amp",
    "scanner_vibrato"
  };

  const std::string kEffectSlotTypeNames[] = {
    "Off",
    "Chorus",
    "Compressor",
    "Delay",
    "Distortion",
    "EQ",
    "Filter",
    "Flanger",
    "Phaser",
    "Reverb",
    "Utility",
    "Phase Shift",
    "Frequency Shifter",
    "Limiter",
    "Dimension Expander",
    "Waveshaper",
    "Convolution Reverb",
    "Mid Side EQ",
    "Spectral Gate",
    "Erosion",
    "Floaty Delay",
    "Spectral Compressor",
    "Scream",
    "Tremolo Pan",
    "Tape",
    "Defuse",
    "Suppressor",
    "Trance gate",
    "Spectral Lock",
    "Resonator",
    "Analogue Chorus",
    "Analogue Flanger",
    "Analogue Echo",
    "Analogue Rotary",
    "Analogue Spring",
    "Analogue Wah",
    "Spectral Balance",
    "Glitch Delay",
    "OpenWurli Amp",
    "Scanner Vibrato"
  };

  // Both tables are indexed by effect; a missing entry would read out of bounds.
  static_assert(sizeof(kEffectOrder) / sizeof(kEffectOrder[0]) == vital::constants::kNumEffects);
  static_assert(sizeof(kEffectSlotTypeNames) / sizeof(kEffectSlotTypeNames[0]) == vital::constants::kNumEffects + 1);

  const std::string kConvolutionPredelaySyncNames[] = {
    "Free",
    "Tempo",
    "Tempo Dotted",
    "Tempo Triplets",
  };

  const std::string kConvolutionSlopeNames[] = {
    "6 dB/octave",
    "12 dB/octave",
    "18 dB/octave",
    "24 dB/octave",
    "30 dB/octave",
    "36 dB/octave",
    "42 dB/octave",
    "48 dB/octave"
  };

  const std::string kCutFilterSlopeNames[] = {
    "6 dB/octave", "12 dB/octave", "18 dB/octave", "24 dB/octave",
    "30 dB/octave", "36 dB/octave", "42 dB/octave", "48 dB/octave"
  };
  const std::string kDefuseSpreadStyleNames[] = { "Octaves", "Linear" };
  const std::string kTapeSlopeNames[] = { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
  // One entry per recording in Factory/Samples/Tape (tape::kNumNoiseSounds).
  const std::string kTapeNoiseNames[] = { "Vinyl 1", "Vinyl 2", "Vinyl 3", "Vinyl 4", "Vinyl 5", "Vinyl 6", "Vinyl 7", "Vinyl 8" };
  const std::string kTranceGateTriggerNames[] = { "Playback", "MIDI legato" };
  const std::string kTranceGateGridNames[] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128" };
  const std::string kTranceGateModeNames[] = { "Long", "Medium", "Short", "Ramp Down (-inf)", "Ramp Up (0 dB)" };
  const std::string kTremoloPanSyncNames[] = { "Free", "Sync", "Dotted", "Triplet" };
  const std::string kTremoloPanShapeNames[] = { "Sine", "Triangle", "Square", "Ramp Up", "Ramp Down" };
  const std::string kSpectralLockModeNames[] = { "Filterbank", "Spectral" };
  const std::string kSpectralLockRootNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  const std::string kSpectralLockScaleNames[] = { "Chromatic", "Major", "Minor", "Harm Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Penta Major", "Penta Minor", "Whole Tone", "Octaves" };
  const std::string kResonatorModelNames[] = { "Beam", "Marimba", "String", "Membrane", "Plate", "Pipe", "Tube" };
  const std::string kResonatorQualityNames[] = { "Eco", "Normal", "High" };
  const std::string kResonatorLfoShapeNames[] = { "Sine", "Square", "Triangle", "Saw Up", "Saw Down", "Random Stepped", "Random Smooth" };
  const std::string kResonatorNotePriorityNames[] = { "Last", "Lowest" };
  const std::string kAnalogueChorusModelNames[] = { "Juno", "Dimension", "CE", "Ensemble" };
  const std::string kAnalogueChorusJunoModeNames[] = { "I", "II", "I+II" };
  const std::string kAnalogueChorusDimensionModeNames[] = { "1", "2", "3", "4" };
  const std::string kAnalogueMediumNames[] = { "BBD", "Tape" };
  const std::string kAnalogueEchoMediumNames[] = { "Tape", "BBD" };
  const std::string kAnalogueRotaryModelNames[] = { "Organ 122", "Guitar 16", "OpenB3 Whirl" };
  const std::string kAnalogueRotarySpeedNames[] = { "Slow", "Fast", "Stop" };
  const std::string kAnalogueSpringTankNames[] = { "Short", "Medium", "Long" };
  const std::string kScannerVibratoModeNames[] = { "V1", "V2", "V3", "C1", "C2", "C3" };
  const std::string kAnalogueWahModelNames[] = { "Cry Baby", "V846", "Mu-Tron III" };
  const std::string kAnalogueWahModeNames[] = { "LP", "BP", "HP" };
  const std::string kAnalogueWahRangeNames[] = { "Lo", "Hi" };
  const std::string kSpectralCompressorCurveNames[] = { "Flat", "Smooth", "OTT" };
  const std::string kSpectralBalanceTargetNames[] = {
    "Perceptual", "Flat", "-1.5 dB/oct", "-3 dB/oct", "-4.5 dB/oct", "-6 dB/oct"
  };
  // Glitch Delay's note values, shortest first (as in 4/4), and Stutter's subset of them up to
  // a quarter note. Must match atlas::glitch_delay::NoteValues::labels().
  const std::string kGlitchDelayNoteNames[] = {
    "1/64 triplet", "1/64", "1/32 triplet", "1/64 dotted", "1/32", "1/16 triplet", "1/32 dotted",
    "1/16", "1/8 triplet", "1/16 dotted", "1/8", "1/4 triplet", "1/8 dotted", "1/4", "1/2 triplet",
    "1/4 dotted", "1/2", "1 bar triplet", "1/2 dotted", "1 bar", "2 bars triplet", "1 bar dotted",
    "2 bars", "4 bars triplet", "2 bars dotted", "4 bars"
  };
  const std::string kGlitchDelayStutterNoteNames[] = {
    "1/64 triplet", "1/64", "1/32 triplet", "1/64 dotted", "1/32", "1/16 triplet", "1/32 dotted",
    "1/16", "1/8 triplet", "1/16 dotted", "1/8", "1/4 triplet", "1/8 dotted", "1/4"
  };
  const std::string kGlitchDelayTapNames[] = { "Tap 1", "Tap 2", "Tap 3", "Tap 4" };
  const std::string kGlitchDelayPlacementNames[] = { "Feedback path", "Output and feedback" };
  const std::string kGlitchDelayPitchModeNames[] = { "Sweep", "Varispeed" };
  const std::string kSpectralGateFftSizeNames[] = { "256", "512", "1024", "2048", "4096", "8192" };

  const std::string kDimensionModeNames[] = { "Mode 1", "Mode 2" };
  const std::string kQuoraxSlopeNames[] = { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
  const std::string kErosionModeNames[] = { "Noise", "Sine" };

  const std::string kWaveShaperShapeNames[] = {
    "Curve",
    "S-Curve",
    "Stairs",
    "Sine Wave",
    "Hard Clip",
    "Fold",
    "Double Fold"
  };

  const std::string kWaveShaperWarpTypeNames[] = {
    "None",
    "Bend Up",
    "Bend Down",
    "Bend Up Down",
    "Skew Up",
    "Skew Down",
    "Skew Up Down"
  };

  const std::string kWaveShaperModeNames[] = {
    "Unipolar",
    "Bipolar"
  };

  const std::string kArpTimeModeNames[] = {
    "Sync",
    "Free"
  };

  const std::string kArpStyleNames[] = {
    "Up",
    "Down",
    "Up Down",
    "Down Up",
    "Up and Down",
    "Down and Up",
    "Converge",
    "Diverge",
    "Converge and Diverge",
    "Pinky Up",
    "Pinky Up Down",
    "Thumb Up",
    "Thumb Up Down",
    "Random",
    "Random Other",
    "Random Once",
    "Chord Trigger"
  };

  const std::string kArpSyncRateNames[] = {
    "8 Bars",
    "4 Bars",
    "2 Bars",
    "1 Bar",
    "1/2",
    "1/2 Dotted",
    "1/2 Triplet",
    "1/4",
    "1/4 Dotted",
    "1/4 Triplet",
    "1/8",
    "1/8 Dotted",
    "1/8 Triplet",
    "1/16",
    "1/16 Dotted",
    "1/16 Triplet",
    "1/32",
    "1/32 Dotted",
    "1/32 Triplet",
    "1/64",
    "1/64 Dotted",
    "1/64 Triplet",
    "1/128"
  };

  const std::string kArpRetriggerNames[] = {
    "Off",
    "Note",
    "Beat"
  };

  const std::string kArpPlayOrderNames[] = {
    "Pitch",
    "As Played"
  };

  // Matches AtlasArpeggiator::kMaxLoops and kLoopInfinite.
  const std::string kArpLoopNames[] = {
    "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "10", "11", "12", "13", "14", "15", "16",
    "Infinite"
  };

  // Matches AtlasArpeggiator::retriggerDivisions().
  const std::string kArpRetriggerDivisionNames[] = {
    "1/16",
    "1/8",
    "1/4",
    "1/2",
    "1 Bar",
    "2 Bars",
    "4 Bars",
    "8 Bars",
    "16 Bars"
  };

  const std::string kArpChannelNames[] = {
    "Input",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "10",
    "11",
    "12",
    "13",
    "14",
    "15",
    "16"
  };

  const std::string kSyncedFrequencyNames[] = {
    "Freeze",
    "32/1",
    "16/1",
    "8/1",
    "4/1",
    "2/1",
    "1/1",
    "1/2",
    "1/4",
    "1/8",
    "1/16",
    "1/32",
    "1/64",
  };

  const std::string kStereoModeNames[vital::StereoEncoder::kNumStereoModes] = {
    "SPREAD",
    "ROTATE"
  };

  const std::string kSmoothModeNames[] = {
    "SMOOTH OFF",
    "SMOOTH"
  };

  const std::string kSyncShortNames[] = {
    "Trigger",
    "Sync",
    "Envelope",
    "Sustain Env",
    "Loop Point",
    "Loop Hold",
  };

  const std::string kSyncNames[] = {
    "Trigger",
    "Sync",
    "Envelope",
    "Sustain Envelope",
    "Loop Point",
    "Loop Hold",
  };

  const std::string kRandomShortNames[] = {
    "Perlin",
    "S & H",
    "Sine",
    "Lorenz",
  };

  const std::string kRandomNames[] = {
    "Perlin",
    "Sample & Hold",
    "Sine Interpolate",
    "Lorenz Attractor",
  };

  const std::string kPaintPatternNames[] = {
    "Step",
    "Half",
    "Down",
    "Up",
    "Tri"
  };

  const std::string kVoicePriorityNames[] = {
    "Newest",
    "Oldest",
    "Highest",
    "Lowest",
    "Round Robin"
  };

  const std::string kVoiceOverrideNames[] = {
    "Kill",
    "Steal"
  };

  const std::string kEqHighModeNames[] = {
    "Shelf",
    "Low Pass"
  };

  const std::string kEqBandModeNames[] = {
    "Shelf",
    "Notch"
  };

  const std::string kEqLowModeNames[] = {
    "Shelf",
    "High Pass"
  };

  const std::string kDestinationNames[vital::constants::kNumDestinations] = {
    "FILTER 1",
    "FILTER 2",
    "FILTER 1+2",
    "EFFECTS",
    "BUS 1",
    "BUS 2",
    "DIRECT OUT",
    "BUS 3",
    "CHORUS",
    "COMPRESSOR",
    "DELAY",
    "DISTORTION",
    "EQ",
    "FX FILTER",
    "FLANGER",
    "PHASER",
    "REVERB",
    "UTILITY",
    "PHASE SHIFT",
    "FREQUENCY SHIFTER",
    "LIMITER",
    "DIMENSION EXPANDER",
    "WAVESHAPER",
    "CONVOLUTION REVERB",
    "MID SIDE EQ",
    "SPECTRAL GATE",
    "BP A-EROSION",
    "FLOATY DELAY",
    "SPECTRAL COMPRESSOR",
    "SCREAM",
    "TREMOLO PAN",
    "TAPE",
    "DEFUSE",
    "SUPPRESSOR",
    "TRANCE GATE",
    "SPECTRAL LOCK",
    "RESONATOR",
    "ANALOGUE CHORUS",
    "ANALOGUE FLANGER",
    "ANALOGUE ECHO",
    "ANALOGUE ROTARY",
    "ANALOGUE SPRING",
    "ANALOGUE WAH",
    "SPECTRAL BALANCE",
    "GLITCH DELAY",
    "OPENWURLI AMP",
    "SCANNER VIBRATO",
  };

  const std::string kDestinationMenuNames[vital::constants::kNumDestinations] = {
    "Filter 1",
    "Filter 2",
    "Filter 1+2",
    "Effects",
    "Bus 1",
    "Bus 2",
    "Direct Out",
    "Bus 3",
    "Chours",
    "Compressor",
    "Delay",
    "Distortion",
    "Equalizer",
    "Effects Filter",
    "Flanger",
    "Phaser",
    "Reverb",
    "Utility",
    "Phase Shift",
    "Frequency Shifter",
    "Limiter",
    "Dimension Expander",
    "Waveshaper",
    "Convolution Reverb",
    "Mid Side EQ",
    "Spectral Gate",
    "Erosion",
    "Floaty Delay",
    "Spectral Compressor",
    "Scream",
    "Tremolo Pan",
    "Tape",
    "Defuse",
    "Suppressor",
    "Trance gate",
    "Spectral Lock",
    "Resonator",
    "Analogue Chorus",
    "Analogue Flanger",
    "Analogue Echo",
    "Analogue Rotary",
    "Analogue Spring",
    "Analogue Wah",
    "Spectral Balance",
    "Glitch Delay",
    "OpenWurli Amp",
    "Scanner Vibrato",
  };

  const std::string kBusDestinationNames[vital::constants::kNumBusDestinations] = {
    "MAIN",
    "BUS 1",
    "BUS 2",
    "BUS 3",
    "DIRECT OUT",
  };

  const std::string kBusDestinationMenuNames[vital::constants::kNumBusDestinations] = {
    "Main",
    "Bus 1",
    "Bus 2",
    "Bus 3",
    "Direct Out",
  };

  const std::string kPhaseDistortionNames[] = {
    "None",
    "Sync",
    "Formant",
    "Quantize",
    "Bend",
    "Squeeze",
    "Pulse",
    "FM from Oscillator",
    "FM from Oscillator",
    "FM from Sample",
    "Ring Mod from Oscillator",
    "Ring Mod from Oscillator",
    "Ring Mod from Sample",
    "FM from Oscillator",
    "FM from Granular",
    "Ring Mod from Oscillator",
    "Ring Mod from Granular",
    "FM from Noise",
    "Ring Mod from Noise",
    "FM from Spectral",
    "Ring Mod from Spectral"
  };

  const std::string kNoiseTypeNames[] = {
    "White",
    "Bright White",
    "Pink",
    "Brown",
    "Dark",
    "Air"
  };


  const std::string kSpectralMorphNames[] = {
    "None",
    "Vocode",
    "Formant Scale",
    "Harmonic Stretch",
    "Inharmonic Stretch",
    "Smear",
    "Random Amplitudes",
    "Low Pass",
    "High Pass",
    "Phase Disperse",
    "Shepard Tone",
    "Spectral Time Skew",
  };
} // namespace strings
