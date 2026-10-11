/* Copyright 2013-2019 Matt Tytel
 *
 * pylon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pylon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pylon.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once
#include "suppressor_parameter_utils.h"

#include "JuceHeader.h"
#include "value.h"
#include "synth_parameters.h"
#include "scream_parameter_utils.h"
#include "envelope_time_utils.h"

#include <algorithm>
#include <cmath>

class ValueBridge : public AudioProcessorParameterWithID {
  public:
    class Listener {
      public:
        virtual ~Listener() { }
        virtual void parameterChanged(const std::string& name, vital::mono_float value) = 0;
    };

    ValueBridge() = delete;

    ValueBridge(std::string name, vital::Value* value, const String& hostParameterId = {}) :
        AudioProcessorParameterWithID(
            ParameterID(hostParameterId.isNotEmpty() ? hostParameterId : String(name),
                        std::max(1, vital::Parameters::getDetails(name).version_added)),
            vital::Parameters::getDetails(name).display_name),
        name_(name), engine_name_(name), value_(value), listener_(nullptr),
        source_changed_(false) {
      details_ = vital::Parameters::getDetails(name);
      span_ = details_.max - details_.min;
      if (details_.value_scale == vital::ValueDetails::kIndexed)
        span_ = std::round(span_);
    }

    std::function<double()> suppressorSampleRate;
    double getSuppressorSampleRate() const { return suppressorSampleRate ? suppressorSampleRate() : 44100.0; }

    float getValue() const override {
      return convertToPluginValue(value_->value());
    }

    void setValue(float value) override {
      // Hosts can send NaN/out-of-range normalized values; don't pass them on.
      if (!std::isfinite(value))
        return;
      value = std::clamp(value, 0.0f, 1.0f);
      if (listener_ && !source_changed_) {
        source_changed_ = true;
        vital::mono_float synth_value = convertToEngineValue(value);
        listener_->parameterChanged(engine_name_, synth_value);
        source_changed_ = false;
      }
    }

    void setListener(Listener* listener) {
      listener_ = listener;
    }

    float getDefaultValue() const override {
      return convertToPluginValue(details_.default_value);
    }

    String getName(int maximumStringLength) const override {
      return String(details_.display_name).substring(0, maximumStringLength);
    }

    String getLabel() const override {
      return "";
    }

    String getText(float value, int maximumStringLength) const override {
      if (isEnvelopeTimeSynced())
        return String(vital::envelope_time::divisions[envelopeTimeDivision(value)].name).substring(0, maximumStringLength);
      float adjusted = convertToEngineValue(value);
      if (vital::suppressor::isFrequency(details_.name))
        return (String(vital::suppressor::positionHz(adjusted, getSuppressorSampleRate()), 2) + " Hz").substring(0, maximumStringLength);
      if (vital::scream::parameterIs(details_.name, "cutoff"))
        return formatDisplayValue(vital::scream::cutoffToHz(adjusted)).substring(0, maximumStringLength).trim();
      if (usesRangePercentDisplay(details_))
        return rangePercentText(adjusted).substring(0, maximumStringLength);
      String result = "";
      if (details_.string_lookup)
        result = details_.string_lookup[std::max<int>(0, std::min(adjusted, details_.max))];
      else {
        float display_value = details_.display_multiply * skewValue(adjusted) + details_.post_offset;
        result = formatDisplayValue(display_value);
      }
      return result.substring(0, maximumStringLength).trim();
    }

    float getValueForText(const String &text) const override {
      const String trimmed = text.trim();
      if (vital::suppressor::isFrequency(details_.name)) {
        const double hz = trimmed.getDoubleValue() * (trimmed.containsIgnoreCase("k") ? 1000.0 : 1.0);
        return convertToPluginValue(jlimit(details_.min, details_.max,
            static_cast<float>(vital::suppressor::hzPosition(hz, getSuppressorSampleRate()))));
      }
      if (isEnvelopeTimeSynced()) {
        const String division_text = trimmed.toLowerCase().replace("dotted", "d").replace("triplet", "t")
                                            .removeCharacters(" ");
        for (int i = 0; i < static_cast<int>(vital::envelope_time::divisions.size()); ++i)
          if (division_text == vital::envelope_time::divisions[i].name)
            return valueForEnvelopeTimeDivision(i);
      }
      if (vital::scream::parameterIs(details_.name, "cutoff")) {
        float hz = trimmed.getFloatValue() * (trimmed.containsIgnoreCase("kHz") ? 1000.0f : 1.0f);
        return convertToPluginValue(vital::scream::cutoffFromHz(hz));
      }
      if (details_.string_lookup) {
        for (int i = 0; i <= static_cast<int>(span_); ++i) {
          if (trimmed.equalsIgnoreCase(String(details_.string_lookup[i])))
            return convertToPluginValue(details_.min + i);
        }
      }
      if (usesRangePercentDisplay(details_))
        return convertToPluginValue(rangePercentEngineValue(details_, trimmed));

      float display_value = trimmed.getFloatValue();
      if ((name_.startsWith("tape_") || name_.contains("_tape_") || name_.endsWith("defuse_frequency") ||
           name_.startsWith("resonator_") || name_.contains("_resonator_")) &&
          String(details_.display_units).trim() == "Hz" && trimmed.containsIgnoreCase("kHz"))
        display_value *= 1000.0f;
      if (trimmed.toLowerCase().contains("ms"))
        display_value *= 0.001f;
      else if (usesPercentDisplay() && trimmed.contains("%") &&
               details_.display_multiply == 1.0f && details_.max <= 1.0f)
        display_value /= 100.0f;
      else if (usesPercentDisplay() && details_.display_multiply == 1.0f &&
               details_.max <= 1.0f && std::abs(display_value) > 1.0f)
        display_value /= 100.0f;

      float scaled_value = (display_value - details_.post_offset) /
                           (details_.display_multiply == 0.0f ? 1.0f : details_.display_multiply);
      if (usesPercentDisplay() && details_.display_multiply == 1.0f)
        scaled_value = display_value;

      float engine_value = unskewValue(scaled_value);
      return convertToPluginValue(std::max(details_.min, std::min(engine_value, details_.max)));
    }

    bool isAutomatable() const override {
      return true;
    }

    int getNumSteps() const override {
      if (isDiscrete())
        return 1 + (int)span_;
      return AudioProcessorParameter::getNumSteps();
    }

    bool isDiscrete() const override {
      static constexpr int kMaxIndexedSteps = 300;
      return details_.value_scale == vital::ValueDetails::kIndexed &&
             (span_ < kMaxIndexedSteps || name_.endsWith("defuse_stages") ||
              name_.endsWith("spectral_morph_seed"));
    }

    bool isBoolean() const override {
      return isDiscrete() && span_ == 1.0f;
    }

    const String& getParameterId() const { return name_; }
    const vital::ValueDetails& getDetails() const { return details_; }

    // Unitless levels and amounts read as a percentage of their range: 0% to
    // 100%, or -100% to 100% for bipolar ones such as modulation amounts. A
    // level shows its control position, not the squared gain behind it.
    static bool usesRangePercentDisplay(const vital::ValueDetails& details) {
      if (details.string_lookup != nullptr || String(details.display_units).trim().isNotEmpty() ||
          details.display_multiply != 1.0f || details.post_offset != 0.0f || details.max != 1.0f ||
          (details.min != 0.0f && details.min != -1.0f))
        return false;
      if (details.value_scale != vital::ValueDetails::kLinear && details.value_scale != vital::ValueDetails::kQuadratic)
        return false;
      // Mixes already read as percentages. Ratios, a cycle phase and the trance
      // gate's step shapes are not amounts.
      const String name(details.name);
      return !name.endsWith("_mix") && !name.endsWith("_dry_wet") && !name.contains("ratio") &&
             !name.endsWith("phase") && !name.contains("trance_gate_step_");
    }

    static String rangePercentText(float engineValue) {
      const float percent = engineValue * 100.0f;
      return compactFloat(std::abs(percent) < 0.05f ? 0.0f : percent, 1) + "%";
    }

    // As for the mix controls: a % sign, or a number beyond 1, is a percentage.
    static float rangePercentEngineValue(const vital::ValueDetails& details, const String& text) {
      float amount = text.trim().getFloatValue();
      if (text.contains("%") || std::abs(amount) > 1.0f)
        amount /= 100.0f;
      return std::clamp(amount, details.min, details.max);
    }

    void setEnvelopeSyncContext(const vital::Value* sync, const vital::Value* beats_per_second) {
      envelope_sync_ = sync;
      envelope_beats_per_second_ = beats_per_second;
    }
    bool hasEnvelopeSyncContext() const { return envelope_sync_ != nullptr; }
    bool isEnvelopeTimeSynced() const { return envelope_sync_ != nullptr && envelope_sync_->value() >= 0.5f; }
    float envelopeBeatsPerSecond() const {
      return std::max(.0001f, envelope_beats_per_second_ != nullptr ? envelope_beats_per_second_->value() : 2.0f);
    }
    int envelopeTimeDivision(float normalized) const {
      return vital::envelope_time::closestDivision(skewValue(convertToEngineValue(normalized)), envelopeBeatsPerSecond());
    }
    float valueForEnvelopeTimeDivision(int index) const {
      index = std::clamp(index, 0, static_cast<int>(vital::envelope_time::divisions.size()) - 1);
      const float seconds = vital::envelope_time::divisions[index].beats / envelopeBeatsPerSecond();
      return convertToPluginValue(std::clamp(unskewValue(seconds), details_.min, details_.max));
    }
    int maximumEnvelopeTimeDivision() const {
      // Include the endpoint's nearest division: that is what the DSP plays
      // even when its exact duration lies just outside the free-time range.
      return envelopeTimeDivision(1.0f);
    }

    // Converts internal value to value from 0.0 to 1.0.
    float convertToPluginValue(vital::mono_float synth_value) const {
      return (synth_value - details_.min) / span_;
    }

    // Converts from value from 0.0 to 1.0 to internal engine value.
    float convertToEngineValue(vital::mono_float plugin_value) const {
      float value = plugin_value * span_ + details_.min;

      if (details_.value_scale == vital::ValueDetails::kIndexed)
        return std::round(value);

      return value;
    }

    void setValueNotifyHost(float new_value) {
      if (!source_changed_) {
        source_changed_ = true;
        setValueNotifyingHost(new_value);
        source_changed_ = false;
      }
    }

  private:
    float getSkewedValue() const {
      return skewValue(value_->value());
    }

    bool usesPercentDisplay() const {
      return details_.display_units == "%" || name_.endsWith("_mix") || name_.endsWith("_dry_wet");
    }

    static String compactFloat(float value, int decimals) {
      String result(value, decimals);
      if (result.containsChar('.')) {
        while (result.endsWithChar('0'))
          result = result.dropLastCharacters(1);
        if (result.endsWithChar('.'))
          result = result.dropLastCharacters(1);
      }
      return result;
    }

    String formatDisplayValue(float display_value) const {
      if (usesPercentDisplay()) {
        float percent = details_.display_units == "%" ? display_value : display_value * 100.0f;
        if (details_.name.rfind("suppressor_", 0) == 0 || details_.name.find("_suppressor_") != std::string::npos)
          return compactFloat(percent, 2) + "%";
        return String(static_cast<int>(std::round(percent))) + "%";
      }

      const String units(details_.display_units);
      const String trimmed_units = units.trim();
      if (trimmed_units == "secs") {
        if (std::abs(display_value) < 1.0f)
          return compactFloat(display_value * 1000.0f, std::abs(display_value) < 0.01f ? 2 : 1) + " ms";
        return compactFloat(display_value, display_value < 10.0f ? 2 : 1) + " secs";
      }

      if (trimmed_units == "dB")
        return compactFloat(display_value, 1) + " dB";
      if (trimmed_units == "dB/oct")
        return compactFloat(display_value, 1) + " dB/oct";
      // Resonator's Fine Tune and Spread step 0.1 cent with Shift; show that step.
      if (trimmed_units == "cents" && (name_.startsWith("resonator_") || name_.contains("_resonator_")))
        return compactFloat(std::abs(display_value) < 0.05f ? 0.0f : display_value, 1) + " cents";
      if (trimmed_units == "semitones" || trimmed_units == "cents" || trimmed_units == "voices")
        return String(static_cast<int>(std::round(display_value))) + " " + trimmed_units;
      if (trimmed_units == "Hz")
        return compactFloat(display_value, std::abs(display_value) < 10.0f ? 2 : 1) + " Hz";

      const int decimals = std::abs(display_value) < 10.0f ? 3 : (std::abs(display_value) < 100.0f ? 2 : 1);
      return compactFloat(display_value, decimals) + units;
    }

    float skewValue(float value) const {
      switch (details_.value_scale) {
        case vital::ValueDetails::kQuadratic:
          return value * value;
        case vital::ValueDetails::kCubic:
          return value * value * value;
        case vital::ValueDetails::kQuartic:
          value *= value;
          return value * value;
        case vital::ValueDetails::kExponential:
          if (details_.display_invert)
            return 1.0f / powf(2.0f, value);
          return powf(2.0f, value);
        case vital::ValueDetails::kSquareRoot:
          return sqrtf(value);
        default:
          return value;
      }
    }

    float unskewValue(float value) const {
      switch (details_.value_scale) {
        case vital::ValueDetails::kQuadratic:
          return sqrtf(value);
        case vital::ValueDetails::kCubic:
          return powf(value, 1.0f / 3.0f);
        case vital::ValueDetails::kQuartic:
          return powf(value, 1.0f / 4.0f);
        case vital::ValueDetails::kSquareRoot:
          return value * value;
        case vital::ValueDetails::kExponential:
          if (details_.display_invert)
            return log2(1.0f / value);
          return log2(value);
        default:
          return value;
      }
    }

    String name_;
    const std::string engine_name_;
    vital::ValueDetails details_;
    vital::mono_float span_;
    vital::Value* value_;
    Listener* listener_;
    bool source_changed_;
    const vital::Value* envelope_sync_ = nullptr;
    const vital::Value* envelope_beats_per_second_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ValueBridge)
};
