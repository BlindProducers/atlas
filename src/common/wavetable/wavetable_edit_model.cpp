// SPDX-License-Identifier: GPL-3.0-or-later
#include "wavetable_edit_model.h"

#include "file_source.h"
#include "frequency_filter_modifier.h"
#include "line_generator.h"
#include "phase_modifier.h"
#include "shepard_tone_source.h"
#include "slew_limit_modifier.h"
#include "synth_constants.h"
#include "tuning.h"
#include "utils.h"
#include "wave_fold_modifier.h"
#include "wave_frame.h"
#include "wave_line_source.h"
#include "wave_source.h"
#include "wave_warp_modifier.h"
#include "wave_window_modifier.h"
#include "wavetable.h"
#include "wavetable_creator.h"
#include "wavetable_group.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace atlas::wavetable_edit {
  namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kMaxWindowSize = 9999.9;
    constexpr float kMaxLineCurve = 10.0f;

    using Type = WavetableComponentFactory::ComponentType;

    Parameter choice(const std::string& id, const juce::String& title, const juce::String& description,
                     const juce::StringArray& choices, bool per_keyframe = false) {
      Parameter parameter;
      parameter.id = id;
      parameter.title = title;
      parameter.description = description;
      parameter.choices = choices;
      parameter.minimum = 0.0;
      parameter.maximum = choices.size() - 1;
      parameter.interval = 1.0;
      parameter.decimals = 0;
      parameter.per_keyframe = per_keyframe;
      return parameter;
    }

    Parameter slider(const std::string& id, const juce::String& title, const juce::String& description,
                     double minimum, double maximum, double interval, const juce::String& suffix,
                     int decimals, bool per_keyframe) {
      Parameter parameter;
      parameter.id = id;
      parameter.title = title;
      parameter.description = description;
      parameter.minimum = minimum;
      parameter.maximum = maximum;
      parameter.interval = interval;
      parameter.suffix = suffix;
      parameter.decimals = decimals;
      parameter.per_keyframe = per_keyframe;
      return parameter;
    }

    const juce::StringArray kKeyframeInterpolation { "None", "Linear", "Smooth" };
    const juce::StringArray kWaveInterpolation { "None", "Waveform blend", "Spectral blend",
                                                 "Smooth waveform blend", "Smooth spectral blend" };
    const juce::StringArray kOffOn { "Off", "On" };

    Parameter keyframeInterpolation(const juce::String& what) {
      return choice("interpolation", "Keyframe interpolation",
                    "How " + what + " moves between keyframes along the timeline: None holds each keyframe "
                    "until the next, Linear glides evenly, Smooth glides on a curve", kKeyframeInterpolation);
    }

    int waveInterpolationIndex(const WaveSource* source) {
      if (source->getInterpolationStyle() == WavetableComponent::kNone)
        return 0;
      return 2 * source->getInterpolationStyle() + source->getInterpolationMode() - 1;
    }

    void setWaveInterpolationIndex(WaveSource* source, int index) {
      index = std::clamp(index, 0, 4);
      if (index == 0) {
        source->setInterpolationStyle(WavetableComponent::kNone);
        source->setInterpolationMode(WaveSource::kTime);
        return;
      }
      source->setInterpolationStyle(static_cast<WavetableComponent::InterpolationStyle>((index + 1) / 2));
      source->setInterpolationMode(static_cast<WaveSource::InterpolationMode>((index + 1) % 2));
    }

    bool validKeyframe(WavetableComponent* component, int keyframe) {
      return component != nullptr && keyframe >= 0 && keyframe < component->numFrames();
    }

    double clampTo(const Parameter& parameter, double value) {
      value = std::clamp(value, parameter.minimum, parameter.maximum);
      if (parameter.isChoice())
        value = std::round(value);
      return value;
    }

    double fileStartMaximum(FileSource* source) {
      return std::max(0.0, double(source->buffer()->size) - source->getWindowSize());
    }
  }

  juce::String componentName(WavetableComponent* component) {
    if (component == nullptr)
      return {};
    switch (component->getType()) {
      case WavetableComponentFactory::kWaveSource: return "Wave source";
      case WavetableComponentFactory::kLineSource: return "Line source";
      case WavetableComponentFactory::kFileSource: return "Audio file source";
      case WavetableComponentFactory::kShepardToneSource: return "Shepard tone source";
      default: return juce::String(WavetableComponentFactory::getComponentName(component->getType()));
    }
  }

  bool isSource(WavetableComponent* component) {
    return component != nullptr && component->getType() < WavetableComponentFactory::kBeginModifierTypes;
  }

  bool hasKeyframes(WavetableComponent* component) {
    return component != nullptr && component->hasKeyframes();
  }

  std::vector<Parameter> parameters(WavetableComponent* component) {
    std::vector<Parameter> list;
    if (component == nullptr)
      return list;

    if (auto* file = dynamic_cast<FileSource*>(component)) {
      list.push_back(keyframeInterpolation("the start position in the audio"));
      list.push_back(choice("fade_style", "Frame blend",
          "How each frame is cut from the audio: File blend crossfades whole cycles found in the file, None takes "
          "the window as it is, Time crossfades neighbouring windows, Spectral blends their harmonics",
          { "File blend", "None", "Time", "Spectral" }));
      list.push_back(choice("phase_style", "Phase style",
          "None keeps the audio's phases, Clear zeroes them for a clean tone, Vocode randomises them for a "
          "breathy, vocoded tone", { "None", "Clear", "Vocode" }));
      list.push_back(choice("normalize_gain", "Normalize gain",
          "Bring every frame to the same level", kOffOn));
      list.push_back(slider("window_size", "Window size",
          "Samples of audio in each frame. Type a note name such as A2 to fit one period of that note",
          1.0, kMaxWindowSize, 0.1, " samples", 1, false));
      list.push_back(slider("start_position", "Start position",
          "Where in the audio this keyframe's window begins. Moving the first and last keyframes crops the audio",
          0.0, std::max(1.0, fileStartMaximum(file)), 0.0, " samples", 0, true));
      list.push_back(slider("window_fade", "Window fade",
          "How much of the window crossfades into the next, which smooths loops", 0.0, 100.0, 0.0, "%", 1, true));
      return list;
    }
    if (auto* shepard = dynamic_cast<ShepardToneSource*>(component)) {
      (void) shepard;
      return list;
    }
    if (auto* wave = dynamic_cast<WaveSource*>(component)) {
      (void) wave;
      list.push_back(choice("interpolation", "Frame interpolation",
          "How positions between source frames are generated: Waveform blend crossfades the samples, Spectral "
          "blend crossfades the harmonics, Smooth versions glide on a curve. Only positions between frames are "
          "generated, so frames on neighbouring positions sound the same in every mode", kWaveInterpolation));
      return list;
    }
    if (dynamic_cast<WaveLineSource*>(component) != nullptr) {
      list.push_back(keyframeInterpolation("the line shape"));
      list.push_back(choice("smooth", "Smooth line", "Round the corners between line points", kOffOn));
      list.push_back(slider("pull_power", "Keyframe pull",
          "Bends the morph from this keyframe to the next: 0 is even, higher values stay near this shape longer",
          0.0, 5.0, 0.0, "", 2, true));
      return list;
    }
    if (auto* phase = dynamic_cast<PhaseModifier*>(component)) {
      (void) phase;
      list.push_back(keyframeInterpolation("the phase shift"));
      list.push_back(choice("phase_style", "Phase style",
          "Normal shifts the whole wave, Even/odd shifts even and odd harmonics in opposite directions, Harmonic "
          "shifts each harmonic by its number, Clear zeroes every phase",
          { "Normal", "Even/odd", "Harmonic", "Harmonic even/odd", "Clear" }));
      list.push_back(slider("phase", "Phase", "Phase shift in degrees", -180.0, 180.0, 0.0, " degrees", 1, true));
      list.push_back(slider("mix", "Mix", "Blend between the unshifted and shifted wave", 0.0, 100.0, 0.0, "%", 1, true));
      return list;
    }
    if (dynamic_cast<WaveWindowModifier*>(component) != nullptr) {
      list.push_back(keyframeInterpolation("the window"));
      list.push_back(choice("window_shape", "Window shape", "Shape of the fade at each edge of the window",
                            { "Raised cosine", "Half sine", "Linear", "Square", "Wiggle" }));
      list.push_back(slider("left", "Left edge", "Where the window finishes fading in, across the cycle",
                            0.0, 100.0, 0.0, "%", 1, true));
      list.push_back(slider("right", "Right edge", "Where the window starts fading out, across the cycle",
                            0.0, 100.0, 0.0, "%", 1, true));
      return list;
    }
    if (dynamic_cast<FrequencyFilterModifier*>(component) != nullptr) {
      list.push_back(keyframeInterpolation("the filter"));
      list.push_back(choice("filter_style", "Filter style", "Which harmonics the filter keeps",
                            { "Low pass", "Band pass", "High pass", "Comb" }));
      list.push_back(choice("normalize", "Normalize", "Restore the level after filtering", kOffOn));
      list.push_back(slider("cutoff", "Cutoff", "Filter cutoff in octaves of harmonics above the fundamental",
                            0.0, vital::WaveFrame::kWaveformBits - 1, 0.0, " octaves", 2, true));
      list.push_back(slider("shape", "Shape", "Steepness, or the comb's spacing", 0.0, 100.0, 0.0, "%", 1, true));
      return list;
    }
    if (dynamic_cast<SlewLimitModifier*>(component) != nullptr) {
      list.push_back(keyframeInterpolation("the slew limits"));
      list.push_back(slider("up", "Rise limit", "How fast the wave may rise; lower values round off rising edges",
                            0.0, 100.0, 0.0, "%", 1, true));
      list.push_back(slider("down", "Fall limit", "How fast the wave may fall; lower values round off falling edges",
                            0.0, 100.0, 0.0, "%", 1, true));
      return list;
    }
    if (dynamic_cast<WaveFoldModifier*>(component) != nullptr) {
      list.push_back(keyframeInterpolation("the fold amount"));
      list.push_back(slider("fold", "Fold amount", "How far the wave is boosted before folding back on itself",
                            1.0, 32.0, 0.0, "x", 2, true));
      return list;
    }
    if (dynamic_cast<WaveWarpModifier*>(component) != nullptr) {
      list.push_back(keyframeInterpolation("the warp"));
      list.push_back(choice("horizontal_asymmetric", "Horizontal asymmetric",
                            "Warp time from one end of the cycle instead of from the middle", kOffOn));
      list.push_back(choice("vertical_asymmetric", "Vertical asymmetric",
                            "Warp level from the bottom instead of from the centre line", kOffOn));
      list.push_back(slider("horizontal", "Horizontal warp", "Squeezes the wave towards one side of the cycle",
                            -20.0, 20.0, 0.0, "", 2, true));
      list.push_back(slider("vertical", "Vertical warp", "Bends the wave's levels towards the peaks or the centre",
                            -20.0, 20.0, 0.0, "", 2, true));
      return list;
    }
    return list;
  }

  const Parameter* findParameter(const std::vector<Parameter>& list, const std::string& id) {
    for (const auto& parameter : list)
      if (parameter.id == id)
        return &parameter;
    return nullptr;
  }

  double getValue(WavetableComponent* component, const std::string& id, int keyframe) {
    if (component == nullptr)
      return 0.0;
    if (id == "interpolation") {
      if (auto* wave = dynamic_cast<WaveSource*>(component); wave != nullptr && component->getType() == Type::kWaveSource)
        return waveInterpolationIndex(wave);
      return component->getInterpolationStyle();
    }
    const bool frame_ok = validKeyframe(component, keyframe);

    if (auto* file = dynamic_cast<FileSource*>(component)) {
      if (id == "fade_style") return file->getFadeStyle();
      if (id == "phase_style") return file->getPhaseStyle();
      if (id == "normalize_gain") return file->getNormalizeGain() ? 1.0 : 0.0;
      if (id == "window_size") return file->getWindowSize();
      if (!frame_ok) return 0.0;
      auto* frame = file->getKeyframe(keyframe);
      if (id == "start_position") return frame->getStartPosition();
      if (id == "window_fade") return 100.0 * frame->getWindowFade();
      return 0.0;
    }
    if (auto* line = dynamic_cast<WaveLineSource*>(component)) {
      if (id == "smooth")
        return line->numFrames() > 0 && line->getKeyframe(0)->getLineGenerator()->smooth() ? 1.0 : 0.0;
      if (id == "pull_power" && frame_ok) return line->getKeyframe(keyframe)->getPullPower();
      return 0.0;
    }
    if (auto* phase = dynamic_cast<PhaseModifier*>(component)) {
      if (id == "phase_style") return phase->getPhaseStyle();
      if (!frame_ok) return 0.0;
      if (id == "phase") return phase->getKeyframe(keyframe)->getPhase() * 180.0 / kPi;
      if (id == "mix") return 100.0 * phase->getKeyframe(keyframe)->getMix();
      return 0.0;
    }
    if (auto* window = dynamic_cast<WaveWindowModifier*>(component)) {
      if (id == "window_shape") return window->getWindowShape();
      if (!frame_ok) return 0.0;
      if (id == "left") return 100.0 * window->getKeyframe(keyframe)->getLeft();
      if (id == "right") return 100.0 * window->getKeyframe(keyframe)->getRight();
      return 0.0;
    }
    if (auto* filter = dynamic_cast<FrequencyFilterModifier*>(component)) {
      if (id == "filter_style") return filter->getStyle();
      if (id == "normalize") return filter->getNormalize() ? 1.0 : 0.0;
      if (!frame_ok) return 0.0;
      if (id == "cutoff") return filter->getKeyframe(keyframe)->getCutoff();
      if (id == "shape") return 100.0 * filter->getKeyframe(keyframe)->getShape();
      return 0.0;
    }
    if (auto* slew = dynamic_cast<SlewLimitModifier*>(component)) {
      if (!frame_ok) return 0.0;
      if (id == "up") return 100.0 * slew->getKeyframe(keyframe)->getSlewUpLimit();
      if (id == "down") return 100.0 * slew->getKeyframe(keyframe)->getSlewDownLimit();
      return 0.0;
    }
    if (auto* fold = dynamic_cast<WaveFoldModifier*>(component)) {
      if (id == "fold" && frame_ok) return fold->getKeyframe(keyframe)->getWaveFoldBoost();
      return 0.0;
    }
    if (auto* warp = dynamic_cast<WaveWarpModifier*>(component)) {
      if (id == "horizontal_asymmetric") return warp->getHorizontalAsymmetric() ? 1.0 : 0.0;
      if (id == "vertical_asymmetric") return warp->getVerticalAsymmetric() ? 1.0 : 0.0;
      if (!frame_ok) return 0.0;
      if (id == "horizontal") return warp->getKeyframe(keyframe)->getHorizontalPower();
      if (id == "vertical") return warp->getKeyframe(keyframe)->getVerticalPower();
      return 0.0;
    }
    return 0.0;
  }

  bool setValue(WavetableComponent* component, const std::string& id, int keyframe, double raw) {
    if (component == nullptr)
      return false;
    const auto list = parameters(component);
    const Parameter* parameter = findParameter(list, id);
    if (parameter == nullptr)
      return false;
    if (parameter->per_keyframe && !validKeyframe(component, keyframe))
      return false;
    const double value = clampTo(*parameter, raw);
    const int index = int(value);
    const float v = float(value);

    if (id == "interpolation") {
      if (auto* wave = dynamic_cast<WaveSource*>(component); wave != nullptr && component->getType() == Type::kWaveSource)
        setWaveInterpolationIndex(wave, index);
      else
        component->setInterpolationStyle(static_cast<WavetableComponent::InterpolationStyle>(index));
      return true;
    }
    if (auto* file = dynamic_cast<FileSource*>(component)) {
      if (id == "fade_style") file->setFadeStyle(static_cast<FileSource::FadeStyle>(index));
      else if (id == "phase_style") file->setPhaseStyle(static_cast<FileSource::PhaseStyle>(index));
      else if (id == "normalize_gain") file->setNormalizeGain(index != 0);
      else if (id == "window_size") {
        file->setWindowSize(value);
        // A longer window leaves less audio to start in: keep every keyframe inside the file.
        const double maximum = fileStartMaximum(file);
        for (int i = 0; i < file->numFrames(); ++i)
          file->getKeyframe(i)->setStartPosition(std::min(file->getKeyframe(i)->getStartPosition(), maximum));
      }
      else if (id == "start_position") file->getKeyframe(keyframe)->setStartPosition(std::min(value, fileStartMaximum(file)));
      else if (id == "window_fade") file->getKeyframe(keyframe)->setWindowFade(value / 100.0);
      else return false;
      return true;
    }
    if (auto* line = dynamic_cast<WaveLineSource*>(component)) {
      if (id == "smooth") return setLineSmooth(line, index != 0);
      if (id == "pull_power") { line->getKeyframe(keyframe)->setPullPower(v); return true; }
      return false;
    }
    if (auto* phase = dynamic_cast<PhaseModifier*>(component)) {
      if (id == "phase_style") phase->setPhaseStyle(static_cast<PhaseModifier::PhaseStyle>(index));
      else if (id == "phase") phase->getKeyframe(keyframe)->setPhase(float(value * kPi / 180.0));
      else if (id == "mix") phase->getKeyframe(keyframe)->setMix(v / 100.0f);
      else return false;
      return true;
    }
    if (auto* window = dynamic_cast<WaveWindowModifier*>(component)) {
      if (id == "window_shape") window->setWindowShape(static_cast<WaveWindowModifier::WindowShape>(index));
      else if (id == "left") window->getKeyframe(keyframe)->setLeft(v / 100.0f);
      else if (id == "right") window->getKeyframe(keyframe)->setRight(v / 100.0f);
      else return false;
      return true;
    }
    if (auto* filter = dynamic_cast<FrequencyFilterModifier*>(component)) {
      if (id == "filter_style") filter->setStyle(static_cast<FrequencyFilterModifier::FilterStyle>(index));
      else if (id == "normalize") filter->setNormalize(index != 0);
      else if (id == "cutoff") filter->getKeyframe(keyframe)->setCutoff(v);
      else if (id == "shape") filter->getKeyframe(keyframe)->setShape(v / 100.0f);
      else return false;
      return true;
    }
    if (auto* slew = dynamic_cast<SlewLimitModifier*>(component)) {
      if (id == "up") slew->getKeyframe(keyframe)->setSlewUpLimit(v / 100.0f);
      else if (id == "down") slew->getKeyframe(keyframe)->setSlewDownLimit(v / 100.0f);
      else return false;
      return true;
    }
    if (auto* fold = dynamic_cast<WaveFoldModifier*>(component)) {
      if (id != "fold") return false;
      fold->getKeyframe(keyframe)->setWaveFoldBoost(v);
      return true;
    }
    if (auto* warp = dynamic_cast<WaveWarpModifier*>(component)) {
      if (id == "horizontal_asymmetric") warp->setHorizontalAsymmetric(index != 0);
      else if (id == "vertical_asymmetric") warp->setVerticalAsymmetric(index != 0);
      else if (id == "horizontal") warp->getKeyframe(keyframe)->setHorizontalPower(v);
      else if (id == "vertical") warp->getKeyframe(keyframe)->setVerticalPower(v);
      else return false;
      return true;
    }
    return false;
  }

  juce::String formatValue(const Parameter& parameter, double value) {
    if (parameter.isChoice())
      return parameter.choices[std::clamp(int(std::round(value)), 0, parameter.choices.size() - 1)];
    return juce::String(value, parameter.decimals) + parameter.suffix;
  }

  juce::String describeKeyframe(WavetableComponent* component, int index) {
    if (!hasKeyframes(component))
      return "No keyframes";
    if (!validKeyframe(component, index))
      return "No keyframe selected";
    return "Keyframe " + juce::String(index + 1) + " of " + juce::String(component->numFrames()) +
           ", position " + juce::String(component->getFrameAt(index)->position() + 1) + " of " +
           juce::String(kNumPositions);
  }

  int suggestKeyframePosition(WavetableComponent* component, int after_index) {
    if (!hasKeyframes(component))
      return -1;
    const int count = component->numFrames();
    if (count == 0)
      return 0;
    after_index = std::clamp(after_index, 0, count - 1);
    const int here = component->getFrameAt(after_index)->position();
    // Midway to the next keyframe; after the last one, the end of the timeline,
    // so a new keyframe leaves positions for interpolation to fill.
    if (after_index + 1 < count) {
      const int next = component->getFrameAt(after_index + 1)->position();
      if (next - here > 1)
        return here + (next - here) / 2;
    }
    else if (here < kNumPositions - 1)
      return kNumPositions - 1;
    // Otherwise the middle of the widest gap anywhere, or before the first keyframe.
    int best = -1, best_gap = 1;
    if (component->getFrameAt(0)->position() > 0) {
      best = component->getFrameAt(0)->position() / 2;
      best_gap = component->getFrameAt(0)->position();
    }
    for (int i = 0; i + 1 < count; ++i) {
      const int gap = component->getFrameAt(i + 1)->position() - component->getFrameAt(i)->position();
      if (gap > best_gap) {
        best_gap = gap;
        best = component->getFrameAt(i)->position() + gap / 2;
      }
    }
    const int last = component->getFrameAt(count - 1)->position();
    if (kNumPositions - 1 - last > best_gap)
      best = kNumPositions - 1;
    return best;
  }

  int addKeyframe(WavetableComponent* component, int position) {
    if (!hasKeyframes(component) || position < 0 || position >= kNumPositions)
      return -1;
    for (int i = 0; i < component->numFrames(); ++i)
      if (component->getFrameAt(i)->position() == position)
        return -1;
    auto* keyframe = component->insertNewKeyframe(position);
    if (auto* wave = dynamic_cast<WaveSource*>(component)) {
      // The rendered in-between frame carries the neighbours' pitch metadata.
      if (wave->numFrames() > 1) {
        const int index = component->indexOf(keyframe);
        auto* neighbour = wave->getKeyframe(index > 0 ? index - 1 : index + 1)->wave_frame();
        auto* frame = wave->getKeyframe(index)->wave_frame();
        frame->setFrequencyRatio(neighbour->frequency_ratio);
        frame->setSampleRate(neighbour->sample_rate);
      }
    }
    return component->indexOf(keyframe);
  }

  bool removeKeyframe(WavetableComponent* component, int index) {
    if (!validKeyframe(component, index) || component->numFrames() <= 1)
      return false;
    component->remove(component->getFrameAt(index));
    return true;
  }

  int moveKeyframe(WavetableComponent* component, int index, int position) {
    if (!validKeyframe(component, index))
      return -1;
    auto* keyframe = component->getFrameAt(index);
    const int minimum = index > 0 ? component->getFrameAt(index - 1)->position() + 1 : 0;
    const int maximum = index + 1 < component->numFrames() ?
        component->getFrameAt(index + 1)->position() - 1 : kNumPositions - 1;
    const int target = std::clamp(position, minimum, maximum);
    if (target != keyframe->position()) {
      keyframe->setPosition(target);
      component->reposition(keyframe);
    }
    return target;
  }

  bool spreadKeyframes(WavetableComponent* component) {
    if (!hasKeyframes(component) || component->numFrames() < 2)
      return false;
    const int count = component->numFrames();
    // Positions only grow with the index, so the order never changes.
    for (int i = 0; i < count; ++i)
      component->getFrameAt(i)->setPosition(int(std::lround(double(i) * (kNumPositions - 1) / (count - 1))));
    return true;
  }

  juce::StringArray sourceTypeNames() { return { "Wave source", "Line source", "Audio file source" }; }

  juce::StringArray modifierTypeNames() {
    juce::StringArray names;
    for (int i = 0; i < WavetableComponentFactory::numModifierTypes(); ++i)
      names.add(WavetableComponentFactory::getComponentName(WavetableComponentFactory::getModifierType(i)));
    return names;
  }

  WavetableGroup* addLayer(WavetableCreator& creator, int source_type) {
    if (source_type < 0 || source_type >= WavetableComponentFactory::numSourceTypes())
      return nullptr;
    auto* component = WavetableComponentFactory::createComponent(WavetableComponentFactory::getSourceType(source_type));
    if (component == nullptr)
      return nullptr;
    component->insertNewKeyframe(0);
    if (auto* wave = dynamic_cast<WaveSource*>(component)) {
      // A new wave layer starts as a sine rather than silence.
      auto* frame = wave->getWaveFrame(0);
      for (int i = 0; i < vital::WaveFrame::kWaveformSize; ++i)
        frame->time_domain[i] = std::sin(2.0f * float(kPi) * i / vital::WaveFrame::kWaveformSize);
      frame->toFrequencyDomain();
    }
    else if (auto* line = dynamic_cast<WaveLineSource*>(component)) {
      line->setNumPoints(line->getKeyframe(0)->getNumPoints());
    }
    auto* group = new WavetableGroup();
    group->addComponent(component);
    creator.addGroup(group);
    return group;
  }

  bool removeLayer(WavetableCreator& creator, int index) {
    if (index < 0 || index >= creator.numGroups() || creator.numGroups() <= 1)
      return false;
    creator.removeGroup(index);
    return true;
  }

  bool moveLayer(WavetableCreator& creator, int index, int direction) {
    const int destination = index + (direction < 0 ? -1 : 1);
    if (index < 0 || index >= creator.numGroups() || destination < 0 || destination >= creator.numGroups())
      return false;
    if (direction < 0) creator.moveUp(index);
    else creator.moveDown(index);
    return true;
  }

  WavetableComponent* addModifier(WavetableGroup& group, int modifier_type) {
    if (modifier_type < 0 || modifier_type >= WavetableComponentFactory::numModifierTypes())
      return nullptr;
    auto* component = WavetableComponentFactory::createComponent(WavetableComponentFactory::getModifierType(modifier_type));
    if (component == nullptr)
      return nullptr;
    component->insertNewKeyframe(0);
    group.addComponent(component);
    return component;
  }

  juce::String describeLayer(WavetableCreator& creator, int index) {
    if (index < 0 || index >= creator.numGroups())
      return {};
    auto* group = creator.getGroup(index);
    juce::String text = "Layer " + juce::String(index + 1);
    if (group->numComponents() == 0)
      return text + ", empty";
    auto* source = group->getComponent(0);
    text << ", " << componentName(source);
    if (hasKeyframes(source)) {
      const bool wave = source->getType() == Type::kWaveSource;
      text << ", " << source->numFrames() << (wave ? " frame" : " keyframe") << (source->numFrames() == 1 ? "" : "s");
    }
    if (group->numComponents() > 1) {
      juce::StringArray modifiers;
      for (int i = 1; i < group->numComponents(); ++i)
        modifiers.add(componentName(group->getComponent(i)));
      text << ", modifiers: " << modifiers.joinIntoString(", ");
    }
    return text;
  }

  juce::String describeWavetable(WavetableCreator& creator) {
    juce::String text = juce::String(creator.getName());
    if (text.isEmpty())
      text = "Untitled wavetable";
    const int layers = creator.numGroups();
    text << ", " << layers << (layers == 1 ? " layer" : " layers");
    if (auto* wavetable = creator.getWavetable(); wavetable != nullptr && wavetable->getAllData() != nullptr)
      text << ", " << wavetable->getAllData()->num_frames << " positions";
    for (int i = 0; i < layers; ++i)
      text << ". " << describeLayer(creator, i);
    return text;
  }

  int numLinePoints(WaveLineSource* source) {
    return source != nullptr && source->numFrames() > 0 ? source->getKeyframe(0)->getNumPoints() : 0;
  }

  int addLinePoint(WaveLineSource* source, int after_index) {
    const int count = numLinePoints(source);
    if (count == 0 || count >= LineGenerator::kMaxPoints)
      return -1;
    // addMiddlePoint(i) inserts between points i - 1 and i.
    const int insert = std::clamp(after_index + 1, 1, count);
    const int at = std::min(insert, count - 1);
    for (int i = 0; i < source->numFrames(); ++i)
      source->getKeyframe(i)->addMiddlePoint(at);
    source->setNumPoints(count + 1);
    return at;
  }

  bool removeLinePoint(WaveLineSource* source, int index) {
    const int count = numLinePoints(source);
    if (count <= 2 || index < 0 || index >= count)
      return false;
    for (int i = 0; i < source->numFrames(); ++i)
      source->getKeyframe(i)->removePoint(index);
    source->setNumPoints(count - 1);
    return true;
  }

  bool setLineSmooth(WaveLineSource* source, bool smooth) {
    if (source == nullptr || source->numFrames() == 0)
      return false;
    for (int i = 0; i < source->numFrames(); ++i)
      source->getKeyframe(i)->setSmooth(smooth);
    return true;
  }

  double linePointValue(WaveLineSource* source, int keyframe, int point, const std::string& field) {
    if (source == nullptr || keyframe < 0 || keyframe >= source->numFrames())
      return 0.0;
    auto* frame = source->getKeyframe(keyframe);
    if (point < 0 || point >= frame->getNumPoints())
      return 0.0;
    const auto position = frame->getPoint(point);
    if (field == "phase") return 100.0 * position.first;
    if (field == "level") return 100.0 * (1.0 - 2.0 * position.second);
    if (field == "curve") return frame->getPower(point);
    return 0.0;
  }

  bool setLinePointValue(WaveLineSource* source, int keyframe, int point, const std::string& field, double value) {
    if (source == nullptr || keyframe < 0 || keyframe >= source->numFrames())
      return false;
    auto* frame = source->getKeyframe(keyframe);
    const int count = frame->getNumPoints();
    if (point < 0 || point >= count)
      return false;
    auto position = frame->getPoint(point);
    if (field == "phase") {
      // Points stay in order and the ends stay at the ends of the cycle.
      float x = float(std::clamp(value, 0.0, 100.0) / 100.0);
      const float low = point > 0 ? frame->getPoint(point - 1).first : 0.0f;
      const float high = point + 1 < count ? frame->getPoint(point + 1).first : 1.0f;
      if (point == 0) x = 0.0f;
      else if (point == count - 1) x = 1.0f;
      position.first = std::clamp(x, low, high);
    }
    else if (field == "level")
      position.second = float((1.0 - std::clamp(value, -100.0, 100.0) / 100.0) / 2.0);
    else if (field == "curve") {
      frame->setPower(float(std::clamp(value, double(-kMaxLineCurve), double(kMaxLineCurve))), point);
      return true;
    }
    else
      return false;
    frame->setPoint(position, point);
    return true;
  }

  int fileSourceSamples(WavetableComponent* component) {
    auto* file = dynamic_cast<FileSource*>(component);
    return file != nullptr ? file->buffer()->size : 0;
  }

  int fileSourceSampleRate(WavetableComponent* component) {
    auto* file = dynamic_cast<FileSource*>(component);
    return file != nullptr ? file->buffer()->sample_rate : 0;
  }

  double windowSizeFromText(const juce::String& text, int sample_rate) {
    const juce::String trimmed = text.trim();
    const int note = Tuning::noteToMidiKey(trimmed);
    if (note >= 0 && note < vital::kMidiSize && sample_rate > 0)
      return std::clamp(double(sample_rate) / vital::utils::midiNoteToFrequency(float(note)), 1.0, kMaxWindowSize);
    return std::clamp(trimmed.getDoubleValue(), 1.0, kMaxWindowSize);
  }

  bool loadAudioLayer(WavetableComponent* component, const float* samples, int num_samples, int sample_rate) {
    auto* file = dynamic_cast<FileSource*>(component);
    if (file == nullptr || samples == nullptr || num_samples <= 0 || sample_rate <= 0)
      return false;
    file->loadBuffer(samples, num_samples, sample_rate);
    file->detectPitch();
    file->setFadeStyle(FileSource::kWaveBlend);
    if (file->numFrames() == 0)
      file->insertNewKeyframe(0);
    if (file->numFrames() < 2)
      file->insertNewKeyframe(kNumPositions - 1);
    file->getKeyframe(0)->setStartPosition(0);
    auto* last = file->getKeyframe(file->numFrames() - 1);
    last->setStartPosition(std::max(0, num_samples - last->getSamplesNeeded()));
    return true;
  }

  bool replaceFileSourceAudio(WavetableComponent* component, const float* samples, int num_samples, int sample_rate) {
    auto* file = dynamic_cast<FileSource*>(component);
    if (file == nullptr || samples == nullptr || num_samples <= 0 || sample_rate <= 0)
      return false;
    file->loadBuffer(samples, num_samples, sample_rate);
    file->detectPitch();
    const double maximum = fileStartMaximum(file);
    for (int i = 0; i < file->numFrames(); ++i)
      file->getKeyframe(i)->setStartPosition(std::min(file->getKeyframe(i)->getStartPosition(), maximum));
    return true;
  }

  WaveSource* waveSourceOfLayer(WavetableCreator* creator, int layer) {
    if (creator == nullptr || layer < 0 || layer >= creator->numGroups())
      return nullptr;
    auto* group = creator->getGroup(layer);
    if (group == nullptr || group->numComponents() == 0)
      return nullptr;
    auto* component = group->getComponent(0);
    if (component == nullptr || component->getType() != Type::kWaveSource)
      return nullptr;
    return dynamic_cast<WaveSource*>(component);
  }

  int interpolatedPositions(const WaveSource* source) {
    if (source == nullptr || source->numFrames() < 2)
      return 0;
    int positions = 0;
    for (int frame = 1; frame < source->numFrames(); ++frame)
      positions += std::max(0, source->getFrameAt(frame)->position() - source->getFrameAt(frame - 1)->position() - 1);
    return positions;
  }

  WaveSource* flattenToWaveSource(WavetableCreator* creator) {
    if (creator == nullptr || creator->getWavetable() == nullptr)
      return nullptr;
    auto* wavetable = creator->getWavetable();
    const auto* data = wavetable->getAllData();
    if (data == nullptr || data->num_frames <= 0)
      return nullptr;

    const std::string name = creator->getName();
    const std::string author = creator->getAuthor();
    auto* group = new WavetableGroup();
    auto* source = new WaveSource();
    const int frames = std::clamp(data->num_frames, 1, vital::kNumOscillatorWaveFrames);
    for (int frame = 0; frame < frames; ++frame) {
      source->insertNewKeyframe(frame);
      auto* wave_frame = source->getKeyframe(frame)->wave_frame();
      std::memcpy(wave_frame->time_domain, data->wave_data[frame], sizeof(vital::mono_float) * vital::WaveFrame::kWaveformSize);
      wave_frame->index = frame;
      wave_frame->setFrequencyRatio(data->frequency_ratio);
      wave_frame->setSampleRate(data->sample_rate);
      wave_frame->toFrequencyDomain();
    }
    source->setInterpolationStyle(WavetableComponent::kLinear);
    group->addComponent(source);
    creator->clear();
    creator->setName(name);
    creator->setAuthor(author);
    creator->addGroup(group);
    creator->render();
    return source;
  }
}
