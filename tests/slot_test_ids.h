#pragma once

#include "source_types.h"

#include <string>

// Parameter names of the fixed Sample, Granular, Spectral and Noise sources that
// existed before oscillator slots, mapped to the slot that replaced each source
// (Sample 5, Granular 6, Spectral 7, Noise 8). Tests written against the fixed
// sources keep their legacy spelling and go through this mapping.
namespace slot_ids {
  constexpr int kSampleSlot = 4;
  constexpr int kGranularSlot = 5;
  constexpr int kSpectralSlot = 6;
  constexpr int kNoiseSlot = 7;

  inline std::string id(const std::string& legacy) {
    static const struct { const char* prefix; int slot; } kLegacy[] = {
      { "sample", kSampleSlot }, { "granular", kGranularSlot }, { "spectral", kSpectralSlot }, { "noise", kNoiseSlot } };
    static const char* kSlotLevel[] = {
      "on", "solo", "destination", "key_zone_start", "key_zone_end", "velocity_zone_start", "velocity_zone_end",
      "level", "pan" };
    for (const auto& source : kLegacy) {
      const std::string prefix = std::string(source.prefix) + "_";
      if (legacy.rfind(prefix, 0) != 0)
        continue;
      const std::string suffix = legacy.substr(prefix.size());
      const std::string slot_prefix = "osc_" + std::to_string(source.slot + 1) + "_";
      if (suffix == "phase")
        return slot_prefix + "playhead";
      for (const char* slot_level : kSlotLevel) {
        if (suffix == slot_level)
          return slot_prefix + suffix;
      }
      return slot_prefix + legacy;
    }
    return legacy;
  }

  // A new patch makes every slot a wavetable oscillator. Tests that play the fixed
  // sources first give slots 5-8 the Sample, Granular, Spectral and Noise types.
  template<typename Plugin>
  void useLegacySourceTypes(Plugin& plugin) {
    for (int i = 0; i < vital::source_types::kNumLegacySources; ++i)
      plugin.parameterChanged("osc_" + std::to_string(vital::source_types::kFirstLegacySourceSlot + i + 1) + "_type",
                              float(vital::source_types::kLegacySourceTypes[i]));
  }
}
