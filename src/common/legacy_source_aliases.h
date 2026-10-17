// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "source_types.h"
#include "synth_parameters.h"

#include <string>

namespace vital {
namespace source_types {
  // The fixed Sample, Granular, Spectral and Noise sources became oscillator
  // slots 5-8. Their old parameter names stay in the parameter list as host
  // aliases so numbering is stable; this maps an alias to the slot control,
  // or gives an empty string for any other name.
  inline std::string legacySourceAliasTarget(const std::string& name) {
    struct Legacy { const char* prefix; int slot; };
    static const Legacy kLegacy[] = { { "sample", 4 }, { "granular", 5 }, { "spectral", 6 }, { "noise", 7 } };
    static const char* kSlotLevel[] = {
      "on", "solo", "destination", "key_zone_start", "key_zone_end", "velocity_zone_start", "velocity_zone_end",
      "level", "pan"
    };
    for (const Legacy& legacy : kLegacy) {
      for (int arp = 1; arp <= 2; ++arp) {
        if (name == "arp_" + std::to_string(arp) + "_destination_" + legacy.prefix)
          return "arp_" + std::to_string(arp) + "_destination_osc_" + std::to_string(legacy.slot + 1);
      }
      const std::string legacy_prefix = std::string(legacy.prefix) + "_";
      if (name.rfind(legacy_prefix, 0) != 0)
        continue;
      const std::string suffix = name.substr(legacy_prefix.size());
      const std::string slot_prefix = slotPrefix(legacy.slot);
      for (const char* slot_level : kSlotLevel) {
        if (suffix == slot_level)
          return slot_prefix + suffix;
      }
      const std::string target = slot_prefix + legacy_prefix + suffix;
      // Only a type-group parameter is an alias target; the oscillator's own
      // spectral morph controls merely share the Spectral prefix.
      const int legacy_type = kLegacySourceTypes[legacy.slot - kFirstLegacySourceSlot];
      if (typeForParameterSuffix(legacy_prefix + suffix) != legacy_type || !Parameters::isParameter(target))
        return {};
      return target;
    }
    return {};
  }
} // namespace source_types
} // namespace vital
