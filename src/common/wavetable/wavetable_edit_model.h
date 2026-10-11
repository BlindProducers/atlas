// SPDX-License-Identifier: GPL-3.0-or-later
// Atlas: everything Vital's graphical wavetable editor can change, as plain
// operations an accessible editor (or a test) can call. A wavetable is a list
// of layers (Vital's groups); each layer is a source followed by modifiers,
// and every source and modifier keeps keyframes on the 256-position timeline.
// Parameters are described in display units (percent, degrees, samples), so
// the editor can build its rows from them without knowing each type.
#pragma once

#include "JuceHeader.h"
#include "wavetable_component_factory.h"

#include <string>
#include <vector>

class WavetableCreator;
class WavetableComponent;
class WavetableGroup;
class WaveSource;
class WaveLineSource;

namespace atlas::wavetable_edit {
  constexpr int kNumPositions = 256;

  struct Parameter {
    std::string id;
    juce::String title;
    juce::String description;
    double minimum = 0.0;
    double maximum = 1.0;
    double interval = 0.0;
    juce::String suffix;
    int decimals = 2;
    juce::StringArray choices;    // non-empty: a choice, the value is the index
    bool per_keyframe = false;    // otherwise it applies to the whole source or modifier
    bool isChoice() const { return !choices.isEmpty(); }
  };

  juce::String componentName(WavetableComponent* component);
  bool isSource(WavetableComponent* component);
  bool hasKeyframes(WavetableComponent* component);

  // Options first, then the per-keyframe values, in the order Vital shows them.
  std::vector<Parameter> parameters(WavetableComponent* component);
  const Parameter* findParameter(const std::vector<Parameter>& list, const std::string& id);
  double getValue(WavetableComponent* component, const std::string& id, int keyframe);
  // Clamps to the parameter's range; returns false for an unknown id or keyframe.
  bool setValue(WavetableComponent* component, const std::string& id, int keyframe, double value);
  juce::String formatValue(const Parameter& parameter, double value);

  // Keyframes. Positions are 0..255; a keyframe never crosses its neighbours.
  juce::String describeKeyframe(WavetableComponent* component, int index);
  int suggestKeyframePosition(WavetableComponent* component, int after_index);
  // Adds a keyframe holding the interpolated state at that position; -1 when
  // the position is taken or the component has no keyframes.
  int addKeyframe(WavetableComponent* component, int position);
  bool removeKeyframe(WavetableComponent* component, int index);
  // Returns the position the keyframe ends up at.
  int moveKeyframe(WavetableComponent* component, int index, int position);
  // Spreads the keyframes evenly from the first to the last timeline position.
  bool spreadKeyframes(WavetableComponent* component);

  // Layers.
  juce::StringArray sourceTypeNames();     // Wave source, Line source, Audio file source
  juce::StringArray modifierTypeNames();
  WavetableGroup* addLayer(WavetableCreator& creator, int source_type);
  bool removeLayer(WavetableCreator& creator, int index);
  bool moveLayer(WavetableCreator& creator, int index, int direction);
  WavetableComponent* addModifier(WavetableGroup& group, int modifier_type);
  juce::String describeLayer(WavetableCreator& creator, int index);
  juce::String describeWavetable(WavetableCreator& creator);

  // Line source points: the count is shared by every keyframe, as in Vital.
  int numLinePoints(WaveLineSource* source);
  int addLinePoint(WaveLineSource* source, int after_index);   // returns the new point's index
  bool removeLinePoint(WaveLineSource* source, int index);
  bool setLineSmooth(WaveLineSource* source, bool smooth);
  // Phase 0..100 percent across the cycle, level -100..100 percent, curve -10..10.
  double linePointValue(WaveLineSource* source, int keyframe, int point, const std::string& field);
  bool setLinePointValue(WaveLineSource* source, int keyframe, int point, const std::string& field, double value);

  // Audio file sources.
  int fileSourceSamples(WavetableComponent* component);
  int fileSourceSampleRate(WavetableComponent* component);
  // Window size from samples, or from a note name such as "A2" (one period of that note).
  double windowSizeFromText(const juce::String& text, int sample_rate);
  // A new audio layer, set up like Vital's pitched import: the window follows
  // the detected pitch and two keyframes sweep from the start to the end.
  bool loadAudioLayer(WavetableComponent* component, const float* samples, int num_samples, int sample_rate);
  bool replaceFileSourceAudio(WavetableComponent* component, const float* samples, int num_samples,
                              int sample_rate);

  // Wave source helpers.
  WaveSource* waveSourceOfLayer(WavetableCreator* creator, int layer);
  // How many timeline positions are generated between this source's frames.
  int interpolatedPositions(const WaveSource* source);
  // Replaces every layer with one wave source holding the rendered frames, so
  // frames can be edited harmonic by harmonic. Destructive by design: the
  // editor only calls it when asked to.
  WaveSource* flattenToWaveSource(WavetableCreator* creator);
}
