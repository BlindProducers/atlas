// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "distortion.h"
#include "distortion_filter.h"

namespace distortion_filter_tests {
  using Values = std::vector<std::pair<std::string, float>>;
  constexpr int kRate = 48000, kBlock = 128, kFftOrder = 16, kFftSize = 1 << kFftOrder;
  constexpr int kSettled = kRate / 2;

  std::vector<float> render(const std::vector<int>& notes, const Values& values, size_t length = kSettled + kFftSize) {
    SynthPlugin plugin;
    plugin.setNonRealtime(true);
    plugin.setPlayConfigDetails(0, 2, kRate, kBlock);
    plugin.prepareToPlay(kRate, kBlock);
    const auto controls = plugin.getControls();
    controls.at("oversampling")->set(1);
    controls.at("osc_1_unison_voices")->set(1);
    controls.at("osc_1_random_phase")->set(0);
    for (const auto& value : values)
      controls.at(value.first)->set(value.second);
    plugin.checkOversampling();
    AudioSampleBuffer audio(2, kBlock);
    MidiBuffer midi;
    for (int note : notes)
      midi.addEvent(MidiMessage::noteOn(1, note, .8f), 0);
    std::vector<float> left;
    while (left.size() < length) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
      for (int i = 0; i < kBlock; ++i)
        left.push_back(audio.getSample(0, i));
    }
    return left;
  }

  // Filter 1 as the Distortion model, or the Distortion effect in the first FX slot.
  Values filterModel(int type, float drive, int order = 0) {
    return { { "filter_1_on", 1 }, { "filter_1_model", vital::constants::kDistortionFilter },
             { "filter_1_style", float(type) }, { "filter_1_distortion_drive", drive },
             { "filter_1_distortion_filter_order", float(order) }, { "filter_1_cutoff", 70 },
             { "filter_1_resonance", .4f }, { "filter_1_blend", .5f } };
  }

  Values effect(int type, float drive, int order = 0) {
    return { { "fx_slot_1_type", vital::constants::kDistortion + 1 }, { "fx_slot_1_enabled", 1 },
             { "fx_slot_1_distortion_type", float(type) }, { "fx_slot_1_distortion_drive", drive },
             { "fx_slot_1_distortion_filter_order", float(order) }, { "fx_slot_1_distortion_filter_cutoff", 70 },
             { "fx_slot_1_distortion_filter_resonance", .4f }, { "fx_slot_1_distortion_filter_blend", .5f } };
  }

  Values with(Values values, const Values& more) {
    values.insert(values.end(), more.begin(), more.end());
    return values;
  }

  double frequencyOf(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

  // Share of the held render's power that is not a harmonic of any held note, in dB.
  double inharmonicDb(const std::vector<float>& audio, const std::vector<int>& notes) {
    std::vector<float> data(2 * kFftSize, 0.0f);
    for (int i = 0; i < kFftSize; ++i) {
      const double x = 2.0 * vital::kPi * i / (kFftSize - 1);
      const double window = .35875 - .48829 * std::cos(x) + .14128 * std::cos(2 * x) - .01168 * std::cos(3 * x);
      data[size_t(i)] = float(audio[size_t(kSettled + i)] * window);
    }
    juce::dsp::FFT(kFftOrder).performFrequencyOnlyForwardTransform(data.data());
    double total = 0.0, inharmonic = 0.0;
    for (int i = 4; i < kFftSize / 2; ++i) {
      const double power = double(data[size_t(i)]) * data[size_t(i)];
      const double frequency = double(i) * kRate / kFftSize;
      bool harmonic = false;
      for (int note : notes) {
        const double f = frequencyOf(note), k = std::round(frequency / f);
        harmonic = harmonic || (k >= 1.0 && std::abs(frequency - k * f) < 6.0 * kRate / kFftSize);
      }
      total += power;
      inharmonic += harmonic ? 0.0 : power;
    }
    return 10.0 * std::log10(std::max(inharmonic / std::max(total, 1e-30), 1e-30));
  }

  // RMS of the difference between two held renders, relative to the first's RMS.
  double relativeDifference(const std::vector<float>& reference, const std::vector<float>& audio) {
    double difference = 0.0, energy = 0.0;
    for (size_t i = kSettled; i < reference.size(); ++i) {
      difference += (double(audio[i]) - reference[i]) * (double(audio[i]) - reference[i]);
      energy += double(reference[i]) * reference[i];
    }
    return std::sqrt(difference / std::max(energy, 1e-30));
  }

  // The point of the model: each note is distorted on its own, so a held chord
  // keeps only its notes' harmonics, while the effect adds combination tones.
  void testChordsGainNoCombinationTones() {
    const std::vector<int> chord { 60, 64, 67 };
    for (int type = vital::Distortion::kSoftClip; type <= vital::Distortion::kBitCrush; ++type) {
      const double model = inharmonicDb(render(chord, filterModel(type, 12.0f)), chord);
      const double mix = inharmonicDb(render(chord, effect(type, 12.0f)), chord);
      std::cout << "Distortion type " << type << ": combination tones " << mix << " dB through the effect, "
                << model << " dB through the filter model\n";
      expect(mix > -20.0, "the effect distorts a chord as a mix, type " + std::to_string(type));
      expect(model < (type == vital::Distortion::kBitCrush ? -20.0 : -35.0),
             "the filter model distorts each note on its own, type " + std::to_string(type));
    }
  }

  // A lone note sounds as it does through the effect, also below full level,
  // with the effect's filter before or after the shaper.
  void testSingleNoteMatchesEffect() {
    const Values quieter { { "env_1_sustain", .5f } };
    double largest = 0.0;
    for (int type = vital::Distortion::kSoftClip; type < vital::Distortion::kNumTypes; ++type) {
      for (int order = vital::DistortionFilter::kNoFilter; order < vital::DistortionFilter::kNumFilterOrders; ++order) {
        // The effect's drive glides from 0 dB over the first block, and a hold
        // that starts at another time stays out of step, so Down Sample keeps 0 dB.
        const float drive = type == vital::Distortion::kDownSample ? 0.0f : 9.0f;
        const auto reference = render({ 60 }, with(effect(type, drive, order), quieter));
        const auto model = render({ 60 }, with(filterModel(type, drive, order), quieter));
        const double difference = relativeDifference(reference, model);
        largest = std::max(largest, difference);
        const std::string context = "type " + std::to_string(type) + ", filter order " + std::to_string(order);
        expect(difference < .001,
               "a lone note matches the effect, " + context + " (" + std::to_string(difference) + ")");
      }
    }
    std::cout << "A lone note differs from the effect by at most " << largest << " relative RMS\n";
  }

  // The amp envelope reaches the shaper first, as in the effect chain: a note
  // played quieter is distorted less, rather than only turned down.
  void testQuietNotesDistortLess() {
    const auto loud = render({ 60 }, filterModel(vital::Distortion::kHardClip, 18.0f));
    const auto quiet = render({ 60 }, with(filterModel(vital::Distortion::kHardClip, 18.0f), { { "env_1_sustain", .05f } }));
    auto crest = [](const std::vector<float>& audio) {
      double peak = 0.0, energy = 0.0;
      for (size_t i = kSettled; i < audio.size(); ++i) {
        peak = std::max(peak, double(std::abs(audio[i])));
        energy += double(audio[i]) * audio[i];
      }
      return peak / std::sqrt(energy / double(audio.size() - kSettled));
    };
    // Hard clipping flattens a saw towards a square, which lowers its crest factor.
    expect(crest(quiet) > crest(loud) * 1.2, "a quieter note is clipped less");
  }

  // Filter FX offers the model too. There it distorts the mix, as the effect does.
  void testFilterFxMatchesEffect() {
    const std::vector<int> chord { 60, 64, 67 };
    const Values filter_fx { { "fx_slot_1_type", vital::constants::kFilterFx + 1 }, { "fx_slot_1_enabled", 1 },
                             { "fx_slot_1_filter_fx_model", vital::constants::kDistortionFilter },
                             { "fx_slot_1_filter_fx_style", vital::Distortion::kSoftClip },
                             { "fx_slot_1_filter_fx_distortion_drive", 12.0f } };
    const auto reference = render(chord, effect(vital::Distortion::kSoftClip, 12.0f));
    const double difference = relativeDifference(reference, render(chord, filter_fx));
    expect(difference < .001, "Filter FX's Distortion model matches the effect (" + std::to_string(difference) + ")");
  }

  void testStateRoundTrip() {
    juce::MemoryBlock saved;
    {
      SynthPlugin plugin;
      for (const auto& value : with(filterModel(vital::Distortion::kSinFold, 7.5f, vital::DistortionFilter::kPostFilter),
                                    { { "filter_2_distortion_drive", -12.0f } }))
        plugin.getControls().at(value.first)->set(value.second);
      plugin.getStateInformation(saved);
    }
    SynthPlugin restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    const auto controls = restored.getControls();
    expect(controls.at("filter_1_model")->value() == vital::constants::kDistortionFilter &&
           controls.at("filter_1_style")->value() == vital::Distortion::kSinFold &&
           controls.at("filter_1_distortion_drive")->value() == 7.5f &&
           controls.at("filter_1_distortion_filter_order")->value() == vital::DistortionFilter::kPostFilter &&
           controls.at("filter_2_distortion_drive")->value() == -12.0f,
           "the Distortion model's settings survive saving and loading");
  }

  int run() {
    ScopedJuceInitialiser_GUI gui;
    testChordsGainNoCombinationTones();
    testSingleNoteMatchesEffect();
    testQuietNotesDistortLess();
    testFilterFxMatchesEffect();
    testStateRoundTrip();
    std::cout << "Distortion filter: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
} // namespace distortion_filter_tests
