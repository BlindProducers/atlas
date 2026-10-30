/* Copyright 2013-2019 Matt Tytel
 *
 * pylon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "JuceHeader.h"
#include "synth_parameters.h"
#include "value.h"
#include "value_bridge.h"

#include <atomic>
#include <cmath>
#include <limits>

// A stable host parameter which can be pointed at one internal FX control.
// Bindings are owned by SynthPlugin and remain alive for the whole processor
// lifetime, so changing an assignment is a single lock-free pointer exchange.
struct FxAutomationBinding {
  const vital::ValueDetails* details = nullptr;
  vital::Value* value = nullptr;
  const std::atomic<double>* suppressor_sample_rate = nullptr;
};

class FxAutomationParameter : public AudioProcessorParameterWithID {
  public:
    FxAutomationParameter(const String& parameterId, const String& parameterName,
                          ValueBridge::Listener* listener) :
        AudioProcessorParameterWithID(ParameterID(parameterId, 0x020100), parameterName),
        listener_(listener) { }

    float getValue() const override {
      const FxAutomationBinding* binding = binding_.load(std::memory_order_acquire);
      if (binding == nullptr || binding->details == nullptr || binding->value == nullptr)
        return current_value_.load(std::memory_order_relaxed);

      return convertToPluginValue(*binding->details, binding->value->value());
    }

    void setValue(float value) override {
      // Hosts can send NaN; jlimit would pass it through to the engine control.
      if (!std::isfinite(value))
        return;
      const float normalized = jlimit(0.0f, 1.0f, value);
      current_value_.store(normalized, std::memory_order_relaxed);

      const FxAutomationBinding* binding = binding_.load(std::memory_order_acquire);
      if (binding == nullptr || binding->details == nullptr || listener_ == nullptr ||
          source_changed_.load(std::memory_order_acquire))
        return;

      applying_host_value_.store(true, std::memory_order_release);
      listener_->parameterChanged(binding->details->name,
                                  convertToEngineValue(*binding->details, normalized));
      applying_host_value_.store(false, std::memory_order_release);
    }

    float getDefaultValue() const override {
      const FxAutomationBinding* binding = binding_.load(std::memory_order_acquire);
      if (binding == nullptr || binding->details == nullptr)
        return 0.0f;
      return convertToPluginValue(*binding->details, binding->details->default_value);
    }

    String getText(float value, int maximumStringLength) const override {
      const FxAutomationBinding* binding = binding_.load(std::memory_order_acquire);
      if (binding == nullptr || binding->details == nullptr)
        return String(jlimit(0.0f, 1.0f, value), 3).substring(0, maximumStringLength);

      const auto& details = *binding->details;
      const float adjusted = convertToEngineValue(details, value);
      if (vital::suppressor::isFrequency(details.name))
        return (String(vital::suppressor::positionHz(adjusted, binding->suppressor_sample_rate
            ? binding->suppressor_sample_rate->load(std::memory_order_relaxed) : 44100.0), 2) + " Hz").substring(0, maximumStringLength);
      if (vital::scream::parameterIs(details.name, "cutoff"))
        return formatDisplayValue(details, vital::scream::cutoffToHz(adjusted)).substring(0, maximumStringLength).trim();
      if (String text; ValueBridge::destroyDisplay(details, adjusted, text))
        return text.substring(0, maximumStringLength);
      if (ValueBridge::usesRangePercentDisplay(details))
        return ValueBridge::rangePercentText(adjusted).substring(0, maximumStringLength);
      String result;
      if (details.string_lookup != nullptr) {
        // Names are indexed by engine value, as ValueBridge reads them.
        const int index = jlimit(0, static_cast<int>(std::round(details.max)), static_cast<int>(std::round(adjusted)));
        result = details.string_lookup[index];
      }
      else {
        const float display_value = details.display_multiply * skewValue(details, adjusted) +
                                    details.post_offset;
        result = formatDisplayValue(details, display_value);
      }
      return result.substring(0, maximumStringLength).trim();
    }

    float getValueForText(const String& text) const override {
      const FxAutomationBinding* binding = binding_.load(std::memory_order_acquire);
      if (binding == nullptr || binding->details == nullptr)
        return jlimit(0.0f, 1.0f, text.getFloatValue());

      const auto& details = *binding->details;
      const String trimmed = text.trim();
      if (vital::suppressor::isFrequency(details.name)) {
        const double hz = trimmed.getDoubleValue() * (trimmed.containsIgnoreCase("k") ? 1000.0 : 1.0);
        return convertToPluginValue(details, jlimit(details.min, details.max,
            static_cast<float>(vital::suppressor::hzPosition(hz, binding->suppressor_sample_rate
                ? binding->suppressor_sample_rate->load(std::memory_order_relaxed) : 44100.0))));
      }
      if (vital::scream::parameterIs(details.name, "cutoff")) {
        float hz = trimmed.getFloatValue() * (trimmed.containsIgnoreCase("kHz") ? 1000.0f : 1.0f);
        return convertToPluginValue(details, vital::scream::cutoffFromHz(hz));
      }
      if (float engine_value = 0.0f; ValueBridge::destroyEngineValue(details, trimmed, engine_value))
        return convertToPluginValue(details, engine_value);
      if (details.string_lookup != nullptr) {
        const int first = static_cast<int>(std::round(details.min));
        const int last = static_cast<int>(std::round(details.max));
        for (int i = first; i <= last; ++i) {
          if (trimmed.equalsIgnoreCase(String(details.string_lookup[i])))
            return convertToPluginValue(details, static_cast<float>(i));
        }
      }
      if (ValueBridge::usesRangePercentDisplay(details))
        return convertToPluginValue(details, ValueBridge::rangePercentEngineValue(details, trimmed));

      float display_value = trimmed.getFloatValue();
      if ((String(details.name).endsWith("defuse_frequency") || String(details.name).contains("resonator_")) &&
          String(details.display_units).trim() == "Hz" && trimmed.containsIgnoreCase("kHz"))
        display_value *= 1000.0f;
      if (trimmed.toLowerCase().contains("ms") && String(details.display_units).trim() == "secs")
        display_value *= 0.001f;
      else if (usesPercentDisplay(details) && trimmed.contains("%") &&
               details.display_multiply == 1.0f && details.max <= 1.0f)
        display_value /= 100.0f;
      else if (usesPercentDisplay(details) && details.display_multiply == 1.0f &&
               details.max <= 1.0f && std::abs(display_value) > 1.0f)
        display_value /= 100.0f;

      float scaled_value = (display_value - details.post_offset) /
                           (details.display_multiply == 0.0f ? 1.0f : details.display_multiply);
      if (usesPercentDisplay(details) && details.display_multiply == 1.0f)
        scaled_value = display_value;

      const float engine_value = unskewValue(details, scaled_value);
      return convertToPluginValue(details, jlimit(details.min, details.max, engine_value));
    }

    bool isAutomatable() const override { return true; }

    void setBinding(const FxAutomationBinding* binding, bool notifyHost) {
      binding_.store(binding, std::memory_order_release);
      const float normalized = getValue();
      current_value_.store(normalized, std::memory_order_relaxed);
      if (notifyHost)
        sendValueChangedMessageToListeners(normalized);
    }

    const FxAutomationBinding* getBinding() const {
      return binding_.load(std::memory_order_acquire);
    }

    bool isAssignedTo(const std::string& parameterName) const {
      const FxAutomationBinding* binding = getBinding();
      return binding != nullptr && binding->details != nullptr &&
             binding->details->name == parameterName;
    }

    bool isApplyingHostValue() const {
      return applying_host_value_.load(std::memory_order_acquire);
    }

    void setEngineValueNotifyHost(float engineValue) {
      const FxAutomationBinding* binding = getBinding();
      if (binding == nullptr || binding->details == nullptr)
        return;

      const float normalized = convertToPluginValue(*binding->details, engineValue);
      current_value_.store(normalized, std::memory_order_relaxed);
      source_changed_.store(true, std::memory_order_release);
      setValueNotifyingHost(normalized);
      source_changed_.store(false, std::memory_order_release);
    }

  private:
    static float parameterSpan(const vital::ValueDetails& details) {
      float span = details.max - details.min;
      if (details.value_scale == vital::ValueDetails::kIndexed)
        span = std::round(span);
      return std::max(span, std::numeric_limits<float>::epsilon());
    }

    static float convertToPluginValue(const vital::ValueDetails& details, float engineValue) {
      return jlimit(0.0f, 1.0f, (engineValue - details.min) / parameterSpan(details));
    }

    static float convertToEngineValue(const vital::ValueDetails& details, float pluginValue) {
      float value = jlimit(0.0f, 1.0f, pluginValue) * parameterSpan(details) + details.min;
      if (details.value_scale == vital::ValueDetails::kIndexed)
        value = std::round(value);
      return jlimit(details.min, details.max, value);
    }

    static bool usesPercentDisplay(const vital::ValueDetails& details) {
      return details.display_units == "%" ||
             String(details.name).endsWith("_mix") || String(details.name).endsWith("_dry_wet");
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

    static String formatDisplayValue(const vital::ValueDetails& details, float displayValue) {
      if (usesPercentDisplay(details)) {
        const float percent = details.display_units == "%" ? displayValue : displayValue * 100.0f;
        if (details.name.rfind("suppressor_", 0) == 0 || details.name.find("_suppressor_") != std::string::npos)
          return compactFloat(percent, 2) + "%";
        return String(static_cast<int>(std::round(percent))) + "%";
      }

      const String units(details.display_units);
      const String trimmed_units = units.trim();
      if (trimmed_units == "secs") {
        if (std::abs(displayValue) < 1.0f)
          return compactFloat(displayValue * 1000.0f, std::abs(displayValue) < 0.01f ? 2 : 1) + " ms";
        return compactFloat(displayValue, displayValue < 10.0f ? 2 : 1) + " secs";
      }
      if (trimmed_units == "dB")
        return compactFloat(displayValue, 1) + " dB";
      // Resonator's Fine Tune and Spread step 0.1 cent with Shift; show that step.
      if (trimmed_units == "cents" && String(details.name).contains("resonator_"))
        return compactFloat(std::abs(displayValue) < 0.05f ? 0.0f : displayValue, 1) + " cents";
      if (trimmed_units == "semitones" || trimmed_units == "cents" || trimmed_units == "voices")
        return String(static_cast<int>(std::round(displayValue))) + " " + trimmed_units;
      if (trimmed_units == "Hz")
        return compactFloat(displayValue, std::abs(displayValue) < 10.0f ? 2 : 1) + " Hz";

      const int decimals = std::abs(displayValue) < 10.0f ? 3 :
                           (std::abs(displayValue) < 100.0f ? 2 : 1);
      return compactFloat(displayValue, decimals) + units;
    }

    static float skewValue(const vital::ValueDetails& details, float value) {
      switch (details.value_scale) {
        case vital::ValueDetails::kQuadratic: return value * value;
        case vital::ValueDetails::kCubic: return value * value * value;
        case vital::ValueDetails::kQuartic: value *= value; return value * value;
        case vital::ValueDetails::kExponential:
          return details.display_invert ? 1.0f / std::pow(2.0f, value) : std::pow(2.0f, value);
        case vital::ValueDetails::kSquareRoot: return std::sqrt(value);
        default: return value;
      }
    }

    static float unskewValue(const vital::ValueDetails& details, float value) {
      switch (details.value_scale) {
        case vital::ValueDetails::kQuadratic: return std::sqrt(std::max(0.0f, value));
        case vital::ValueDetails::kCubic: return std::cbrt(value);
        case vital::ValueDetails::kQuartic: return std::pow(std::max(0.0f, value), 0.25f);
        case vital::ValueDetails::kSquareRoot: return value * value;
        case vital::ValueDetails::kExponential:
          if (value <= 0.0f)
            return details.min;
          return details.display_invert ? std::log2(1.0f / value) : std::log2(value);
        default: return value;
      }
    }

    ValueBridge::Listener* listener_ = nullptr;
    std::atomic<const FxAutomationBinding*> binding_ { nullptr };
    std::atomic<float> current_value_ { 0.0f };
    std::atomic<bool> source_changed_ { false };
    std::atomic<bool> applying_host_value_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxAutomationParameter)
};
