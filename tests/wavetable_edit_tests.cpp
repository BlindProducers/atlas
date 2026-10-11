// SPDX-License-Identifier: GPL-3.0-or-later
// The accessible wavetable editor's model: layers, keyframes and every
// source and modifier setting, checked on the rendered wavetable.
#include "JuceHeader.h"

#include "file_source.h"
#include "line_generator.h"
#include "phase_modifier.h"
#include "wave_frame.h"
#include "wave_line_source.h"
#include "wave_source.h"
#include "wavetable.h"
#include "wavetable_creator.h"
#include "wavetable_edit_model.h"
#include "wavetable_group.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
  int failures = 0;
  void expect(bool condition, const std::string& message) {
    if (!condition) {
      ++failures;
      std::cout << "FAIL: " << message << '\n';
    }
  }

  namespace edit = atlas::wavetable_edit;

  struct Table {
    vital::Wavetable wavetable { vital::kNumOscillatorWaveFrames };
    WavetableCreator creator { &wavetable };
  };

  std::vector<float> frameAt(Table& table, int position) {
    const auto* data = table.wavetable.getAllData();
    std::vector<float> frame(vital::WaveFrame::kWaveformSize, 0.0f);
    if (data == nullptr || position >= data->num_frames)
      return frame;
    for (int i = 0; i < vital::WaveFrame::kWaveformSize; ++i)
      frame[i] = data->wave_data[position][i];
    return frame;
  }

  double difference(const std::vector<float>& a, const std::vector<float>& b) {
    double total = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
      total += std::abs(a[i] - b[i]);
    return total / double(a.size());
  }

  void setShape(WaveSource* source, int keyframe, bool square) {
    auto* frame = source->getKeyframe(keyframe)->wave_frame();
    for (int i = 0; i < vital::WaveFrame::kWaveformSize; ++i) {
      const float phase = float(i) / vital::WaveFrame::kWaveformSize;
      frame->time_domain[i] = square ? (phase < 0.5f ? 0.8f : -0.8f) : 0.8f * std::sin(6.2831853f * phase);
    }
    frame->toFrequencyDomain();
  }

  // The reported bug: the blend modes must change what plays between frames.
  void testFrameInterpolationIsAudible() {
    Table table;
    table.creator.loadDefaultCreator();
    auto* source = edit::waveSourceOfLayer(&table.creator, 0);
    expect(source != nullptr && source->numFrames() == 1, "the default table is one wave source frame");
    if (source == nullptr) return;
    setShape(source, 0, false);
    const int position = edit::suggestKeyframePosition(source, 0);
    expect(position == edit::kNumPositions - 1, "a frame added after the last one goes to the end of the timeline");
    const int added = edit::addKeyframe(source, position);
    expect(added == 1, "the new frame is the second");
    setShape(source, 1, true);

    std::vector<std::vector<float>> middles;
    for (int mode = 0; mode < 5; ++mode) {
      expect(edit::setValue(source, "interpolation", 0, mode), "each frame interpolation mode can be chosen");
      expect(int(edit::getValue(source, "interpolation", 0)) == mode, "the chosen mode reads back");
      table.creator.render();
      middles.push_back(frameAt(table, 128));
      if (mode == 0)
        expect(difference(middles[0], frameAt(table, 0)) < 1.0e-6, "None holds the first frame until the next");
    }
    for (int a = 0; a < 5; ++a)
      for (int b = a + 1; b < 5; ++b)
        expect(difference(middles[a], middles[b]) > 1.0e-4,
               "interpolation modes " + std::to_string(a) + " and " + std::to_string(b) + " render differently");
    expect(edit::interpolatedPositions(source) == 254, "254 positions are generated between the two frames");
    // A hand-built table with frames on neighbouring positions spreads out.
    edit::moveKeyframe(source, 1, 1);
    edit::addKeyframe(source, 2);
    expect(edit::interpolatedPositions(source) == 0, "frames on neighbouring positions leave nothing to interpolate");
    expect(edit::spreadKeyframes(source) && source->getFrameAt(1)->position() == 128 &&
           source->getFrameAt(2)->position() == 255, "spreading puts three frames at 1, 129 and 256");
  }

  // Changing an audio file layer's blend never flattens it into frames.
  void testAudioLayerKeepsItsStructure() {
    Table table;
    const int rate = 44100;
    std::vector<float> audio(rate);
    for (int i = 0; i < rate; ++i) {
      const float t = float(i) / rate;
      audio[i] = 0.5f * std::sin(6.2831853f * 220.0f * t) + 0.3f * t * std::sin(6.2831853f * 660.0f * t);
    }
    table.creator.initFromAudioFile(audio.data(), rate, rate, WavetableCreator::kWavetableSplice, FileSource::kTimeInterpolate);
    auto* component = table.creator.getGroup(0)->getComponent(0);
    expect(component->getType() == WavetableComponentFactory::kFileSource, "a spliced import is an audio file source");
    expect(edit::waveSourceOfLayer(&table.creator, 0) == nullptr, "an audio layer offers no wave source to edit");
    const auto list = edit::parameters(component);
    expect(edit::findParameter(list, "fade_style") != nullptr && edit::findParameter(list, "start_position") != nullptr &&
           edit::findParameter(list, "window_size") != nullptr && edit::findParameter(list, "phase_style") != nullptr,
           "an audio layer exposes frame blend, phase style, window size and start position");

    std::vector<std::vector<float>> middles;
    for (int style = 1; style <= 3; ++style) {
      edit::setValue(component, "fade_style", 0, style);
      table.creator.render();
      middles.push_back(frameAt(table, 100));
    }
    expect(table.creator.numGroups() == 1 && table.creator.getGroup(0)->getComponent(0) == component,
           "changing the frame blend keeps the audio layer");
    expect(difference(middles[0], middles[1]) > 1.0e-5 || difference(middles[1], middles[2]) > 1.0e-5,
           "frame blend styles render differently");

    // Cropping: move the last keyframe earlier and its start position to the middle of the file.
    const int last = component->numFrames() - 1;
    const int moved = edit::moveKeyframe(component, last, 127);
    expect(moved == 127, "the last keyframe moves to position 128");
    edit::setValue(component, "start_position", last, rate / 2);
    expect(std::abs(edit::getValue(component, "start_position", last) - rate / 2) < 1.0, "the start position reads back");
    edit::setValue(component, "start_position", last, 1.0e9);
    expect(edit::getValue(component, "start_position", last) <= rate - edit::getValue(component, "window_size", 0) + 1.0,
           "the start position stays inside the file");
    table.creator.render();
    expect(table.wavetable.getAllData()->num_frames == 128, "the table ends at the last keyframe");
    // Vital's note names: C3 is MIDI 48, 130.81 Hz.
    expect(std::abs(edit::windowSizeFromText("C3", 44100) - 44100.0 / 130.8128) < 0.5, "a note name sets one period");
    expect(std::abs(edit::windowSizeFromText("2048", 44100) - 2048.0) < 1.0e-6, "a number sets samples");
  }

  void testKeyframesAndModifiers() {
    Table table;
    table.creator.loadDefaultCreator();
    auto* group = table.creator.getGroup(0);
    auto* phase = edit::addModifier(*group, 0);
    expect(phase != nullptr && phase->getType() == WavetableComponentFactory::kPhaseModifier, "a phase shift is added");
    if (phase == nullptr) return;
    const int second = edit::addKeyframe(phase, edit::suggestKeyframePosition(phase, 0));
    expect(second == 1 && phase->getFrameAt(1)->position() == 255, "the modifier gets a second keyframe at the end");
    expect(edit::addKeyframe(phase, 255) < 0, "a taken position is refused");
    edit::setValue(phase, "phase", 0, 0.0);
    edit::setValue(phase, "phase", 1, 180.0);
    expect(std::abs(edit::getValue(phase, "phase", 1) - 180.0) < 1.0e-3, "the phase reads back in degrees");
    edit::setValue(phase, "phase", 1, 999.0);
    expect(std::abs(edit::getValue(phase, "phase", 1) - 180.0) < 1.0e-3, "values clamp to their range");
    // A table needs frames up to the last keyframe for the modifier to sweep.
    auto* source = edit::waveSourceOfLayer(&table.creator, 0);
    edit::addKeyframe(source, 255);
    table.creator.render();
    const auto start = frameAt(table, 0), middle = frameAt(table, 128), end = frameAt(table, 255);
    expect(difference(start, middle) > 1.0e-4 && difference(middle, end) > 1.0e-4, "the phase sweeps across the table");
    edit::setValue(phase, "interpolation", 0, 0);
    table.creator.render();
    const auto held_start = frameAt(table, 0), held_middle = frameAt(table, 128);
    expect(difference(held_middle, held_start) < 1.0e-5, "with no interpolation the first keyframe holds");

    expect(edit::moveKeyframe(phase, 1, 0) == 1, "a keyframe cannot cross its neighbour");
    expect(edit::removeKeyframe(phase, 1) && phase->numFrames() == 1, "a keyframe can be removed");
    expect(!edit::removeKeyframe(phase, 0), "the last keyframe stays");
    expect(edit::describeKeyframe(phase, 0) == "Keyframe 1 of 1, position 1 of 256", "keyframes are described");

    for (int type = 0; type < int(edit::modifierTypeNames().size()); ++type) {
      auto* modifier = edit::addModifier(*group, type);
      expect(modifier != nullptr, "modifier " + edit::modifierTypeNames()[type].toStdString() + " is added");
      for (const auto& parameter : edit::parameters(modifier)) {
        const double target = parameter.isChoice() ? parameter.maximum : parameter.minimum + 0.25 * (parameter.maximum - parameter.minimum);
        expect(edit::setValue(modifier, parameter.id, 0, target), "set " + parameter.id);
        expect(std::abs(edit::getValue(modifier, parameter.id, 0) - target) < 1.0e-3 * (1.0 + std::abs(target)),
               edit::modifierTypeNames()[type].toStdString() + " " + parameter.id + " reads back");
      }
    }
    table.creator.render();
  }

  void testLayersAndLines() {
    Table table;
    table.creator.loadDefaultCreator();
    expect(!edit::removeLayer(table.creator, 0), "the only layer stays");
    for (int type = 0; type < 3; ++type)
      expect(edit::addLayer(table.creator, type) != nullptr, "a " + edit::sourceTypeNames()[type].toStdString() + " layer is added");
    expect(table.creator.numGroups() == 4, "four layers");
    expect(edit::describeLayer(table.creator, 1).startsWith("Layer 2, Wave source"), "layers describe their source");
    expect(edit::moveLayer(table.creator, 1, 1) &&
           table.creator.getGroup(2)->getComponent(0)->getType() == WavetableComponentFactory::kWaveSource,
           "a layer moves later");
    expect(!edit::moveLayer(table.creator, 0, -1), "the first layer cannot move earlier");

    auto* line = dynamic_cast<WaveLineSource*>(table.creator.getGroup(1)->getComponent(0));
    expect(line != nullptr, "layer 2 is now the line source");
    if (line != nullptr) {
      edit::addKeyframe(line, 255);
      const int points = edit::numLinePoints(line);
      const int added = edit::addLinePoint(line, 0);
      expect(added == 1 && edit::numLinePoints(line) == points + 1, "a line point is added after the first");
      expect(line->getKeyframe(1)->getNumPoints() == points + 1, "every keyframe gets the point");
      edit::setLinePointValue(line, 1, added, "level", 100.0);
      expect(std::abs(edit::linePointValue(line, 1, added, "level") - 100.0) < 1.0e-3, "point level reads back");
      edit::setLinePointValue(line, 0, 0, "phase", 50.0);
      expect(edit::linePointValue(line, 0, 0, "phase") == 0.0, "the first point stays at the start of the cycle");
      expect(edit::removeLinePoint(line, added) && line->getKeyframe(1)->getNumPoints() == points,
             "a line point is removed from every keyframe");
      expect(edit::setValue(line, "smooth", 0, 1) && edit::getValue(line, "smooth", 0) == 1.0, "Smooth line turns on");
    }
    table.creator.render();
    expect(edit::removeLayer(table.creator, 3) && table.creator.numGroups() == 3, "a layer is removed");

    auto* flat = edit::flattenToWaveSource(&table.creator);
    expect(flat != nullptr && table.creator.numGroups() == 1 && flat->numFrames() == 256,
           "flattening makes one wave source with every rendered position");
  }
}

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  testFrameInterpolationIsAudible();
  testAudioLayerKeepsItsStructure();
  testKeyframesAndModifiers();
  testLayersAndLines();
  std::cout << "Wavetable edit failures: " << failures << '\n';
  return failures == 0 ? 0 : 1;
}
