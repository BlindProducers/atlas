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

#include "synth_constants.h"

#include <string>

namespace vital {
  // The registry of oscillator types an oscillator slot can take. Each slot
  // (`osc_1` .. `osc_8`) owns the parameters of every type under its own prefix
  // and runs only the active type's engine.
  //
  // To add a type: append an enum value (values are stored in presets, never
  // renumber), describe it below, add its display name to
  // strings::kOscillatorTypeNames, give its parameters a slot parameter list in
  // synth_parameters.cpp, and teach SourceSlotModule::createTypeModule how to
  // build its module.
  namespace source_types {
    enum Type {
      kWavetable,
      kAnalog,
      kSample,
      kGranular,
      kSpectral,
      kNoise,
      kMultiSample,
      kBraids,
      kNumTypes
    };

    struct Description {
      const char* id;
      // Parameter group inside the slot: "" shares the slot's wavetable
      // parameters (osc_N_wave_frame ...), "sample" owns osc_N_sample_*,
      // "braids" owns osc_N_braids_*.
      const char* group;
      bool uses_wavetable;
      bool uses_sample;
      bool pitched;
      bool has_playhead;
      // Slot-level modulation (slot_modulation.h). Wavetable and Analog do their
      // own FM/RM inside SynthOscillator and leave these false.
      bool slot_fm;
      bool slot_rm;
    };

    inline constexpr Description kDescriptions[kNumTypes] = {
      { "wavetable", "", true, false, true, false, false, false },
      { "analog", "", true, false, true, false, false, false },
      { "sample", "sample", false, true, true, true, true, true },
      { "granular", "granular", false, true, true, true, true, true },
      { "spectral", "spectral", false, true, true, false, false, true },
      { "noise", "noise", false, false, false, false, false, true },
      { "multisample", "multisample", false, false, true, false, false, false },
      { "braids", "braids", false, false, true, false, true, true },
    };

    inline constexpr const Description& describe(int type) {
      return kDescriptions[type < 0 || type >= kNumTypes ? kWavetable : type];
    }

    // "osc_5_"
    inline std::string slotPrefix(int slot) {
      return "osc_" + std::to_string(slot + 1) + "_";
    }

    // The slot a control belongs to ("osc_5_level" -> 4), or -1.
    inline int slotForControl(const std::string& name) {
      for (int slot = 0; slot < kNumOscillators; ++slot) {
        if (name.rfind(slotPrefix(slot), 0) == 0)
          return slot;
      }
      return -1;
    }

    // A slot's audio as a modulation source: "osc_5_output".
    inline std::string outputSourceName(int slot) {
      return slotPrefix(slot) + "output";
    }

    // The slot whose audio the modulation source `name` is, or -1.
    inline int slotForOutputSource(const std::string& name) {
      const int slot = slotForControl(name);
      return slot >= 0 && name == outputSourceName(slot) ? slot : -1;
    }

    // "osc_5_sample_" for Sample, "osc_5_" for Wavetable and Analog.
    inline std::string groupPrefix(int slot, int type) {
      const Description& description = describe(type);
      if (description.group[0] == '\0')
        return slotPrefix(slot);
      return slotPrefix(slot) + description.group + "_";
    }

    // Whether a type can be frequency / ring modulated by another slot.
    inline constexpr bool supportsFm(int type) { return describe(type).uses_wavetable || describe(type).slot_fm; }
    inline constexpr bool supportsRm(int type) { return describe(type).uses_wavetable || describe(type).slot_rm; }

    // Slot parameters every type reads: level and pan, and the distortion mode
    // and amount pairs that choose FM/RM between slots. They live in the
    // wavetable group but keep their modulation routes across type changes.
    inline bool isSharedSlotParameter(const std::string& suffix) {
      return suffix == "level" || suffix == "pan" ||
             suffix == "distortion_type" || suffix == "distortion_amount" ||
             suffix == "secondary_distortion_type" || suffix == "secondary_distortion_amount";
    }

    // Types sharing a parameter group (Wavetable and Analog) also share one module.
    inline constexpr bool shareModule(int first, int second) {
      return first == second ||
             (describe(first).uses_wavetable && describe(second).uses_wavetable);
    }

    // The type whose parameter group a slot-prefixed parameter belongs to, or -1
    // for slot-level and wavetable/analog parameters.
    inline int typeForParameterSuffix(const std::string& suffix) {
      // The wavetable oscillator's own spectral morph controls share the
      // Spectral type's prefix; they belong to the wavetable group.
      if (suffix.rfind("spectral_morph_", 0) == 0 || suffix == "spectral_unison")
        return -1;
      for (int type = 0; type < kNumTypes; ++type) {
        const std::string group = kDescriptions[type].group;
        if (!group.empty() && suffix.rfind(group + "_", 0) == 0)
          return type;
      }
      return -1;
    }

    // Types that old presets stored as fixed sources, in slot order 5..8.
    inline constexpr int kLegacySourceTypes[] = { kSample, kGranular, kSpectral, kNoise };
    inline constexpr int kNumLegacySources = 4;
    inline constexpr int kFirstLegacySourceSlot = 4;
  } // namespace source_types
} // namespace vital
