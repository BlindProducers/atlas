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

#include "synth_editor.h"
#include "multisample_container.h"
#include "multisample_manager.h"
#include "speech_manager.h"
#include "delay.h"

#include "synth_plugin.h"
#include "screen_reader_speech.h"
#include "value_bridge.h"
#include "tape_controls.h"
#include "resonator/resonator_engine.h"
#include "analogue/chorus_engine.h"
#include "analogue/echo_engine.h"
#include "analogue/rotary_engine.h"
#include "analogue/wah_engine.h"
#include "convolution_reverb_module.h"
#include "table_filter.h"
#include "sound_engine.h"
#include "load_save.h"
#include "line_generator.h"
#include "modulation_connection_processor.h"
#include "producers_module.h"
#include "filter_routing.h"
#include "sample_source.h"
#include "source_types.h"
#include "braids_engine.h"
#include "synth_lfo.h"
#include "synth_oscillator.h"
#include "synth_strings.h"
#include "tuning.h"
#include "utils.h"
#include "formant_filter.h"
#include "ring_mod_filter.h"
#include "sample_hold_filter.h"
#include "distortion.h"
#include "frequency_filter_modifier.h"
#include "file_source.h"
#include "phase_modifier.h"
#include "slew_limit_modifier.h"
#include "wavetable.h"
#include "wavetable_component_factory.h"
#include "wavetable_group.h"
#include "wave_fold_modifier.h"
#include "wave_source.h"
#include "wave_line_source.h"
#include "wavetable_edit_model.h"
#include "wave_warp_modifier.h"
#include "wave_window_modifier.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace {
  constexpr const char* kPresetSection = "Presets";
  constexpr const char* kSignalRoutingSection = "Signal routing";
  constexpr const char* kEffectsChainSection = "Chain and routing";
  constexpr const char* kAllLibraries = "All libraries";
  constexpr const char* kFactoryLibrary = "Factory";
  constexpr const char* kUserLibrary = "User";
  constexpr const char* kOtherLibrary = "Other";
  constexpr const char* kAllBanks = "All banks";
  constexpr const char* kAllCategories = "All categories";
  constexpr const char* kAllTags = "All tags";
  constexpr int kBrowserRescanMenuId = 0x3fff0001;
  // How long after a rebuild the screen reader is moved back to the focused control.
  constexpr int kScreenReaderRefocusDelayMs = 250;
  // How long the inline prompt stays on screen after focus has left it.
  constexpr int kInlinePromptHideDelayMs = 600;
  // Whether a rebuild keeps the focused choice that started it and its groups
  // (SynthEditor::keepFocusedSwitchingRow). NVDA reads every group around a
  // control it is moved to; VoiceOver's behaviour stays as it was.
#if JUCE_WINDOWS
  constexpr bool kKeepSwitchingFocus = true;
#else
  constexpr bool kKeepSwitchingFocus = false;
#endif
  // Whether comma and period, which step from element to element, also step
  // through the effect slots of an effects chain on the way
  // (SynthEditor::moveThroughEffectSlots). Asked for on Windows; on the Mac
  // they still go straight from the chain to the next element.
#if JUCE_WINDOWS
  constexpr bool kSectionKeysStepThroughEffectSlots = true;
#else
  constexpr bool kSectionKeysStepThroughEffectSlots = false;
#endif
  // Whether a step to a bus's effects chain (comma, period, a number key)
  // lands on its Bus heading, as Ctrl 1 to 3 do, and says the bus ("Bus 2").
  // Otherwise it lands on the chain's Bus controls and says "Chain and
  // routing", the name of every bus's chain (SynthEditor::selectSectionByName).
  // Asked for on Windows.
#if JUCE_WINDOWS
  constexpr bool kBusChainsStepToTheirHeading = true;
#else
  constexpr bool kBusChainsStepToTheirHeading = false;
#endif
  // Whether the right bracket key opens a control's menu, as Shift M does
  // (isContextMenuKey). Off the Mac the brackets only move effects in a chain,
  // as asked, and menus open with Shift M or Shift F10.
#if JUCE_MAC
  constexpr bool kRightBracketOpensMenus = true;
#else
  constexpr bool kRightBracketOpensMenus = false;
#endif

  enum PresetFacetKind {
    kLibraryFacet = 1,
    kBankFacet,
    kCategoryFacet,
    kTagFacet,
    kCollectionFacet,
  };

  enum PresetTableColumn {
    kPresetNameColumn = 1,
    kPresetAuthorColumn,
    kPresetFavoriteColumn,
    kPresetTagsColumn,
    kPresetCategoryColumn,
    kPresetBankColumn,
    kPresetLibraryColumn,
    kPresetModifiedColumn,
  };

  using AccessibilitySpeechSettings = atlas::speech::Settings;

  AccessibilitySpeechSettings& accessibilitySpeechSettings() {
    return atlas::speech::settings();
  }

  void saveAccessibilitySpeechSettings() {
    atlas::speech::saveSettings();
  }

  String onOff(bool enabled) {
    return enabled ? "On" : "Off";
  }

  bool containsAny(const String& text, std::initializer_list<const char*> words) {
    for (const auto* word : words) {
      if (text.contains(word))
        return true;
    }
    return false;
  }

  String compactFloat(float value, int decimals) {
    String result(value, decimals);
    if (result.containsChar('.')) {
      while (result.endsWithChar('0'))
        result = result.dropLastCharacters(1);
      if (result.endsWithChar('.'))
        result = result.dropLastCharacters(1);
    }
    return result;
  }

  String formatHz(float frequency) {
    frequency = std::max(0.0f, frequency);
    const int decimals = frequency < 10.0f ? 2 : (frequency < 100.0f ? 1 : 0);
    return compactFloat(frequency, decimals) + " Hz";
  }

  bool displayAccessibleFrequencyValuesInHz() {
    return accessibilitySpeechSettings().frequency_values_in_hz || LoadSave::displayHzFrequency();
  }

  bool parseNoteNameToMidi(const String& text, float& midi_note) {
    String note = text.trim().removeCharacters(" ");
    if (note.isEmpty())
      return false;

    note = note.toLowerCase();
    if (note.endsWith("hz"))
      return false;

    const juce_wchar root = note[0];
    int semitone = 0;
    switch (root) {
      case 'c': semitone = 0; break;
      case 'd': semitone = 2; break;
      case 'e': semitone = 4; break;
      case 'f': semitone = 5; break;
      case 'g': semitone = 7; break;
      case 'a': semitone = 9; break;
      case 'b': semitone = 11; break;
      default: return false;
    }

    int index = 1;
    if (note.substring(index).startsWith("sharp")) {
      semitone += 1;
      index += 5;
    }
    else if (note.substring(index).startsWith("flat")) {
      semitone -= 1;
      index += 4;
    }
    else if (index < note.length() && note[index] == '#') {
      semitone += 1;
      ++index;
    }
    else if (index < note.length() && note[index] == 'b') {
      semitone -= 1;
      ++index;
    }

    const String octave_text = note.substring(index);
    if (octave_text.isEmpty())
      return false;

    bool valid_octave = true;
    for (int i = 0; i < octave_text.length(); ++i) {
      const juce_wchar c = octave_text[i];
      if (!((c >= '0' && c <= '9') || (i == 0 && c == '-'))) {
        valid_octave = false;
        break;
      }
    }
    if (!valid_octave)
      return false;

    const int octave = octave_text.getIntValue();
    // Cb4 is the B below C4 and B#3 the C above B3: an accidental carries
    // into the next octave instead of wrapping within this one.
    midi_note = static_cast<float>((octave + 1) * 12 + semitone);
    return true;
  }

  bool isSemitoneFrequencyParameter(const String& parameter_id, const vital::ValueDetails& details) {
    if (details.string_lookup != nullptr || details.value_scale != vital::ValueDetails::kLinear)
      return false;

    const String id = parameter_id.toLowerCase();
    const String display = String(details.display_name).toLowerCase();
    const bool frequency_name = id.contains("cutoff") || display.contains("cutoff") ||
                                display.contains("low cut") || display.contains("high cut") ||
                                id.endsWith("_center") || display.endsWith(" center");
    if (!frequency_name)
      return false;

    return details.min >= 0.0f && details.max >= 120.0f && details.max <= 140.0f;
  }

  int oscillatorIndexFromDistortionParameter(const String& parameter_id) {
    if (!parameter_id.startsWith("osc_") || !parameter_id.endsWith("_distortion_type"))
      return -1;

    const String number_text = parameter_id.substring(4).upToFirstOccurrenceOf("_", false, false);
    const int oscillator_index = number_text.getIntValue() - 1;
    return isPositiveAndBelow(oscillator_index, vital::kNumOscillators) ? oscillator_index : -1;
  }

  String oscillatorDistortionValueText(const ValueBridge& bridge, double normalized_value) {
    const int oscillator_index = oscillatorIndexFromDistortionParameter(bridge.getParameterId());
    if (oscillator_index < 0)
      return "";

    const int distortion_type = static_cast<int>(std::round(bridge.convertToEngineValue(
        static_cast<float>(normalized_value))));
    auto oscillator_source_text = [](int source_index) {
      return "Oscillator " + String(source_index + 1);
    };

    const int position = vital::SynthOscillator::modulatorPosition(distortion_type);
    if (position < 0)
      return "";
    const String source = oscillator_source_text(vital::ProducersModule::getModulationIndex(oscillator_index, position));
    if (vital::SynthOscillator::isFmType(distortion_type))
      return "FM from " + source;
    return "Ring Mod from " + source;
  }

  // Oscillator slot a slot-prefixed id belongs to (0-based), or -1.
  int sourceSlotForId(const String& id) {
    if (!id.startsWith("osc_"))
      return -1;
    const String number = id.fromFirstOccurrenceOf("osc_", false, false).upToFirstOccurrenceOf("_", false, false);
    const int slot = number.getIntValue() - 1;
    return isPositiveAndBelow(slot, vital::kNumOscillators) && id.startsWith("osc_" + number + "_") ? slot : -1;
  }

  // Type-group parameters keep their legacy spelling for labels, ranks and help:
  // "osc_5_sample_start" reads as "sample_start". Other ids are returned unchanged.
  String legacySourceId(const String& id) {
    if (sourceSlotForId(id) < 0)
      return id;
    const String suffix = id.fromFirstOccurrenceOf("_", false, false).fromFirstOccurrenceOf("_", false, false);
    return vital::source_types::typeForParameterSuffix(suffix.toStdString()) >= 0 ? suffix : id;
  }

  bool isSampleLoopPointParameter(const String& id) {
    const String legacy = legacySourceId(id);
    return legacy == "sample_loop_start" || legacy == "sample_loop_end";
  }

  String accessibleParameterText(const ValueBridge& bridge, double normalized_value) {
    const String distortion_text = oscillatorDistortionValueText(bridge, normalized_value);
    if (distortion_text.isNotEmpty())
      return distortion_text;

    if (displayAccessibleFrequencyValuesInHz() && isSemitoneFrequencyParameter(bridge.getParameterId(),
                                                                               bridge.getDetails())) {
      const float midi_note = bridge.convertToEngineValue(static_cast<float>(normalized_value));
      return formatHz(vital::utils::midiNoteToFrequency(midi_note));
    }

    return bridge.getText(static_cast<float>(normalized_value), 128);
  }

  double accessibleParameterValueForText(const ValueBridge& bridge, const String& text) {
    if (isSemitoneFrequencyParameter(bridge.getParameterId(), bridge.getDetails())) {
      const String trimmed = text.trim();
      if (trimmed.isNotEmpty()) {
        float midi_note = 0.0f;
        if (parseNoteNameToMidi(trimmed, midi_note)) {
          const auto& details = bridge.getDetails();
          return bridge.convertToPluginValue(jlimit(details.min, details.max, midi_note));
        }
        if (displayAccessibleFrequencyValuesInHz()) {
          const String lower = trimmed.removeCharacters(" ").toLowerCase();
          const bool kilohertz = lower.endsWith("khz") || lower.endsWith("k");
          const float frequency = lower.removeCharacters("khz").getFloatValue() * (kilohertz ? 1000.0f : 1.0f);
          if (frequency > 0.0f) {
            midi_note = vital::utils::frequencyToMidiNote(frequency);
            const auto& details = bridge.getDetails();
            return bridge.convertToPluginValue(jlimit(details.min, details.max, midi_note));
          }
        }
      }
    }

    return static_cast<double>(bridge.getValueForText(text));
  }

  bool shouldPostPluginAnnouncement(const String& message) {
    const auto& settings = accessibilitySpeechSettings();
    if (!settings.speech_feedback)
      return false;

    const String lower = message.toLowerCase();
    if (containsAny(lower, { "preset", "bank" }))
      return settings.presets;
    if (containsAny(lower, { "modulation", "midi learn", "assigned", "destination" }))
      return settings.modulation;
    if (containsAny(lower, { "lfo", "mseg", "point", "curve", "grid", "shape" }))
      return settings.lfo_editor;
    if (containsAny(lower, { "wavetable", "harmonic", "fundamental", "wave frame", "frame" }) ||
        (lower.startsWith("loaded ") && lower.contains(" in oscillator")) ||
        (lower.startsWith("normalized ") && lower.contains(" frame")))
      return settings.wavetable_editor;
    if (containsAny(lower, { "sample", "granular", "spectral", "instrument", "sfz" }))
      return settings.sample_browser;
    if (containsAny(lower, { "type value", "reset to", "macro" }))
      return settings.parameters;
    if (containsAny(lower, { "oscillator", "mixer", "routing", "filter", "envelope", "effects", "global", "zones" }))
      return settings.navigation;
    return settings.other;
  }

  // While a rebuild waits to move the screen reader back onto its control,
  // announcements wait with it: the move makes the reader speak the control,
  // which would cut off whatever it was saying. Held messages are spoken
  // together, since each new announcement would also cut off the one before.
  struct HeldAnnouncements {
    int holds = 0;
    StringArray messages;
    AccessibilityHandler::AnnouncementPriority priority = AccessibilityHandler::AnnouncementPriority::low;
  };

  HeldAnnouncements& heldAnnouncements() {
    static HeldAnnouncements held;
    return held;
  }

  void postAnnouncement(const String& message, AccessibilityHandler::AnnouncementPriority priority) {
    auto& held = heldAnnouncements();
    if (held.holds == 0) {
      atlas::speech::announce(message, priority);
      return;
    }
    held.messages.add(message);
    held.priority = jmax(held.priority, priority);
  }

  void holdAnnouncements() {
    ++heldAnnouncements().holds;
  }

  void releaseAnnouncements() {
    auto& held = heldAnnouncements();
    if (held.holds == 0 || --held.holds > 0 || held.messages.isEmpty())
      return;
    const String message = held.messages.joinIntoString(". ");
    const auto priority = held.priority;
    held.messages.clear();
    held.priority = AccessibilityHandler::AnnouncementPriority::low;
    atlas::speech::announce(message, priority);
  }

  struct ScopedAnnouncementHold {
    ScopedAnnouncementHold() { holdAnnouncements(); }
    ~ScopedAnnouncementHold() { releaseAnnouncements(); }
    ScopedAnnouncementHold(const ScopedAnnouncementHold&) = delete;
    ScopedAnnouncementHold& operator=(const ScopedAnnouncementHold&) = delete;
  };

  void postPluginAnnouncement(const String& message, AccessibilityHandler::AnnouncementPriority priority) {
    if (shouldPostPluginAnnouncement(message))
      postAnnouncement(message, priority);
  }

  // Where a step landed, for a name that holds none of the navigation words
  // above ("Bus 2"), so that the Navigation setting still decides.
  void postNavigationAnnouncement(const String& message) {
    const auto& settings = accessibilitySpeechSettings();
    if (settings.speech_feedback && settings.navigation)
      postAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
  }

  // Direct responses to a preset-browser command must not disappear when the
  // optional background preset-announcement category is disabled. These are
  // confirmations of an action the user just performed, not browser chatter.
  void postPresetActionAnnouncement(const String& message,
                                    AccessibilityHandler::AnnouncementPriority priority) {
    if (accessibilitySpeechSettings().speech_feedback)
      postAnnouncement(message, priority);
  }

  // What the screen reader would read from a control where JUCE gives NVDA
  // nothing to read: the row a list's selection keys landed on (see
  // SelectionKeyList) and a toggle's new state (see SpokenToggleButton).
  // VoiceOver reads these itself, so Windows says them here, and no optional
  // category silences them.
  void postWindowsReadout(const String& message) {
#if JUCE_WINDOWS
    if (accessibilitySpeechSettings().speech_feedback)
      postAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
#else
    ignoreUnused(message);
#endif
  }

  // JUCE moves accessibility focus onto a popup menu's items and drops it when
  // the menu closes, while the control that opened the menu keeps the keyboard
  // focus, so Windows gets no focus event and NVDA stays on the closed menu.
  // Opens `menu`, then hands the focus back to whichever control has it.
  void showAccessibleMenuAsync(PopupMenu& menu, const PopupMenu::Options& options,
                               std::function<void(int)> callback = {}) {
    menu.showMenuAsync(options, [callback = std::move(callback)](int result) {
      if (callback)
        callback(result);
#if JUCE_WINDOWS
      MessageManager::callAsync([] {
        if (auto* focused = Component::getCurrentlyFocusedComponent())
          if (auto* handler = focused->getAccessibilityHandler())
            handler->grabFocus();
      });
#endif
    });
  }

  String listRowText(const String& name, int row, int rows) {
    return name + ", " + String(row + 1) + " of " + String(rows);
  }

  void postLfoAnnouncement(const String& message) {
    const auto& settings = accessibilitySpeechSettings();
    if (settings.speech_feedback && settings.lfo_editor)
      postAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
  }

  // The wavetable keyboard editor's frames, harmonics, layers, keyframes and
  // lines. Its messages ("Layer 2", "Point removed") do not all name the
  // editor, so they skip postPluginAnnouncement's guess from the wording.
  void postWavetableEditorAnnouncement(const String& message,
                                       AccessibilityHandler::AnnouncementPriority priority) {
    const auto& settings = accessibilitySpeechSettings();
    if (settings.speech_feedback && settings.wavetable_editor)
      postAnnouncement(message, priority);
  }

  void postModulationRemapAnnouncement(const String& message) {
    // Short edit feedback may contain only a value or curve name. Keep it in
    // the modulation category without repeating a spoken category prefix.
    const auto& settings = accessibilitySpeechSettings();
    if (settings.speech_feedback && settings.modulation)
      postAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
  }

  struct MsegTimeDivision {
    const char* name;
    float quarter_notes;
  };

  const std::vector<MsegTimeDivision>& msegTimeDivisions() {
    static const std::vector<MsegTimeDivision> divisions {
      { "8 Bars", 32.0f }, { "4 Bars", 16.0f }, { "2 Bars", 8.0f }, { "1 Bar", 4.0f },
      { "1/2 Dotted", 3.0f }, { "1/2", 2.0f }, { "1/4 Dotted", 1.5f }, { "1/2 Triplet", 4.0f / 3.0f },
      { "1/4", 1.0f }, { "1/8 Dotted", 0.75f }, { "1/4 Triplet", 2.0f / 3.0f }, { "1/8", 0.5f },
      { "1/16 Dotted", 0.375f }, { "1/8 Triplet", 1.0f / 3.0f }, { "1/16", 0.25f },
      { "1/32 Dotted", 0.1875f }, { "1/16 Triplet", 1.0f / 6.0f }, { "1/32", 0.125f },
      { "1/64 Dotted", 0.09375f }, { "1/32 Triplet", 1.0f / 12.0f }, { "1/64", 0.0625f },
      // Vital's tempo table ends at 1/64, so no 1/128 entries: they would
      // announce a length the LFO cannot take.
      { "1/64 Triplet", 1.0f / 24.0f }
    };
    return divisions;
  }

  // Named in LineGenerator's shape mode order.
  const StringArray& lfoModeNames() {
    static const StringArray names { "LFO", "Performer", "Step Sequencer" };
    return names;
  }

  // Indexed as SynthEditor::applyLfoShapePreset() numbers the shapes.
  const StringArray& lfoShapePresetNames() {
    static const StringArray names { "Custom", "Sine", "Triangle", "Square", "Saw up", "Saw down",
                                     "Pulse", "Ramp hold", "Step climb", "Step fall",
                                     "Flat 0", "Flat 50", "Flat 100", "Gate", "Double pulse",
                                     "Trance gate", "Up down", "Down up", "Half sine up",
                                     "Half sine down", "Short pulse", "Offbeat pulse",
                                     "Triplet gate", "Sidechain dip", "Reverse sidechain",
                                     "Stutter 4", "Stutter 8", "Stair up 4", "Stair down 4",
                                     "Zigzag steps", "Ramp up hold", "Ramp down hold",
                                     "Curve up", "Curve down", "Smooth dip", "Smooth rise" };
    return names;
  }

  String wavetableAudioLoadStyleName(int audio_load_style) {
    switch (audio_load_style) {
      case WavetableCreator::kVocoded:
        return "vocode";
      case WavetableCreator::kPitched:
        return "pitched";
      case WavetableCreator::kWavetableSplice:
      default:
        return "wavetable splice";
    }
  }

  int closestVitalTempoIndex(float quarter_notes) {
    if (quarter_notes <= 0.0f)
      return 8;

    const float target = 1.0f / quarter_notes;
    int best = 8;
    float best_distance = std::numeric_limits<float>::max();
    for (int i = 1; i < vital::constants::kNumSyncedFrequencyRatios; ++i) {
      const float distance = std::abs(std::log2(jmax(0.000001f, vital::constants::kSyncedFrequencyRatios[i] / target)));
      if (distance < best_distance) {
        best_distance = distance;
        best = i;
      }
    }
    return best;
  }

  float engineValueFor(AudioProcessorParameter* parameter) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
      return bridge->convertToEngineValue(bridge->getValue());
    return parameter ? parameter->getValue() : 0.0f;
  }

  // Oscillator controls that only apply to one oscillator type.
  // Controls every oscillator type shares: switch, type, level, pan, routing, solo and zones.
  bool isSlotLevelOscillatorSuffix(const String& suffix) {
    return suffix == "on" || suffix == "type" || suffix == "level" || suffix == "pan" ||
           suffix == "destination" || suffix == "solo" ||
           suffix == "key_zone_start" || suffix == "key_zone_end" ||
           suffix == "velocity_zone_start" || suffix == "velocity_zone_end";
  }

  bool isAnalogOnlyOscillatorSuffix(const String& suffix) {
    return suffix == "analog_waveform" || suffix == "analog_shape" || suffix == "sub_level" ||
           suffix == "sub_octave" || suffix == "sub_waveform" || suffix == "analog_drift" ||
           suffix == "analog_voice_spread" || suffix == "analog_wear" || suffix == "analog_noise" ||
           suffix == "analog_hum" || suffix == "analog_hum_frequency" || suffix == "analog_sag";
  }

  bool isWavetableOnlyOscillatorSuffix(const String& suffix) {
    static const StringArray suffixes {
      "wave_frame", "frame_spread", "spectral_unison", "smooth_interpolation", "distortion_phase",
      "spectral_morph_type", "spectral_morph_amount", "spectral_morph_seed", "spectral_morph_spread",
      "secondary_spectral_morph_type", "secondary_spectral_morph_amount", "secondary_spectral_morph_seed",
      "secondary_spectral_morph_spread"
    };
    return suffixes.contains(suffix);
  }

  bool isOscillatorDistortionSuffix(const String& suffix) {
    return suffix == "distortion_type" || suffix == "distortion_amount" || suffix == "distortion_spread" ||
           suffix == "secondary_distortion_type" || suffix == "secondary_distortion_amount" ||
           suffix == "secondary_distortion_spread";
  }

  // Where a row sits inside an oscillator section. Output rows stay at the top
  // level; the rest go into Pitch and unison (with Scale lock inside), Warp,
  // the wavetable / sample material groups, or the type's own group.
  // kTypeMode holds parameters whose meaning follows a mode of the type (Braids Timbre and Color).
  enum class OscillatorRowGroup { kOutput, kWavetable, kSample, kAnalog, kPitch, kScaleLock, kWarp, kType, kTypeMode };

  OscillatorRowGroup oscillatorRowGroup(const String& suffix) {
    if (vital::source_types::typeForParameterSuffix(suffix.toStdString()) >= 0) {
      // "sample_transpose" -> "transpose": the sample types carry their own pitch rows.
      const String rest = suffix.fromFirstOccurrenceOf("_", false, false);
      if (rest == "transpose_quantize" || rest.startsWith("transpose_quantize_"))
        return OscillatorRowGroup::kScaleLock;
      if (rest == "transpose" || rest == "tune" || rest == "keytrack" || rest == "root_key")
        return OscillatorRowGroup::kPitch;
      // Braids random phase sits with pitch, as the Wavetable's does.
      if (suffix == "braids_random_phase")
        return OscillatorRowGroup::kPitch;
      if (suffix == "braids_timbre" || suffix == "braids_color")
        return OscillatorRowGroup::kTypeMode;
      return suffix.startsWith("sample_") ? OscillatorRowGroup::kSample : OscillatorRowGroup::kType;
    }
    if (isSlotLevelOscillatorSuffix(suffix))
      return OscillatorRowGroup::kOutput;
    if (suffix == "transpose_quantize" || suffix.startsWith("transpose_quantize_"))
      return OscillatorRowGroup::kScaleLock;
    static const StringArray pitch_and_unison {
      "transpose", "tune", "fixed_frequency_mode", "fixed_frequency", "midi_track", "unison_voices",
      "unison_detune", "unison_blend", "stack_style", "detune_power", "detune_range", "stereo_spread",
      "phase", "random_phase"
    };
    if (pitch_and_unison.contains(suffix))
      return OscillatorRowGroup::kPitch;
    if (isAnalogOnlyOscillatorSuffix(suffix))
      return OscillatorRowGroup::kAnalog;
    if (suffix.startsWith("distortion_") || suffix.startsWith("secondary_") ||
        suffix.startsWith("spectral_morph_") || suffix == "spectral_unison")
      return OscillatorRowGroup::kWarp;
    if (suffix == "wave_frame" || suffix == "frame_spread" || suffix == "smooth_interpolation")
      return OscillatorRowGroup::kWavetable;
    return OscillatorRowGroup::kOutput;
  }

  // Distortion modes an Analog oscillator uses, in their display order.
  std::vector<int> analogDistortionOrder(int waveform) {
    std::vector<int> order;
    for (int type : vital::SynthOscillator::kDistortionPresentationOrder) {
      const bool fm_or_rm = type >= vital::SynthOscillator::kFmOscillatorA;
      if (type == vital::SynthOscillator::kNone || fm_or_rm ||
          (type == vital::SynthOscillator::kSync && waveform != vital::analog::kPitchedNoise))
        order.push_back(type);
    }
    return order;
  }

  // Distortion modes a Sample, Granular, Spectral or Noise slot uses: None,
  // the FM sources when the type can be frequency modulated, then ring modulation.
  std::vector<int> slotDistortionOrder(int slot_type) {
    std::vector<int> order;
    const bool fm = vital::source_types::supportsFm(slot_type);
    for (int type : vital::SynthOscillator::kDistortionPresentationOrder) {
      if (type == vital::SynthOscillator::kNone || (fm && vital::SynthOscillator::isFmType(type)) ||
          vital::SynthOscillator::isRmType(type))
        order.push_back(type);
    }
    return order;
  }

  String analogShapeDescription(int waveform) {
    switch (waveform) {
      case vital::analog::kSine:
        return "Phase distortion. 0% is a pure sine; higher values bend it into brighter, saw-like tones.";
      case vital::analog::kTriangle:
        return "Symmetry. 0% is a pure triangle; higher values tilt it towards a saw.";
      case vital::analog::kSquare:
        return "Pulse width. 0% is a square; higher values narrow it down to a thin 1% pulse.";
      case vital::analog::kWhiteNoise:
        return "Colour. 0% is white noise; higher values darken it towards brown noise at a steady level.";
      case vital::analog::kPitchedNoise:
        return "Loop. 0% is random chip noise; higher values repeat its pattern more often, and 100% repeats it every cycle for a pitched tone.";
      default:
        return "Second saw offset. 0% is a pure saw; higher values add a second saw offset in phase for a hollow, pulse-like sweep.";
    }
  }

  String filterPrefixForParameter(const String& parameter_id) {
    for (const auto* prefix : { "filter_1_", "filter_2_", "filter_fx_" }) {
      if (parameter_id.startsWith(prefix))
        return prefix;
    }
    // Each inserted filter has its own model and style, including on buses.
    if (parameter_id.contains("_filter_fx_"))
      return parameter_id.upToFirstOccurrenceOf("_filter_fx_", true, false);
    return {};
  }

  String filterSuffixForParameter(const String& parameter_id) {
    const String prefix = filterPrefixForParameter(parameter_id);
    if (prefix.isEmpty())
      return {};
    return parameter_id.fromFirstOccurrenceOf(prefix, false, false);
  }

  bool isFilterParameterSuffix(const String& suffix) {
    static const std::array<const char*, 34> allowed_suffixes = {{
      "destination",
      "table_frame",
      "reverb_cutoff",
      "reverb_damp",
      "reverb_output",
      "reverb_bass_preserve",
      "scream_amount",
      "scream_input_gain",
      "scream_output_gain",
      "scream_autogain",
      "distortion_drive",
      "distortion_filter_order",
      "mix",
      "cutoff",
      "resonance",
      "drive",
      "blend",
      "style",
      "model",
      "on",
      "blend_transpose",
      "keytrack",
      "formant_x",
      "formant_y",
      "formant_transpose",
      "formant_resonance",
      "formant_spread",
      "osc1_input",
      "osc2_input",
      "osc3_input",
      "osc4_input",
      "sample_input",
      "granular_input",
      "filter_input",
    }};
    for (const auto* allowed : allowed_suffixes) {
      if (suffix == allowed)
        return true;
    }
    return false;
  }

  int numFilterStylesForModel(int model, bool filter_fx = false) {
    switch (static_cast<vital::constants::FilterModel>(model)) {
      case vital::constants::kFilterTable:
        return vital::TableFilter::numStyles(filter_fx);
      case vital::constants::kAnalog:
      case vital::constants::kDirty:
      case vital::constants::kLadder:
      case vital::constants::kDigital:
        return 5;
      case vital::constants::kDiode:
      case vital::constants::kFormant:
      case vital::constants::kPhase:
        return 2;
      case vital::constants::kComb:
        return 6;
      case vital::constants::kRingModFilter:
        return vital::RingModFilter::kNumWaveforms;
      case vital::constants::kSampleHoldFilter:
        return vital::SampleHoldFilter::kNumStyles;
      case vital::constants::kDistortionFilter:
        return vital::Distortion::kNumTypes;
      default:
        return 1;
    }
  }

  String filterStyleNameForModel(int model, int style, bool filter_fx = false) {
    style = jlimit(0, jmax(0, numFilterStylesForModel(model, filter_fx) - 1), style);
    switch (static_cast<vital::constants::FilterModel>(model)) {
      case vital::constants::kFilterTable:
        return filter_fx ? strings::kFilterTableStyleNames[style] : strings::kFilterTableVoiceStyleNames[style];
      case vital::constants::kAnalog:
      case vital::constants::kDirty:
      case vital::constants::kLadder:
      case vital::constants::kDigital:
        return strings::kFilterStyleNames[style];
      case vital::constants::kDiode:
        return strings::kDiodeStyleNames[style];
      case vital::constants::kFormant:
        if (style == vital::FormantFilter::kVocalTract)
          return "The Mouth";
        if (style == vital::FormantFilter::kAIUO)
          return "AIUO";
        return "AOIE";
      case vital::constants::kComb:
        return strings::kCombStyleNames[style];
      case vital::constants::kPhase:
        return style ? "Negative" : "Positive";
      case vital::constants::kRingModFilter:
        return strings::kRingModStyleNames[style];
      case vital::constants::kSampleHoldFilter:
        return strings::kSampleHoldStyleNames[style];
      case vital::constants::kDistortionFilter:
        return strings::kDistortionTypeNames[style];
      default:
        return {};
    }
  }

  String filterBlendText(int model, double normalized_value) {
    const double engine_value = jlimit(0.0, 1.0, normalized_value) * 2.0;
    if (model == vital::constants::kComb) {
      if (engine_value <= 0.05)
        return "Low comb";
      if (engine_value >= 1.95)
        return "High comb";
      return "Low to high comb, " + String(engine_value, 2);
    }
    if (model == vital::constants::kPhase)
      return "Phaser blend, " + String(engine_value, 2);
    if (model == vital::constants::kSampleHoldFilter) {
      const float bits = vital::SampleHoldFilter::crushBits(static_cast<float>(engine_value));
      return bits <= 0.0f ? String("Off") : String(bits, 1) + " bits";
    }

    if (engine_value <= 0.05)
      return "Low pass";
    if (std::abs(engine_value - 1.0) <= 0.05)
      return "Band pass";
    if (engine_value >= 1.95)
      return "High pass";
    if (engine_value < 1.0)
      return "Between low pass and band pass, " + String(engine_value, 2);
    return "Between band pass and high pass, " + String(engine_value, 2);
  }

  // A preference, so shared by every Atlas instance. The section and preset
  // browser state are per instance: SynthPlugin::EditorMemory.
  bool last_preset_preview = false;

  force_inline int chunkNameToData(const char* chunk_name) {
    return static_cast<int>(ByteOrder::littleEndianInt(chunk_name));
  }

  String getWavetableDataString(InputStream& input_stream) {
    input_stream.setPosition(0);
    int first_chunk = input_stream.readInt();
    if (first_chunk != chunkNameToData("RIFF"))
      return "";

    int length = input_stream.readInt();
    int data_end = static_cast<int>(input_stream.getPosition()) + length;

    if (input_stream.readInt() != chunkNameToData("WAVE"))
      return "";

    while (!input_stream.isExhausted() && input_stream.getPosition() < data_end) {
      int chunk_label = input_stream.readInt();
      int chunk_length = input_stream.readInt();

      if (chunk_label == chunkNameToData("clm ")) {
        MemoryBlock memory_block;
        input_stream.readIntoMemoryBlock(memory_block, chunk_length);
        return memory_block.toString();
      }

      input_stream.setPosition(input_stream.getPosition() + chunk_length);
    }

    return "";
  }

  FileSource::FadeStyle getFadeStyleFromWavetableString(String data) {
    if (data.isEmpty())
      return FileSource::kFreqInterpolate;

    if (data.substring(0, 3) == "<!>") {
      StringArray tokens;
      tokens.addTokens(data.substring(3), " ", "");
      if (tokens.size() < 2 || tokens[1].isEmpty())
        return FileSource::kFreqInterpolate;

      char fade_character = tokens[1][0];
      if (fade_character == '0')
        return FileSource::kNoInterpolate;
      if (fade_character == '1')
        return FileSource::kTimeInterpolate;
    }

    return FileSource::kFreqInterpolate;
  }

  String getAuthorFromWavetableString(String data) {
    if (data.substring(0, 3) == "<!>") {
      int start = data.indexOf("[");
      int end = data.indexOf("]");
      if (start < end && start >= 0)
        return data.substring(start + 1, end);
    }

    return "";
  }

  int loadAudioFile(AudioSampleBuffer& destination, InputStream* audio_stream) {
    AudioFormatManager format_manager;
    format_manager.registerBasicFormats();

    audio_stream->setPosition(0);
    std::unique_ptr<AudioFormatReader> format_reader(
        format_manager.createReaderFor(std::unique_ptr<InputStream>(audio_stream)));
    if (format_reader == nullptr)
      return 0;

    int num_samples = static_cast<int>(format_reader->lengthInSamples);
    destination.setSize(format_reader->numChannels, num_samples);
    format_reader->read(&destination, 0, num_samples, 0, true, true);
    return static_cast<int>(format_reader->sampleRate);
  }

  struct BrowserNode {
    std::map<String, std::unique_ptr<BrowserNode>> folders;
    Array<File> files;
  };

  String browserRootName(const File& root, const String& folder_name) {
    const File data_folder = LoadSave::getDataDirectory().getChildFile(folder_name);
    if (root == data_folder)
      return "Factory";

    const File user_folder = LoadSave::getUserDirectory().getChildFile(folder_name);
    if (root == user_folder)
      return "User";

    return root.getFileName().isNotEmpty() ? root.getFileName() : root.getFullPathName();
  }

  void addBrowserFile(BrowserNode& root, const File& base, const File& file) {
    StringArray parts;
    parts.addTokens(file.getRelativePathFrom(base), File::getSeparatorString(), "");
    if (parts.isEmpty())
      return;

    BrowserNode* node = &root;
    for (int i = 0; i < parts.size() - 1; ++i) {
      const String folder = parts[i];
      if (!node->folders.count(folder))
        node->folders[folder] = std::make_unique<BrowserNode>();
      node = node->folders[folder].get();
    }
    node->files.add(file);
  }

  void sortBrowserNode(BrowserNode& node) {
    node.files.sort();
    for (auto& folder : node.folders)
      sortBrowserNode(*folder.second);
  }

  bool addBrowserNodeToMenu(PopupMenu& menu, const BrowserNode& node,
                            const std::shared_ptr<std::vector<File>>& choices, int& item_id) {
    bool has_items = false;
    for (const auto& folder : node.folders) {
      PopupMenu sub_menu;
      if (addBrowserNodeToMenu(sub_menu, *folder.second, choices, item_id)) {
        menu.addSubMenu(folder.first, sub_menu, true);
        has_items = true;
      }
    }

    for (const auto& file : node.files) {
      choices->push_back(file);
      menu.addItem(item_id++, file.getFileNameWithoutExtension());
      has_items = true;
    }

    return has_items;
  }

  bool browserFileBelongsToRoot(const File& file, const File& root) {
    const String root_path = root.getFullPathName();
    const String file_path = file.getFullPathName();
    return file_path == root_path || file_path.startsWith(root_path + File::getSeparatorString());
  }

  void refreshBrowserFileCache(std::vector<File>& cache, const std::vector<File>& roots, const String& wildcard,
                               const std::atomic<bool>* cancelled = nullptr) {
    cache.clear();
    for (const auto& root : roots) {
      if (!root.isDirectory())
        continue;

      for (const auto& entry : RangedDirectoryIterator(root, true, wildcard, File::findFiles | File::ignoreHiddenFiles)) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) return;
        cache.push_back(entry.getFile());
      }
    }
    // Sort once per scan, with one lowercase key per file. Re-sorting a large
    // IR library on every arrow press can block the message thread for seconds.
    std::vector<std::pair<String, File>> keyed_files;
    keyed_files.reserve(cache.size());
    for (const auto& file : cache)
      keyed_files.emplace_back(file.getFullPathName().toLowerCase(), file);
    std::stable_sort(keyed_files.begin(), keyed_files.end(), [](const auto& a, const auto& b) {
      return a.first.compareNatural(b.first) < 0;
    });
    cache.clear();
    for (auto& entry : keyed_files)
      if (cache.empty() || cache.back() != entry.second) cache.push_back(std::move(entry.second));
  }

  std::vector<File> browserRootsWithAdditionalFolders(const String& folder_name,
                                                      const std::string& additional_folders_name) {
    auto roots = LoadSave::getDirectories(folder_name);
    for (const auto& path : LoadSave::getAdditionalFolders(additional_folders_name))
      roots.push_back(File(path));
    return roots;
  }

  File shiftedBrowserFile(const std::vector<File>& browser_files, const File& current_file, int64_t shift) {
    if (browser_files.empty())
      return File();

    const auto current = std::find(browser_files.begin(), browser_files.end(), current_file);
    if (current == browser_files.end())
      return browser_files.front();
    const auto size = static_cast<int64_t>(browser_files.size());
    const auto index = std::distance(browser_files.begin(), current);
    return browser_files[static_cast<size_t>((index + shift % size + size) % size)];
  }

  PopupMenu createFileBrowserMenu(const std::vector<File>& roots, const String& folder_name,
                                  const std::vector<File>& browser_files,
                                  const std::shared_ptr<std::vector<File>>& choices) {
    PopupMenu menu;
    int item_id = 1;

    for (const auto& root : roots) {
      if (!root.isDirectory())
        continue;

      BrowserNode root_node;
      for (const auto& file : browser_files) {
        if (browserFileBelongsToRoot(file, root))
          addBrowserFile(root_node, root, file);
      }
      sortBrowserNode(root_node);

      PopupMenu root_menu;
      if (addBrowserNodeToMenu(root_menu, root_node, choices, item_id))
        menu.addSubMenu(browserRootName(root, folder_name), root_menu, true);
    }

    if (choices->empty())
      menu.addItem(1, "No files found", false);
    return menu;
  }

  int indexOfPathPart(const StringArray& parts, const String& name) {
    for (int i = 0; i < parts.size(); ++i) {
      if (parts[i] == name)
        return i;
    }
    return -1;
  }

  StringArray presetPathParts(const File& preset) {
    StringArray parts;
    parts.addTokens(preset.getRelativePathFrom(LoadSave::getDataDirectory()), File::getSeparatorString(), "");
    return parts;
  }

  String sanitizePresetPathPart(String text, const String& fallback) {
    text = text.trim().removeCharacters("\\/:*?\"<>|");
    while (text.contains(".."))
      text = text.replace("..", ".");
    if (text.isEmpty())
      text = fallback;
    return text;
  }

  File presetCategoryDirectory(File base, String category) {
    category = category.replaceCharacter('\\', '/');
    StringArray parts;
    parts.addTokens(category, "/", "");
    for (auto part : parts) {
      part = sanitizePresetPathPart(part, "");
      if (part.isNotEmpty() && part != ".")
        base = base.getChildFile(part);
    }
    return base;
  }

  File userPresetBankDirectory(const String& bank) {
    File base = LoadSave::getUserPresetDirectory();
    if (bank.isEmpty() || bank == kUserLibrary)
      return base;
    return base.getChildFile(sanitizePresetPathPart(bank, "User"));
  }

  String presetLibraryName(const File& preset) {
    const auto parts = presetPathParts(preset);
    if (parts.size() > 0) {
      if (parts[0] == kFactoryLibrary)
        return kFactoryLibrary;
      if (parts[0] == kUserLibrary)
        return kUserLibrary;
    }
    return kOtherLibrary;
  }

  String presetBankName(const File& preset) {
    const auto parts = presetPathParts(preset);
    const int preset_folder = indexOfPathPart(parts, LoadSave::kPresetFolderName);
    if (preset_folder >= 0 && preset_folder + 1 < parts.size() - 1 &&
        preset_folder == 1 && (parts[0] == kFactoryLibrary || parts[0] == kUserLibrary)) {
      return parts[preset_folder + 1];
    }
    if (preset_folder == 0 && preset_folder + 1 < parts.size() - 1)
      return parts[preset_folder + 1];
    if (preset_folder > 0)
      return parts[preset_folder - 1];
    return preset.getParentDirectory().getFileName();
  }

  String presetPathCategoryName(const File& preset) {
    const auto parts = presetPathParts(preset);
    const int preset_folder = indexOfPathPart(parts, LoadSave::kPresetFolderName);
    int category_start = preset_folder + 1;
    if (preset_folder >= 0 && preset_folder == 1 && (parts[0] == kFactoryLibrary || parts[0] == kUserLibrary))
      category_start = preset_folder + 2;
    else if (preset_folder == 0)
      category_start = preset_folder + 2;

    if (preset_folder >= 0 && category_start < parts.size() - 1) {
      StringArray category_parts;
      for (int i = category_start; i < parts.size() - 1; ++i)
        category_parts.add(parts[i]);
      return category_parts.joinIntoString(" / ");
    }

    return {};
  }

  String presetCategoryName(const File& preset) {
    const String path_category = presetPathCategoryName(preset);
    if (path_category.isNotEmpty())
      return path_category;

    const String style = LoadSave::getStyleFromFile(preset);
    if (style.isNotEmpty())
      return style;

    return "Uncategorized";
  }

  StringArray presetTagNames(const File& preset) {
    StringArray tags;
    tags.addTokens(LoadSave::getTagsFromFile(preset), ",", "");
    for (int i = tags.size() - 1; i >= 0; --i) {
      tags.set(i, tags[i].trim());
      if (tags[i].isEmpty())
        tags.remove(i);
    }
    tags.removeDuplicates(false);
    return tags;
  }

  // Presets are written with sorted top-level keys, so metadata such as
  // "comments" sits before "settings" in the first kilobytes. Read it from
  // there instead of parsing a file that can be megabytes of settings.
  String presetHeadStringField(const File& preset, const char* field, bool& found_answer) {
    static constexpr int kHeadSize = 65536;
    found_answer = false;
    std::string head;
    if (!vital::multisample::container::readPresetText(preset, head, kHeadSize))
      return {};
    const std::string key = "\"" + std::string(field) + "\"";
    const size_t settings = head.find("\"settings\"");
    const size_t at = head.find(key);
    if (at == std::string::npos || (settings != std::string::npos && settings < at)) {
      // Sorted keys: no field before settings means the preset has none.
      found_answer = settings != std::string::npos;
      return {};
    }
    size_t cursor = head.find(':', at + key.size());
    if (cursor == std::string::npos)
      return {};
    ++cursor;
    while (cursor < head.size() && std::isspace(static_cast<unsigned char>(head[cursor])))
      ++cursor;
    if (cursor >= head.size() || head[cursor] != '"') {
      found_answer = cursor < head.size();
      return {};
    }
    size_t end = cursor + 1;
    while (end < head.size() && head[end] != '"')
      end += head[end] == '\\' ? 2 : 1;
    if (end >= head.size())
      return {};
    try {
      const json value = json::parse(head.substr(cursor, end - cursor + 1));
      found_answer = true;
      return value.is_string() ? String::fromUTF8(value.get<std::string>().c_str()) : String();
    }
    catch (const json::exception&) {
      return {};
    }
  }

  String presetJsonStringField(const File& preset, const char* field) {
    bool found_answer = false;
    const String head_value = presetHeadStringField(preset, field, found_answer);
    if (found_answer)
      return head_value;
    try {
      std::string text;
      vital::multisample::container::readPresetText(preset, text);
      const json data = json::parse(text, nullptr, false);
      if (!data.is_discarded() && data.count(field) && data[field].is_string())
        return String(data[field].get<std::string>());
    }
    catch (const json::exception&) { }
    return {};
  }

  bool writePresetMetadata(const File& preset, const String& name, const String& author,
                           const String& category, const String& tags, const String& comments) {
    try {
      std::string text;
      if (!vital::multisample::container::readPresetText(preset, text))
        return false;
      json data = json::parse(text, nullptr, false);
      if (data.is_discarded() || !data.is_object())
        return false;
      data["preset_name"] = name.toStdString();
      data["author"] = author.toStdString();
      data["comments"] = comments.toStdString();
      data["preset_style"] = category.toStdString();
      data["preset_tags"] = tags.toStdString();
      std::string error;
      return vital::multisample::container::replacePresetText(preset, data.dump(), error);
    }
    catch (const json::exception&) {
      return false;
    }
  }

  PresetBrowserItem buildPresetBrowserItem(const File& preset, const File& data_directory) {
    PresetBrowserItem item;
    item.file = preset;
    item.file_size = preset.getSize();
    item.modified_time = preset.getLastModificationTime().toMilliseconds();
    item.library = presetLibraryName(preset);
    item.bank = presetBankName(preset);
    item.style = LoadSave::getStyleFromFile(preset);
    const String path_category = presetPathCategoryName(preset);
    item.category = path_category.isNotEmpty() ? path_category
                                               : item.style.isNotEmpty() ? item.style : "Uncategorized";
    item.tags = presetTagNames(preset);
    item.author = LoadSave::getAuthorFromFile(preset);
    item.comments = presetJsonStringField(preset, "comments");
    item.label = preset.getFileNameWithoutExtension() + " - " + item.library + " - " + item.bank;
    if (item.category.isNotEmpty() && item.category != "Uncategorized")
      item.label += " - " + item.category;
    item.searchable = (preset.getFileNameWithoutExtension() + " " +
                       item.library + " " + item.bank + " " + item.category + " " +
                       item.tags.joinIntoString(" ") + " " +
                       preset.getRelativePathFrom(data_directory) + " " +
                       item.author + " " + item.comments + " " + item.style).toLowerCase();
    return item;
  }

  constexpr int kPresetIndexSchemaVersion = 2;

  File presetIndexDatabaseFile() {
    return LoadSave::getDataDirectory().getChildFile("AtlasPresetIndex.db");
  }

  String presetPathKey(const File& file) {
    return file.getFullPathName();
  }

  bool presetFileStampMatches(const PresetBrowserItem& item, const File& file) {
    return item.file_size == file.getSize() &&
           item.modified_time == file.getLastModificationTime().toMilliseconds();
  }

  json stringArrayToJson(const StringArray& values) {
    json result = json::array();
    for (const auto& value : values)
      result.push_back(value.toStdString());
    return result;
  }

  StringArray stringArrayFromJson(const json& data) {
    StringArray result;
    if (!data.is_array())
      return result;
    for (const auto& value : data) {
      if (value.is_string())
        result.add(String(value.get<std::string>()));
    }
    result.removeDuplicates(false);
    return result;
  }

  json presetBrowserItemToJson(const PresetBrowserItem& item) {
    json data;
    data["path"] = item.file.getFullPathName().toStdString();
    data["size"] = item.file_size;
    data["modified"] = item.modified_time;
    data["library"] = item.library.toStdString();
    data["bank"] = item.bank.toStdString();
    data["category"] = item.category.toStdString();
    data["tags"] = stringArrayToJson(item.tags);
    data["author"] = item.author.toStdString();
    data["comments"] = item.comments.toStdString();
    data["style"] = item.style.toStdString();
    data["label"] = item.label.toStdString();
    data["searchable"] = item.searchable.toStdString();
    return data;
  }

  bool presetBrowserItemFromJson(const json& data, PresetBrowserItem& item) {
    if (!data.is_object() || !data.count("path") || !data["path"].is_string())
      return false;

    item.file = File(String(data["path"].get<std::string>()));
    item.file_size = data.value("size", static_cast<int64>(0));
    item.modified_time = data.value("modified", static_cast<int64>(0));
    item.library = String(data.value("library", std::string()));
    item.bank = String(data.value("bank", std::string()));
    item.category = String(data.value("category", std::string("Uncategorized")));
    item.tags = data.count("tags") ? stringArrayFromJson(data["tags"]) : StringArray();
    item.author = String(data.value("author", std::string()));
    item.comments = String(data.value("comments", std::string()));
    item.style = String(data.value("style", std::string()));
    item.label = String(data.value("label", std::string()));
    item.searchable = String(data.value("searchable", std::string()));

    if (item.label.isEmpty()) {
      item.label = item.file.getFileNameWithoutExtension() + " - " + item.library + " - " + item.bank;
      if (item.category.isNotEmpty() && item.category != "Uncategorized")
        item.label += " - " + item.category;
    }
    if (item.searchable.isEmpty()) {
      item.searchable = (item.file.getFileNameWithoutExtension() + " " +
                         item.library + " " + item.bank + " " + item.category + " " +
                         item.tags.joinIntoString(" ") + " " + item.author + " " + item.style).toLowerCase();
    }
    return true;
  }

  std::map<std::string, PresetBrowserItem> loadPresetIndexDatabase() {
    std::map<std::string, PresetBrowserItem> items;
    const File database = presetIndexDatabaseFile();
    if (!database.existsAsFile())
      return items;

    try {
      const json data = json::parse(database.loadFileAsString().toStdString(), nullptr, false);
      if (data.is_discarded() || !data.is_object() ||
          data.value("schema", 0) != kPresetIndexSchemaVersion ||
          !data.count("presets") || !data["presets"].is_array())
        return items;

      for (const auto& entry : data["presets"]) {
        PresetBrowserItem item;
        if (presetBrowserItemFromJson(entry, item))
          items[presetPathKey(item.file).toStdString()] = item;
      }
    }
    catch (const json::exception&) {
      return {};
    }
    return items;
  }

  void savePresetIndexDatabase(const std::vector<PresetBrowserItem>& items) {
    File database = presetIndexDatabaseFile();
    database.getParentDirectory().createDirectory();

    json data;
    data["schema"] = kPresetIndexSchemaVersion;
    data["data_directory"] = LoadSave::getDataDirectory().getFullPathName().toStdString();
    data["updated"] = Time::getCurrentTime().toMilliseconds();
    data["presets"] = json::array();
    for (const auto& item : items)
      data["presets"].push_back(presetBrowserItemToJson(item));

    database.replaceWithText(String(data.dump(2)));
  }

  CriticalSection& presetBrowserCacheLock() {
    static CriticalSection lock;
    return lock;
  }

  std::vector<PresetBrowserItem>& cachedPresetBrowserItems() {
    static std::vector<PresetBrowserItem> items;
    return items;
  }

  bool& cachedPresetBrowserItemsValid() {
    static bool valid = false;
    return valid;
  }

  bool isRoutingParameter(const String& id) {
    if (id == "effect_chain_order" || id == "post_effect_order" ||
        id == "filter_1_destination" || id == "filter_2_destination")
      return true;
    if (id.startsWith("effect_chain_slot_"))
      return true;
    if (id.startsWith("bus_") && id.endsWith("_destination"))
      return true;
    if (id.startsWith("bus_") && (id.endsWith("effect_chain_order") || id.endsWith("post_effect_order")))
      return true;
    if (id.startsWith("bus_") && id.contains("_effect_chain_slot_"))
      return true;
    if (id.startsWith("filter_") &&
        (id.endsWith("_osc1_input") || id.endsWith("_osc2_input") ||
         id.endsWith("_osc3_input") || id.endsWith("_osc4_input") || id.endsWith("_sample_input") ||
         id.endsWith("_granular_input") || id.endsWith("_noise_input") || id.endsWith("_filter_input")))
      return true;
    return false;
  }

  int sectionRank(const String& section) {
    static const std::vector<String> order = [] {
      std::vector<String> result {
        "Master and global",
        kPresetSection,
        "Voice and performance",
        kSignalRoutingSection,
        "Oscillator 1",
        "Oscillator 2",
        "Oscillator 3",
        "Oscillator 4",
        "Oscillator 5",
        "Oscillator 6",
        "Oscillator 7",
        "Oscillator 8",
        "Arpeggiator 1",
        "Arpeggiator 2",
        "Filter 1",
        "Filter 2",
        kEffectsChainSection,
        "Distortion",
        "Filter",
        "Chorus",
        "Flanger",
        "Phaser",
        "Delay",
        "Reverb",
        "Utility",
        "Phase Shift",
        "Equalizer",
        "Compressor",
        "Limiter",
        "Frequency Shifter",
        "Dimension Expander",
        "Waveshaper",
        "Convolution Reverb",
        "Mid Side EQ",
        "Spectral Gate",
        "Spectral Lock",
        "Resonator",
        "Analogue Chorus",
        "Analogue Flanger",
        "Analogue Echo",
        "Analogue Rotary",
        "Analogue Spring",
        "Analogue Wah",
        "Spectral Balance",
        "Spectral Compressor",
        "Scream",
        "Erosion",
        "Floaty Delay",
        "Glitch Delay",
        "OpenWurli Amp",
        "Scanner Vibrato"
      };
      for (int envelope = 1; envelope <= vital::kNumEnvelopes; ++envelope)
        result.push_back("Envelope " + String(envelope));
      for (int lfo = 1; lfo <= vital::kNumLfos; ++lfo)
        result.push_back("LFO " + String(lfo));
      for (int random = 1; random <= vital::kNumRandomLfos; ++random)
        result.push_back("Random " + String(random));
      result.insert(result.end(), {
        "Macros",
        "Modulation routing",
        "Zones - Oscillator 1",
        "Zones - Oscillator 2",
        "Zones - Oscillator 3",
        "Zones - Oscillator 4",
        "Zones - Oscillator 5",
        "Zones - Oscillator 6",
        "Zones - Oscillator 7",
        "Zones - Oscillator 8",
        "Other"
      });
      return result;
    }();

    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      if (section == order[i])
        return i;
    }
    if (section.startsWith("Bus 1 - "))
      return 220 + sectionRank(section.fromFirstOccurrenceOf("Bus 1 - ", false, false));
    if (section.startsWith("Bus 2 - "))
      return 250 + sectionRank(section.fromFirstOccurrenceOf("Bus 2 - ", false, false));
    if (section.startsWith("Bus 3 - "))
      return 280 + sectionRank(section.fromFirstOccurrenceOf("Bus 3 - ", false, false));
    return static_cast<int>(order.size());
  }

  String groupForSection(const String& section) {
    if (section == kPresetSection)
      return "Presets";
    if (section.startsWith("Oscillator"))
      return "Oscillators";
    if (section.startsWith("Arpeggiator "))
      return "Arpeggiators";
    if (section == "Voice and performance" || section == kSignalRoutingSection)
      return "Mixer and routing";
    if (section.startsWith("Filter "))
      return "Filters";
    if (section.startsWith("Bus 1 - "))
      return "Bus 1";
    if (section.startsWith("Bus 2 - "))
      return "Bus 2";
    if (section.startsWith("Bus 3 - "))
      return "Bus 3";
    if (section.startsWith("Envelope "))
      return "Envelopes";
    if (section.startsWith("LFO "))
      return "LFOs";
    if (section.startsWith("Random "))
      return "Random modulation";
    if (section == "Macros")
      return "Macros";
    if (section.startsWith("Zones - "))
      return "Zones";
    if (section == kEffectsChainSection || section == "Distortion" || section == "Filter" ||
        section == "Chorus" || section == "Flanger" || section == "Phaser" ||
        section == "Delay" || section == "Reverb" || section == "Utility" || section == "Phase Shift" ||
        section == "Equalizer" || section == "Compressor" || section == "Limiter" ||
        section == "Frequency Shifter" || section == "Dimension Expander" || section == "Waveshaper" ||
        section == "Convolution Reverb" || section == "Mid Side EQ" || section == "Spectral Gate" ||
        section == "Erosion" || section == "Floaty Delay" || section == "Spectral Lock" || section == "Spectral Compressor" || section == "Scream" || section == "Trance gate" ||
        section == "Resonator" || section == "Analogue Chorus" ||
        section == "Analogue Flanger" || section == "Analogue Echo" ||
        section == "Analogue Rotary" || section == "Analogue Spring" ||
        section == "Analogue Wah" || section == "Spectral Balance" || section == "Glitch Delay" ||
        section == "OpenWurli Amp" || section == "Scanner Vibrato")
      return "Effects";
    if (section == "Modulation routing")
      return "Modulation";
    return "Global";
  }

  int groupRank(const String& group) {
    static const std::array<const char*, 16> order = {{
      "Presets",
      "Oscillators",
      "Mixer and routing",
      "Arpeggiators",
      "Filters",
      "Bus 1",
      "Bus 2",
      "Bus 3",
      "Envelopes",
      "LFOs",
      "Random modulation",
      "Macros",
      "Effects",
      "Modulation",
      "Zones",
      "Global"
    }};

    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      if (group == order[i])
        return i;
    }
    return static_cast<int>(order.size());
  }

  int modulationSlotForParameterId(const String& id);
  int modulationParameterSuffixRank(const String& id);
  int macroIndexForControlId(const String& id);
  bool isMacroBipolarParameterId(const String& id);
  String macroBipolarParameterId(int macroIndex);

  int equalizerParameterRank(const String& suffix) {
    static const std::array<const char*, 24> order = {{
      "low_mode", "low_cutoff", "low_gain", "low_resonance",
      "band_mode", "band_cutoff", "band_gain", "band_resonance",
      "band_2_mode", "band_2_cutoff", "band_2_gain", "band_2_resonance",
      "band_3_mode", "band_3_cutoff", "band_3_gain", "band_3_resonance",
      "band_4_mode", "band_4_cutoff", "band_4_gain", "band_4_resonance",
      "high_mode", "high_cutoff", "high_gain", "high_resonance"
    }};
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      if (suffix == order[i])
        return i;
    }
    return 900;
  }

  // Glitch Delay: the global controls, then Show Tap and the shown tap's rows.
  int glitchDelayParameterRank(const String& suffix) {
    static const StringArray global { "sync", "glide", "freeze", "freeze_sustain", "stop_tape", "stop_time",
                                      "start_time", "placement", "buffer_size", "buffer_sync", "max_glitches",
                                      "length_min", "length_max", "reproducible", "seed", "smear_amount",
                                      "smear_size", "mix", "output_gain", "tap_view" };
    static const StringArray tap { "enabled", "time", "time_sync", "volume", "pan", "feedback", "low_cut",
                                   "high_cut", "reverse_prob", "stutter_prob", "stutter_min", "stutter_max",
                                   "stutter_sync_min", "stutter_sync_max", "grain_prob", "grain_size_min",
                                   "grain_size_max", "grain_density_min", "grain_density_max", "pitch_prob",
                                   "pitch_mode", "pitch_min", "pitch_max", "pitch_speed_min", "pitch_speed_max",
                                   "lpc_prob", "lpc_min", "lpc_max", "ceps_prob", "ceps_min", "ceps_max",
                                   "ring_prob", "ring_min", "ring_max", "fm_prob", "fm_ratio_min", "fm_ratio_max",
                                   "fm_index_min", "fm_index_max", "crush_prob", "crush_bits_min",
                                   "crush_bits_max", "crush_rate_min", "crush_rate_max" };
    if (global.contains(suffix))
      return global.indexOf(suffix);
    if (suffix.startsWith("tap_")) {
      const String rest = suffix.substring(4);
      const int tap_number = rest.upToFirstOccurrenceOf("_", false, false).getIntValue();
      const int index = tap.indexOf(rest.fromFirstOccurrenceOf("_", false, false));
      if (tap_number > 0 && index >= 0)
        return 100 * tap_number + index;
    }
    return 900;
  }

  int effectSlotParameterRank(const String& effect_id, const String& suffix) {
    auto rank = [&suffix](std::initializer_list<const char*> order) {
      int index = 0;
      for (const auto* name : order) {
        if (suffix == name)
          return index;
        ++index;
      }
      return 900;
    };

    if (effect_id == "chorus")
      return rank({ "voices", "delay_1", "delay_2", "sync", "frequency", "tempo",
                    "mod_depth", "feedback", "cutoff", "spread", "dry_wet" });
    if (effect_id == "compressor")
      return rank({ "enabled_bands", "attack", "release",
                    "low_upper_threshold", "low_upper_ratio", "low_lower_threshold", "low_lower_ratio", "low_gain",
                    "band_upper_threshold", "band_upper_ratio", "band_lower_threshold", "band_lower_ratio", "band_gain",
                    "high_upper_threshold", "high_upper_ratio", "high_lower_threshold", "high_lower_ratio", "high_gain",
                    "mix" });
    if (effect_id == "delay")
      return rank({ "style", "sync", "frequency", "tempo", "aux_sync", "aux_frequency", "aux_tempo",
                    "feedback", "filter_cutoff", "filter_spread", "dry_wet" });
    if (effect_id == "distortion")
      return rank({ "type", "drive", "filter_order", "filter_cutoff", "filter_resonance", "filter_blend", "mix" });
    if (effect_id == "eq")
      return equalizerParameterRank(suffix);
    if (effect_id == "filter_fx")
      return rank({ "model", "style", "distortion_drive", "distortion_filter_order", "cutoff", "reverb_cutoff", "scream_amount", "resonance", "reverb_damp", "reverb_bass_preserve", "scream_input_gain", "table_frame", "drive", "blend", "blend_transpose", "keytrack",
                    "formant_x", "formant_y", "formant_transpose", "formant_resonance", "formant_spread", "mix", "reverb_output", "scream_output_gain", "scream_autogain" });
    if (effect_id == "flanger")
      return rank({ "sync", "frequency", "tempo", "center", "mod_depth", "phase_offset", "feedback", "dry_wet" });
    if (effect_id == "phaser")
      return rank({ "sync", "frequency", "tempo", "center", "mod_depth", "phase_offset", "blend", "feedback", "dry_wet" });
    if (effect_id == "reverb")
      return rank({ "size", "delay", "decay_time", "pre_low_cutoff", "pre_high_cutoff",
                    "low_shelf_cutoff", "low_shelf_gain", "high_shelf_cutoff", "high_shelf_gain",
                    "chorus_frequency", "chorus_amount", "dry_wet" });
    if (effect_id == "utility")
      return rank({ "width", "low_cutoff", "high_cutoff", "filter_slope", "output_gain" });
    if (effect_id == "phase_shift")
      return rank({ "amount", "tone", "dry_wet" });
    if (effect_id == "suppressor")
      return rank({ "amount", "smoothing", "release", "frequency", "width", "slope", "delta" });
    if (effect_id == "defuse")
      return rank({ "stages", "frequency", "resonance", "spread", "spread_style", "automation_precision" });
    if (effect_id == "tape") {
      for (int i = 0; i < vital::tape::kNumControls; ++i)
        if (suffix == vital::tape::controls[i].suffix) return i;
      return 900;
    }
    if (effect_id == "trance_gate")
      return rank({ "mix", "trigger", "grid", "start", "stop", "attack", "decay", "sustain" });
    if (effect_id == "tremolo_pan")
      return rank({ "sync", "frequency", "tempo", "shape", "stereo_phase", "offset", "depth", "smoothing", "mix" });
    if (effect_id == "scream")
      return rank({ "cutoff", "scream", "resonance", "input_gain", "wet", "output_gain",
                    "gate_release", "autogain", "keytracking" });
    if (effect_id == "spectral_lock")
      return rank({ "amount", "isolate", "shimmer", "bend", "bend_range", "range_high", "range_low",
                    "mute_high", "mute_low", "spray", "tilt", "pitch", "root", "scale", "mix" });
    if (effect_id == "resonator")
      return rank({ "model", "quality", "decay", "material", "brightness", "inharmonics", "ratio", "radius",
                    "opening", "hit", "listen_left", "listen_right", "midi_pitch", "tune", "transpose", "fine",
                    "spread", "glide", "note_priority", "off_decay", "lfo_amount", "lfo_shape", "lfo_sync",
                    "lfo_frequency", "lfo_tempo", "lfo_stereo_phase", "lfo_spin", "lfo_offset", "filter",
                    "filter_frequency", "filter_bandwidth", "bleed", "width", "excitation", "wet_gain",
                    "limiter", "output_gain", "mix" });
    if (effect_id == "analoguechorus")
      return rank({ "model", "juno_mode", "dimension_mode", "rate", "depth", "noise", "width", "mix" });
    if (effect_id == "analogueflanger")
      return rank({ "medium", "sync", "frequency", "tempo", "depth", "manual", "feedback", "stereo_phase",
                    "noise", "age", "mix" });
    if (effect_id == "analogueecho")
      return rank({ "medium", "sync", "frequency", "tempo", "head_1", "head_1_pan", "head_2", "head_2_pan",
                    "head_3", "head_3_pan", "width", "feedback", "inertia", "drive", "bass", "treble", "wow",
                    "flutter", "noise", "age", "mix" });
    if (effect_id == "analoguerotary")
      return rank({ "model", "speed", "inertia", "drive", "balance", "spread", "mix" });
    if (effect_id == "analoguespring")
      return rank({ "tank", "dwell", "tone", "mono", "pan", "noise", "mix" });
    if (effect_id == "analoguewah")
      return rank({ "model", "position", "envelope", "decay", "mode", "range", "peak", "mix" });
    if (effect_id == "openwurli_amp")
      return rank({ "volume", "tremolo_depth", "speaker" });
    if (effect_id == "scanner_vibrato")
      return rank({ "mode", "rate" });
    if (effect_id == "glitch_delay")
      return glitchDelayParameterRank(suffix);
    if (effect_id == "spectral_balance")
      return rank({ "amount", "target", "bias", "speed", "range_low", "range_high", "tame_recover", "mix" });
    if (effect_id == "spectral_compressor")
      return rank({ "compression", "downward", "upward", "target", "attack", "release",
                    "smooth", "curve", "tilt", "makeup", "mix" });
    if (effect_id == "spectral_gate")
      return rank({ "fft_size", "cutoff", "balance", "tilt_enabled", "tilt", "mix" });
    if (effect_id == "frequency_shifter")
      return rank({ "shift", "delay", "feedback", "dry_wet" });
    if (effect_id == "limiter")
      return rank({ "gain", "ceiling", "release", "dry_wet" });
    if (effect_id == "dimension_expander")
      return rank({ "mode", "delay", "amount", "low_cutoff", "high_cutoff", "quorax_size", "quorax_motion",
                    "quorax_low_cut", "quorax_high_cut", "quorax_slope", "quorax_mix", "quorax_output" });
    if (effect_id == "erosion")
      return rank({ "mode", "frequency", "width", "amount", "stereo" });
    if (effect_id == "floaty_delay")
      return rank({ "sync", "time", "playback_rate", "feedback", "warp_rate", "warp_amount",
                    "cutoff", "resonance", "clamp", "stereo_offset", "mix" });
    if (effect_id == "waveshaper")
      return rank({ "mode", "shape", "drive", "tension", "warp_type", "warp_amount",
                    "remove_dc", "output_gain", "dry_wet" });
    if (effect_id == "convolution_reverb")
      return rank({ "predelay_sync", "predelay", "predelay_tempo", "ir_start", "ir_end", "fade_in", "fade_out",
                    "ir_size", "ir_decay", "ir_dampening", "ir_gain", "ir_reverse", "width", "tone",
                    "low_cutoff", "high_cutoff", "filter_slope", "dry_wet" });
    if (effect_id == "mid_side_eq") {
      if (suffix.startsWith("mid_eq_"))
        return equalizerParameterRank(suffix.fromFirstOccurrenceOf("mid_eq_", false, false));
      if (suffix.startsWith("side_eq_"))
        return 100 + equalizerParameterRank(suffix.fromFirstOccurrenceOf("side_eq_", false, false));
    }
    return 900;
  }

  // Which rows a slot shows for a choice control's value: Dimension Expander's
  // mode, or the group of Resonator models that share the same rows. The editor
  // rebuilds a slot only when this changes, not on every Model step.
  int effectSlotRowVariant(const String& control_id, float value) {
    if (control_id.endsWith("resonator_model")) {
      const int model = roundToInt(value);
      if (model == atlas::resonator::kMembrane || model == atlas::resonator::kPlate) return 1;
      if (model == atlas::resonator::kPipe) return 2;
      if (model == atlas::resonator::kTube) return 3;
      return 0;
    }
    if (control_id.endsWith("analoguechorus_model") || control_id.endsWith("analoguerotary_model") ||
        control_id.endsWith("glitch_delay_tap_view"))
      return roundToInt(value);
    // The two pedals share their rows; the Mu-Tron III adds its own.
    if (control_id.endsWith("analoguewah_model"))
      return roundToInt(value) == atlas::analogue::wah::kMuTron ? 1 : 0;
    return value >= 0.5f ? 1 : 0;
  }

  // Choices that change which rows the editor shows, so choosing one rebuilds
  // it: oscillator type and analog waveform, frequency morph type (Random
  // Amplitudes adds a seed), filter model and style, and the effect slot
  // variants above. They are menus; arrows step through the choices and each
  // step rebuilds the rows.
  bool isRowSwitchingChoice(const String& id) {
    if (const int slot = sourceSlotForId(id); slot >= 0) {
      const String suffix = id.substring(String("osc_" + String(slot + 1) + "_").length());
      return suffix == "type" || suffix == "analog_waveform" || suffix == "braids_model" ||
             suffix.endsWith("spectral_morph_type");
    }
    const String filter_suffix = filterSuffixForParameter(id);
    if (filter_suffix == "model" || filter_suffix == "style")
      return true;
    for (const char* variant : { "dimension_expander_mode", "resonator_model", "analoguechorus_model",
                                 "analogueecho_medium", "analoguerotary_model", "analoguewah_model",
                                 "convolution_reverb_predelay_sync", "spectral_lock_mode",
                                 "glitch_delay_tap_view" }) {
      if (id == variant || id.endsWith(String("_") + variant))
        return true;
    }
    return false;
  }

  // Switches that change which rows the editor shows, so flipping one
  // rebuilds it: Envelope 1's analog mode swaps its Power rows for Analog Age.
  bool isRowSwitchingToggle(const String& id) {
    return id == "env_1_analog";
  }

  // Convolution Reverb: free predelay in milliseconds, or a synced division.
  bool isConvolutionReverbParameterRelevant(const String& suffix, int synced) {
    if (suffix == "predelay") return synced == 0;
    if (suffix == "predelay_tempo") return synced == 1;
    return true;
  }

  // Glitch Delay shows only the tap chosen by Show Tap, and with Host Sync on,
  // the note values in place of the times they stand in for.
  bool isGlitchDelayParameterRelevant(const String& suffix, int shown_tap, bool synced) {
    if (suffix == "buffer_size") return !synced;
    if (suffix == "buffer_sync") return synced;
    if (suffix == "tap_view" || !suffix.startsWith("tap_"))
      return true;
    const String rest = suffix.substring(4);
    if (rest.upToFirstOccurrenceOf("_", false, false).getIntValue() != shown_tap + 1)
      return false;
    const String control = rest.fromFirstOccurrenceOf("_", false, false);
    if (control == "time" || control == "stutter_min" || control == "stutter_max") return !synced;
    if (control == "time_sync" || control == "stutter_sync_min" || control == "stutter_sync_max") return synced;
    return true;
  }

  // Spectral Lock: the filterbank's resynthesis controls hide in Spectral
  // mode, which has its own onset and reach controls.
  bool isSpectralLockParameterRelevant(const String& suffix, int mode) {
    static const StringArray filterbank_only { "isolate", "shimmer", "spray", "tilt", "width", "follow",
                                               "mute_low", "mute_high" };
    static const StringArray spectral_only { "transients", "reach" };
    if (filterbank_only.contains(suffix)) return mode == 0;
    if (spectral_only.contains(suffix)) return mode == 1;
    return true;
  }

  // Resonator rows that only affect some models are hidden for the others.
  bool isResonatorParameterRelevant(const String& suffix, int model) {
    const bool solid = model < atlas::resonator::kPipe;
    if (suffix == "ratio")
      return model == atlas::resonator::kMembrane || model == atlas::resonator::kPlate;
    if (suffix == "radius")
      return !solid;
    if (suffix == "opening")
      return model == atlas::resonator::kPipe;
    if (suffix == "quality" || suffix == "material" || suffix == "brightness" || suffix == "inharmonics" ||
        suffix == "hit" || suffix == "listen_left" || suffix == "listen_right" || suffix == "bleed")
      return solid;
    return true;
  }

  // Each Analogue Chorus model shows only its own mode switch.
  bool isAnalogueChorusParameterRelevant(const String& suffix, int model) {
    if (suffix == "juno_mode")
      return model == atlas::analogue::chorus::kJuno;
    if (suffix == "dimension_mode")
      return model == atlas::analogue::chorus::kDimension;
    return true;
  }

  // Tape shows its three heads; BBD shows its single tap's Width.
  bool isAnalogueEchoParameterRelevant(const String& suffix, int medium) {
    if (suffix.startsWith("head_"))
      return medium == atlas::analogue::echo::kTape;
    if (suffix == "width")
      return medium == atlas::analogue::echo::kBbd;
    return true;
  }

  // The Organ 122 and the OpenB3 Whirl have a horn and a drum to balance;
  // the Whirl has no amplifier to drive.
  bool isAnalogueRotaryParameterRelevant(const String& suffix, int model) {
    if (suffix == "balance")
      return model == atlas::analogue::rotary::kOrgan || model == atlas::analogue::rotary::kWhirl;
    if (suffix == "drive")
      return model != atlas::analogue::rotary::kWhirl;
    return true;
  }

  // Mode, Range and Peak are the Mu-Tron III's; the pedals have none.
  bool isAnalogueWahParameterRelevant(const String& suffix, int model) {
    if (suffix == "mode" || suffix == "range" || suffix == "peak")
      return model == atlas::analogue::wah::kMuTron;
    return true;
  }

  bool shouldExposeEffectSlotParameter(const String& effect_id, const String& suffix) {
    // Dynamic slots are switched by fx_slot_N_enabled. The cloned legacy effect
    // switch is not consulted by ReorderableEffectChain and would be a misleading
    // second bypass control in the accessible UI.
    if (suffix == "on")
      return false;
    // The keyboard sequencer is the only step editor: no per-step sliders.
    if (effect_id == "trance_gate" && suffix.startsWith("step_"))
      return false;
    // Keep the retired gain's saved parameter/DSP for old presets, but present
    // only the useful output gain in new Utility editing and assignments.
    if (effect_id == "utility" && suffix == "input_gain")
      return false;
    if (effect_id == "compressor" && suffix == "low_band_unused")
      return false;
    if (effect_id == "filter_fx" &&
        (suffix.endsWith("_input") || suffix == "sample_input" || suffix == "filter_input"))
      return false;
    return true;
  }

  int parameterRank(const String& id) {
    String rank_id = id;
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "bus_" + String(bus) + "_";
      if (rank_id.startsWith(prefix)) {
        rank_id = rank_id.fromFirstOccurrenceOf(prefix, false, false);
        break;
      }
    }

    auto ordered = [](const String& value, std::initializer_list<const char*> names) {
      int rank = 0;
      for (const auto* name : names) {
        if (value == name)
          return rank;
        ++rank;
      }
      return 900;
    };

    // Keep signal flow and related controls together in every presentation,
    // including the legacy effect pages and modulation destination lists.
    if (rank_id.startsWith("env_")) {
      const String suffix = rank_id.substring(4).fromFirstOccurrenceOf("_", false, false);
      return ordered(suffix, { "analog", "analog_age", "tempo_sync", "delay", "attack", "attack_power", "hold", "decay",
                               "decay_power", "sustain", "release", "release_power" });
    }
    for (const char* prefix : { "filter_1_", "filter_2_" }) {
      if (rank_id.startsWith(prefix)) {
        const String suffix = rank_id.substring(String(prefix).length());
        if (suffix == "on")
          return 0;
        const int signal_rank = effectSlotParameterRank("filter_fx", suffix);
        if (signal_rank < 900)
          return 1 + signal_rank;
        return 100 + ordered(suffix, { "osc1_input", "osc2_input", "osc3_input", "osc4_input",
                                      "sample_input", "filter_input" });
      }
    }
    for (int effect = 0; effect < vital::constants::kNumEffects; ++effect) {
      const String effect_id(strings::kEffectOrder[effect]);
      const String prefix = effect_id + "_";
      if (rank_id.startsWith(prefix)) {
        const String suffix = rank_id.substring(prefix.length());
        return suffix == "on" ? 0 : 1 + effectSlotParameterRank(effect_id, suffix);
      }
    }
    const int global_rank = ordered(rank_id, {
        "volume", "oversampling", "beats_per_minute", "velocity_track", "stereo_mode", "stereo_routing",
        "pitch_bend_range", "pitch_wheel", "mod_wheel", "mpe_enabled", "bypass", "view_spectrogram",
        "polyphony", "legato", "portamento_time", "portamento_slope", "portamento_force", "portamento_scale",
        "portamento_glide_zones", "zone_crossfade", "voice_priority", "voice_override",
        "release_velocity_target", "release_velocity_mode", "release_velocity_glide_time", "release_velocity_glide_curve", "voice_transpose",
        "voice_tune", "voice_amplitude", "destination", "effect_chain_order", "post_effect_order" });
    if (global_rank < 900)
      return global_rank;
    if (rank_id.startsWith("sub_"))
      return ordered(rank_id.substring(4), { "on", "direct_out", "waveform", "transpose", "tune",
                                            "transpose_quantize", "level", "pan" });

    auto oscillator_suffix_rank = [](const String& suffix) {
      static const std::array<const char*, 59> oscillator_order = {{
        "on",
        "type",
        "level",
        "pan",
        "destination",
        "solo",
        "transpose",
        "tune",
        "fixed_frequency_mode",
        "fixed_frequency",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "analog_waveform",
        "analog_shape",
        "sub_level",
        "sub_octave",
        "sub_waveform",
        "analog_drift",
        "analog_voice_spread",
        "analog_wear",
        "analog_sag",
        "analog_noise",
        "analog_hum",
        "analog_hum_frequency",
        "wave_frame",
        "unison_voices",
        "unison_detune",
        "unison_blend",
        "stack_style",
        "detune_power",
        "detune_range",
        "spectral_unison",
        "frame_spread",
        "stereo_spread",
        "phase",
        "random_phase",
        "midi_track",
        "smooth_interpolation",
        "distortion_type",
        "distortion_amount",
        "distortion_spread",
        "secondary_distortion_type",
        "secondary_distortion_amount",
        "secondary_distortion_spread",
        "spectral_morph_type",
        "spectral_morph_amount",
        "spectral_morph_seed",
        "spectral_morph_spread",
        "secondary_spectral_morph_type",
        "secondary_spectral_morph_amount",
        "secondary_spectral_morph_spread",
        "secondary_spectral_morph_seed",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end",
        "view_2d"
      }};
      for (int i = 0; i < static_cast<int>(oscillator_order.size()); ++i) {
        if (suffix == oscillator_order[i])
          return 100 + i;
      }
      return 500;
    };

    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (id.startsWith(prefix)) {
        // A type's own parameters follow the shared oscillator controls in their legacy order.
        const String legacy = legacySourceId(id);
        if (legacy != id)
          return osc * 1000 + 600 + parameterRank(legacy) % 100;
        return osc * 1000 + oscillator_suffix_rank(id.fromFirstOccurrenceOf(prefix, false, false));
      }
    }
    if (id.startsWith("sample_")) {
      const String suffix = id.fromFirstOccurrenceOf("sample_", false, false);
      static const std::array<const char*, 29> sample_order = {{
        "on",
        "solo",
        "destination",
        "transpose",
        "tune",
        "keytrack",
        "root_key",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "level",
        "pan",
        "playback_mode",
        "loop",
        "bounce",
        "random_phase",
        "start",
        "end",
        "loop_start",
        "loop_end",
        "loop_crossfade",
        "low_cutoff",
        "high_cutoff",
        "filter_slope",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end"
      }};
      for (int i = 0; i < static_cast<int>(sample_order.size()); ++i) {
        if (suffix == sample_order[i])
          return 4500 + i;
      }
      if (suffix == "level")
        return 4525;
      if (suffix == "pan")
        return 4526;
      return 4590;
    }

    if (id.startsWith("granular_")) {
      const String suffix = id.fromFirstOccurrenceOf("granular_", false, false);
      static const std::array<const char*, 44> granular_order = {{
        "on",
        "solo",
        "destination",
        "transpose",
        "tune",
        "keytrack",
        "root_key",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "level",
        "pan",
        "mode",
        "grain_count",
        "timing_mode",
        "grid_rate",
        "density",
        "midi_density",
        "timing_jitter",
        "size_mode",
        "grain_size",
        "grid_length",
        "window_shape",
        "speed",
        "position",
        "position_grid",
        "position_mod",
        "position_mod_rate",
        "random_position",
        "random_volume",
        "random_pan",
        "random_pitch",
        "interval",
        "interval_chance",
        "direction",
        "start",
        "end",
        "low_cutoff",
        "high_cutoff",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end"
      }};
      for (int i = 0; i < static_cast<int>(granular_order.size()); ++i) {
        if (suffix == granular_order[i])
          return 4600 + i;
      }
      return 4690;
    }

    if (id.startsWith("spectral_") && !id.startsWith("spectral_gate_") && !id.startsWith("spectral_compressor_") &&
        !id.startsWith("spectral_lock_") && !id.startsWith("spectral_balance_")) {
      const String suffix = id.fromFirstOccurrenceOf("spectral_", false, false);
      static const std::array<const char*, 39> spectral_order = {{
        "on",
        "solo",
        "destination",
        "transpose",
        "tune",
        "keytrack",
        "root_key",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "level",
        "pan",
        "spectral_unison",
        "play_mode",
        "position",
        "speed",
        "loop_start",
        "loop_end",
        "bin_low",
        "bin_high",
        "shift",
        "stretch",
        "blur",
        "tilt",
        "phase_randomize",
        "freeze",
        "keep_fundamental",
        "phase_mode",
        "preserve_transients",
        "effect",
        "effect_amount",
        "spectral_morph_type",
        "spectral_morph_amount",
        "spectral_morph_spread",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end"
      }};
      for (int i = 0; i < static_cast<int>(spectral_order.size()); ++i) {
        if (suffix == spectral_order[i])
          return 4700 + i;
      }
      return 4790;
    }

    if (id.startsWith("noise_")) {
      const String suffix = id.fromFirstOccurrenceOf("noise_", false, false);
      static const std::array<const char*, 14> noise_order = {{
        "on",
        "solo",
        "destination",
        "type",
        "width",
        "low_cutoff",
        "high_cutoff",
        "filter_slope",
        "level",
        "pan",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end"
      }};
      for (int i = 0; i < static_cast<int>(noise_order.size()); ++i) {
        if (suffix == noise_order[i])
          return 5600 + i;
      }
      return 5690;
    }

    // Braids pitch rows: random phase follows Transpose and Tune, as on the
    // Wavetable. The other Braids rows keep their source order.
    if (id.startsWith("braids_")) {
      const String suffix = id.fromFirstOccurrenceOf("braids_", false, false);
      static const std::array<const char*, 3> braids_pitch_order = {{ "transpose", "tune", "random_phase" }};
      for (int i = 0; i < static_cast<int>(braids_pitch_order.size()); ++i) {
        if (suffix == braids_pitch_order[i])
          return 5700 + i;
      }
    }

    if (id.startsWith("wurli_")) {
      const String suffix = id.fromFirstOccurrenceOf("wurli_", false, false);
      static const std::array<const char*, 10> wurli_order = {{
        "amp_env", "bark", "damping", "condition", "treble_voicing", "register_balance", "pickup_low_cut", "mlp",
        "transpose", "tune"
      }};
      for (int i = 0; i < static_cast<int>(wurli_order.size()); ++i) {
        if (suffix == wurli_order[i])
          return 5720 + i;
      }
    }

    // OpenB3: the drawbars left to right, as on the organ, then percussion's
    // four switches in the organ's order, the key click and the pitch rows.
    if (id.startsWith("b3_")) {
      const String suffix = id.fromFirstOccurrenceOf("b3_", false, false);
      static const std::array<const char*, 19> b3_order = {{
        "amp_env", "drawbar_16", "drawbar_5_1_3", "drawbar_8", "drawbar_4", "drawbar_2_2_3", "drawbar_2",
        "drawbar_1_3_5", "drawbar_1_1_3", "drawbar_1", "percussion", "percussion_volume", "percussion_decay",
        "percussion_harmonic", "key_click", "release_click", "temperament", "transpose", "tune"
      }};
      for (int i = 0; i < static_cast<int>(b3_order.size()); ++i) {
        if (suffix == b3_order[i])
          return 5740 + i;
      }
    }

    // Speech: how notes take the words, the voice, how it sounds, then the
    // envelope mode; the pitch rows follow in their own group.
    if (id.startsWith("speech_")) {
      const String suffix = id.fromFirstOccurrenceOf("speech_", false, false);
      static const std::array<const char*, 12> speech_order = {{
        "mode", "split", "language", "variant", "formant", "breath", "speed", "vibrato", "vibrato_rate",
        "amp_env", "transpose", "tune"
      }};
      for (int i = 0; i < static_cast<int>(speech_order.size()); ++i) {
        if (suffix == speech_order[i])
          return 5770 + i;
      }
    }

    for (int random = 1; random <= vital::kNumRandomLfos; ++random) {
      const String prefix = "random_" + String(random) + "_";
      if (!id.startsWith(prefix))
        continue;

      const String suffix = id.fromFirstOccurrenceOf(prefix, false, false);
      static const std::array<const char*, 8> random_order = {{
        "style",
        "frequency",
        "rate_x10",
        "sync",
        "tempo",
        "sync_type",
        "stereo",
        "keytrack_transpose",
      }};
      for (int i = 0; i < static_cast<int>(random_order.size()); ++i) {
        if (suffix == random_order[i])
          return 7000 + random * 100 + i;
      }
      if (suffix == "keytrack_tune")
        return 7000 + random * 100 + 8;
      return 7000 + random * 100 + 90;
    }

    for (int lfo = 1; lfo <= vital::kNumLfos; ++lfo) {
      const String prefix = "lfo_" + String(lfo) + "_";
      if (!id.startsWith(prefix))
        continue;

      const String suffix = id.fromFirstOccurrenceOf(prefix, false, false);
      static const std::array<const char*, 15> lfo_order = {{
        "phase",
        "sync_type",
        "frequency",
        "rate_x10",
        "sync",
        "tempo",
        "deform",
        "swing",
        "fade_time",
        "smooth_mode",
        "smooth_time",
        "delay_time",
        "stereo",
        "keytrack_transpose",
        "keytrack_tune"
      }};
      for (int i = 0; i < static_cast<int>(lfo_order.size()); ++i) {
        if (suffix == lfo_order[i])
          return 7200 + lfo * 100 + i;
      }
      return 7200 + lfo * 100 + 90;
    }

    for (int arp = 1; arp <= vital::kNumArpeggiators; ++arp) {
      const String prefix = "arp_" + String(arp) + "_";
      if (!id.startsWith(prefix))
        continue;

      const String suffix = id.fromFirstOccurrenceOf(prefix, false, false);
      static const std::array<const char*, 29> arp_order = {{
        "on",
        "time_mode",
        "sync_rate",
        "free_rate",
        "style",
        "play_order",
        "gate",
        "random_gate",
        "repeats",
        "loop",
        "offset",
        "distance",
        "octave",
        "steps",
        "velocity",
        "velocity_decay",
        "random_velocity",
        "hold",
        "retrigger",
        "retrigger_division",
        "channel",
        "destination_osc_1",
        "destination_osc_2",
        "destination_osc_3",
        "destination_osc_4",
        "destination_osc_5",
        "destination_osc_6",
        "destination_osc_7",
        "destination_osc_8"
      }};
      for (int i = 0; i < static_cast<int>(arp_order.size()); ++i) {
        if (suffix == arp_order[i])
          return 8500 + arp * 100 + i;
      }
      return 8500 + arp * 100 + 90;
    }

    const int modulation_slot = modulationSlotForParameterId(id);
    if (modulation_slot > 0)
      return 200000 + modulation_slot * 10 + modulationParameterSuffixRank(id);

    const int macro_index = macroIndexForControlId(id);
    if (macro_index >= 0)
      return 10000 + macro_index;
    if (isMacroBipolarParameterId(id))
      return 11000 + id.fromFirstOccurrenceOf("macro_bipolar_", false, false).getIntValue();

    // Unlisted controls retain their source order rather than being sorted by label.
    return 1000;
  }

  String readableId(String id) {
    return id.replaceCharacter('_', ' ').toLowerCase().substring(0, 1).toUpperCase()
           + id.replaceCharacter('_', ' ').toLowerCase().substring(1);
  }

  String effectName(int effect) {
    // kEffectSlotTypeNames is "Off" followed by proper display names.
    if (isPositiveAndBelow(effect, vital::constants::kNumEffects))
      return strings::kEffectSlotTypeNames[effect + 1];
    return {};
  }

  // Effect indices sorted by display name, for menus that list every effect.
  const std::vector<int>& alphabeticalEffects() {
    static const std::vector<int> order = [] {
      std::vector<int> result;
      for (int effect = 0; effect < vital::constants::kNumEffects; ++effect)
        if (effect != vital::constants::kScream) // Retained for old presets; new instances use Filter -> Scream.
          result.push_back(effect);
      std::sort(result.begin(), result.end(), [](int a, int b) {
        return effectName(a).compareIgnoreCase(effectName(b)) < 0;
      });
      return result;
    }();
    return order;
  }

  // Effects grouped for the Add effect and Replace menus. Utility stays at the top level.
  struct EffectCategory {
    const char* name;
    std::vector<int> effects;
  };
  const std::vector<EffectCategory>& effectCategories() {
    using namespace vital::constants;
    static const std::vector<EffectCategory> categories = [] {
      std::vector<EffectCategory> result {
        { "Analogue", { kAnalogueChorus, kAnalogueFlanger, kAnalogueEcho, kAnalogueRotary, kAnalogueSpring, kAnalogueWah,
                        kTape, kOpenWurliAmp, kScannerVibrato } },
        { "Dynamics", { kCompressor, kLimiter, kSuppressor } },
        { "Space", { kReverb, kConvolutionReverb, kDelay, kFloatyDelay, kGlitchDelay } },
        { "Modulation", { kChorus, kFlanger, kPhaser, kTremoloPan, kDimensionExpander, kPhaseShift,
                          kFrequencyShifter, kTranceGate } },
        { "Distortion and character", { kDistortion, kWaveShaper, kErosion, kResonator } },
        { "Filter and EQ", { kFilterFx, kEq, kMidSideEq, kDefuse } },
        { "Spectral", { kSpectralGate, kSpectralCompressor, kSpectralLock, kSpectralBalance } },
      };
      for (auto& category : result) {
        std::sort(category.effects.begin(), category.effects.end(), [](int a, int b) {
          return effectName(a).compareIgnoreCase(effectName(b)) < 0;
        });
      }
      return result;
    }();
    return categories;
  }

  // Effects offered outside any category, in alphabetical order.
  std::vector<int> uncategorizedEffects() {
    std::vector<int> result;
    for (int effect : alphabeticalEffects()) {
      bool categorized = false;
      for (const auto& category : effectCategories())
        categorized = categorized || std::find(category.effects.begin(), category.effects.end(), effect) != category.effects.end();
      if (!categorized)
        result.push_back(effect);
    }
    return result;
  }

  // Adds every effect to `menu`, grouped by category, with item id effect + 1
  // (or an action when `action` is given). `disabled` is shown greyed out.
  void addGroupedEffectItems(PopupMenu& menu, int disabled,
                             const std::function<void(int)>& action = nullptr) {
    auto add = [&](PopupMenu& target, int effect) {
      if (action)
        target.addItem(effectName(effect), effect != disabled, false, [action, effect] { action(effect); });
      else
        target.addItem(effect + 1, effectName(effect), effect != disabled);
    };
    for (const auto& category : effectCategories()) {
      PopupMenu submenu;
      for (int effect : category.effects)
        add(submenu, effect);
      menu.addSubMenu(category.name, submenu);
    }
    for (int effect : uncategorizedEffects())
      add(menu, effect);
  }

  String effectNameForParameterPrefix(const String& prefix) {
    if (prefix == "filter_fx")
      return "Filter";
    return readableId(prefix);
  }

  String effectSectionForPrefixedId(const String& id, const String& section_prefix) {
    const String rest = id.fromFirstOccurrenceOf(section_prefix, false, false);
    if (rest == "destination")
      return "Chain and routing";
    if (rest == "effect_chain_order" || rest == "post_effect_order" || rest.startsWith("effect_chain_slot_"))
      return "Chain and routing";
    if (rest.startsWith("chorus_")) return "Chorus";
    if (rest.startsWith("compressor_")) return "Compressor";
    if (rest.startsWith("convolution_reverb_")) return "Convolution Reverb";
    if (rest.startsWith("delay_")) return "Delay";
    if (rest.startsWith("dimension_expander_")) return "Dimension Expander";
    if (rest.startsWith("distortion_")) return "Distortion";
    if (rest.startsWith("eq_")) return "Equalizer";
    if (rest.startsWith("flanger_")) return "Flanger";
    if (rest.startsWith("filter_fx_")) return "Filter";
    if (rest.startsWith("frequency_shifter_")) return "Frequency Shifter";
    if (rest.startsWith("mid_side_eq_")) return "Mid Side EQ";
    if (rest.startsWith("scream_")) return "Scream";
    if (rest.startsWith("suppressor_")) return "Suppressor";
    if (rest.startsWith("defuse_")) return "Defuse";
    if (rest.startsWith("tape_")) return "Tape";
    if (rest.startsWith("trance_gate_")) return "Trance gate";
    if (rest.startsWith("tremolo_pan_")) return "Tremolo Pan";
    if (rest.startsWith("spectral_lock_")) return "Spectral Lock";
    if (rest.startsWith("resonator_")) return "Resonator";
    if (rest.startsWith("analoguechorus_")) return "Analogue Chorus";
    if (rest.startsWith("analogueflanger_")) return "Analogue Flanger";
    if (rest.startsWith("analogueecho_")) return "Analogue Echo";
    if (rest.startsWith("analoguerotary_")) return "Analogue Rotary";
    if (rest.startsWith("analoguespring_")) return "Analogue Spring";
    if (rest.startsWith("analoguewah_")) return "Analogue Wah";
    if (rest.startsWith("spectral_balance_")) return "Spectral Balance";
    if (rest.startsWith("glitch_delay_")) return "Glitch Delay";
    if (rest.startsWith("openwurli_amp_")) return "OpenWurli Amp";
    if (rest.startsWith("scanner_vibrato_")) return "Scanner Vibrato";
    if (rest.startsWith("spectral_compressor_")) return "Spectral Compressor";
    if (rest.startsWith("spectral_gate_")) return "Spectral Gate";
    if (rest.startsWith("erosion_")) return "Erosion";
    if (rest.startsWith("floaty_delay_")) return "Floaty Delay";
    if (rest.startsWith("limiter_")) return "Limiter";
    if (rest.startsWith("phase_shift_")) return "Phase Shift";
    if (rest.startsWith("phaser_")) return "Phaser";
    if (rest.startsWith("reverb_")) return "Reverb";
    if (rest.startsWith("utility_")) return "Utility";
    if (rest.startsWith("waveshaper_")) return "Waveshaper";
    return {};
  }

  String busEffectSectionForParameter(const String& id) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "bus_" + String(bus) + "_";
      if (!id.startsWith(prefix))
        continue;

      const String effect_section = effectSectionForPrefixedId(id, prefix);
      if (effect_section.isNotEmpty())
        return "Bus " + String(bus) + " - " + effect_section;
    }
    return {};
  }

  bool isEffectChainSection(const String& section) {
    if (section == kEffectsChainSection)
      return true;
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      if (section == "Bus " + String(bus) + " - Chain and routing")
        return true;
    }
    return false;
  }

  String effectChainPrefixForSection(const String& section) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "Bus " + String(bus) + " - ";
      if (section.startsWith(prefix))
        return "bus_" + String(bus) + "_";
    }
    return {};
  }

  int fxBusForSection(const String& section) {
    const String prefix = effectChainPrefixForSection(section);
    return prefix.isEmpty() ? 0 : prefix.fromFirstOccurrenceOf("bus_", false, false).getIntValue();
  }

  String fxBusNameForSection(const String& section) {
    const int bus = fxBusForSection(section);
    return bus == 0 ? "Main effects bus" : "Bus " + String(bus);
  }

  String accessibleSectionTitle(const String& section) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "Bus " + String(bus) + " - ";
      if (section.startsWith(prefix))
        return section.fromFirstOccurrenceOf(prefix, false, false);
    }
    return section;
  }

  String accessibleParameterTitle(const String& section, String name) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String section_prefix = "Bus " + String(bus) + " - ";
      const String name_prefix = "Bus " + String(bus) + " ";
      if (section.startsWith(section_prefix) && name.startsWith(name_prefix))
        return name.fromFirstOccurrenceOf(name_prefix, false, false);
    }
    return name;
  }

  String effectIdForSection(const String& section) {
    const String effect_section = accessibleSectionTitle(section);
    for (int i = 0; i < vital::constants::kNumEffects; ++i) {
      const String effect_id = strings::kEffectOrder[i];
      if (effectSectionForPrefixedId(effect_id + "_on", {}) == effect_section)
        return effect_id;
    }
    return {};
  }

  String effectPresetDisplayName(const String& effect_id) {
    return effectSectionForPrefixedId(effect_id + "_on", {});
  }

  String bankRelativePathForFile(const File& file, const File& source_folder, const String& folder_name) {
    StringArray parts;
    parts.addTokens(file.getRelativePathFrom(source_folder), File::getSeparatorString(), "");
    const int folder_index = indexOfPathPart(parts, folder_name);
    if (folder_index >= 0 && folder_index + 1 < parts.size()) {
      StringArray stripped;
      for (int i = folder_index + 1; i < parts.size(); ++i)
        stripped.add(parts[i]);
      return stripped.joinIntoString("/");
    }
    return parts.joinIntoString("/");
  }

  bool pathContainsFolderName(const File& file, const File& source_folder, const String& folder_name) {
    StringArray parts;
    parts.addTokens(file.getRelativePathFrom(source_folder), File::getSeparatorString(), "");
    return indexOfPathPart(parts, folder_name) >= 0;
  }

  // The arpeggiator (1-based) an "arp_N_" control belongs to, or 0.
  int arpeggiatorForId(const String& id) {
    for (int arp = 1; arp <= vital::kNumArpeggiators; ++arp) {
      if (id.startsWith("arp_" + String(arp) + "_"))
        return arp;
    }
    return 0;
  }

  String sectionForParameter(const String& id) {
    const String bus_section = busEffectSectionForParameter(id);
    if (bus_section.isNotEmpty())
      return bus_section;
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (id.startsWith(prefix) &&
          (id.endsWith("_key_zone_start") || id.endsWith("_key_zone_end") ||
           id.endsWith("_velocity_zone_start") || id.endsWith("_velocity_zone_end"))) {
        return "Zones - Oscillator " + String(osc);
      }
    }
    if (isRoutingParameter(id))
      return (id == "effect_chain_order" || id == "post_effect_order" || id.startsWith("effect_chain_slot_") ||
              id.endsWith("effect_chain_order") || id.endsWith("post_effect_order") ||
              id.contains("_effect_chain_slot_")) ?
             kEffectsChainSection : kSignalRoutingSection;
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      if (id.startsWith("osc_" + String(osc)))
        return "Oscillator " + String(osc);
    }
    if (id.startsWith("scream_")) return "Scream";
    if (id.startsWith("suppressor_")) return "Suppressor";
    if (id.startsWith("defuse_")) return "Defuse";
    if (id.startsWith("tape_")) return "Tape";
    if (id.startsWith("trance_gate_")) return "Trance gate";
    if (id.startsWith("tremolo_pan_")) return "Tremolo Pan";
    if (id.startsWith("spectral_lock_")) return "Spectral Lock";
    if (id.startsWith("resonator_")) return "Resonator";
    if (id.startsWith("analoguechorus_")) return "Analogue Chorus";
    if (id.startsWith("analogueflanger_")) return "Analogue Flanger";
    if (id.startsWith("analogueecho_")) return "Analogue Echo";
    if (id.startsWith("analoguerotary_")) return "Analogue Rotary";
    if (id.startsWith("analoguespring_")) return "Analogue Spring";
    if (id.startsWith("analoguewah_")) return "Analogue Wah";
    if (id.startsWith("spectral_balance_")) return "Spectral Balance";
    if (id.startsWith("glitch_delay_")) return "Glitch Delay";
    if (id.startsWith("openwurli_amp_")) return "OpenWurli Amp";
    if (id.startsWith("scanner_vibrato_")) return "Scanner Vibrato";
    if (id.startsWith("spectral_compressor_")) return "Spectral Compressor";
    if (id.startsWith("spectral_gate_")) return "Spectral Gate";
    if (const int arp = arpeggiatorForId(id); arp > 0) return "Arpeggiator " + String(arp);
    if (id.startsWith("filter_1")) return "Filter 1";
    if (id.startsWith("filter_2")) return "Filter 2";
    if (id.startsWith("env_")) {
      const String number = id.fromFirstOccurrenceOf("env_", false, false)
                              .upToFirstOccurrenceOf("_", false, false);
      const int envelope = number.getIntValue();
      if (envelope > 0)
        return "Envelope " + String(envelope);
      return "Envelope 1";
    }
    if (id.startsWith("lfo_")) {
      const String number = id.fromFirstOccurrenceOf("lfo_", false, false)
                              .upToFirstOccurrenceOf("_", false, false);
      const int lfo = number.getIntValue();
      if (lfo > 0)
        return "LFO " + String(lfo);
      return "LFO 1";
    }
    if (id.startsWith("random_")) {
      const String number = id.fromFirstOccurrenceOf("random_", false, false)
                              .upToFirstOccurrenceOf("_", false, false);
      const int random = number.getIntValue();
      if (random > 0)
        return "Random " + String(random);
      return "Random 1";
    }
    if (id.startsWith("modulation_")) return "Modulation routing";
    if (id.startsWith("chorus_")) return "Chorus";
    if (id.startsWith("compressor_")) return "Compressor";
    if (id.startsWith("convolution_reverb_")) return "Convolution Reverb";
    if (id.startsWith("delay_")) return "Delay";
    if (id.startsWith("dimension_expander_")) return "Dimension Expander";
    if (id.startsWith("erosion_")) return "Erosion";
    if (id.startsWith("floaty_delay_")) return "Floaty Delay";
    if (id.startsWith("distortion_")) return "Distortion";
    if (id.startsWith("eq_")) return "Equalizer";
    if (id.startsWith("flanger_")) return "Flanger";
    if (id.startsWith("filter_fx_")) return "Filter";
    if (id.startsWith("frequency_shifter_")) return "Frequency Shifter";
    if (id.startsWith("limiter_")) return "Limiter";
    if (id.startsWith("mid_side_eq_")) return "Mid Side EQ";
    if (id.startsWith("phase_shift_")) return "Phase Shift";
    if (id.startsWith("phaser_")) return "Phaser";
    if (id.startsWith("reverb_")) return "Reverb";
    if (id.startsWith("utility_")) return "Utility";
    if (id.startsWith("waveshaper_")) return "Waveshaper";
    if (id.startsWith("macro_")) return "Macros";
    if (id.startsWith("release_velocity_") || id.contains("voice") || id.startsWith("polyphony") || id.startsWith("portamento") ||
        id.startsWith("legato") || id.startsWith("zone_")) return "Voice and performance";
    return "Master and global";
  }

  String modulationDestinationLabelForId(const String& id) {
    if (vital::Parameters::isParameter(id.toStdString()))
      return vital::Parameters::getDisplayName(id.toStdString());
    return readableId(id);
  }

  String arpeggiatorControlTitle(const String& id) {
    const int arp = arpeggiatorForId(id);
    if (arp == 0)
      return {};
    const String suffix = id.fromFirstOccurrenceOf("arp_" + String(arp) + "_", false, false);

    if (suffix == "on") return "Switch";
    if (suffix == "time_mode") return "Time mode";
    if (suffix == "sync_rate") return "Sync rate";
    if (suffix == "free_rate") return "Free rate";
    if (suffix == "style") return "Style";
    if (suffix == "play_order") return "Play order";
    if (suffix == "gate") return "Gate";
    if (suffix == "random_gate") return "Random gate";
    if (suffix == "repeats") return "Repeats";
    if (suffix == "loop") return "Loop";
    if (suffix == "offset") return "Offset";
    if (suffix == "distance") return "Distance";
    if (suffix == "octave") return "Octave";
    if (suffix == "steps") return "Steps";
    if (suffix == "velocity") return "Velocity";
    if (suffix == "velocity_decay") return "Velocity decay";
    if (suffix == "random_velocity") return "Random velocity";
    if (suffix == "hold") return "Hold";
    if (suffix == "retrigger") return "Retrigger";
    if (suffix == "retrigger_division") return "Retrigger division";
    if (suffix == "channel") return "Output channel";
    // These rows sit in the arpeggiator's Destinations group, which says what they are.
    if (suffix.startsWith("destination_osc_"))
      return "Oscillator " + suffix.fromLastOccurrenceOf("_", false, false);
    return readableId(suffix);
  }

  bool isArpeggiatorDestination(const String& section, const String& id) {
    const int arp = arpeggiatorForId(id);
    return arp > 0 && section == "Arpeggiator " + String(arp) &&
           id.startsWith("arp_" + String(arp) + "_destination_");
  }

  String midiControlName(int midi_id) {
    switch (midi_id) {
      case 1:   return "mod wheel";
      case 2:   return "breath controller";
      case 4:   return "foot controller";
      case 5:   return "portamento time";
      case 7:   return "channel volume";
      case 8:   return "balance";
      case 10:  return "pan";
      case 11:  return "expression";
      case 64:  return "sustain pedal";
      case 65:  return "portamento";
      case 66:  return "sostenuto pedal";
      case 67:  return "soft pedal";
      case 71:  return "resonance";
      case 74:  return "cutoff";
      case 91:  return "reverb depth";
      case 93:  return "chorus depth";
      default:  return "CC " + String(midi_id);
    }
  }

  String modulationSourceLabelForId(const String& id) {
    if (const int slot = sourceSlotForId(id); slot >= 0 && id.endsWith("_playhead"))
      return "Oscillator " + String(slot + 1) + " playhead";
    if (const int slot = vital::source_types::slotForOutputSource(id.toStdString()); slot >= 0)
      return "Oscillator " + String(slot + 1);
    if (id.startsWith("lfo_") && id.fromFirstOccurrenceOf("lfo_", false, false).containsOnly("0123456789"))
      return "LFO " + id.fromFirstOccurrenceOf("lfo_", false, false);
    if (id.startsWith("env_") && id.fromFirstOccurrenceOf("env_", false, false).containsOnly("0123456789"))
      return "Envelope " + id.fromFirstOccurrenceOf("env_", false, false);
    if (id == "random") return "Note-on random";
    if (id == "mod_wheel") return "Mod wheel";
    if (id == "pitch_wheel") return "Pitch bend";
    if (id == "note_in_octave") return "Note in octave";
    if (id == "lift") return "Release velocity";
    if (id == "slide") return "MPE slide";
    if (id == "aftertouch") return "Aftertouch";
    if (id == "velocity") return "Velocity";
    if (id == "note") return "Note";
    if (id == "stereo") return "Stereo voice split";
    return readableId(id);
  }

  // The arpeggiator runs outside the voices and reads its modulation as one
  // value a block, and every step it plays is a new note. A source that a
  // note restarts or sets (an envelope, the note-on random, velocity, pitch,
  // pressure on the key, which never reaches an arp note anyway) cannot shape
  // its speed, so its rates and time mode take only sources that run on
  // their own or that a player moves by hand.
  bool sourceFitsArpeggiator(const String& source_id) {
    return source_id.startsWith("lfo_") || source_id.startsWith("random_") ||
           source_id.startsWith("macro_") || source_id == "mod_wheel" || source_id == "pitch_wheel";
  }

  bool isInvalidModulationPair(const String& source_id, const String& destination_id) {
    if (source_id.isEmpty() || destination_id.isEmpty())
      return false;

    if (arpeggiatorForId(destination_id) > 0 && !sourceFitsArpeggiator(source_id))
      return true;

    // An oscillator's audio can't modulate the oscillator itself (no self FM yet).
    if (const int slot = vital::source_types::slotForOutputSource(source_id.toStdString()); slot >= 0)
      return vital::source_types::slotForControl(destination_id.toStdString()) == slot;

    const StringArray self_modulating_prefixes { "lfo_", "env_", "random_" };
    for (const auto& prefix : self_modulating_prefixes) {
      if (!source_id.startsWith(prefix))
        continue;

      const String source_number = source_id.fromFirstOccurrenceOf(prefix, false, false)
                                       .upToFirstOccurrenceOf("_", false, false);
      if (source_number.isEmpty())
        continue;

      const String destination_prefix = prefix + source_number + "_";
      if (destination_id.startsWith(destination_prefix))
        return true;
    }
    return false;
  }

  int modulationSlotForParameterId(const String& id) {
    if (!id.startsWith("modulation_"))
      return -1;

    const String slot = id.fromFirstOccurrenceOf("modulation_", false, false)
                          .upToFirstOccurrenceOf("_", false, false);
    return slot.getIntValue();
  }

  int modulationParameterSuffixRank(const String& id) {
    static const std::array<const char*, 5> order = {{ "amount", "power", "bipolar", "stereo", "bypass" }};
    const String suffix = id.fromLastOccurrenceOf("_", false, false);
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
      if (suffix == order[i])
        return i;
    return static_cast<int>(order.size());
  }

  String descriptionForCommonSuffix(const String& suffix, const String& subject) {
    if (suffix == "on")
      return "Enable or mute " + subject + ".";
    if (suffix == "level")
      return "Set the output level of " + subject + ".";
    if (suffix == "pan")
      return "Place " + subject + " between the left and right channels.";
    if (suffix == "destination")
      return "Choose where " + subject + " is routed next.";
    if (suffix == "transpose")
      return "Transpose " + subject + " in semitones.";
    if (suffix == "tune")
      return "Fine tune " + subject + " in cents.";
    if (suffix == "keytrack")
      return "Control how much " + subject + " follows the played MIDI note.";
    if (suffix == "root_key")
      return "Set the note that plays the sample at its original pitch.";
    if (suffix == "low_cutoff")
      return "Remove low frequencies from " + subject + ".";
    if (suffix == "high_cutoff")
      return "Remove high frequencies from " + subject + ".";
    if (suffix == "key_zone_start")
      return "Set the lowest MIDI note that can trigger " + subject + ".";
    if (suffix == "key_zone_end")
      return "Set the highest MIDI note that can trigger " + subject + ".";
    if (suffix == "velocity_zone_start")
      return "Set the lowest velocity that can trigger " + subject + ".";
    if (suffix == "velocity_zone_end")
      return "Set the highest velocity that can trigger " + subject + ".";
    if (suffix == "transpose_quantize_key")
      return "Choose the root key used when pitch modulation is quantized.";
    if (suffix == "transpose_quantize_scale")
      return "Choose the scale used when pitch modulation is quantized.";
    if (suffix == "transpose_quantize_mode")
      return "Choose whether pitch quantize uses this source's scale or the global scale.";
    if (suffix == "transpose_quantize")
      return "Snap pitch modulation to the selected key and scale.";
    return {};
  }

  String glitchDelayHelpText(const String& suffix) {
    if (suffix == "on") return "Enable or bypass this effect.";
    if (suffix == "tap_view") return "Choose which tap's controls are shown. Every tap keeps playing whichever one is shown.";
    if (suffix == "sync") return "Follow the host tempo: tap times, the glitch buffer and Stutter slices become note values, and glitch chunks line up with the beat while the host plays. Bars are counted in 4/4.";
    if (suffix == "glide") return "Set how long a tap takes to move to a new time, up to 2 seconds. The move bends the pitch of the repeats, as changing a tape's speed would.";
    if (suffix == "freeze") return "Hold the repeats: new input fades out, every tap's feedback rises to 100 percent and the feedback filters are bypassed.";
    if (suffix == "freeze_sustain") return "While frozen, keep the loops audible when glitches wear them down: each tap saves a copy of its loop and blends some of it back as the processed audio loses level.";
    if (suffix == "placement") return "Choose where glitches act. Feedback path changes only what goes round again, so nothing is heard at zero feedback. Output and feedback also changes what you hear.";
    if (suffix == "buffer_size") return "Set the glitch chunk length, from 10 ms to 2 seconds. Glitches start on chunk boundaries and last a whole number of chunks.";
    if (suffix == "buffer_sync") return "Set the glitch chunk length as a note value while Host Sync is on.";
    if (suffix == "max_glitches") return "Set how many glitch types may run at once in each tap, from 1 to 4.";
    if (suffix == "length_min") return "Set the shortest glitch, in chunks. Each glitch lasts between the minimum and maximum.";
    if (suffix == "length_max") return "Set the longest glitch, in chunks. Each glitch lasts between the minimum and maximum.";
    if (suffix == "reproducible") return "Repeat the same glitches each time host playback starts or jumps, following the Seed. Off, every run is different.";
    if (suffix == "seed") return "Choose the pattern Reproducible Randomness repeats, from 0 to 9999.";
    if (suffix == "smear_amount") return "Blend in a diffused copy of the repeats. Zero percent leaves them untouched.";
    if (suffix == "smear_size") return "Set how far the smear spreads the repeats, from 10 to 500 milliseconds.";
    if (suffix == "stop_tape") return "Slow the repeats to a halt, dropping in pitch like a tape machine switched off. Turning it off brings them back up to speed. The dry signal is not affected, so raise Mix for the full effect.";
    if (suffix == "stop_time") return "Set how long the tape takes to slow from full speed to a stop, from 50 ms to 2 seconds.";
    if (suffix == "start_time") return "Set how long the tape takes to get back to full speed, from 50 ms to 2 seconds.";
    if (suffix == "mix") return "Blend dry audio with the repeats. Zero percent is dry.";
    if (suffix == "output_gain") return "Set the output level of dry and delayed audio, from minus 24 to plus 12 dB.";
    if (!suffix.startsWith("tap_"))
      return {};

    const String control = suffix.substring(4).fromFirstOccurrenceOf("_", false, false);
    const auto chance = [](const char* what) {
      return "Set the chance, at each chunk, that this tap " + String(what) + " Zero percent never does.";
    };
    if (control == "enabled") return "Switch this tap on or off. A tap switched off fades out and clears its delay line.";
    if (control == "time") return "Set this tap's delay, from 1 ms to 5 seconds. With Host Sync on, Synced Time takes its place.";
    if (control == "time_sync") return "Set this tap's delay as a note value while Host Sync is on, up to 4 bars.";
    if (control == "volume") return "Set this tap's level, from minus 60 dB, which is silent, to plus 6 dB.";
    if (control == "pan") return "Place this tap's repeats from left to right.";
    if (control == "feedback") return "Set how much of each repeat goes round the tap again.";
    if (control == "low_cut") return "Remove lows from the feedback path at 12 dB per octave, so each repeat is thinner than the last.";
    if (control == "high_cut") return "Remove highs from the feedback path at 12 dB per octave, so each repeat is darker than the last.";
    if (control == "reverse_prob") return chance("plays its audio backwards, a chunk at a time.");
    if (control == "stutter_prob") return chance("repeats a short slice of its audio.");
    if (control == "stutter_min") return "Set the shortest slice Stutter repeats, from 5 to 250 ms.";
    if (control == "stutter_max") return "Set the longest slice Stutter repeats, from 5 to 250 ms.";
    if (control == "stutter_sync_min") return "Set the shortest Stutter slice as a note value while Host Sync is on.";
    if (control == "stutter_sync_max") return "Set the longest Stutter slice as a note value while Host Sync is on.";
    if (control == "grain_prob") return chance("breaks its audio into overlapping grains from the last second.");
    if (control == "grain_size_min") return "Set the smallest grain, from 5 to 200 ms.";
    if (control == "grain_size_max") return "Set the largest grain, from 5 to 200 ms.";
    if (control == "grain_density_min") return "Set the fewest grains a second, from 1 to 100.";
    if (control == "grain_density_max") return "Set the most grains a second, from 1 to 100.";
    if (control == "pitch_prob") return chance("shifts its pitch. Pitch Mode chooses how.");
    if (control == "pitch_mode") return "Choose Sweep, which shifts the audio on every pass and turns back at the ends of the pitch range, or Varispeed, which changes the tap's speed like a tape machine. Varispeed suits frozen loops: it wears them down less and doesn't jump.";
    if (control == "pitch_min") return "Set the low end of the pitch range, from minus 24 to plus 24 semitones. A sweep turns back there; Varispeed picks its speed change from the range.";
    if (control == "pitch_max") return "Set the high end of the pitch range, from minus 24 to plus 24 semitones. A sweep turns back there; Varispeed picks its speed change from the range.";
    if (control == "pitch_speed_min") return "Set the slowest sweep, in semitones each pass through the tap. Varispeed ignores it.";
    if (control == "pitch_speed_max") return "Set the fastest sweep, in semitones each pass through the tap. Varispeed ignores it.";
    if (control == "lpc_prob") return chance("moves its formants with linear prediction, a coarse, vocoder-like sound. The pitch stays.");
    if (control == "lpc_min" || control == "ceps_min") return "Set the lowest place the formants are moved to, up to 12 semitones either way.";
    if (control == "lpc_max" || control == "ceps_max") return "Set the highest place the formants are moved to, up to 12 semitones either way.";
    if (control == "ceps_prob") return chance("moves its formants with a cepstral envelope, closer to the original timbre. The pitch stays.");
    if (control == "ring_prob") return chance("ring modulates its audio.");
    if (control == "ring_min") return "Set the lowest ring modulation frequency, from 1 Hz to 5 kHz.";
    if (control == "ring_max") return "Set the highest ring modulation frequency, from 1 Hz to 5 kHz.";
    if (control == "fm_prob") return chance("frequency modulates its audio, adding sidebands around the pitch it detects.");
    if (control == "fm_ratio_min") return "Set the lowest modulator frequency, as a ratio of the detected pitch from 0.25 to 16.";
    if (control == "fm_ratio_max") return "Set the highest modulator frequency, as a ratio of the detected pitch from 0.25 to 16.";
    if (control == "fm_index_min") return "Set the least frequency modulation depth, from 0 to 10.";
    if (control == "fm_index_max") return "Set the most frequency modulation depth, from 0 to 10.";
    if (control == "crush_prob") return chance("reduces its bit depth and sample rate.");
    if (control == "crush_bits_min") return "Set the lowest bit depth, from 1 to 16 bits.";
    if (control == "crush_bits_max") return "Set the highest bit depth, from 1 to 16 bits.";
    if (control == "crush_rate_min") return "Set the least sample rate reduction, from 1 to 64 times.";
    if (control == "crush_rate_max") return "Set the most sample rate reduction, from 1 to 64 times.";
    return {};
  }

  String accessibleParameterHelpText(const String& parameter_id, const String& accessible_name, bool toggle) {
    if (parameter_id.endsWith("spectral_gate_fft_size"))
      return "Choose 256, 512, 1024, 2048, 4096 or 8192 FFT samples. Larger sizes resolve finer spectral detail but add delay and processing cost.";

    if (parameter_id.isEmpty())
      return toggle ? "Enable or disable " + accessible_name + "." : "Adjust " + accessible_name + ".";

    const String id = parameter_id.toLowerCase();
    const String legacy_id = legacySourceId(id);
    auto suffixAfter = [&id](const String& prefix) {
      return id.fromFirstOccurrenceOf(prefix, false, false);
    };

    if (id == "volume")
      return "Set Atlas's final output level.";
    if (id == "bypass")
      return "Bypass the whole instrument output.";
    if (id == "polyphony")
      return "Set the maximum number of voices Atlas can play at once.";
    if (id == "legato")
      return "When enabled, overlapping mono notes glide without retriggering the amp envelope.";
    if (id == "portamento_time")
      return "Set how long pitch glide takes between notes.";
    if (id == "portamento_slope")
      return "Shape the curve of the portamento glide.";
    if (id == "portamento_force")
      return "Force portamento even when notes are not overlapping.";
    if (id == "portamento_glide_zones")
      return "When enabled, the glide crosses zone boundaries and each zone's pitch is clamped to its key range.";
    if (id == "zone_crossfade")
      return "Set how long zones fade in and out as a glide crosses their boundaries.";
    if (id == "voice_transpose")
      return "Transpose the whole synth in semitones.";
    if (id == "release_velocity_mode")
      return "Choose Instant to jump to the release target, or Glide to reach it over the glide time when each note enters release.";
    if (id == "release_velocity_target")
      return "Choose MIDI release velocity to scale the modulation by the velocity sent when a key is released, or Full amount to reach the entire assigned depth on every release, including zero-velocity note-offs. Applies to every Release velocity modulation route.";
    if (id == "release_velocity_glide_time")
      return "Set the release velocity glide duration from 0 to 10 seconds. Zero is instant. Each voice glides independently.";
    if (id == "release_velocity_glide_curve")
      return "Shape the release velocity glide. Zero is linear; negative values start faster and slow down; positive values start slowly and accelerate.";
    if (id == "voice_tune")
      return "Fine tune the whole synth in cents.";
    if (id == "voice_amplitude")
      return "Set the base voice amplitude before the final output stage.";
    if (id == "velocity_track")
      return "Control how much note velocity changes voice level.";
    if (id == "pitch_bend_range")
      return "Set the pitch bend wheel range in semitones.";
    if (id == "stereo_routing")
      return "Set how strongly the final signal is widened or rotated in stereo.";
    if (id == "stereo_mode")
      return "Choose Spread for width or Rotate for left-right rotation.";
    if (id == "mpe_enabled")
      return "Enable MPE input for per-note expression.";

    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (!id.startsWith(prefix))
        continue;
      const String suffix = suffixAfter(prefix);
      const String subject = "oscillator " + String(osc);
      if (const String common = descriptionForCommonSuffix(suffix, subject); common.isNotEmpty())
        return common;
      if (suffix == "wave_frame") return "Move through the frames of the oscillator wavetable.";
      if (suffix == "fixed_frequency_mode")
        return "Use the fixed frequency slider when MIDI tracking is off.";
      if (suffix == "fixed_frequency")
        return "Set the oscillator frequency in Hertz when fixed frequency mode is on and MIDI tracking is off.";
      if (suffix == "unison_voices") return "Set how many detuned copies this oscillator plays.";
      if (suffix == "unison_detune") return "Set the pitch spread between unison voices.";
      if (suffix == "unison_blend") return "Balance the main voice against the unison voices.";
      if (suffix == "stack_style") return "Choose how unison voices are pitch-stacked.";
      if (suffix == "detune_power") return "Shape how detune is distributed across unison voices.";
      if (suffix == "detune_range") return "Limit the pitch range used by unison detune.";
      if (suffix == "spectral_unison") return "Add extra spectral copies inside the oscillator warp engine.";
      if (suffix == "frame_spread") return "Spread unison voices across different wavetable frames.";
      if (suffix == "stereo_spread") return "Spread unison voices across the stereo field.";
      if (suffix == "phase") return "Set the oscillator start phase.";
      if (suffix == "random_phase") return "Randomize oscillator start phase on each triggered note.";
      if (suffix == "midi_track") return "Choose how strongly this oscillator follows MIDI note pitch.";
      if (suffix == "smooth_interpolation") return "Smooth movement between wavetable frames.";
      if (suffix == "secondary_distortion_type") return "Choose a second warp, FM or ring modulation mode after the primary distortion. None bypasses this stage.";
      if (suffix == "secondary_distortion_amount") return "Set the secondary distortion amount independently of the primary amount.";
      if (suffix == "secondary_spectral_morph_type") return "Choose a second frequency morph after the primary morph. None bypasses this stage.";
      if (suffix == "secondary_spectral_morph_amount") return "Set the secondary frequency morph amount independently of the primary amount.";
      if (suffix == "distortion_type") return "Choose the oscillator distortion or ring modulation mode.";
      if (suffix == "distortion_amount") return "Set how strongly the oscillator distortion is applied.";
      if (suffix == "distortion_spread") return "Offset oscillator distortion amount across stereo or unison voices.";
      if (suffix == "secondary_distortion_spread") return "Offset the secondary distortion amount across unison voices, as Distortion Spread does for the primary mode.";
      if (suffix == "secondary_spectral_morph_spread") return "Offset the secondary frequency morph amount across unison voices. Needs Spectral Unison, like Frequency Morph Spread.";
      if (suffix == "spectral_morph_type") return "Choose the oscillator frequency morph mode.";
      if (suffix == "spectral_morph_seed" || suffix == "secondary_spectral_morph_seed")
        return "Choose a repeatable Random Amplitudes pattern from 0 to 65535. Zero restores the original pattern. "
               "The seed is saved with the preset. Use the following Randomize button to try a new pattern.";
      if (suffix == "spectral_morph_amount") return "Set how strongly the frequency morph is applied.";
      if (suffix == "spectral_morph_spread") return "Offset frequency morph amount across stereo or unison voices.";
    }

    if (legacy_id.startsWith("sample_")) {
      const String suffix = legacy_id.fromFirstOccurrenceOf("sample_", false, false);
      if (const String common = descriptionForCommonSuffix(suffix, "the sample oscillator"); common.isNotEmpty())
        return common;
      if (suffix == "playback_mode") return "Choose whether notes retrigger the sample or only open the amp envelope.";
      if (suffix == "loop") return "Loop the selected sample range while notes are held.";
      if (suffix == "bounce") return "Play the sample loop forward then backward.";
      if (suffix == "random_phase") return "Randomize the sample start point on each triggered note.";
      if (suffix == "start") return "Set where sample playback begins.";
      if (suffix == "end") return "Set where sample playback stops.";
      if (suffix == "loop_start") return "Set the start point of the sample loop.";
      if (suffix == "loop_end") return "Set the end point of the sample loop.";
      if (suffix == "loop_crossfade") return "Crossfade the sample loop boundary to reduce clicks.";
    }

    if (legacy_id.startsWith("granular_")) {
      const String suffix = legacy_id.fromFirstOccurrenceOf("granular_", false, false);
      if (const String common = descriptionForCommonSuffix(suffix, "the granular oscillator"); common.isNotEmpty())
        return common;
      if (suffix == "mode") return "Choose play-through or manual position mode for grains.";
      if (suffix == "grain_count") return "Set how many grains can overlap at once.";
      if (suffix == "density") return "Set how often new grains are created.";
      if (suffix == "midi_density") return "Let MIDI note rate control grain density.";
      if (suffix == "grain_size") return "Set the length of each grain.";
      if (suffix == "speed") return "Set the grain playback speed through the sample.";
      if (suffix == "position") return "Choose the sample position used for new grains.";
      if (suffix == "position_mod") return "Set the automatic scan depth. At 100 percent it moves across the full selected region.";
      if (suffix == "position_mod_rate") return "Set the speed of automatic position movement.";
      if (suffix == "random_position") return "Spread grain start positions around the current position. At 100 percent grains can start anywhere in the selected region.";
      if (suffix == "random_volume") return "Vary each grain from full level down to 24 decibels quieter at 100 percent.";
      if (suffix == "random_pan") return "Spread each grain across the full stereo field at 100 percent.";
      if (suffix == "random_pitch") return "Randomize the pitch of each grain.";
      if (suffix == "interval") return "Set the pitch interval used by random pitch jumps.";
      if (suffix == "interval_chance") return "Set how often the granular pitch interval is applied.";
      if (suffix == "direction") return "Choose forward, reverse, or mixed grain direction.";
      if (suffix == "timing_mode") return "Choose free timing in grains per second, or spawn grains on the host tempo grid. Tempo grid timing replaces the density and MIDI density controls.";
      if (suffix == "grid_rate") return "Set the note value between grains when timing follows the tempo grid.";
      if (suffix == "timing_jitter") return "Delay each grain by a random amount, up to half the time between grains at 100 percent. Breaks up the buzz of perfectly regular grains.";
      if (suffix == "size_mode") return "Set grain length in milliseconds, or as a multiple of the tempo grid step.";
      if (suffix == "grid_length") return "Grain length in grid steps when size mode is Grid Steps. 1 fills one step; above 1 grains overlap.";
      if (suffix == "window_shape") return "Choose the grain envelope. Soft Plateau keeps more of each grain at full level for a clearer sound; Percussive gives sharp attacks.";
      if (suffix == "position_grid") return "Snap grain start positions to equal slices of the selected region.";
      if (suffix == "start") return "Set the start of the sample region used by the granular oscillator.";
      if (suffix == "end") return "Set the end of the sample region used by the granular oscillator.";
    }

    if (legacy_id.startsWith("braids_")) {
      const String suffix = legacy_id.fromFirstOccurrenceOf("braids_", false, false);
      if (suffix == "model")
        return "Choose one of the 48 synthesis modes of Mutable Instruments Braids. Timbre and Color take a new "
               "meaning with each mode, and their group is named after it.";
      if (suffix == "timbre") return "Set Timbre, the mode's first parameter. What it changes depends on the synthesis mode.";
      if (suffix == "color") return "Set Color, the mode's second parameter. What it changes depends on the synthesis mode.";
      if (suffix == "resolution") return "Reduce the bit depth, as Braids' BITS setting does. 16 bit is the clean signal.";
      if (suffix == "sample_rate") return "Hold samples to reduce the sample rate, as Braids' RATE setting does. 96 kHz is the clean signal.";
      if (suffix == "transpose") return "Transpose the Braids oscillator in semitones.";
      if (suffix == "tune") return "Fine tune the Braids oscillator in cents.";
      if (suffix == "random_phase")
        return "Randomize where the oscillator starts its cycle on each note that starts from silence. "
               "0% starts every note the same way.";
    }

    if (legacy_id.startsWith("b3_")) {
      const String suffix = legacy_id.fromFirstOccurrenceOf("b3_", false, false);
      if (suffix == "amp_env")
        return "Choose what shapes each note. Organ sounds while the key is down, with the key click as it closes "
               "and as it opens, and keeps the voice open while it sounds. Envelope 1 lets Envelope 1 "
               "shape the organ instead, sounding on through its release.";
      if (suffix.startsWith("drawbar_")) {
        const String footage = suffix.fromFirstOccurrenceOf("drawbar_", false, false);
        String harmonic;
        if (footage == "16") harmonic = "an octave below the note, the sub-octave";
        else if (footage == "5_1_3") harmonic = "a fifth above the note, the sub-third harmonic";
        else if (footage == "8") harmonic = "the note itself, the fundamental";
        else if (footage == "4") harmonic = "an octave above the note";
        else if (footage == "2_2_3") harmonic = "an octave and a fifth above the note, the third harmonic";
        else if (footage == "2") harmonic = "two octaves above the note";
        else if (footage == "1_3_5") harmonic = "two octaves and a major third above the note, the fifth harmonic";
        else if (footage == "1_1_3") harmonic = "two octaves and a fifth above the note, the sixth harmonic";
        else harmonic = "three octaves above the note";
        return "Pull out this drawbar to add the tonewheels " + harmonic + ". 0 is in, silent; 8 is fully out. "
               "Each step adds an eighth of the full level. With percussion on, the 1 foot drawbar is silent: its "
               "contact triggers the percussion, as on the organ.";
      }
      if (suffix == "percussion")
        return "Add the percussion, a decaying second or third harmonic on the attack. It is single-trigger: it "
               "sounds only on a key played with no other key held, so legato playing does not repeat it. With "
               "Normal volume the drawbars play a little quieter, as on the organ.";
      if (suffix == "percussion_volume")
        return "Choose Normal or Soft percussion. Soft is quieter and leaves the drawbars at full level.";
      if (suffix == "percussion_decay") return "Choose Fast or Slow percussion decay.";
      if (suffix == "percussion_harmonic")
        return "Choose which harmonic percussion sounds: Second, an octave above the note, or Third, an octave "
               "and a fifth above.";
      if (suffix == "key_click")
        return "Set the click of the key contacts as they close. 0% closes them cleanly, with no click; 50% is "
               "OpenB3's click, each contact closing at its own moment with a burst of noise; above that the "
               "contacts sound worn, chattering louder and for longer, up to about 10 milliseconds.";
      if (suffix == "release_click")
        return "Set the click of the key contacts as they open when a key is released. 0% lets the note fade out "
               "cleanly; 50% is setBfree's release click; above that the contacts chatter louder and for longer, "
               "up to about 10 milliseconds.";
      if (suffix == "temperament")
        return "Choose the tonewheels' tuning. Gear 60 Hz uses the gear ratios of an organ built for 60 hertz "
               "mains, as OpenB3 does; Gear 50 Hz estimates those of a 50 hertz organ; Equal is equal "
               "temperament.";
      if (suffix == "transpose")
        return "Transpose the organ in semitones. The nearest of the 61 keys, from C2 to C7, gives the wiring.";
      if (suffix == "tune") return "Fine tune the organ in cents.";
    }

    if (legacy_id.startsWith("speech_")) {
      const String suffix = legacy_id.fromFirstOccurrenceOf("speech_", false, false);
      if (suffix == "language")
        return "Choose the language espeak-ng reads the words in. It decides how they are pronounced and split "
               "into syllables.";
      if (suffix == "variant")
        return "Choose an espeak-ng voice variant, such as a different speaker or the Klatt synthesizer. Default "
               "is the language's own voice.";
      if (suffix == "amp_env")
        return "Choose what shapes each note. Voice lets each syllable shape its note: its vowel holds while the "
               "key is down, and after release the syllable sings on to its last consonant, keeping the voice "
               "open. Envelope 1 shapes the voice with Envelope 1 instead, so a short release can cut a final "
               "consonant.";
      if (suffix == "transpose")
        return "Transpose the voice in semitones. The voice keeps its formants, so it sings higher or lower "
               "without sounding smaller or larger.";
      if (suffix == "tune") return "Fine tune the voice in cents.";
      if (suffix == "mode")
        return "Choose how the words are voiced. Sung speaks them in a monotone and sings each note's pitch, "
               "holding the vowel while the key is down. Spoken keeps espeak-ng's own intonation and plays "
               "the words straight through: middle C speaks at the voice's own pitch, and other notes move "
               "the whole intonation up or down.";
      if (suffix == "split")
        return "Choose what each note takes from the words: the next syllable, the next word, or the whole "
               "text. Sung, the last syllable's vowel holds while the key is down.";
      if (suffix == "formant")
        return "Move the voice's formants up or down in semitones without changing its pitch. Up sounds "
               "smaller and brighter, down larger and darker. Choosing another variant sets it back to 0.";
      if (suffix == "breath")
        return "Add breath to the voice. 0% is the voice as espeak-ng spoke it; higher values blend in a "
               "whisper of the same words, and 100% whispers them. Choosing another variant sets it back "
               "to 0%.";
      if (suffix == "speed")
        return "Set how fast the consonants and the vowels that are not held play, from 0% to 400%. 100% is "
               "a little slower than espeak-ng speaks, which sings too fast; about 150% is its own speed. The pitch "
               "stays.";
      if (suffix == "vibrato")
        return "Set the depth of a vibrato that fades in after each note starts, in cents either side of the "
               "pitch. 0 cents is no vibrato.";
      if (suffix == "vibrato_rate") return "Set how fast the vibrato wavers, in hertz.";
    }

    if (legacy_id.startsWith("wurli_")) {
      const String suffix = legacy_id.fromFirstOccurrenceOf("wurli_", false, false);
      if (suffix == "amp_env")
        return "Choose what shapes each note. Reed lets the reed ring and decay as on the instrument, with the "
               "damper stopping it when the note is released, and keeps the voice open while it sounds. "
               "Envelope 1 shapes the reed with Envelope 1 instead.";
      if (suffix == "mlp")
        return "Apply OpenWurli's per-note corrections, learned from recordings of a real 200A, to the reed's "
               "upper partials. Off plays the physical model alone, as OpenWurli does by default.";
      if (suffix == "bark")
        return "Set how far the reeds swing into the pickup when struck. A deeper swing gives more of the 200A's "
               "bark, its growling upper harmonics, and is a few decibels louder, as a harder strike would be. "
               "50% is OpenWurli's calibration; 0% halves the swing and 100% doubles it, up to the pickup's limit "
               "in the bass.";
      if (suffix == "damping")
        return "Set the air drag on each reed, which makes a loud note lose energy faster just after the strike. "
               "100% is OpenWurli's estimate, 45% to 224% the physically plausible range, and 0% none.";
      if (suffix == "condition")
        return "Set how well the instrument is in tune. At 0% each reed is within 3 cents of pitch, as from the "
               "factory; at 100% within 14 cents, as on an unserviced vintage 200A. Each key keeps its own offset.";
      if (suffix == "treble_voicing")
        return "Raise or lower the keys above middle C, as a technician voices the reeds. The change grows from "
               "nothing at middle C to this many decibels at the top key, C7. 0 dB is OpenWurli's voicing.";
      if (suffix == "register_balance")
        return "Apply OpenWurli's level balance between registers, measured against recordings of a real 200A. "
               "Off plays the physical model's own levels.";
      if (suffix == "pickup_low_cut")
        return "Choose the pickup's low cut. The schematic leaves where capacitor C-2 returns unclear: to ground, "
               "as OpenWurli assumes, the cut is at 880 hertz; to the first transistor's emitter, it is at 1436 "
               "hertz, for a thinner, brighter tone.";
      if (suffix == "transpose")
        return "Transpose the reed in semitones. The nearest of the 64 keys, from A1 to C7, gives the timbre.";
      if (suffix == "tune") return "Fine tune the reed in cents.";
    }

    if (id.startsWith("filter_") || id.startsWith("filter_fx_") || id.contains("_filter_fx_")) {
      String suffix = id;
      if (id.startsWith("filter_1_")) suffix = suffixAfter("filter_1_");
      else if (id.startsWith("filter_2_")) suffix = suffixAfter("filter_2_");
      else if (id.startsWith("filter_fx_")) suffix = suffixAfter("filter_fx_");
      else if (id.contains("_filter_fx_")) suffix = id.fromFirstOccurrenceOf("_filter_fx_", false, false);

      if (suffix == "on") return "Enable or bypass this filter.";
      if (suffix == "mix") return "Blend between the dry signal and this filter.";
      if (suffix == "reverb_cutoff") return "Tune the reverb filter delay network, from 8 to 20000 Hz.";
      if (suffix == "reverb_damp") return "Dampen high frequencies in the reverb filter feedback.";
      if (suffix == "reverb_bass_preserve") return "Preserve dry bass with a smooth transition around 350 Hz. 0% keeps the original JSFX sound; 100% gives the strongest bass protection.";
      if (suffix == "reverb_output") return "Set the reverb filter output level after the dry/wet mix.";
      if (suffix == "scream_amount") return "Move the high-pass filter in Scream's nonlinear feedback path relative to Cutoff.";
      if (suffix == "scream_input_gain") return "Drive the Scream filter from minus 72 to plus 24 dB.";
      if (suffix == "scream_output_gain") return "Set Scream's output level after its mix and autogain.";
      if (suffix == "scream_autogain") return "Compensate Scream's output level. Env 1 controls the voice's release; there is no additional resonance tail.";
      if (suffix == "distortion_drive") return "Drive the Distortion model from minus 30 to plus 30 dB, as in the Distortion effect. For Bit Crush, more drive leaves fewer levels; for Down Sample, it lowers the sample rate.";
      if (suffix == "distortion_filter_order") return "Choose where the Distortion model's filter sits: None leaves it out, Pre filters before the distortion and Post after it. Cutoff, Resonance and Blend set the filter.";
      if (suffix == "model") return "Choose the filter model. Wavetable turns a wavetable frame's spectrum into the filter response: harmonic 24 sits at Cutoff, so a saw is flat and a frame's spectral shape becomes the curve. Distortion is the Distortion effect working on each note on its own, so a chord gains no extra tones between its notes.";
      if (suffix == "table_frame") return "Choose the frame of the filter's wavetable, 0 to 256; scanning frames morphs the response. Load any Atlas wavetable or sample from this filter's wavetable controls.";
      if (suffix == "style") return "Choose the filter style or slope inside the selected model. For the Wavetable model, choose the phase mode: Minimum is tight and adds no delay, Raw uses the frame waveform itself as the kernel, and in Filter FX Linear keeps every phase aligned and Original keeps the wavetable's phases, both adding 2048 samples of compensated delay. For Ring Mod, choose the carrier waveform: sine, square, saw or triangle. For Sample & Hold, choose Hold for steps, Smooth for ramps between held values, or Inverted, which leaves only the aliasing residue at 50% Mix. For Distortion, choose the type: Soft Clip, Hard Clip, Linear Fold, Sine Fold, Bit Crush or Down Sample.";
      if (suffix == "cutoff") return "Set the main filter cutoff frequency. For Ring Mod, set the carrier frequency that multiplies the input. For Sample & Hold, set the hold rate; with Key Track, aliasing follows the played note. For Distortion, set the cutoff of its filter.";
      if (suffix == "resonance") return "Boost frequencies around the cutoff point. For the Wavetable model, exaggerate or flatten the frame's peaks and troughs: 50 percent leaves its shape as it is, 100 percent raises it to the fourth power, 0 percent to the fourth root.";
      if (suffix == "drive") return "Drive the filter input for more saturation.";
      if (suffix == "blend") return "Morph between the available responses of the selected filter model. For Sample & Hold, reduce bit depth from off to 1 bit.";
      if (suffix == "blend_transpose") return "Tune the comb filter blend pitch.";
      if (suffix == "keytrack") return "Control how much filter cutoff follows the played note.";
      if (suffix == "formant_x") return "Move the formant filter along its X vowel axis.";
      if (suffix == "formant_y") return "Move the formant filter along its Y vowel axis.";
      if (suffix == "formant_transpose") return "Transpose the formant filter.";
      if (suffix == "formant_resonance") return "Control the sharpness of the formant peaks.";
      if (suffix == "formant_spread") return "Spread the formant peaks apart.";
      if (suffix.endsWith("_input")) return "Choose whether this source feeds the filter.";
      if (suffix == "destination") return "Choose where this filter output is routed. In serial mode, the last filter sets the chain's output destination.";
    }

    if (id.startsWith("env_")) {
      const String suffix = id.fromFirstOccurrenceOf("_", false, false).fromFirstOccurrenceOf("_", false, false);
      if (suffix == "delay") return "Wait before this envelope begins after a note starts.";
      if (suffix == "attack") return "Set how long the envelope takes to rise.";
      if (suffix == "hold") return "Hold the envelope at full level before decay begins.";
      if (suffix == "decay") return "Set how long the envelope takes to fall to sustain.";
      if (suffix == "sustain") return "Set the level held while the note is held.";
      if (suffix == "release") return "Set how long the envelope takes to fade after note release.";
      if (suffix == "attack_power" || suffix == "decay_power" || suffix == "release_power")
        return "Shape the curve of this envelope stage.";
      if (suffix == "analog")
        return "Behave like an analog envelope circuit. Attack charges toward a level above full and "
               "bends over at the top, decay and release fall away exponentially, and a new note carries "
               "on from the current level. Curve Power settings are kept but not used while this is on.";
      if (suffix == "analog_age")
        return "How worn the envelope's timing parts are. Each voice's attack, decay and release times "
               "differ and wander, the shortest times get sluggish, the attack peak drops, and sustain "
               "slowly leaks away while a note is held. At 0% the envelope is unworn.";
    }

    if (id.startsWith("lfo_")) {
      const String suffix = id.fromFirstOccurrenceOf("_", false, false).fromFirstOccurrenceOf("_", false, false);
      if (suffix == "frequency") return "Set the free-running LFO rate.";
      if (suffix == "tempo") return "Choose the tempo-synced LFO rate.";
      if (suffix == "sync") return "Choose seconds, tempo, dotted, triplet, or keytracked timing.";
      if (suffix == "sync_type") return "Choose whether the LFO follows host transport or retriggers freely.";
      if (suffix == "phase") return "Set where the LFO cycle starts.";
      if (suffix == "delay_time") return "Delay the LFO after note start.";
      if (suffix == "fade_time") return "Fade the LFO modulation in after note start, including when smoothing is enabled.";
      if (suffix == "smooth_mode") return "Turn LFO movement smoothing on or off. Fade In remains independent.";
      if (suffix == "smooth_time") return "Set the amount of smoothing applied to LFO movement.";
      if (suffix == "stereo") return "Offset the LFO phase between left and right channels.";
      if (suffix == "rate_x10") return "Multiply this LFO rate by ten.";
      if (suffix == "keytrack_transpose") return "Transpose the keytracked LFO rate.";
      if (suffix == "keytrack_tune") return "Fine tune the keytracked LFO rate.";
    }

    if (id.startsWith("random_")) {
      const String suffix = id.fromFirstOccurrenceOf("_", false, false).fromFirstOccurrenceOf("_", false, false);
      if (suffix == "style") return "Choose the random modulation shape.";
      if (suffix == "frequency") return "Set the free-running random modulation rate.";
      if (suffix == "rate_x10") return "Multiply this random modulation rate by ten.";
      if (suffix == "sync") return "Choose seconds, tempo, dotted, triplet, or keytracked timing.";
      if (suffix == "tempo") return "Choose the tempo-synced random modulation rate.";
      if (suffix == "sync_type") return "Choose whether random modulation follows host transport.";
      if (suffix == "stereo") return "Choose whether left and right channels share or split random values.";
      if (suffix == "keytrack_transpose") return "Transpose the keytracked random rate.";
      if (suffix == "keytrack_tune") return "Fine tune the keytracked random rate.";
    }

    if (id.startsWith("modulation_")) {
      if (id.endsWith("_amount")) return "Set the depth of this modulation route.";
      if (id.endsWith("_power")) return "Shape the curve of this modulation route.";
      if (id.endsWith("_bipolar")) return "Use the modulation source in both positive and negative directions.";
      if (id.endsWith("_stereo")) return "Invert this modulation amount between left and right channels.";
      if (id.endsWith("_bypass")) return "Temporarily disable this modulation route.";
    }

    if (id.startsWith("macro_control_"))
      return "Control this macro value for modulation assignments.";
    if (id.startsWith("macro_bipolar_"))
      return "Choose whether this macro ranges from zero to one hundred or minus one hundred to one hundred.";

    auto effectSuffix = [&id](const String& prefix) -> String {
      if (id.startsWith(prefix))
        return id.fromFirstOccurrenceOf(prefix, false, false);
      if (id.contains("_" + prefix))
        return id.fromFirstOccurrenceOf("_" + prefix, false, false);
      return {};
    };
    // Before Delay's: Glitch Delay's ids contain "_delay_" too.
    if (const String glitch_delay_suffix = effectSuffix("glitch_delay_"); glitch_delay_suffix.isNotEmpty()) {
      const String help = glitchDelayHelpText(glitch_delay_suffix);
      if (help.isNotEmpty())
        return help;
    }
    if (effectSuffix("delay_") == "style")
      return "Choose the delay style. Ping Pong L-R starts echoes on the left; Ping Pong R-L starts on the right.";
    const String suppressor_suffix = effectSuffix("suppressor_");
    if (suppressor_suffix == "amount") return "Set how much of a resonance is removed, from zero to 100 percent. At 100 percent a peak is pulled down to the spectral envelope around it, up to 24 dB.";
    if (suppressor_suffix == "smoothing") return "Spread each cut over neighbouring frequencies, up to a sixth of an octave each side, so narrow notches sound less surgical.";
    if (suppressor_suffix == "release") return "Set how slowly a cut lets go after the resonance stops, from one (about 20 ms) to eight (about half a second). Arrows change 0.05; Page keys 0.5.";
    if (suppressor_suffix == "frequency") return "Centre of the frequency window where suppression acts. Arrows change 10 Hz, Shift arrows 1 Hz, Page keys 100 Hz. Enter accepts Hz or kHz.";
    if (suppressor_suffix == "width") return "Set the window width, from zero to 1000: 62.5 per octave each side, never narrower than a quarter octave, and fully open above 312.5. Arrows change 10; Page keys 100.";
    if (suppressor_suffix == "slope") return "Shape the suppression region from zero to 100 percent. Higher values broaden the top and steepen its edges.";
    if (suppressor_suffix == "delta") return "Listen to what is being removed: the dry signal minus the processed one. Turn off to hear normal processing.";
    const String defuse_suffix = effectSuffix("defuse_");
    if (defuse_suffix == "stages") return "Number of Diopser all-pass filters in series, from zero to 512. Zero passes audio unchanged. Changing stages resets the filters. Arrows change one stage; Page keys ten.";
    if (defuse_suffix == "frequency") return "Center frequency from 20 to 20000 Hz. Spread is also limited to 20 Hz per stage. Arrows change 10 Hz, Shift arrows 1 Hz, Page keys 100 Hz. Uses Diopser's 100 ms logarithmic smoothing.";
    if (defuse_suffix == "resonance") return "All-pass filter Q from 0.01 to 30. Arrows change 0.01, Shift arrows 0.001, Page keys 0.1.";
    if (defuse_suffix == "spread") return "Spread filter frequencies from minus five to plus five octaves. Arrows change 0.1 octave, Shift arrows 0.01, Page keys one octave.";
    if (defuse_suffix == "spread_style") return "Choose Octaves or Linear frequency spacing. Changing the style resets the filters.";
    if (defuse_suffix == "automation_precision") return "Coefficient update precision. 100 percent updates each sample while smoothing; zero percent updates every 512 samples. Arrows change one percent, Page keys ten percent.";
    const String tape_suffix = effectSuffix("tape_");
    if (tape_suffix.isNotEmpty()) {
      if (tape_suffix == "noise_source") return "Choose Vinyl 1 through Vinyl 8 from Factory, Samples, Tape. Sounds load in the background and crossfade when changed.";
      if (tape_suffix == "noise_follow") return "Make noise follow the input level. Zero keeps it constant; 100 percent follows the signal with a 10 ms attack and 180 ms release.";
      if (tape_suffix == "noise_pitch") return "Change the recording playback pitch and speed from minus 24 to plus 24 semitones.";
      if (tape_suffix == "noise_tone") return "Darken or brighten the noise with Opus's high shelf at 1800 Hz.";
      if (tape_suffix == "noise_level") return "Set the noise level from minus 60 to zero dB. Noise Enabled turns the noise off completely.";
      if (tape_suffix == "drive_amount") return "Set Opus Tape Drive from 0 to 100%, with automatic level compensation. Arrows change one percent.";
      if (tape_suffix == "drive_tone") return "Set the Tape Drive tone filter from dark to bright.";
      if (tape_suffix == "drive_mix") return "Blend dry audio and tape distortion before the Drive output and the remaining Tape processing.";
      if (tape_suffix == "drive_output") return "Adjust the Tape Drive output from minus 12 to plus 6 dB.";
      if (tape_suffix == "wow") return "Set the slow tape pitch variation depth. Full depth uses Opus's 45-cent wow setting.";
      if (tape_suffix == "wow_rate") return "Set wow speed from 0.1 to 1.5 Hz. Arrows change 0.01 Hz.";
      if (tape_suffix == "flutter") return "Set the faster tape pitch variation depth. Full depth uses Opus's 32-cent flutter setting.";
      if (tape_suffix == "flutter_rate") return "Set flutter speed from 2 to 14 Hz. Arrows change 0.1 Hz.";
      if (tape_suffix == "dropouts") return "Set the depth of random tape volume dips. Zero disables dropouts.";
      if (tape_suffix == "dropout_density") return "Set how frequently tape dropouts occur. Zero disables dropouts.";
      if (tape_suffix == "noise_width") return "Set only the noise recording's width before it is mixed with the signal. Zero is mono, 100 percent preserves width, and 200 percent doubles the side signal.";
      if (tape_suffix == "global_width") return "Set the entire Tape output's width, including noise. Zero is mono, 100 percent preserves width, and 200 percent doubles the side signal.";
      if (tape_suffix.endsWith("_slope")) return "Choose a Butterworth slope of 12, 24, 36 or 48 dB per octave for this section's filter.";
      if (tape_suffix.endsWith("_cutoff")) return "Set this section's high-pass or low-pass cutoff in Hz. Arrows change 10 Hz; Shift arrows change 1 Hz. Noise filters affect only the noise; Global filters act on the final Tape output, including noise.";
      if (tape_suffix.endsWith("_enabled")) return "Turn this Tape section or filter on or off. Use the left and right arrows to choose Off or On.";
    }
    const String trance_suffix = effectSuffix("trance_gate_");
    if (trance_suffix == "mix") return "Blend dry audio with Trance gate. Zero percent is dry; 100 percent uses the full sequence. Arrows change one percent.";
    if (trance_suffix == "trigger") return "Playback follows the host song position and passes audio through when stopped. MIDI legato restarts on the first held key; overlapping notes and chords do not retrigger. Release all keys to rearm; the sequence continues through release tails.";
    if (trance_suffix == "grid") return "Set each step's duration from a whole note to 1/128, using BP StepGate's eight grid sizes.";
    if (trance_suffix == "start" || trance_suffix == "stop") return "Set the first and last of 32 steps. A start above the stop plays the selected range backwards.";
    if (trance_suffix == "attack" || trance_suffix == "decay") return "Set the global step envelope time from zero to 500 ms. Medium and Short close the shape at their gate boundary. Arrows change one millisecond.";
    if (trance_suffix == "sustain") return "Set the global sustain level applied to every step, from zero to 100 percent.";
    const String tremolo_suffix = effectSuffix("tremolo_pan_");
    if (tremolo_suffix == "sync") return "Choose Free rate in Hz, or host tempo sync with straight, dotted or triplet timing.";
    if (tremolo_suffix == "frequency") return "Set the modulation rate from 0.01 to 20 Hz in Free mode. Arrows change 0.1 Hz, Shift arrows 0.01 Hz, Page keys 1 Hz.";
    if (tremolo_suffix == "tempo") return "Set the cycle length in Sync, Dotted or Triplet mode. Freeze stops the cycle. Free mode keeps its own rate.";
    if (tremolo_suffix == "shape") return "Choose Sine, Triangle, Square, Ramp Up or Ramp Down for the volume modulation.";
    if (tremolo_suffix == "stereo_phase") return "Set the right channel phase relative to the left. Zero degrees gives tremolo; 180 degrees alternates left and right for autopan. Arrows change one degree.";
    if (tremolo_suffix == "offset") return "Shift the modulation cycle on both channels from zero to 360 degrees. Arrows change one degree.";
    if (tremolo_suffix == "depth") return "Set the volume modulation depth. Zero is unchanged audio; 100 percent allows full attenuation.";
    if (tremolo_suffix == "smoothing") return "Soften the modulation edges from zero to 20 ms, especially for Square and Ramp shapes. Arrows change 0.1 ms.";
    if (tremolo_suffix == "mix") return "Blend dry audio with Tremolo Pan. Zero percent is dry; 100 percent uses the full effect.";
    const String scream_suffix = effectSuffix("scream_");
    if (scream_suffix == "cutoff") return "Set the Scream filter cutoff from 20 Hz to 20 kHz using BP Scream's original frequency scale.";
    if (scream_suffix == "scream") return "Move the high-pass filter in the nonlinear feedback path relative to Cutoff.";
    if (scream_suffix == "resonance") return "Set feedback gain and feedback filter resonance.";
    if (scream_suffix == "input_gain") return "Set the filter input gain from minus 72 to plus 24 dB.";
    if (scream_suffix == "wet") return "Blend dry and filtered audio before BP Scream's autogain and output gain stages.";
    if (scream_suffix == "output_gain") return "Set final output gain from minus 24 to zero dB.";
    if (scream_suffix == "gate_release") return "Set the output gate release from 5 to 500 ms to control the resonance tail.";
    if (scream_suffix == "autogain") return "Enable BP Scream's automatic level compensation after the filter.";
    if (scream_suffix == "keytracking") return "Offset Cutoff relative to the last MIDI note. MIDI note 24 adds no offset; each octave transposes the filter by one octave.";
    const String lock_suffix = effectSuffix("spectral_lock_");
    if (lock_suffix == "amount") return "Set how far each band's pitch snaps to the scale: zero plays the bands at their own notes, 100 percent fully on the scale.";
    if (lock_suffix == "isolate") return "Gate the weaker bands relative to the loudest one, so only the strongest tones are re-played.";
    if (lock_suffix == "shimmer") return "Add phase-locked octave-up harmonics to the resynthesized tones.";
    if (lock_suffix == "bend") return "Bend resynthesized tones down or up by the Bend Range.";
    if (lock_suffix == "bend_range") return "Set the maximum pitch bend from one to 24 semitones.";
    if (lock_suffix == "range_high" || lock_suffix == "range_low") return "Set the filterbank range from 60 to 4800 Hz. The upper edge stays at least 20 Hz above the lower edge. Arrows change 10 Hz, Shift arrows one Hz, Page keys 100 Hz.";
    if (lock_suffix == "mute_high" || lock_suffix == "mute_low") return "Mute the dry audio above or below the range in the wet signal. Off lets it pass through unchanged.";
    if (lock_suffix == "spray") return "Detune each band by its own fixed random offset, up to a quarter tone, for a looser chorus of tones.";
    if (lock_suffix == "tilt") return "Darken or brighten the re-played tones, up to 6 dB per octave around the middle of the range.";
    if (lock_suffix == "pitch") return "Transpose the resynthesized tones from minus 24 to plus 24 semitones.";
    if (lock_suffix == "root") return "Choose the root note for the selected scale.";
    if (lock_suffix == "scale") return "Choose the scale the bands snap to: Chromatic, Major, Minor, Harmonic Minor, Dorian, Phrygian, Lydian, Mixolydian, Pentatonic Major, Pentatonic Minor, Whole Tone or Octaves.";
    if (lock_suffix == "mix") return "Blend the dry input with Spectral Lock. Zero percent is dry; 100 percent is fully processed.";
    if (lock_suffix == "amount") return "How strongly Spectral Lock acts, up to 200 percent. In Filterbank mode it balances the re-played tones against the dry mid band and above 100 percent boosts the tones; in Spectral mode it sets how far notes snap to the scale and above 100 percent boosts the retuned partials.";
    if (lock_suffix == "mode") return "Choose Filterbank, which re-plays the input on scale notes with its own tones and no latency, or Spectral, which retunes the material itself: every note and its harmonics move together to the nearest scale note, with about 170 ms of latency that Atlas compensates.";
    if (lock_suffix == "transients") return "Onset sensitivity in Spectral mode. On an onset every phase is reset to the input so attacks stay sharp; higher values treat smaller level jumps as onsets.";
    if (lock_suffix == "reach") return "How far a note may be moved in Spectral mode, from one to twelve semitones. A note whose nearest scale note is farther away is left alone.";
    if (lock_suffix == "width") return "Spread the re-played tones across the stereo field. Zero centres every band for a mono result; 100 percent is the full per-band placement.";
    if (lock_suffix == "follow") return "Hold the re-played tones at or below the dry signal's own envelope, so they stop when the input stops instead of ringing on. Zero leaves the tails as they are.";
    const String resonator_suffix = effectSuffix("resonator_");
    if (resonator_suffix.isNotEmpty()) {
      if (resonator_suffix == "model") return "Choose the resonating body: Beam, Marimba, String, Membrane, Plate, Pipe or Tube. Controls that do not apply to the chosen model are hidden.";
      if (resonator_suffix == "quality") return "Choose Eco with 8 modes, Normal with 20 or High with 40. More modes add upper partials and use more CPU. Pipe and Tube always use 40 modes.";
      if (resonator_suffix == "decay") return "Set how long the resonance takes to fall by 60 dB, from 20 ms to 30 seconds. Arrows move in even ratio steps.";
      if (resonator_suffix == "material") return "Shape how the upper modes decay, from soft, where they fade quickly, to metallic, where they ring longer.";
      if (resonator_suffix == "brightness") return "Set the level of the upper modes. Negative values darken; positive values brighten.";
      if (resonator_suffix == "inharmonics") return "Compress or stretch the spacing of the upper modes.";
      if (resonator_suffix == "ratio") return "Set the Membrane or Plate side ratio from 0.25 to 4, which changes the spacing of its modes. Arrows change 0.01.";
      if (resonator_suffix == "radius") return "Set the Pipe or Tube radius. Larger values ring longer with more upper partials.";
      if (resonator_suffix == "opening") return "Open the Pipe from closed, with odd harmonics only, to open, with every harmonic.";
      if (resonator_suffix == "hit") return "Set where the body is struck, from the center to the edge.";
      if (resonator_suffix == "listen_left") return "Set where the left channel picks up the vibration, from the center to the edge.";
      if (resonator_suffix == "listen_right") return "Set where the right channel picks up the vibration, from the center to the edge.";
      if (resonator_suffix == "midi_pitch") return "Follow the notes Atlas plays, including Atlas's pitch bend and Pitch Bend Range. When off, and until the first key, Tune sets the pitch.";
      if (resonator_suffix == "tune") return "Set the pitch from 20 to 5000 Hz when MIDI Pitch is off. Arrows change one semitone, Shift arrows 10 cents, Page keys 10 semitones. Enter accepts Hz or kHz.";
      if (resonator_suffix == "transpose") return "Transpose the played notes from minus 48 to plus 48 semitones when MIDI Pitch is on.";
      if (resonator_suffix == "fine") return "Fine tune the resonator from minus 100 to plus 100 cents, with or without MIDI Pitch.";
      if (resonator_suffix == "spread") return "Detune the left and right channels apart by this many cents in total. Negative values reverse the direction.";
      if (resonator_suffix == "glide") return "Set how long the pitch glides to a new note or Tune value, up to 500 ms. Glide does not slow the LFO or Spread. Arrows change one millisecond.";
      if (resonator_suffix == "note_priority") return "Choose which held key sets the pitch when MIDI Pitch is on: the Last key pressed or the Lowest.";
      if (resonator_suffix == "off_decay") return "When MIDI Pitch is on, shorten the resonance and close its input once every key is released. Zero ignores key releases.";
      if (resonator_suffix == "lfo_amount") return "Set the LFO pitch modulation depth, up to 24 semitones either way. Arrows change 0.1 semitone.";
      if (resonator_suffix == "lfo_shape") return "Choose Sine, Square, Triangle, Saw Up, Saw Down, Random Stepped or Random Smooth for the pitch LFO.";
      if (resonator_suffix == "lfo_sync") return "Choose Free rate in Hz, or host tempo sync with straight, dotted or triplet timing.";
      if (resonator_suffix == "lfo_frequency") return "Set the LFO rate from 0.01 to 20 Hz in Free mode.";
      if (resonator_suffix == "lfo_tempo") return "Set the LFO cycle length in Sync, Dotted or Triplet mode. Freeze stops the cycle. Free mode keeps its own rate.";
      if (resonator_suffix == "lfo_stereo_phase") return "Set the right channel's LFO phase relative to the left. Zero degrees moves both channels together. Arrows change one degree.";
      if (resonator_suffix == "lfo_spin") return "Run the right channel's LFO faster or slower than the left, by up to half an octave. Applies in Free mode only.";
      if (resonator_suffix == "lfo_offset") return "Shift the LFO cycle on both channels from zero to 360 degrees. Arrows change one degree.";
      if (resonator_suffix == "filter") return "Band-pass filter the resonated signal.";
      if (resonator_suffix == "filter_frequency") return "Set the band-pass center frequency from 20 Hz to 18 kHz. Enter accepts Hz or kHz.";
      if (resonator_suffix == "filter_bandwidth") return "Set the band-pass width from 0.1 to 6 octaves. Arrows change 0.1 octave.";
      if (resonator_suffix == "bleed") return "Blend unprocessed input into the resonated signal, restoring high frequencies.";
      if (resonator_suffix == "width") return "Set the stereo width of the resonated signal. Zero percent is mono.";
      if (resonator_suffix == "excitation") return "Set the level driving the resonator, from minus 24 to plus 24 dB. Arrows change 0.1 dB.";
      if (resonator_suffix == "wet_gain") return "Set the resonated signal level, from minus 24 to plus 24 dB. Arrows change 0.1 dB.";
      if (resonator_suffix == "limiter") return "Keep peaks of the resonated signal below 0.98, about minus 0.2 dB. The dry signal is never limited.";
      if (resonator_suffix == "output_gain") return "Set the output level of dry and resonated audio, from minus 24 to plus 12 dB. Arrows change 0.1 dB.";
      if (resonator_suffix == "mix") return "Blend dry audio with the resonator. Mix scales the excitation, so ringing tails continue when it changes. Zero percent is dry.";
    }
    const String analogue_chorus_suffix = effectSuffix("analoguechorus_");
    if (analogue_chorus_suffix.isNotEmpty()) {
      if (analogue_chorus_suffix == "model") return "Choose the bucket-brigade chorus: Juno, Dimension, CE or Ensemble. Every model sums the input to mono for its chorus; the dry signal stays stereo.";
      if (analogue_chorus_suffix == "juno_mode") return "Choose Juno chorus I, II, or I and II together, as on the original buttons.";
      if (analogue_chorus_suffix == "dimension_mode") return "Choose Dimension button 1, 2, 3 or 4, from subtle to strongest.";
      if (analogue_chorus_suffix == "rate") return "Scale the model's LFO speed. 100 percent is the original; zero stops the sweep.";
      if (analogue_chorus_suffix == "depth") return "Scale the model's delay sweep. 100 percent is the original; zero holds the delay at its center.";
      if (analogue_chorus_suffix == "noise") return "Set the bucket-brigade hiss and clock noise. 50 percent is the original level; zero is silent.";
      if (analogue_chorus_suffix == "width") return "Set the stereo width of the chorus signal. Zero percent is mono; 100 percent is the original spread.";
      if (analogue_chorus_suffix == "mix") return "Blend dry audio with the chorus. 50 percent is the original balance.";
    }
    const String analogue_flanger_suffix = effectSuffix("analogueflanger_");
    if (analogue_flanger_suffix.isNotEmpty()) {
      if (analogue_flanger_suffix == "medium") return "Choose BBD, a bucket-brigade flanger with regeneration, or Tape, two-machine flanging that sweeps through the zero point.";
      if (analogue_flanger_suffix == "sync") return "Choose Free rate in Hz, or host tempo sync with straight, dotted or triplet timing.";
      if (analogue_flanger_suffix == "frequency") return "Set the sweep rate from 0.01 to 10 Hz in Free mode.";
      if (analogue_flanger_suffix == "tempo") return "Set the sweep cycle length in Sync, Dotted or Triplet mode. Freeze stops the sweep. Free mode keeps its own rate.";
      if (analogue_flanger_suffix == "depth") return "Set the sweep width. Zero percent stops the sweep so Manual sets a fixed comb.";
      if (analogue_flanger_suffix == "manual") return "Set the sweep centre. In Tape mode, 50 percent centres the sweep on the zero point.";
      if (analogue_flanger_suffix == "feedback") return "Set the regeneration. Positive values sound resonant, negative values hollow. In Tape mode negative values invert the moving machine so the zero point cancels.";
      if (analogue_flanger_suffix == "stereo_phase") return "Offset the right channel's sweep from the left, from zero to 180 degrees.";
      if (analogue_flanger_suffix == "noise") return "Set the bucket-brigade or tape hiss. 50 percent is the original level; zero is silent.";
      if (analogue_flanger_suffix == "age") return "Wear the machine: more noise and clock bleed for BBD, duller heads and transport wobble for Tape.";
      if (analogue_flanger_suffix == "mix") return "Blend dry audio with the flanger. 50 percent gives the deepest comb. In Tape mode, 100 percent is the classic two-machine sound.";
    }
    const String analogue_echo_suffix = effectSuffix("analogueecho_");
    if (analogue_echo_suffix.isNotEmpty()) {
      if (analogue_echo_suffix == "medium") return "Choose Tape, a three-head tape echo, or BBD, a bucket-brigade echo with compander. Controls that do not apply to the chosen medium are hidden.";
      if (analogue_echo_suffix == "sync") return "Choose Free time in seconds, or host tempo sync with straight, dotted or triplet timing. Synced tape heads sit at exactly two and three times head 1.";
      if (analogue_echo_suffix == "frequency") return "Set head 1's delay, or the BBD delay, in Free mode: 20 ms to 1 second for Tape, 10 ms to 1.2 seconds for BBD. Longer times run the tape or clock slower, so the repeats are darker.";
      if (analogue_echo_suffix == "tempo") return "Set head 1's delay, or the BBD delay, as a note value in Sync, Dotted or Triplet mode.";
      if (analogue_echo_suffix.startsWith("head_") && analogue_echo_suffix.endsWith("_pan")) return "Place this head's repeats from left to right.";
      if (analogue_echo_suffix.startsWith("head_")) return "Switch this playback head on or off. Feedback comes from the furthest active head.";
      if (analogue_echo_suffix == "width") return "Spread the BBD echo: the two sides' clocks wobble independently. Zero percent is mono.";
      if (analogue_echo_suffix == "feedback") return "Set how much of the furthest head's repeat is recorded again. Above about 100 percent the echo builds into self-oscillation, held back by the tape or chip saturation.";
      if (analogue_echo_suffix == "inertia") return "Set how slowly the motor or clock follows a new time. Every change bends the pitch of the repeats. 50 percent matches a Space Echo motor; zero changes speed instantly.";
      if (analogue_echo_suffix == "drive") return "Push the signal harder onto the tape or chip, with the Tape effect's saturation inside the feedback loop. Quiet repeats keep their level.";
      if (analogue_echo_suffix == "bass") return "Shelve the repeats' low end by up to 10 dB either way, inside the feedback loop, so each repeat changes further.";
      if (analogue_echo_suffix == "treble") return "Shelve the repeats' high end by up to 10 dB either way, inside the feedback loop, so each repeat changes further.";
      if (analogue_echo_suffix == "wow") return "Set the slow transport wobble of the tape, or the slow clock wobble of the BBD.";
      if (analogue_echo_suffix == "flutter") return "Set the fast transport wobble of the tape, or the fast clock wobble of the BBD.";
      if (analogue_echo_suffix == "noise") return "Set the tape hiss, or the BBD hiss and clock whine. 50 percent is the original level; zero is silent.";
      if (analogue_echo_suffix == "age") return "Wear the machine: worn heads, dropouts and uneven tape contact for Tape; compander breathing, clock bleed and a fizzier top for BBD.";
      if (analogue_echo_suffix == "mix") return "Blend dry audio with the repeats. Zero percent is dry.";
    }
    const String analogue_rotary_suffix = effectSuffix("analoguerotary_");
    if (analogue_rotary_suffix.isNotEmpty()) {
      if (analogue_rotary_suffix == "model") return "Choose Organ 122, a spinning treble horn and bass drum behind a tube amplifier; Guitar 16, one speaker in a spinning drum; or OpenB3 Whirl, the rotary speaker of the OpenB3 organ, a horn and drum heard along a direct path and two reflections. The Whirl is at its original sound with Spread at 100 percent.";
      if (analogue_rotary_suffix == "speed") return "Choose Slow, Fast or Stop, like the half-moon switch. The rotors speed up and slow down with their own weight: the horn quickly, the drum slowly. Modulation can switch it, for example from the mod wheel.";
      if (analogue_rotary_suffix == "inertia") return "Scale how long the rotors take to change speed. 50 percent matches the original cabinet; zero changes speed at once.";
      if (analogue_rotary_suffix == "drive") return "Drive the cabinet's tube amplifier harder for growl. Quiet passages keep their level.";
      if (analogue_rotary_suffix == "balance") return "Balance the treble horn against the bass drum. Zero is the original balance.";
      if (analogue_rotary_suffix == "spread") return "Set the angle between the two microphones. Zero is mono; 100 percent places them on opposite sides of the cabinet, where OpenB3 Whirl has them.";
      if (analogue_rotary_suffix == "mix") return "Blend dry audio with the cabinet. 100 percent is the cabinet alone, as when an organ plays through it.";
    }
    const String wurli_amp_suffix = effectSuffix("openwurli_amp_");
    if (wurli_amp_suffix.isNotEmpty()) {
      if (wurli_amp_suffix == "volume")
        return "Set the 200A's volume pot, between the preamp and the power amplifier. As on the instrument, "
               "full volume with loud chords drives the amplifier into clipping. The oscillator's level sets "
               "how hard the reeds drive the preamp; its default level matches the real pickup.";
      if (wurli_amp_suffix == "tremolo_depth")
        return "Set the depth of the built-in tremolo. Its oscillator runs at about 5.6 hertz, and it works "
               "inside the preamp's feedback loop, so it changes the tone as well as the level. Zero is off.";
      if (wurli_amp_suffix == "speaker")
        return "Blend in the response of the 200A's small speakers and cabinet. Zero is flat, as a direct "
               "output; 100 percent is the full cabinet.";
    }
    const String scanner_suffix = effectSuffix("scanner_vibrato_");
    if (scanner_suffix.isNotEmpty()) {
      if (scanner_suffix == "mode")
        return "Choose the tonewheel organ's vibrato and chorus knob. V1, V2 and V3 bend the pitch a little, more "
               "and most; C1, C2 and C3 mix that vibrato with the dry sound for the organ's chorus. C3 is the "
               "classic setting. Modulation can switch it.";
      if (scanner_suffix == "rate")
        return "Set how fast the scanner turns. 7.25 hertz is OpenB3's; a real organ's motor runs near 7.";
    }
    const String analogue_spring_suffix = effectSuffix("analoguespring_");
    if (analogue_spring_suffix.isNotEmpty()) {
      if (analogue_spring_suffix == "tank") return "Choose a Short, Medium or Long spring tank. Longer tanks ring longer.";
      if (analogue_spring_suffix == "dwell") return "Set how hard the tube driver hits the springs. Higher Dwell gives more drip and splash.";
      if (analogue_spring_suffix == "tone") return "Darken or brighten the reverb, as the tone control of an outboard tube reverb.";
      if (analogue_spring_suffix == "mono") return "A real tank has one pickup, so the reverb is mono and Pan places it. Turn Mono off for two tanks in stereo; Pan then balances them.";
      if (analogue_spring_suffix == "pan") return "Place the reverb from left to right, or balance the two tanks when Mono is off. The dry signal is never panned.";
      if (analogue_spring_suffix == "noise") return "Set the tube and tank hiss. 50 percent is the original level; zero is silent.";
      if (analogue_spring_suffix == "mix") return "Blend dry audio with the reverb. Zero percent is dry.";
    }
    const String analogue_wah_suffix = effectSuffix("analoguewah_");
    if (analogue_wah_suffix.isNotEmpty()) {
      if (analogue_wah_suffix == "model") return "Choose Cry Baby, the classic vocal wah pedal, V846, the same circuit voiced rounder and smoother, or Mu-Tron III, an envelope filter tuned by light.";
      if (analogue_wah_suffix == "position") return "Rock the pedal from heel to toe; for the Mu-Tron III, set where the filter rests. Modulate it from an LFO, an envelope or the mod wheel to play the wah.";
      if (analogue_wah_suffix == "envelope") return "Let the input's level move the wah. Positive opens it from Position as you play harder; negative closes it. Zero leaves the wah where Position puts it.";
      if (analogue_wah_suffix == "decay") return "Set how slowly the envelope falls back after each note. 50 percent matches the Mu-Tron III.";
      if (analogue_wah_suffix == "mode") return "Choose the Mu-Tron III's low pass, band pass or high pass output.";
      if (analogue_wah_suffix == "range") return "Choose the Mu-Tron III's low or high frequency range.";
      if (analogue_wah_suffix == "peak") return "Set the Mu-Tron III's resonance: higher is sharper and more vocal.";
      if (analogue_wah_suffix == "mix") return "Blend dry audio with the wah. 100 percent is the wah alone, as with a pedal.";
    }
    const String balance_suffix = effectSuffix("spectral_balance_");
    if (balance_suffix.isNotEmpty()) {
      if (balance_suffix == "amount") return "Set how far the spectrum is moved toward the target balance. Resonances and boomy notes are cut by up to 12 dB and weak regions lifted by up to 6 dB at 100 percent; 25 percent is a gentle clean-up.";
      if (balance_suffix == "target") return "Choose the balance the broad layer levels toward: Perceptual, the long-term balance of finished music; Flat, equal energy per hertz; or a straight tilt of minus 1.5, 3, 4.5 or 6 dB per octave. Minus 3 is pink noise, minus 4.5 a typical finished mix.";
      if (balance_suffix == "bias") return "Tilt the target darker or brighter, in dB per octave around 1 kHz. Negative favours the lows, positive the highs.";
      if (balance_suffix == "speed") return "Scale how quickly the balancer reacts. 50 percent is 25 milliseconds to catch a resonance, 250 to let it go, and one second for the overall tone; each end is four times faster or slower.";
      if (balance_suffix == "range_low") return "Lowest frequency the balancer changes; everything below is left alone with a half-octave fade. Enter accepts a value in Hz.";
      if (balance_suffix == "range_high") return "Highest frequency the balancer changes; everything above is left alone with a half-octave fade. Enter accepts a value in Hz.";
      if (balance_suffix == "tame_recover") return "Balance taming against recovering. Negative values only lift weak or masked regions, positive values only cut peaks, and zero does both equally.";
      if (balance_suffix == "mix") return "Blend the aligned dry and balanced signals. Both include the spectral processing delay of 2048 samples.";
    }
    const String compressor_suffix = effectSuffix("spectral_compressor_");
    if (compressor_suffix == "compression") return "Move Downward and Upward together. Either amount can then be adjusted independently.";
    if (compressor_suffix == "downward") return "Pull bands above the target curve down toward it: 100 percent holds them at the curve, 50 percent halves the excess.";
    if (compressor_suffix == "upward") return "Lift bands below the target curve toward it, by up to 24 dB; silence is never lifted.";
    if (compressor_suffix == "target") return "Set the target level in dBFS on the band scale: white noise at that level reads it in every band, while a single tone reads well above its own level and is compressed harder. Lower targets squash more.";
    if (compressor_suffix == "attack") return "Set how quickly each band's detector rises, from instant to 100 ms. At zero the effect clips spectrally.";
    if (compressor_suffix == "release") return "Set how quickly each band's detector falls, from instant to one second. With attack at zero and some release it limits spectrally.";
    if (compressor_suffix == "smooth") return "Average the gain curve across neighbouring bands, up to a third of an octave at 100 percent, for a less surgical result. Low settings stay almost per-band.";
    if (compressor_suffix == "curve") return "Choose the target curve shape: Flat is the same at every frequency, Smooth allows louder lows and highs, OTT dips the mids so they are compressed hardest.";
    if (compressor_suffix == "tilt") return "Tilt the target curve by minus six to zero dB per octave around 1 kHz.";
    if (compressor_suffix == "makeup") return "Set the wet signal makeup gain in dB.";
    if (compressor_suffix == "mix") return "Blend dry and compressed signals. The dry signal is delayed to stay aligned with the processing.";
    const String gate_suffix = effectSuffix("spectral_gate_");
    const String dimension_suffix = effectSuffix("dimension_expander_");
    if (dimension_suffix == "delay") return "Set the Mode 1 stereo delay in milliseconds, from zero to forty.";
    if (dimension_suffix == "mode") return "Choose Mode 1 or Mode 2. Each mode retains its own settings.";
    if (dimension_suffix == "quorax_size") return "Set the Mode 2 delay tap spacing.";
    if (dimension_suffix == "quorax_mix") return "Add the Mode 2 stereo side signal. The direct signal stays at full level, even at one hundred percent.";
    if (dimension_suffix == "quorax_output") return "Set the output gain of Mode 2.";
    if (dimension_suffix == "quorax_motion") return "Set the rate of the Mode 2 tap amplitude rotation. Delay times stay fixed.";
    if (dimension_suffix == "quorax_low_cut") return "Filter low frequencies from the generated side signal only. Zero bypasses the filter.";
    if (dimension_suffix == "quorax_high_cut") return "Filter high frequencies from the generated side signal only. Twenty thousand Hz bypasses the filter.";
    if (dimension_suffix == "quorax_slope") return "Choose the Mode 2 wet filter slope in dB per octave.";
    const String erosion_suffix = effectSuffix("erosion_");
    if (erosion_suffix == "mode") return "Choose filtered Noise or Sine delay modulation.";
    if (erosion_suffix == "frequency") return "Set the noise band center or sine modulator frequency in Hz.";
    if (erosion_suffix == "width") return "Set the noise band bandwidth in octaves. Sine mode ignores Width.";
    if (erosion_suffix == "amount") return "Set delay modulation depth. Zero retains the original sixty-four-sample center delay.";
    if (erosion_suffix == "stereo") return "Blend independent noise into the right modulator. Zero uses the left noise on both channels. Sine mode is always mono.";
    const String floaty_suffix = effectSuffix("floaty_delay_");
    if (floaty_suffix == "time") return "Set the Floaty delay in milliseconds. Tempo Sync overrides this value when enabled.";
    if (floaty_suffix == "warp_rate") return "Set the playback speed modulation rate. Changing this restarts the delay heads, as in the JSFX.";
    if (floaty_suffix == "warp_amount") return "Set playback speed modulation depth. Motion starts after the first forward buffer cycle.";
    if (floaty_suffix == "cutoff") return "Set Floaty's original filter cutoff percentage. Zero bypasses both filters.";
    if (floaty_suffix == "resonance") return "Set Floaty's original filter resonance percentage. Zero bypasses both filters.";
    if (floaty_suffix == "playback_rate") return "Set delay playback speed. Negative values reverse playback and zero stops the read head.";
    if (floaty_suffix == "sync") return "Choose the synced delay duration, such as 1/4, 1/8 or 1/4d. d means dotted. Free uses the Time control. Arrows move one thirty-second note and Page keys move ten. Other fractions retain Floaty's original delay lengths.";
    if (floaty_suffix == "stereo_offset") return "Set the right recording head offset as a percentage of the left head position.";
    if (floaty_suffix == "clamp") return "Set the amplitude clamp before Floaty's feedback filters, using the original JSFX gain curve.";
    if (gate_suffix == "cutoff")
      return "Set the magnitude cutoff separating strong and weak frequency components. Higher values keep fewer strong components.";
    if (gate_suffix == "balance")
      return "Balance weak and strong frequency components. Zero percent keeps only weak components; one hundred percent keeps only strong components.";
    if (gate_suffix == "tilt_enabled")
      return "Enable the frequency tilt of the spectral gate cutoff.";
    if (gate_suffix == "tilt")
      return "Tilt the cutoff across frequencies. Zero is flat; negative values favor high frequencies and positive values favor low frequencies. Enable Tilt must be on.";
    if (gate_suffix == "mix")
      return "Blend the aligned dry and gated signals. Both include the spectral processing delay of 2048 samples.";
    String effect_suffix;
    const String effect_prefixes[] {
      "glitch_delay_", "chorus_", "compressor_", "convolution_reverb_", "delay_", "dimension_expander_", "distortion_",
      "eq_", "flanger_", "frequency_shifter_", "limiter_", "mid_side_eq_", "phase_shift_", "phaser_",
      "reverb_", "utility_", "waveshaper_", "spectral_gate_", "erosion_", "floaty_delay_", "spectral_compressor_", "scream_", "tremolo_pan_", "tape_", "defuse_", "suppressor_", "trance_gate_", "spectral_lock_", "resonator_",
      "analoguechorus_", "analogueflanger_", "analogueecho_", "analoguerotary_", "analoguespring_", "analoguewah_",
      "spectral_balance_", "openwurli_amp_", "scanner_vibrato_"
    };
    for (const auto& prefix : effect_prefixes) {
      effect_suffix = effectSuffix(prefix);
      if (effect_suffix.isNotEmpty())
        break;
    }
    if (effect_suffix.isNotEmpty()) {
      if (effect_suffix == "on") return "Enable or bypass this effect.";
      if (effect_suffix == "ir_gain") return "Set the level of the reverb impulse response.";
      if (effect_suffix == "ir_start") return "Trim the beginning of the impulse response.";
      if (effect_suffix == "ir_end") return "Trim the end of the impulse response.";
      if (effect_suffix == "ir_size") return "Stretch or shrink the impulse response to change the space size.";
      if (effect_suffix == "ir_decay") return "Shorten the reverb tail by fading out the impulse response.";
      if (effect_suffix == "ir_dampening") return "Roll off high frequencies progressively along the reverb tail.";
      if (effect_suffix == "ir_reverse") return "Play the impulse response backwards for reverse reverb.";
      if (effectSuffix("convolution_reverb_").isNotEmpty()) {
        if (effect_suffix == "predelay_sync") return "Choose Free predelay in milliseconds, or host tempo sync with straight, dotted or triplet timing.";
        if (effect_suffix == "predelay") return "Delay the reverb behind the dry sound, up to 500 milliseconds, so the attack stays clear.";
        if (effect_suffix == "predelay_tempo") return "Choose the synced predelay, from a whole note to a sixty-fourth note.";
        if (effect_suffix == "fade_in") return "Fade in the start of the impulse response, softening the reverb's attack.";
        if (effect_suffix == "fade_out") return "Fade out the end of the impulse response, shortening the tail smoothly.";
        if (effect_suffix == "tone") return "Tilt the reverb's tone around 800 hertz: negative values are darker, positive values brighter.";
      }
      if (effect_suffix == "filter_slope") return "Choose the slope of the reverb low cut and high cut filters.";
      if (effect_suffix == "shape") return "Choose the waveshaping transfer curve.";
      if (effect_suffix == "tension") return "Morph the curve: positive values saturate, negative values expand and gate quiet signals.";
      if (effect_suffix == "warp_type") return "Choose how the transfer curve is bent or skewed along the input axis.";
      if (effect_suffix == "warp_amount") return "Set how strongly the transfer curve is warped.";
      if (effect_suffix == "mode") return "Choose symmetric unipolar shaping or full-range bipolar shaping.";
      if (effect_suffix == "remove_dc") return "Filter out DC offset created by asymmetric shaping.";
      if (effect_suffix == "output_gain") return "Set this effect's output gain.";
      if (effect_suffix == "mix" || effect_suffix == "dry_wet") return "Blend between the dry signal and this effect.";
      if (effect_suffix == "feedback") return "Feed effect output back into its input.";
      if (effect_suffix == "frequency" || effect_suffix == "cutoff" || effect_suffix.endsWith("_cutoff") ||
          effect_suffix == "low_cut" || effect_suffix == "high_cut")
        return "Set the filter frequency used by this effect.";
      if (effect_suffix == "tempo") return "Choose this effect's tempo-synced rate.";
      if (effect_suffix == "sync") return "Choose free or tempo-synced timing for this effect.";
      if (effect_suffix == "type" || effect_suffix == "style" || effect_suffix.endsWith("_mode"))
        return "Choose the operating mode for this effect.";
      if (effect_suffix == "drive") return "Set how hard the signal drives this effect.";
      if (effect_suffix == "gain") return "Set this effect's output gain.";
      if (effect_suffix == "width") return "Control the stereo width of this effect.";
      if (effect_suffix == "threshold") return "Set the level where this dynamics effect begins working.";
      if (effect_suffix == "attack") return "Set how quickly this dynamics effect reacts to rising level.";
      if (effect_suffix == "release") return "Set how quickly this dynamics effect relaxes.";
      if (effect_suffix == "ratio") return "Set the amount of compression above the threshold.";
      if (effect_suffix == "size") return "Set the size or length of this effect.";
      if (effect_suffix == "decay") return "Set how long this effect rings out.";
    }

    return toggle ? "Enable or disable " + accessible_name + "." : "Adjust " + accessible_name + ".";
  }

  String numberWord(int number) {
    static const std::array<const char*, 20> words = {{
      "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
      "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
      "eighteen", "nineteen", "twenty"
    }};
    if (number >= 1 && number <= static_cast<int>(words.size()))
      return words[number - 1];
    return String(number);
  }

  bool isModulationMenuKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'm';
  }

  bool isMidiLearnKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'l';
  }

  bool isMidiClearKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'c';
  }

  // Shift M, and Shift F10 as in other Windows programs, open the same menu
  // that VoiceOver exposes through VO+Shift+M (the showMenu action), for
  // Windows screen reader users and anyone without VoiceOver, on every control
  // and heading with a menu. On the Mac the right bracket key does too
  // (kRightBracketOpensMenus); atlas::keys::contextMenuKeys names them.
  bool isContextMenuKey(const KeyPress& key) {
    if (isModulationMenuKey(key))
      return true;
    const ModifierKeys modifiers = key.getModifiers();
    if (key.getKeyCode() == KeyPress::F10Key)
      return modifiers.isShiftDown() && !modifiers.isCommandDown() && !modifiers.isCtrlDown() &&
             !modifiers.isAltDown();
    return kRightBracketOpensMenus && !modifiers.isAnyModifierKeyDown() &&
           (key.getKeyCode() == ']' || key.getTextCharacter() == ']');
  }

  // Command chords: Command on macOS, Ctrl elsewhere. Off the Mac JUCE reports
  // Ctrl as the command modifier, so "Command without Ctrl" never matches there.
  bool isCommandChord(const ModifierKeys& modifiers) {
#if JUCE_MAC
    return modifiers.isCommandDown() && !modifiers.isCtrlDown() && !modifiers.isAltDown();
#else
    return modifiers.isCtrlDown() && !modifiers.isAltDown();
#endif
  }

  // Control chords: Control without Command, a separate key only on macOS.
  bool isControlChord(const ModifierKeys& modifiers) {
#if JUCE_MAC
    return modifiers.isCtrlDown() && !modifiers.isCommandDown() && !modifiers.isAltDown();
#else
    return modifiers.isCtrlDown() && !modifiers.isAltDown();
#endif
  }

  bool isEffectMoveEarlierKey(const KeyPress& key) {
    const ModifierKeys modifiers = key.getModifiers();
    const bool allowed_modifiers = !modifiers.isAnyModifierKeyDown() ||
                                   (modifiers.isShiftDown() && !modifiers.isCommandDown() &&
                                    !modifiers.isCtrlDown() && !modifiers.isAltDown());
    const juce_wchar character = key.getTextCharacter();
    return allowed_modifiers &&
           (key.getKeyCode() == '[' || character == '[' || character == '{');
  }

  bool isEffectMoveLaterKey(const KeyPress& key) {
    const ModifierKeys modifiers = key.getModifiers();
    const bool allowed_modifiers = !modifiers.isAnyModifierKeyDown() ||
                                   (modifiers.isShiftDown() && !modifiers.isCommandDown() &&
                                    !modifiers.isCtrlDown() && !modifiers.isAltDown());
    const juce_wchar character = key.getTextCharacter();
    return allowed_modifiers &&
           (key.getKeyCode() == ']' || character == ']' || character == '}');
  }

  int macroIndexForControlId(const String& id) {
    if (!id.startsWith("macro_control_"))
      return -1;

    const int macro = id.fromFirstOccurrenceOf("macro_control_", false, false).getIntValue() - 1;
    return isPositiveAndBelow(macro, vital::kNumMacros) ? macro : -1;
  }

  bool isMacroBipolarParameterId(const String& id) {
    if (!id.startsWith("macro_bipolar_"))
      return false;

    const int macro = id.fromFirstOccurrenceOf("macro_bipolar_", false, false).getIntValue() - 1;
    return isPositiveAndBelow(macro, vital::kNumMacros);
  }

  String macroBipolarParameterId(int macroIndex) {
    return "macro_bipolar_" + String(macroIndex + 1);
  }

  int modulationSourceSortRank(const String& id) {
    const int macro = macroIndexForControlId(id);
    if (macro >= 0)
      return 4000 + macro;

    if (id.startsWith("lfo_"))
      return 1000 + id.fromFirstOccurrenceOf("lfo_", false, false).getIntValue();
    if (id.startsWith("env_"))
      return 2000 + id.fromFirstOccurrenceOf("env_", false, false).getIntValue();
    if (id == "random")
      return 3000;
    if (id.startsWith("random_"))
      return 3000 + id.fromFirstOccurrenceOf("random_", false, false).getIntValue();
    if (const int slot = vital::source_types::slotForOutputSource(id.toStdString()); slot >= 0)
      return 4400 + slot;
    if (const int slot = sourceSlotForId(id); slot >= 0 && id.endsWith("_playhead"))
      return 4500 + slot;
    if (id == "velocity") return 5000;
    if (id == "aftertouch") return 5001;
    if (id == "mod_wheel") return 5002;
    if (id == "pitch_wheel") return 5003;
    if (id == "lift") return 5004;
    if (id == "slide") return 5005;
    if (id == "note") return 5006;
    if (id == "note_in_octave") return 5007;
    if (id == "stereo") return 5008;
    return 9000;
  }

  String routingDestinationText(int destination) {
    switch (destination) {
      case vital::constants::kFilter1:
        return "Filter 1";
      case vital::constants::kFilter2:
        return "Filter 2";
      case vital::constants::kDualFilters:
        return "Filter 1 and 2";
      case vital::constants::kEffects:
        return "Main effects";
      case vital::constants::kBus1:
        return "Bus 1";
      case vital::constants::kBus2:
        return "Bus 2";
      case vital::constants::kDirectOut:
        return "Direct out";
      case vital::constants::kBus3:
        return "Bus 3";
      default:
        return "Main effects";
    }
  }

  String busRoutingDestinationText(int bus_index, int destination) {
    if (destination == vital::constants::kBusDestinationDirectOut)
      return "Direct out";
    if (destination == vital::constants::kBusDestinationMain)
      return "Main effects";
    if (destination >= vital::constants::kBusDestinationBus1 &&
        destination <= vital::constants::kBusDestinationBus3) {
      const int target_bus = destination - vital::constants::kBusDestinationBus1;
      if (target_bus > bus_index)
        return "Bus " + String(target_bus + 1);
    }
    return "Main effects";
  }

  String percentString(float value) {
    return String(roundToInt(jlimit(0.0f, 1.0f, value) * 100.0f)) + "%";
  }

  String midiRootKeyText(double value) {
    static const std::array<const char*, 12> names = {{
      "C", "C sharp", "D", "E flat", "E", "F", "F sharp", "G", "A flat", "A", "B flat", "B"
    }};
    const int midi_note = jlimit(0, 127, roundToInt(value));
    return String(names[midi_note % vital::kNotesPerOctave]) + ", MIDI " + String(midi_note);
  }

  int shiftedDigitIndex(const KeyPress& key) {
    if (!key.getModifiers().isShiftDown())
      return -1;

    switch (key.getTextCharacter()) {
      case '!': return 0;
      case '@': return 1;
      case '#': return 2;
      case '$': return 3;
      case '%': return 4;
      case '^': return 5;
      case '&': return 6;
      case '*': return 7;
      case '(': return 8;
      case ')': return 9;
      default: break;
    }

    const int key_code = key.getKeyCode();
    if (key_code >= '1' && key_code <= '9')
      return key_code - '1';
    if (key_code == '0')
      return 9;
    return -1;
  }

  bool isTransposeQuantizeParameter(const String& id) {
    return id.endsWith("transpose_quantize");
  }

  bool isTransposeQuantizeModeParameter(const String& id) {
    return id.endsWith("_transpose_quantize_mode");
  }

  bool isEffectChoiceParameter(const String& id, const String& suffix) {
    return id == suffix || id.endsWith("_" + suffix);
  }

  String transposeQuantizeModeTitleForParameter(const String& id) {
    // An oscillator's row sits in its own section, which names the oscillator.
    if (id == "sample_transpose_quantize_mode")
      return "Sample transpose quantize mode";
    if (id == "granular_transpose_quantize_mode")
      return "Granular transpose quantize mode";
    return "Transpose quantize mode";
  }

  struct QuantizeScaleChoice {
    const char* name;
    int mask;
  };

  const std::vector<QuantizeScaleChoice>& transposeQuantizeScales() {
    static const std::vector<QuantizeScaleChoice> scales {
      { "Off", 0 },
      { "Chromatic", (1 << 12) - 1 },
      { "Major", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Natural minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10) },
      { "Harmonic minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11) },
      { "Melodic minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Major pentatonic", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9) },
      { "Minor pentatonic", (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10) },
      { "Blues", (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10) },
      { "Dorian", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) },
      { "Phrygian", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10) },
      { "Lydian", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Mixolydian", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) },
      { "Locrian", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10) },
      { "Custom", -1 },
    };
    return scales;
  }

  int rotateTransposeMask(int mask, int key) {
    int result = 0;
    for (int i = 0; i < vital::kNotesPerOctave; ++i)
      if ((mask >> i) & 1)
        result |= 1 << ((i + key + vital::kNotesPerOctave) % vital::kNotesPerOctave);
    return result;
  }

  String transposeKeyName(int key) {
    static const std::array<const char*, 12> names = {{
      "C", "C sharp", "D", "E flat", "E", "F", "F sharp", "G", "A flat", "A", "B flat", "B"
    }};
    return names[jlimit(0, vital::kNotesPerOctave - 1, key)];
  }

  int scaleChoiceForMask(int pitch_mask, int& key) {
    const auto& scales = transposeQuantizeScales();
    if (pitch_mask == 0) {
      key = 0;
      return 0;
    }

    for (int scale = 1; scale < static_cast<int>(scales.size()) - 1; ++scale) {
      for (int candidate_key = 0; candidate_key < vital::kNotesPerOctave; ++candidate_key) {
        if (rotateTransposeMask(scales[scale].mask, candidate_key) == pitch_mask) {
          key = candidate_key;
          return scale;
        }
      }
    }

    key = 0;
    return static_cast<int>(scales.size()) - 1;
  }

  String transposeQuantizeDescription(int value) {
    constexpr int kPitchMask = (1 << vital::kNotesPerOctave) - 1;
    const int pitch_mask = value & kPitchMask;
    const bool global = vital::utils::isTransposeQuantizeGlobal(value);

    if (pitch_mask == 0)
      return "Off";
    if (pitch_mask == kPitchMask)
      return String(global ? "Global " : "Local ") + "chromatic";

    StringArray notes;
    for (int i = 0; i < vital::kNotesPerOctave; ++i)
      if ((pitch_mask >> i) & 1)
        notes.add(transposeKeyName(i));

    return String(global ? "Global scale, " : "Local scale, ") + notes.joinIntoString(", ");
  }

  int noteIndexForToken(String token) {
    token = token.trim().toLowerCase()
                 .replace("sharp", "#")
                 .replace("flat", "b")
                 .replaceCharacter(juce_wchar(0x266f), '#')
                 .replaceCharacter(juce_wchar(0x266d), 'b');
    if (token == "c") return 0;
    if (token == "c#" || token == "db") return 1;
    if (token == "d") return 2;
    if (token == "d#" || token == "eb") return 3;
    if (token == "e") return 4;
    if (token == "f") return 5;
    if (token == "f#" || token == "gb") return 6;
    if (token == "g") return 7;
    if (token == "g#" || token == "ab") return 8;
    if (token == "a") return 9;
    if (token == "a#" || token == "bb") return 10;
    if (token == "b") return 11;
    return -1;
  }

  int scaleMaskForName(String text) {
    text = text.toLowerCase().trim();
    if (text == "off" || text == "none")
      return 0;
    if (text == "minor")
      text = "natural minor";
    for (const auto& scale : transposeQuantizeScales())
      if (text == String(scale.name).toLowerCase())
        return scale.mask;
    return -1;
  }

  bool parseTransposeQuantizeText(String text, int current_value, int& result) {
    text = text.trim();
    if (text.isEmpty())
      return false;

    if (text.containsOnly("0123456789")) {
      result = jlimit(0, 8191, text.getIntValue());
      return true;
    }

    String lower = text.toLowerCase();
    bool global = vital::utils::isTransposeQuantizeGlobal(current_value);
    if (lower.contains("global")) {
      global = true;
      lower = lower.replace("global", "");
    }
    if (lower.contains("local")) {
      global = false;
      lower = lower.replace("local", "");
    }
    lower = lower.replace("snap", "").replace("scale", "").trim();

    int mask = scaleMaskForName(lower);
    if (mask < 0) {
      mask = 0;
      StringArray tokens;
      tokens.addTokens(lower.replace(",", " ").replace(";", " "), " \t\r\n", "");
      for (const auto& token : tokens) {
        const int note = noteIndexForToken(token);
        if (note >= 0)
          mask |= 1 << note;
      }
      if (mask == 0)
        return false;
    }

    result = mask | (global ? (1 << vital::kNotesPerOctave) : 0);
    return true;
  }

  float pointOutputValue(LineGenerator* generator, int index) {
    if (generator == nullptr || !isPositiveAndBelow(index, generator->getNumPoints()))
      return 0.0f;
    return 1.0f - generator->getPoint(index).second;
  }

  enum AccessibleLfoCurve {
    kCurveHold,
    kCurveLinear,
    kCurveEaseIn,
    kCurveEaseOut,
    kCurveSmooth,
    kCurveSCurve,
    kNumAccessibleLfoCurves
  };

  int curveIndexForPower(float power, bool smooth) {
    if (LineGenerator::isHoldPower(power))
      return kCurveHold;
    if (smooth)
      return std::abs(power) <= 0.5f ? kCurveSCurve : kCurveSmooth;
    if (power > 0.5f)
      return kCurveEaseIn;
    if (power < -0.5f)
      return kCurveEaseOut;
    return kCurveLinear;
  }

  float powerForCurveIndex(int index) {
    static const std::array<float, kNumAccessibleLfoCurves> powers = {{
      20.0f, 0.0f, 3.0f, -3.0f, 0.75f, 0.0f
    }};
    return powers[jlimit(0, static_cast<int>(powers.size()) - 1, index)];
  }

  bool smoothForCurveIndex(int index) {
    return index == kCurveSmooth || index == kCurveSCurve;
  }

  String curveNameForIndex(int index) {
    static const std::array<const char*, kNumAccessibleLfoCurves> names = {{
      "Hold", "Linear", "Ease In", "Ease Out", "Smooth", "S Curve"
    }};
    return names[jlimit(0, static_cast<int>(names.size()) - 1, index)];
  }

  class OrderedFocusTraverser : public ComponentTraverser {
    public:
      OrderedFocusTraverser(SynthEditor& editor, bool keyboardTraversal) :
          editor_(editor), keyboard_traversal_(keyboardTraversal) { }

      Component* getDefaultComponent(Component*) override {
        const auto components = getAllComponents(nullptr);
        return components.empty() ? nullptr : components.front();
      }

      Component* getNextComponent(Component* current) override {
        return adjacent(current, 1);
      }

      Component* getPreviousComponent(Component* current) override {
        return adjacent(current, -1);
      }

      std::vector<Component*> getAllComponents(Component*) override {
        std::vector<Component*> result;
        const auto& order = keyboard_traversal_ ? editor_.getKeyboardFocusOrder()
                                                : editor_.getAccessibleFocusOrder();
        for (auto* component : order) {
          if (component != nullptr && component->isShowing() && component->isEnabled() &&
              component->getWantsKeyboardFocus())
            result.push_back(component);
        }
        return result;
      }

    private:
      Component* adjacent(Component* current, int direction) {
        const auto components = getAllComponents(nullptr);
        const auto found = std::find(components.begin(), components.end(), current);
        if (found == components.end() || components.empty())
          return nullptr;

        if (direction > 0 && std::next(found) != components.end())
          return withVisibleComponent(*std::next(found));
        if (direction < 0 && found != components.begin())
          return withVisibleComponent(*std::prev(found));
        return nullptr;
      }

      Component* withVisibleComponent(Component* component) {
        editor_.ensureComponentVisible(component);
        return component;
      }

      SynthEditor& editor_;
      bool keyboard_traversal_ = false;
  };

  class ChildListTraverser : public ComponentTraverser {
    public:
      explicit ChildListTraverser(const std::vector<Component*>& children) : children_(children) { }

      Component* getDefaultComponent(Component*) override {
        const auto components = getAllComponents(nullptr);
        return components.empty() ? nullptr : components.front();
      }

      Component* getNextComponent(Component* current) override {
        return adjacent(current, 1);
      }

      Component* getPreviousComponent(Component* current) override {
        return adjacent(current, -1);
      }

      std::vector<Component*> getAllComponents(Component*) override {
        std::vector<Component*> result;
        for (auto* component : children_) {
          if (component != nullptr && component->isShowing() && component->isEnabled() &&
              component->getWantsKeyboardFocus())
            result.push_back(component);
        }
        return result;
      }

    private:
      Component* adjacent(Component* current, int direction) {
        const auto components = getAllComponents(nullptr);
        const auto found = std::find(components.begin(), components.end(), current);
        if (found == components.end() || components.empty())
          return nullptr;

        if (direction > 0 && std::next(found) != components.end())
          return *std::next(found);
        if (direction < 0 && found != components.begin())
          return *std::prev(found);
        return nullptr;
      }

      const std::vector<Component*>& children_;
  };

  // Implemented by the offscreen controls so that the accessibility handlers
  // (VoiceOver VO+Shift+M -> showMenu) and the key handlers (isContextMenuKey)
  // can trigger the control's context menu through a common entry point.
  struct AccessibleContextMenuTarget {
    virtual ~AccessibleContextMenuTarget() = default;
    virtual void showAccessibleContextMenu() = 0;
  };

  class OffscreenSliderAccessibilityHandler final : public AccessibilityHandler {
    public:
      explicit OffscreenSliderAccessibilityHandler(ParameterSlider& sliderToWrap) :
          AccessibilityHandler(sliderToWrap, AccessibilityRole::slider, contextMenuActions(sliderToWrap),
                               AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(sliderToWrap)}),
          slider_(sliderToWrap) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      static AccessibilityActions contextMenuActions(Slider& slider) {
        return AccessibilityActions().addAction(AccessibilityActionType::showMenu, [&slider] {
          if (auto* menu_target = dynamic_cast<AccessibleContextMenuTarget*>(&slider))
            menu_target->showAccessibleContextMenu();
        });
      }

      class ValueInterface final : public AccessibilityValueInterface {
        public:
          explicit ValueInterface(ParameterSlider& sliderToWrap) :
              slider_(sliderToWrap), use_max_value_(sliderToWrap.isTwoValue()) { }

          bool isReadOnly() const override { return false; }

          double getCurrentValue() const override {
            return slider_.adjustmentValue(use_max_value_ ? slider_.getMaxValue() : slider_.getValue());
          }

          void setValue(double newValue) override {
            Slider::ScopedDragNotification drag(slider_);
            if (use_max_value_)
              slider_.setMaxValue(slider_.sliderValue(newValue), sendNotificationSync);
            else
              slider_.setValue(slider_.sliderValue(newValue), sendNotificationSync);
          }

          String getCurrentValueAsString() const override {
            return slider_.getTextFromValue(slider_.sliderValue(getCurrentValue()));
          }

          void setValueAsString(const String& newValue) override {
            setValue(slider_.adjustmentValue(slider_.getValueFromText(newValue)));
          }

          AccessibleValueRange getRange() const override {
            return { { slider_.adjustmentValue(slider_.getMinimum()), slider_.adjustmentValue(slider_.getMaximum()) },
                     slider_.getAdjustmentStep() };
          }

        private:
          ParameterSlider& slider_;
          bool use_max_value_ = false;
      };

      ParameterSlider& slider_;
  };

  class OffscreenButtonAccessibilityHandler final : public AccessibilityHandler {
    public:
      explicit OffscreenButtonAccessibilityHandler(Button& buttonToWrap, AccessibilityRole role) :
          AccessibilityHandler(buttonToWrap, role, buttonActions(buttonToWrap), buttonInterfaces(buttonToWrap)),
          button_(buttonToWrap) { }

      AccessibleState getCurrentState() const override {
        auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
        if (button_.isToggleable()) {
          state = state.withCheckable();
          if (button_.getToggleState())
            state = state.withChecked();
        }
        return state;
      }

      String getTitle() const override {
        const auto title = AccessibilityHandler::getTitle();
        return title.isEmpty() ? button_.getButtonText() : title;
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      class ValueInterface final : public AccessibilityTextValueInterface {
        public:
          explicit ValueInterface(Button& buttonToWrap) : button_(buttonToWrap) { }

          bool isReadOnly() const override { return true; }
          String getCurrentValueAsString() const override {
            const char* key = button_.getToggleState() ? "accessibleOnText" : "accessibleOffText";
            const auto custom_text = button_.getProperties()[key].toString();
            return custom_text.isNotEmpty() ? custom_text : (button_.getToggleState() ? "On" : "Off");
          }
          void setValueAsString(const String&) override { }

        private:
          Button& button_;
      };

      static AccessibilityActions buttonActions(Button& button) {
        auto actions = AccessibilityActions().addAction(AccessibilityActionType::press,
                                                        [&button] { button.triggerClick(); });
        if (button.isToggleable()) {
          actions = actions.addAction(AccessibilityActionType::toggle, [&button] {
            if (button.isEnabled())
              button.setToggleState(!button.getToggleState(), sendNotification);
          });
        }
        if (dynamic_cast<AccessibleContextMenuTarget*>(&button)) {
          actions = actions.addAction(AccessibilityActionType::showMenu, [&button] {
            if (auto* menu_target = dynamic_cast<AccessibleContextMenuTarget*>(&button))
              menu_target->showAccessibleContextMenu();
          });
        }
        return actions;
      }

      static AccessibilityHandler::Interfaces buttonInterfaces(Button& button) {
        if (button.isToggleable())
          return AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(button)};
        return {};
      }

      Button& button_;
  };

  class OffscreenTextButton : public TextButton {
    public:
      using TextButton::TextButton;

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenButtonAccessibilityHandler>(*this, AccessibilityRole::button);
      }
  };

  class OffscreenComboBox : public ComboBox {
    public:
      std::function<void()> onFocusGained;

      void focusGained(FocusChangeType cause) override {
        ComboBox::focusGained(cause);
        if (onFocusGained)
          onFocusGained();
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        class Handler final : public AccessibilityHandler {
          public:
            // VoiceOver's show menu opens a parameter's context menu, as on its sliders.
            explicit Handler(ComboBox& comboBoxToWrap) :
                AccessibilityHandler(comboBoxToWrap, AccessibilityRole::comboBox,
                                     AccessibilityActions()
                                         .addAction(AccessibilityActionType::press,
                                                    [&comboBoxToWrap] { comboBoxToWrap.showPopup(); })
                                         .addAction(AccessibilityActionType::showMenu, [&comboBoxToWrap] {
                                           if (auto* menu_target = dynamic_cast<AccessibleContextMenuTarget*>(&comboBoxToWrap))
                                             menu_target->showAccessibleContextMenu();
                                           else
                                             comboBoxToWrap.showPopup();
                                         }),
                                     AccessibilityHandler::Interfaces{
                                         std::make_unique<ValueInterface>(comboBoxToWrap) }),
                combo_box_(comboBoxToWrap) { }

            AccessibleState getCurrentState() const override {
              auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen().withExpandable();
              return combo_box_.isPopupActive() ? state.withExpanded() : state.withCollapsed();
            }

            String getDescription() const override { return {}; }
            String getHelp() const override { return {}; }

          private:
            class ValueInterface final : public AccessibilityTextValueInterface {
              public:
                explicit ValueInterface(ComboBox& comboBoxToWrap) : combo_box_(comboBoxToWrap) { }

                bool isReadOnly() const override { return true; }
                String getCurrentValueAsString() const override { return combo_box_.getText(); }
                void setValueAsString(const String&) override { }

              private:
                ComboBox& combo_box_;
            };

            ComboBox& combo_box_;
        };

        return std::make_unique<Handler>(*this);
      }
  };

  class ParameterComboBox : public OffscreenComboBox, public AccessibleContextMenuTarget {
    public:
      std::function<bool(const KeyPress&, Component&)> onAccessibleCommand;
      std::function<void()> onDefaultCommand;
      std::function<void()> onContextMenuCommand;

      void showAccessibleContextMenu() override {
        if (onContextMenuCommand)
          onContextMenuCommand();
      }

      bool keyPressed(const KeyPress& key) override {
        if (onAccessibleCommand && onAccessibleCommand(key, *this)) return true;
        if (onContextMenuCommand && isContextMenuKey(key)) {
          onContextMenuCommand();
          return true;
        }
        const int code = key.getKeyCode();
        if (code == KeyPress::backspaceKey || code == KeyPress::deleteKey) {
          if (onDefaultCommand) onDefaultCommand();
          return true;
        }
        if (code == KeyPress::spaceKey) {
          if (!isPopupActive())
            showPopup();
          return true;
        }
        // The open menu forwards Left and Right here; they must not change the choice behind it.
        if (isPopupActive() && (code == KeyPress::upKey || code == KeyPress::downKey || code == KeyPress::leftKey ||
                                code == KeyPress::rightKey || code == KeyPress::pageUpKey || code == KeyPress::pageDownKey))
          return true;
        int index = getSelectedItemIndex();
        // Keys move through the list as it reads, top to bottom, as in the open menu.
        if (code == KeyPress::homeKey) index = 0;
        else if (code == KeyPress::endKey) index = getNumItems() - 1;
        else if (code == KeyPress::downKey || code == KeyPress::rightKey) ++index;
        else if (code == KeyPress::upKey || code == KeyPress::leftKey) --index;
        else if (code == KeyPress::pageDownKey) index += 10;
        else if (code == KeyPress::pageUpKey) index -= 10;
        else return OffscreenComboBox::keyPressed(key);
        setSelectedItemIndex(jlimit(0, getNumItems() - 1, index), sendNotificationSync);
        return true;
      }
  };

  class OffscreenSlider : public ParameterSlider, public AccessibleContextMenuTarget {
    public:
      std::function<bool(const KeyPress&, Component&)> onAccessibleCommand;
      std::function<void(Component&)> onTextEntryCommand;
      std::function<void()> onDefaultCommand;
      std::function<void()> onContextMenuCommand;

      void showAccessibleContextMenu() override {
        if (onContextMenuCommand)
          onContextMenuCommand();
      }

      bool keyPressed(const KeyPress& key) override {
        if (onAccessibleCommand && onAccessibleCommand(key, *this))
          return true;
        if (onContextMenuCommand && isContextMenuKey(key)) {
          onContextMenuCommand();
          return true;
        }
        if (key.getKeyCode() == KeyPress::returnKey) {
          if (onTextEntryCommand)
            onTextEntryCommand(*this);
          return true;
        }
        if (key.getKeyCode() == KeyPress::backspaceKey || key.getKeyCode() == KeyPress::deleteKey) {
          if (onDefaultCommand)
            onDefaultCommand();
          if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
          return true;
        }

        return ParameterSlider::keyPressed(key);
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenSliderAccessibilityHandler>(*this);
      }
  };

  class OffscreenToggleButton : public SpokenToggleButton, public AccessibleContextMenuTarget {
    public:
      std::function<bool(const KeyPress&, Component&)> onAccessibleCommand;
      std::function<void()> onContextMenuCommand;

      void showAccessibleContextMenu() override {
        if (onContextMenuCommand)
          onContextMenuCommand();
      }

      bool keyPressed(const KeyPress& key) override {
        if (onAccessibleCommand && onAccessibleCommand(key, *this))
          return true;
        if (onContextMenuCommand && isContextMenuKey(key)) {
          onContextMenuCommand();
          return true;
        }
        return ToggleButton::keyPressed(key);
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenButtonAccessibilityHandler>(*this, AccessibilityRole::toggleButton);
      }
  };
}

void SpokenToggleButton::clicked() {
  ToggleButton::clicked();
#if JUCE_WINDOWS
  // The words a screen reader reads for the state: a row's own names, such as
  // Enabled and Bypassed, or On and Off.
  const bool on = getToggleState();
  const String custom = getProperties()[on ? "accessibleOnText" : "accessibleOffText"].toString();
  const String state = custom.isNotEmpty() ? custom : String(on ? "On" : "Off");
  // After the click's own handlers, which may change the speech settings.
  MessageManager::callAsync([state] { postWindowsReadout(state); });
#endif
}

std::unique_ptr<AccessibilityHandler> AccessibleTextButton::createAccessibilityHandler() {
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(Button& buttonToWrap) :
          AccessibilityHandler(buttonToWrap, AccessibilityRole::button,
                               AccessibilityActions().addAction(AccessibilityActionType::press,
                                                                 [&buttonToWrap] { buttonToWrap.triggerClick(); })),
          button_(buttonToWrap) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getTitle() const override {
        const auto title = AccessibilityHandler::getTitle();
        return title.isEmpty() ? button_.getButtonText() : title;
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      Button& button_;
  };

  return std::make_unique<Handler>(*this);
}

std::unique_ptr<AccessibilityHandler> AccessibleComboBox::createAccessibilityHandler() {
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(ComboBox& comboBoxToWrap) :
          AccessibilityHandler(comboBoxToWrap, AccessibilityRole::comboBox,
                               AccessibilityActions()
                                   .addAction(AccessibilityActionType::press,
                                              [&comboBoxToWrap] { comboBoxToWrap.showPopup(); })
                                   .addAction(AccessibilityActionType::showMenu,
                                              [&comboBoxToWrap] { comboBoxToWrap.showPopup(); }),
                               AccessibilityHandler::Interfaces{
                                   std::make_unique<ValueInterface>(comboBoxToWrap) }),
          combo_box_(comboBoxToWrap) { }

      AccessibleState getCurrentState() const override {
        auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen().withExpandable();
        return combo_box_.isPopupActive() ? state.withExpanded() : state.withCollapsed();
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      class ValueInterface final : public AccessibilityTextValueInterface {
        public:
          explicit ValueInterface(ComboBox& comboBoxToWrap) : combo_box_(comboBoxToWrap) { }

          bool isReadOnly() const override { return true; }
          String getCurrentValueAsString() const override { return combo_box_.getText(); }
          void setValueAsString(const String&) override { }

        private:
          ComboBox& combo_box_;
      };

      ComboBox& combo_box_;
  };

  return std::make_unique<Handler>(*this);
}

std::unique_ptr<AccessibilityHandler> AccessibleTextEditor::createAccessibilityHandler() {
  // JUCE's own TextEditor handler, except for the state and the help (see the
  // class). JUCE keeps that handler private, so the text interface is repeated.
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(TextEditor& editorToWrap) :
          AccessibilityHandler(editorToWrap,
                               editorToWrap.isReadOnly() ? AccessibilityRole::staticText
                                                         : AccessibilityRole::editableText,
                               {}, AccessibilityHandler::Interfaces{ std::make_unique<TextInterface>(editorToWrap) }),
          editor_(editorToWrap) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getHelp() const override {
        const String help = editor_.getHelpText();
        return help.isNotEmpty() ? help : editor_.getTooltip();
      }

    private:
      class TextInterface final : public AccessibilityTextInterface {
        public:
          explicit TextInterface(TextEditor& editorToWrap) : editor_(editorToWrap) { }

          bool isDisplayingProtectedText() const override { return editor_.getPasswordCharacter() != 0; }
          bool isReadOnly() const override { return editor_.isReadOnly(); }
          int getTotalNumCharacters() const override { return editor_.getText().length(); }
          Range<int> getSelection() const override { return editor_.getHighlightedRegion(); }
          void setSelection(Range<int> range) override { editor_.setHighlightedRegion(range); }

          String getText(Range<int> range) const override {
            if (isDisplayingProtectedText())
              return String::repeatedString(String::charToString(editor_.getPasswordCharacter()),
                                            getTotalNumCharacters());
            return editor_.getTextInRange(range);
          }

          void setText(const String& text) override { editor_.setText(text); }
          int getTextInsertionOffset() const override { return editor_.getCaretPosition(); }

          RectangleList<int> getTextBounds(Range<int> range) const override {
            RectangleList<int> bounds;
            for (const auto& rectangle : editor_.getTextBounds(range))
              bounds.add(editor_.localAreaToGlobal(rectangle));
            return bounds;
          }

          int getOffsetAtPoint(Point<int> point) const override {
            return editor_.getTextIndexAt(editor_.getLocalPoint(nullptr, point));
          }

        private:
          TextEditor& editor_;
      };

      TextEditor& editor_;
  };

  return std::make_unique<Handler>(*this);
}

namespace {
  class PresetTableCellLabel final : public Label, public AccessibleContextMenuTarget {
    public:
      std::function<void(Component&)> onContextMenu;

      bool keyPressed(const KeyPress& key) override {
        if (onContextMenu && isContextMenuKey(key)) {
          onContextMenu(*this);
          return true;
        }
        return Label::keyPressed(key);
      }

      void showAccessibleContextMenu() override {
        if (onContextMenu)
          onContextMenu(*this);
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        class Handler final : public AccessibilityHandler {
          public:
            explicit Handler(PresetTableCellLabel& label) :
                AccessibilityHandler(label, AccessibilityRole::staticText,
                                     AccessibilityActions().addAction(
                                         AccessibilityActionType::showMenu,
                                         [&label] { label.showAccessibleContextMenu(); })) { }

            AccessibleState getCurrentState() const override {
              return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
            }
        };
        return std::make_unique<Handler>(*this);
      }
  };

  Component* refreshPresetTableLabel(Component* existing, const String& text,
                                     const String& columnName, bool centred,
                                     bool includeColumnName = true,
                                     std::function<void(Component&)> contextMenu = {}) {
    auto* label = dynamic_cast<PresetTableCellLabel*>(existing);
    if (label == nullptr)
      label = new PresetTableCellLabel();
    label->setText(text, dontSendNotification);
    const String spoken_text = text.isEmpty() ? String("None") : text;
    label->setTitle(includeColumnName ? columnName + ": " + spoken_text : spoken_text);
    label->setDescription(label->getTitle());
    label->setColour(Label::textColourId, Colours::white);
    label->setJustificationType(centred ? Justification::centred : Justification::centredLeft);
    label->onContextMenu = std::move(contextMenu);
    label->setInterceptsMouseClicks(false, false);
    // The table remains the single keyboard focus stop; VoiceOver can still
    // reach each accessible cell and invoke its show-menu action.
    label->setWantsKeyboardFocus(false);
    label->setAccessible(true);
    return label;
  }
}

class PresetResultsTableModel final : public TableListBoxModel {
  public:
    explicit PresetResultsTableModel(SynthEditor& editor) : editor_(editor) { }

    int getNumRows() override { return editor_.filtered_presets_.size(); }

    void paintRowBackground(Graphics& g, int, int, int, bool selected) override {
      g.fillAll(selected ? Colour(0xff315a7d) : Colour(0xff20242b));
    }

    void paintCell(Graphics& g, int row, int column, int width, int height, bool) override {
      g.setColour(Colours::white);
      g.drawText(editor_.presetTableCellText(row, column), 7, 0, width - 14, height,
                 column == kPresetFavoriteColumn ? Justification::centred : Justification::centredLeft, true);
    }

    Component* refreshComponentForCell(int row, int column, bool, Component* existing) override {
      const String column_name = editor_.preset_table_.getHeader().getColumnName(column);
      return refreshPresetTableLabel(existing, editor_.presetTableCellText(row, column), column_name,
                                     column == kPresetFavoriteColumn, column != kPresetNameColumn,
                                     [this, row](Component& target) {
                                       editor_.preset_table_.selectRow(row);
                                       editor_.showSelectedPresetMenu(target);
                                     });
    }

    String getCellTooltip(int row, int column) override {
      return editor_.presetTableCellText(row, column);
    }

    void selectedRowsChanged(int row) override {
      if (!editor_.updating_preset_list_)
        editor_.presetSelectionChanged(row, true);
    }

    void cellClicked(int row, int column, const MouseEvent& event) override {
      editor_.preset_table_.selectRow(row);
      if (event.mods.isPopupMenu()) {
        if (event.eventComponent != nullptr)
          editor_.showSelectedPresetMenu(*event.eventComponent);
        else
          editor_.showSelectedPresetMenu(editor_.preset_table_);
      }
      else if (column == kPresetFavoriteColumn)
        editor_.toggleSelectedPresetFavorite();
    }

    void cellDoubleClicked(int row, int, const MouseEvent&) override {
      editor_.preset_table_.selectRow(row);
      editor_.loadSelectedPreset();
    }

    void sortOrderChanged(int column, bool forwards) override {
      editor_.preset_sort_column_ = column;
      editor_.preset_sort_ascending_ = forwards;
      editor_.filterPresetList();
    }

    void deleteKeyPressed(int) override { editor_.deleteSelectedPreset(); }
    void returnKeyPressed(int) override { editor_.loadSelectedPreset(); }

  private:
    SynthEditor& editor_;
};

class PresetFacetTableModel final : public TableListBoxModel {
  public:
    PresetFacetTableModel(SynthEditor& editor, int kind) : editor_(editor), kind_(kind) { }

    int getNumRows() override { return editor_.presetFacetRowCount(kind_); }

    void paintRowBackground(Graphics& g, int, int, int, bool selected) override {
      g.fillAll(selected ? Colour(0xff315a7d) : Colour(0xff20242b));
    }

    void paintCell(Graphics& g, int row, int, int width, int height, bool) override {
      g.setColour(Colours::white);
      g.drawText(editor_.presetFacetCellText(kind_, row), 7, 0, width - 14, height,
                 Justification::centredLeft, true);
    }

    Component* refreshComponentForCell(int row, int column, bool, Component* existing) override {
      auto* table = editor_.presetFacetTableForKind(kind_);
      const String column_name = table != nullptr ? table->getHeader().getColumnName(column) : String();
      return refreshPresetTableLabel(existing, editor_.presetFacetCellText(kind_, row), column_name, false, false);
    }

    String getCellTooltip(int row, int) override {
      return editor_.presetFacetCellText(kind_, row);
    }

    void selectedRowsChanged(int row) override {
      if (!editor_.updating_preset_list_)
        editor_.activatePresetFacet(kind_, row);
    }

    void cellDoubleClicked(int row, int, const MouseEvent&) override { editor_.activatePresetFacet(kind_, row); }
    void returnKeyPressed(int row) override { editor_.activatePresetFacet(kind_, row); }

  private:
    SynthEditor& editor_;
    int kind_ = 0;
};

class CpuUsageMeter final : public Component {
  public:
    explicit CpuUsageMeter(SynthPlugin& synth) : synth_(synth) {
      setTitle("CPU Usage");
      setDescription("Audio processing load for this Atlas instance");
      setHelpText("Percentage of the available audio time used by this Atlas instance. One hundred percent means the full audio processing budget is in use.");
      setWantsKeyboardFocus(true);
      setAccessible(true);
    }

    double percentage() const { return synth_.getCpuUsagePercent(); }

    void refresh() {
      const auto now = Time::getMillisecondCounter();
      if (static_cast<uint32>(now - last_refresh_) < 250)
        return;
      last_refresh_ = now;
      const double next_percentage = percentage();
      const bool value_changed = std::lround(next_percentage * 10.0) != std::lround(displayed_percentage_ * 10.0);
      displayed_percentage_ = next_percentage;
      if (value_changed) {
        repaint();
#if ! JUCE_WINDOWS
        if (auto* handler = getAccessibilityHandler())
          handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
#endif
      }
    }

    void paint(Graphics& g) override {
      auto bounds = getLocalBounds().reduced(8, 10);
      g.setColour(Colours::white);
      g.drawText("CPU Usage", bounds.removeFromLeft(jmin(260, bounds.getWidth() / 2)), Justification::centredLeft);
      g.drawText(String(displayed_percentage_, 1) + "%", bounds.removeFromRight(150), Justification::centredRight);
      const auto bar = bounds.reduced(8, 5).toFloat();
      g.setColour(Colour(0xff303840));
      g.fillRoundedRectangle(bar, 3.0f);
      g.setColour(displayed_percentage_ >= 90.0 ? Colour(0xffe4a05e) : Colour(0xff69b5cf));
      g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * static_cast<float>(displayed_percentage_ / 100.0)), 3.0f);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class Value final : public AccessibilityValueInterface {
        public:
          explicit Value(CpuUsageMeter& meter) : meter_(meter) { }
          bool isReadOnly() const override { return true; }
          double getCurrentValue() const override { return meter_.percentage(); }
          String getCurrentValueAsString() const override { return String(getCurrentValue(), 1) + "%"; }
          void setValue(double) override { }
          void setValueAsString(const String&) override { }
          AccessibleValueRange getRange() const override { return { { 0.0, 100.0 }, 0.0 }; }
        private:
          CpuUsageMeter& meter_;
      };
      class Handler final : public AccessibilityHandler {
        public:
          explicit Handler(CpuUsageMeter& meter) :
#if JUCE_WINDOWS
              // NVDA beeps at every change of a progress bar's value. A plain
              // readout instead: the title gives the percentage when the meter
              // is reached, and nothing is announced while it changes.
              AccessibilityHandler(meter, AccessibilityRole::staticText),
#else
              AccessibilityHandler(meter, AccessibilityRole::progressBar, {},
                                   Interfaces{ std::make_unique<Value>(meter) }),
#endif
              meter_(meter) { }
          String getTitle() const override {
            // JUCE's macOS bridge exposes AXValue as text. VoiceOver can omit
            // that text for a progress indicator, so include the current
            // percentage in its spoken name as well. Query live on navigation.
            return meter_.getTitle() + ", " + String(meter_.percentage(), 1) + " percent";
          }
          String getDescription() const override { return {}; }
          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }
        private:
          CpuUsageMeter& meter_;
      };
      return std::make_unique<Handler>(*this);
    }

  private:
    SynthPlugin& synth_;
    double displayed_percentage_ = 0.0;
    uint32 last_refresh_ = 0;
};

class AccessibleParameterRow : public Component {
  public:
    explicit AccessibleParameterRow(AudioProcessorParameter& parameter, const String& accessibleName = {},
                                    std::function<String(double)> textFromValue = {},
                                    double maxNormalizedValue = 1.0,
                                    double minNormalizedValue = 0.0,
                                    const String& accessibleOffText = {},
                                    const String& accessibleOnText = {},
                                    const String& accessibleDescription = {},
                                    bool forceSliderControl = false,
                                    bool forceToggleControl = false) :
        parameter_(parameter), text_from_value_(std::move(textFromValue)),
        min_normalized_value_(jlimit(0.0, 1.0, minNormalizedValue)),
        max_normalized_value_(jlimit(min_normalized_value_, 1.0, maxNormalizedValue)) {
      const auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      // A two-way choice whose values have names of their own (Filterbank /
      // Spectral, Tape / BBD, Shared / Independent) is an indexed slider, so
      // assistive technology reads those names instead of On and Off.
      const auto is_off_on = [](const std::string* names) {
        return names == nullptr || names == strings::kOffOnNames ||
               (String(names[0]).equalsIgnoreCase("off") && String(names[1]).equalsIgnoreCase("on")) ||
               (String(names[0]).equalsIgnoreCase("disabled") && String(names[1]).equalsIgnoreCase("enabled"));
      };
      const bool named_binary_choice =
          (parameter_id.startsWith("modulation_") && parameter_id.endsWith("_random_mode")) ||
          (bridge != nullptr && bridge->getDetails().value_scale == vital::ValueDetails::kIndexed &&
           bridge->getDetails().max - bridge->getDetails().min == 1.0f &&
           !is_off_on(bridge->getDetails().string_lookup));
      const bool spectral_lock_control = parameter_id.startsWith("spectral_lock_") || parameter_id.contains("_spectral_lock_");
      const bool row_switching_choice = isRowSwitchingChoice(parameter_id);
      // Speech's language and variant lists are long: a menu reads them best.
      const bool speech_voice_control = parameter_id.endsWith("_speech_language") ||
                                        parameter_id.endsWith("_speech_variant");
      use_combo_control_ = row_switching_choice || speech_voice_control ||
                           (spectral_lock_control && (parameter_id.endsWith("_root") || parameter_id.endsWith("_scale")));
      const bool tape_control = parameter_id.startsWith("tape_") || parameter_id.contains("_tape_");
      const bool suppressor_control = parameter_id.startsWith("suppressor_") || parameter_id.contains("_suppressor_");
      const bool defuse_control = parameter_id.startsWith("defuse_") || parameter_id.contains("_defuse_");
      use_toggle_control_ = !use_combo_control_ &&
                            (forceToggleControl || (parameter_.isBoolean() && !forceSliderControl && !named_binary_choice && !tape_control && !defuse_control && !parameter_id.endsWith("trance_gate_trigger")));
      const auto name = parameter_.getName(128);
      const bool random_rate = parameter_id.startsWith("random_") && parameter_id.endsWith("_frequency");
      const bool trance_control = parameter_id.startsWith("trance_gate_") || parameter_id.contains("_trance_gate_");
      const bool tremolo_control = parameter_id.startsWith("tremolo_pan_") || parameter_id.contains("_tremolo_pan_");
      const bool resonator_control = parameter_id.startsWith("resonator_") || parameter_id.contains("_resonator_");
      const bool glitch_delay_control = parameter_id.startsWith("glitch_delay_") || parameter_id.contains("_glitch_delay_");
      // Analogue Chorus, Flanger and Echo share one prefix family.
      const bool analogue_control = parameter_id.startsWith("analogue") || parameter_id.contains("_analogue");
      const auto accessible_name = accessibleName.isNotEmpty() ? accessibleName : (random_rate ? String("Rate") : name);
      const String parameter_description = accessibleDescription.isNotEmpty()
          ? accessibleDescription
          : accessibleParameterHelpText(parameter_id, accessible_name, use_toggle_control_);
      built_name_ = accessible_name;
      built_description_ = parameter_description;
      label_.setText(accessible_name, dontSendNotification);
      label_.setTitle(accessible_name + " label");
      label_.setColour(Label::textColourId, Colours::white);
      addAndMakeVisible(label_);

      if (use_combo_control_) {
        combo_.setTitle(accessible_name);
        combo_.setDescription(parameter_description);
        combo_.setTooltip(parameter_description);
        combo_.setHelpText("Use arrows to choose, Enter or Space to open the menu, Backspace to reset. Home selects the first choice, End the last. Shift M opens the context menu; Shift L learns MIDI; Shift C clears MIDI learn.");
        combo_.setWantsKeyboardFocus(true);
        // One item per choice in the row's range, named as the row reads it.
        const int steps = parameter_.getNumSteps();
        for (int i = 0; i < steps; ++i) {
          const double value = steps > 1 ? double(i) / double(steps - 1) : 0.0;
          if (value < min_normalized_value_ - 1.0e-6 || value > max_normalized_value_ + 1.0e-6)
            continue;
          const String text = text_from_value_ ? text_from_value_(value)
                            : bridge != nullptr ? accessibleParameterText(*bridge, value)
                                                : parameter_.getText(float(value), 128);
          combo_values_.push_back(float(value));
          combo_.addItem(text, static_cast<int>(combo_values_.size()));
        }
        combo_.onChange = [this] {
          const int index = combo_.getSelectedItemIndex();
          if (updating_ || !isPositiveAndBelow(index, static_cast<int>(combo_values_.size()))) return;
          beginParameterChange();
          parameter_.setValueNotifyingHost(combo_values_[static_cast<size_t>(index)]);
          if (onValueChange) onValueChange();
          endParameterChange();
        };
        combo_.onDefaultCommand = [this] { resetToDefaultValue(); };
        combo_.onContextMenuCommand = [this] { showContextMenu(combo_); };
        addAndMakeVisible(combo_);
      }
      else if (use_toggle_control_) {
        toggle_.setButtonText(accessible_name);
        toggle_.setTitle(accessible_name);
        toggle_.setDescription(parameter_description);
        toggle_.setTooltip(parameter_description);
        if (accessibleOffText.isNotEmpty())
          toggle_.getProperties().set("accessibleOffText", accessibleOffText);
        if (accessibleOnText.isNotEmpty())
          toggle_.getProperties().set("accessibleOnText", accessibleOnText);
        toggle_.setHelpText("Press Enter to toggle. Press " + atlas::keys::contextMenuKeys() +
                            " for the context menu. Press Shift L for MIDI learn, or Shift C to clear MIDI learn.");
        toggle_.setWantsKeyboardFocus(true);
        toggle_.onClick = [this] {
          beginParameterChange();
          const bool on = toggle_.getToggleState();
          parameter_.setValueNotifyingHost(toggle_value_ ? toggle_value_(on) : (on ? 1.0f : 0.0f));
          if (onValueChange) onValueChange();
          endParameterChange();
        };
        toggle_.onContextMenuCommand = [this] { showContextMenu(toggle_); };
        addAndMakeVisible(toggle_);
      }
      else {
        slider_.setSliderStyle(Slider::LinearHorizontal);
        slider_.setTextBoxStyle(Slider::TextBoxRight, false, 150, 28);
        const int steps = parameter_.getNumSteps();
        const bool amplitude_seed = parameter_id.startsWith("osc_") && parameter_id.endsWith("_spectral_morph_seed");
        double interval = steps > 1 && (steps < 10000 || amplitude_seed) ? 1.0 / (steps - 1) : 0.0;
        slider_.setRange(min_normalized_value_, max_normalized_value_, interval);
        if (parameter_id == "release_velocity_glide_time") {
          slider_.toAdjustmentValue = [](double value) { return value * 10.0; };
          slider_.fromAdjustmentValue = [](double seconds) { return seconds / 10.0; };
          slider_.adjustmentStep = [] { return 0.01; };
        }
        if (parameter_id == "release_velocity_glide_curve") {
          slider_.toAdjustmentValue = [](double value) { return value * 16.0 - 8.0; };
          slider_.fromAdjustmentValue = [](double curve) { return (curve + 8.0) / 16.0; };
          slider_.adjustmentStep = [] { return 0.1; };
        }
        if (bridge != nullptr && interval == 0.0 && ValueBridge::usesRangePercentDisplay(bridge->getDetails())) {
          // Levels and amounts shown in percent: keys move one percent, also across -100% to 100%.
          const double minimum = bridge->getDetails().min, span = bridge->getDetails().max - minimum;
          slider_.toAdjustmentValue = [minimum, span](double v) { return (minimum + span * v) * 100.0; };
          slider_.fromAdjustmentValue = [minimum, span](double percent) { return (percent / 100.0 - minimum) / span; };
          slider_.adjustmentStep = [] { return 1.0; };
        }
        if (bridge != nullptr && isSemitoneFrequencyParameter(parameter_id, bridge->getDetails())) {
          // Keep host automation in its original normalized/semitone space;
          // keys and VoiceOver operate in Hz when the display uses Hz.
          const double minimum = bridge->getDetails().min;
          const double span = bridge->getDetails().max - minimum;
          slider_.toAdjustmentValue = [minimum, span](double value) {
            return displayAccessibleFrequencyValuesInHz()
                ? 440.0 * std::exp2((minimum + span * value - 69.0) / 12.0) : value;
          };
          slider_.fromAdjustmentValue = [minimum, span](double value) {
            return displayAccessibleFrequencyValuesInHz()
                ? (69.0 + 12.0 * std::log2(jmax(0.001, value) / 440.0) - minimum) / span : value;
          };
          slider_.adjustmentStep = [this] {
            return displayAccessibleFrequencyValuesInHz() ? 10.0
                : (slider_.getMaximum() - slider_.getMinimum()) / 100.0;
          };
        }
        if (bridge != nullptr && vital::scream::parameterIs(bridge->getDetails().name, "cutoff")) {
          slider_.toAdjustmentValue = [](double value) { return vital::scream::cutoffToHz(value); };
          slider_.fromAdjustmentValue = [](double hz) {
            return hz >= 20000.0 ? 1.0 : static_cast<double>(vital::scream::cutoffFromHz(hz));
          };
          slider_.adjustmentStep = [] { return 10.0; };
        }
        if (bridge != nullptr && spectral_lock_control && interval == 0.0) {
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum, scale = details.display_multiply;
          slider_.toAdjustmentValue = [minimum, span, scale](double v) { return (minimum + span * v) * scale; };
          slider_.fromAdjustmentValue = [minimum, span, scale](double v) { return (v / scale - minimum) / span; };
          const double step = parameter_id.endsWith("_range_low") || parameter_id.endsWith("_range_high") ? 10.0
                            : parameter_id.endsWith("_bend") || parameter_id.endsWith("_tilt") ? 0.02 : 1.0;
          slider_.adjustmentStep = [step] { return step; };
        }
        if (bridge != nullptr && tremolo_control && interval == 0.0) {
          const auto& details = bridge->getDetails();
          const double minimum = details.min;
          const double span = details.max - minimum;
          const bool smoothing = parameter_id.endsWith("_smoothing");
          const double scale = smoothing ? 1.0 : details.display_multiply;
          slider_.toAdjustmentValue = [minimum, span, scale](double value) { return (minimum + span * value) * scale; };
          slider_.fromAdjustmentValue = [minimum, span, scale](double value) { return (value / scale - minimum) / span; };
          const double step = smoothing || parameter_id.endsWith("_frequency") ? 0.1 : 1.0;
          slider_.adjustmentStep = [step] { return step; };
        }
        if (bridge != nullptr && suppressor_control && interval == 0.0) {
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum;
          if (vital::suppressor::isFrequency(details.name)) {
            slider_.toAdjustmentValue = [bridge, minimum, span](double v) {
              return vital::suppressor::positionHz(minimum + span * v, bridge->getSuppressorSampleRate());
            };
            slider_.fromAdjustmentValue = [bridge, minimum, span](double hz) {
              return (vital::suppressor::hzPosition(hz, bridge->getSuppressorSampleRate()) - minimum) / span;
            };
            slider_.adjustmentStep = [] { return 10.0; };
          }
          else {
            slider_.toAdjustmentValue = [minimum, span](double v) { return minimum + span * v; };
            slider_.fromAdjustmentValue = [minimum, span](double v) { return (v - minimum) / span; };
            const double step = parameter_id.endsWith("_release") ? 0.05 : parameter_id.endsWith("_width") ? 10.0 : 1.0;
            slider_.adjustmentStep = [step] { return step; };
          }
        }
        if (bridge != nullptr && defuse_control && interval == 0.0) {
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum, scale = details.display_multiply;
          slider_.toAdjustmentValue = [minimum, span, scale](double v) { return (minimum + span * v) * scale; };
          slider_.fromAdjustmentValue = [minimum, span, scale](double v) { return (v / scale - minimum) / span; };
          const double step = parameter_id.endsWith("_frequency") ? 10.0
                            : parameter_id.endsWith("_resonance") ? 0.01
                            : parameter_id.endsWith("_spread") ? 0.1 : 1.0;
          slider_.adjustmentStep = [step] { return step; };
        }
        if (bridge != nullptr && analogue_control && interval == 0.0 &&
            bridge->getDetails().value_scale != vital::ValueDetails::kExponential) {
          // Keys move in display units: one percent, degree or millisecond.
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum, scale = details.display_multiply;
          slider_.toAdjustmentValue = [minimum, span, scale](double v) { return (minimum + span * v) * scale; };
          slider_.fromAdjustmentValue = [minimum, span, scale](double v) { return (v / scale - minimum) / span; };
          slider_.adjustmentStep = [] { return 1.0; };
        }
        if (bridge != nullptr && trance_control && interval == 0.0) {
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum, scale = details.display_multiply;
          slider_.toAdjustmentValue = [minimum, span, scale](double v) { return (minimum + span * v) * scale; };
          slider_.fromAdjustmentValue = [minimum, span, scale](double v) { return (v / scale - minimum) / span; };
          slider_.adjustmentStep = [] { return 1.0; };
        }
        if (bridge != nullptr && resonator_control && interval == 0.0) {
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum;
          if (parameter_id.endsWith("_tune")) {
            // Tune is stored as log2 Hz; keys move it by semitones on the MIDI note scale.
            const double a4 = std::log2(440.0);
            slider_.toAdjustmentValue = [minimum, span, a4](double v) { return 69.0 + 12.0 * (minimum + span * v - a4); };
            slider_.fromAdjustmentValue = [minimum, span, a4](double note) { return ((note - 69.0) / 12.0 + a4 - minimum) / span; };
            slider_.adjustmentStep = [] { return 1.0; };
          }
          else if (details.value_scale != vital::ValueDetails::kExponential) {
            // Glide is stored in milliseconds; the rest step in their display units.
            const double scale = parameter_id.endsWith("_glide") ? 1.0 : details.display_multiply;
            slider_.toAdjustmentValue = [minimum, span, scale](double v) { return (minimum + span * v) * scale; };
            slider_.fromAdjustmentValue = [minimum, span, scale](double v) { return (v / scale - minimum) / span; };
            const bool tenth = parameter_id.endsWith("_lfo_amount") || parameter_id.endsWith("_filter_bandwidth") ||
                               String(details.display_units).trim() == "dB";
            const double step = parameter_id.endsWith("_ratio") ? 0.01 : tenth ? 0.1 : 1.0;
            slider_.adjustmentStep = [step] { return step; };
          }
        }
        if (bridge != nullptr && tape_control && interval == 0.0) {
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum, scale = details.display_multiply;
          slider_.toAdjustmentValue = [minimum, span, scale](double v) { return (minimum + span * v) * scale; };
          slider_.fromAdjustmentValue = [minimum, span, scale](double v) { return (v / scale - minimum) / span; };
          for (const auto& control : vital::tape::controls) {
            if (parameter_id.endsWith(String("tape_") + control.suffix)) {
              const double step = control.step;
              slider_.adjustmentStep = [step] { return step; };
              break;
            }
          }
        }
        if (bridge != nullptr && glitch_delay_control && interval == 0.0) {
          // Keys move in display units: milliseconds, Hz, semitones, dB or
          // percent, whichever curve the value is stored on.
          const auto& details = bridge->getDetails();
          const double minimum = details.min, span = details.max - minimum;
          const auto scale = details.value_scale;
          const double factor = String(details.display_units).trim() == "secs" ? 1.0 : details.display_multiply;
          slider_.toAdjustmentValue = [minimum, span, scale, factor](double v) {
            const double stored = minimum + span * v;
            switch (scale) {
              case vital::ValueDetails::kQuadratic: return stored * stored * factor;
              case vital::ValueDetails::kCubic: return stored * stored * stored * factor;
              case vital::ValueDetails::kExponential: return std::exp2(stored) * factor;
              default: return stored * factor;
            }
          };
          slider_.fromAdjustmentValue = [minimum, span, scale, factor](double display) {
            const double value = display / factor;
            double stored = value;
            switch (scale) {
              case vital::ValueDetails::kQuadratic: stored = std::sqrt(jmax(0.0, value)); break;
              case vital::ValueDetails::kCubic: stored = std::cbrt(value); break;
              case vital::ValueDetails::kExponential: stored = std::log2(jmax(1.0e-6, value)); break;
              default: break;
            }
            return (stored - minimum) / span;
          };
          const String suffix = parameter_id.fromFirstOccurrenceOf("glitch_delay_", false, false);
          const auto ends = [&suffix](const char* end) { return suffix.endsWith(end); };
          const double step = ends("_time") || suffix == "glide" || ends("stop_time") || ends("start_time") ? 10.0
                            : suffix == "buffer_size" || suffix == "smear_size" ? 5.0
                            : ends("high_cut") ? 100.0
                            : ends("low_cut") || ends("ring_min") || ends("ring_max") ? 10.0
                            : ends("fm_ratio_min") || ends("fm_ratio_max") ? 0.05
                            : ends("fm_index_min") || ends("fm_index_max") || ends("crush_rate_min") ||
                              ends("crush_rate_max") || ends("speed_min") || ends("speed_max") ||
                              ends("lpc_min") || ends("lpc_max") || ends("ceps_min") || ends("ceps_max") ||
                              suffix == "output_gain" ? 0.1
                            : ends("_volume") ? 0.5
                            : 1.0;
          slider_.adjustmentStep = [step] { return step; };
        }
        slider_.setTitle(accessible_name);
        slider_.setDescription(parameter_description);
        slider_.setTooltip(parameter_description);
        slider_.setHelpText(random_rate ? "In free mode this controls how long each random cycle lasts. In synced mode use Tempo."
                                        : suppressor_control ? "Arrows change percentages by one percent, Release by 0.05, Curve Frequency by 10 Hz, and Curve Width by 10. Shift arrows use one tenth of the step; Page keys use ten times the step. Enter types a value, Backspace restores its default, Home selects maximum and End minimum."
                                        : defuse_control ? "Use arrows to adjust, Shift arrows for finer changes, and Page keys for ten times the increment. Stages change by one, Frequency by 10 Hz, Resonance by 0.01, Spread by 0.1 octave, and Automation Precision by one percent. Enter types a value; Backspace restores the default. Home selects maximum and End minimum. Shift M opens the context menu."
                                        : tape_control ? "Use arrows to adjust, Shift arrows for finer changes, and Page keys for larger changes. Wow Rate uses 0.01 Hz, Flutter Rate 0.1 Hz, cutoffs 10 Hz, levels 0.1, Drive and percentages one percent. Enter types a value; Backspace restores the default. Home selects maximum and End minimum. Shift M opens the context menu."
                                        : glitch_delay_control ? "Use arrows for changes, Shift arrows for a tenth of the step, and Page keys for ten times the step. Tap times, Glide and tape times move 10 ms; Buffer and Smear Size 5 ms; slice and grain sizes 1 ms; High Cut 100 Hz; Low Cut and ring frequencies 10 Hz; pitch ranges one semitone; sweep speeds and formant shifts 0.1 semitone; FM ratios 0.05; FM index and rate reduction 0.1; densities one grain a second; Volume 0.5 dB and Output Gain 0.1 dB; percentages one percent. Home for maximum, End for minimum, Enter to type a value, Backspace to reset to default. Press Shift M for the context menu, Shift L for MIDI learn, and Shift C to clear MIDI learn."
                                        : resonator_control ? "Use arrows for changes, Shift arrows for finer changes, and Page keys for ten times the step. Tune moves one semitone; Decay, rates and Filter Frequency move in even ratio steps; percentages, degrees, cents and Glide milliseconds move by one; decibels, LFO Amount and Filter Bandwidth by 0.1; Ratio by 0.01. Named choices move one item at a time. Home for maximum, End for minimum, Enter to type a value, Backspace to reset to default. Press Shift M for the context menu, Shift L for MIDI learn, and Shift C to clear MIDI learn."
                                        : tremolo_control ? "Use arrows for changes, Shift arrows for finer changes, and Page keys for larger changes. Free Rate uses 0.1 Hz steps, Stereo Phase and Offset use one degree, Depth and Mix use one percent, and Smoothing uses 0.1 ms. Named choices move one item at a time. Home for maximum, End for minimum, Enter to type a value, Backspace to reset to default. Press Shift M for the context menu, Shift L for MIDI learn, and Shift C to clear MIDI learn."
                                        : "Use arrows for changes, hold Shift with the arrows for finer changes, Page Up and Page Down for larger changes. For frequencies displayed in Hz, arrows change 10 Hz, Shift arrows 1 Hz, and Page keys 100 Hz. Home for maximum, End for minimum, Enter to type a value, Backspace to reset to default. Press " +
                                          atlas::keys::contextMenuKeys() +
                                          " for the context menu. Press Shift L for MIDI learn, and Shift C to clear MIDI learn.");
        slider_.setWantsKeyboardFocus(true);
        slider_.textFromValueFunction = [this](double value) {
          const double parameter_value = parameterValueForSliderValue(value);
          if (text_from_value_)
            return text_from_value_(parameter_value);
          if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
            return accessibleParameterText(*bridge, parameter_value);
          return parameter_.getText(static_cast<float>(parameter_value), 128);
        };
        slider_.valueFromTextFunction = [this](const String& text) {
          double parameter_value = static_cast<double>(parameter_.getValueForText(text));
          if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
            parameter_value = accessibleParameterValueForText(*bridge, text);
          return sliderValueForParameterValue(parameter_value);
        };
        slider_.onDragStart = [this] { beginParameterChange(); };
        slider_.onDragEnd = [this] { endParameterChange(); };
        slider_.onValueChange = [this] {
          if (!updating_) {
            parameter_.setValueNotifyingHost(static_cast<float>(parameterValueForSliderValue(slider_.getValue())));
            if (onValueChange) onValueChange();
          }
        };
        slider_.onDefaultCommand = [this] { resetToDefaultValue(); };
        slider_.onContextMenuCommand = [this] { showContextMenu(slider_); };
        addAndMakeVisible(slider_);
      }
      refresh();
      // The text box was formatted before textFromValueFunction existed; a value
      // already at the slider's initial position would otherwise keep "0.0000000".
      if (!use_combo_control_ && !use_toggle_control_)
        slider_.updateText();
    }

    std::function<void()> onValueChange;

    void setModulationSourceSubmenuCallback(
        std::function<PopupMenu(const String&, std::map<int, String>&, int)> callback) {
      modulation_source_submenu_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setModulationAssignCallback(std::function<void(const String&, const String&, Component&)> callback) {
      modulation_assign_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setModulationEditCallback(std::function<void(const String&, const String&, Component&)> callback) {
      modulation_edit_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setMidiLearnCallback(std::function<void(const String&, Component&, bool)> callback) {
      midi_learn_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setModulationDestinationPredicate(std::function<bool(const String&)> predicate) {
      is_modulation_destination_ = std::move(predicate);
      updateAccessibleCommand();
    }

    void setModulationRemovalCallbacks(std::function<std::vector<std::pair<String, String>>(const String&)> sources,
                                       std::function<void(const String&, const String&, Component&)> remove) {
      modulation_removal_sources_callback_ = std::move(sources);
      modulation_remove_callback_ = std::move(remove);
      updateAccessibleCommand();
    }

    void setValueEntryCallback(std::function<void(const String&, Component&)> callback) {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      slider_.onTextEntryCommand = [parameter_id, callback = std::move(callback)](Component& target) {
        if (parameter_id.isNotEmpty() && callback)
          callback(parameter_id, target);
      };
    }

    void setExtraCommandCallback(std::function<bool(const String&, const KeyPress&, Component&)> callback) {
      extra_command_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setFineTuneCallbacks(std::function<bool()> getState, std::function<void()> toggle) {
      fine_tune_state_ = std::move(getState);
      fine_tune_toggle_ = std::move(toggle);
    }

    void setFxAutomationCallbacks(std::function<String(const String&)> assignmentName,
                                  std::function<void(const String&, bool)> changeAssignment,
                                  std::function<void(const String&, bool)> gesture) {
      fx_automation_assignment_name_ = std::move(assignmentName);
      fx_automation_change_assignment_ = std::move(changeAssignment);
      fx_automation_gesture_ = std::move(gesture);
      updateAccessibleCommand();
    }

    // `fallbackIndex` names the choice shown for values missing from the
    // order; otherwise the nearest value is shown.
    void setIndexedValueOrder(std::vector<int> engineValueOrder, int fallbackIndex = -1) {
      if (use_toggle_control_ || use_combo_control_ || engineValueOrder.empty())
        return;

      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      if (bridge == nullptr)
        return;

      std::set<int> seen;
      for (int value : engineValueOrder) {
        if (value < static_cast<int>(std::ceil(bridge->getDetails().min)) ||
            value > static_cast<int>(std::floor(bridge->getDetails().max)) || !seen.insert(value).second)
          return;
      }

      indexed_value_fallback_ = fallbackIndex < static_cast<int>(engineValueOrder.size()) ? fallbackIndex : -1;
      indexed_value_order_ = std::move(engineValueOrder);
      slider_.setRange(0.0, static_cast<double>(indexed_value_order_.size() - 1), 1.0);
      slider_.setHelpText("Use arrow keys to move through the choices in their logical display order. Enter types a value and Backspace resets to default. Press " +
                          atlas::keys::contextMenuKeys() + " for the context menu.");
      refresh();
    }

    void setToggleMapping(std::function<bool()> state, std::function<float(bool)> value) {
      toggle_state_ = std::move(state);
      toggle_value_ = std::move(value);
      refresh();
    }

    void notifyDisplayChanged() {
      if (auto* handler = focusableControl()->getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }

    void refresh() {
      ScopedValueSetter<bool> guard(updating_, true);
      if (use_combo_control_) {
        const float value = parameter_.getValue();
        int closest = -1;
        for (int i = 0; i < static_cast<int>(combo_values_.size()); ++i) {
          if (closest < 0 || std::abs(combo_values_[static_cast<size_t>(i)] - value) <
                             std::abs(combo_values_[static_cast<size_t>(closest)] - value))
            closest = i;
        }
        combo_.setSelectedItemIndex(closest, dontSendNotification);
      }
      else if (use_toggle_control_)
        toggle_.setToggleState(toggle_state_ ? toggle_state_() : parameter_.getValue() >= 0.5f, dontSendNotification);
      else {
        auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
        if (bridge != nullptr && bridge->hasEnvelopeSyncContext()) {
          const bool synced = bridge->isEnvelopeTimeSynced();
          const double maximum = synced ? bridge->maximumEnvelopeTimeDivision() : max_normalized_value_;
          if (synced != synced_envelope_time_ || slider_.getMaximum() != maximum) {
            synced_envelope_time_ = synced;
            slider_.setRange(synced ? 0.0 : min_normalized_value_, maximum, synced ? 1.0 : 0.0);
            slider_.setHelpText(synced
                ? "Use arrows to select the next musical duration, or Page keys to move ten divisions. d means dotted and t means triplet. Enter accepts values such as 1/4, 1/4d or 1/4t. Home selects the longest duration, End selects None, and Backspace restores the default. Shift M opens the context menu."
                : "Use arrows to change time, Shift arrows for finer changes, and Page keys for larger changes. Enter accepts milliseconds or seconds. Home selects maximum, End minimum, and Backspace restores the default. Shift M opens the context menu.");
          }
        }
        const double parameter_value = jlimit(min_normalized_value_, max_normalized_value_,
                                              static_cast<double>(parameter_.getValue()));
        slider_.setValue(sliderValueForParameterValue(parameter_value), dontSendNotification);
        if (bridge != nullptr && bridge->hasEnvelopeSyncContext()) {
          const String text = slider_.getTextFromValue(slider_.getValue());
          if (text != envelope_time_text_) {
            envelope_time_text_ = text;
            slider_.updateText();
            notifyDisplayChanged();
          }
        }
      }
    }

    void setAccessibleName(const String& name, const String& description = {}) {
      if (name.isEmpty())
        return;

      if (focusableControl()->getTitle() == name)
        return;

      label_.setText(name, dontSendNotification);
      label_.setTitle(name + " label");
      focusableControl()->setTitle(name);
      if (use_toggle_control_)
        toggle_.setButtonText(name);
      if (description.isNotEmpty()) {
        focusableControl()->setDescription(description);
        if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(focusableControl()))
          tooltip->setTooltip(description);
      }
      if (auto* handler = focusableControl()->getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
      repaint();
    }

    Component* focusableControl() {
      if (use_combo_control_) return &combo_;
      return use_toggle_control_ ? static_cast<Component*>(&toggle_) : static_cast<Component*>(&slider_);
    }

    String parameterId() const {
      if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
        return bridge->getParameterId();
      return {};
    }

    const AudioProcessorParameter& parameter() const { return parameter_; }

    // Whether `fresh`, built just now for the same parameter, is the same
    // choice menu as this row, so a rebuild can keep this row in its place.
    // Compares the parameter by address only: this row's may be gone.
    bool readsLike(const AccessibleParameterRow& fresh) const {
      if (&parameter_ != &fresh.parameter_ || built_name_ != fresh.built_name_ ||
          built_description_ != fresh.built_description_)
        return false;
      if (use_toggle_control_ || fresh.use_toggle_control_)
        return use_toggle_control_ && fresh.use_toggle_control_;
      if (!use_combo_control_ || !fresh.use_combo_control_ || combo_values_ != fresh.combo_values_ ||
          combo_.getNumItems() != fresh.combo_.getNumItems())
        return false;
      for (int i = 0; i < combo_.getNumItems(); ++i) {
        if (combo_.getItemText(i) != fresh.combo_.getItemText(i))
          return false;
      }
      return true;
    }

    // Named as it was built, for a rebuild that keeps this row and names it
    // as it names a fresh one.
    void restoreBuiltName() {
      label_.setText(built_name_, dontSendNotification);
      label_.setTitle(built_name_ + " label");
      focusableControl()->setTitle(built_name_);
      focusableControl()->setDescription(built_description_);
      if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(focusableControl()))
        tooltip->setTooltip(built_description_);
      if (use_toggle_control_)
        toggle_.setButtonText(built_name_);
    }

    void setControlFocusOrder(int order) {
      focusableControl()->setExplicitFocusOrder(order);
    }

    void resized() override {
      auto bounds = getLocalBounds().reduced(8, 4);
      label_.setBounds(bounds.removeFromLeft(jmin(240, bounds.getWidth() / 3)));
      if (use_combo_control_)
        combo_.setBounds(bounds);
      else if (use_toggle_control_)
        toggle_.setBounds(bounds);
      else
        slider_.setBounds(bounds);
    }

  private:
    enum ContextMenuAction {
      kContextTypeValue = 1,
      kContextResetDefault,
      kContextMidiLearn,
      kContextClearMidi,
      kContextRenameMacro,
      kContextToggleBipolar,
      kContextFineTune,
      kContextAutomateFx,
      kContextRemoveFxAutomation,
      kContextAddModulationBase = 1000,
      kContextEditModulationBase = 2000,
      kContextRemoveModulationBase = 3000,
    };

    bool canAddModulation(const String& parameter_id) const {
      if (parameter_id.isEmpty())
        return false;
      if (!(modulation_source_submenu_callback_ && modulation_assign_callback_))
        return false;
      return !is_modulation_destination_ || is_modulation_destination_(parameter_id);
    }

    void showContextMenu(Component& target) {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      if (parameter_id.isEmpty())
        return;

      const bool is_toggle = use_toggle_control_;
      const int macro_index = macroIndexForControlId(parameter_id);
      const auto removable_modulations = modulation_removal_sources_callback_
          ? modulation_removal_sources_callback_(parameter_id)
          : std::vector<std::pair<String, String>>();
      const String fx_automation_slot = fx_automation_assignment_name_
          ? fx_automation_assignment_name_(parameter_id) : String();

      PopupMenu menu;
      std::map<int, String> add_modulation_choices;
      menu.addSectionHeader(parameter_.getName(128));
      if (!is_toggle && slider_.onTextEntryCommand)
        menu.addItem(kContextTypeValue, "Type a value...");
      menu.addItem(kContextResetDefault, "Reset to default");
      if (fine_tune_toggle_)
        menu.addItem(kContextFineTune, "Fine tune", true, fine_tune_state_ && fine_tune_state_());
      if (fx_automation_change_assignment_) {
        if (fx_automation_slot.isEmpty())
          menu.addItem(kContextAutomateFx, "Automate");
        else
          menu.addItem(kContextRemoveFxAutomation, "Remove automation from " + fx_automation_slot);
      }
      if (canAddModulation(parameter_id)) {
        PopupMenu add_menu = modulation_source_submenu_callback_(parameter_id, add_modulation_choices,
                                                                 kContextAddModulationBase);
        menu.addSubMenu("Add modulation source", add_menu, !add_modulation_choices.empty());
      }
      for (int i = 0; i < static_cast<int>(removable_modulations.size()); ++i)
        menu.addItem(kContextEditModulationBase + i,
                     "Edit modulation from " + removable_modulations[static_cast<size_t>(i)].second);
      for (int i = 0; i < static_cast<int>(removable_modulations.size()); ++i)
        menu.addItem(kContextRemoveModulationBase + i,
                     "Remove modulation from " + removable_modulations[static_cast<size_t>(i)].second);
      if (midi_learn_callback_) {
        menu.addItem(kContextMidiLearn, "MIDI learn");
        menu.addItem(kContextClearMidi, "Clear MIDI learn");
      }
      if (macro_index >= 0) {
        menu.addItem(kContextRenameMacro, "Rename macro\xe2\x80\xa6");
        menu.addItem(kContextToggleBipolar, "Toggle bipolar range");
      }

      Component::SafePointer<Component> safe_target(&target);
      showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                         [this, parameter_id, removable_modulations, add_modulation_choices, safe_target](int result) {
        Component* invoked = safe_target.getComponent();
        if (invoked == nullptr)
          return;

        const auto add_found = add_modulation_choices.find(result);
        if (add_found != add_modulation_choices.end()) {
          if (modulation_assign_callback_)
            modulation_assign_callback_(add_found->second, parameter_id, *invoked);
          return;
        }

        if (result >= kContextEditModulationBase &&
            result < kContextEditModulationBase + static_cast<int>(removable_modulations.size())) {
          const int index = result - kContextEditModulationBase;
          if (modulation_edit_callback_ &&
              isPositiveAndBelow(index, static_cast<int>(removable_modulations.size())))
            modulation_edit_callback_(removable_modulations[static_cast<size_t>(index)].first,
                                      parameter_id, *invoked);
          return;
        }

        if (result >= kContextRemoveModulationBase &&
            result < kContextRemoveModulationBase + static_cast<int>(removable_modulations.size())) {
          const int index = result - kContextRemoveModulationBase;
          if (modulation_remove_callback_ &&
              isPositiveAndBelow(index, static_cast<int>(removable_modulations.size())))
            modulation_remove_callback_(removable_modulations[static_cast<size_t>(index)].first,
                                        parameter_id, *invoked);
          return;
        }

        switch (result) {
          case kContextTypeValue:
            if (slider_.onTextEntryCommand)
              slider_.onTextEntryCommand(*invoked);
            break;
          case kContextResetDefault:
            resetToDefaultValue();
            break;
          case kContextMidiLearn:
            if (midi_learn_callback_)
              midi_learn_callback_(parameter_id, *invoked, false);
            break;
          case kContextClearMidi:
            if (midi_learn_callback_)
              midi_learn_callback_(parameter_id, *invoked, true);
            break;
          case kContextRenameMacro:
            if (extra_command_callback_)
              extra_command_callback_(parameter_id,
                                      KeyPress(KeyPress::returnKey, ModifierKeys::shiftModifier, 0), *invoked);
            break;
          case kContextToggleBipolar:
            if (extra_command_callback_)
              extra_command_callback_(parameter_id, KeyPress('b', ModifierKeys(), 'b'), *invoked);
            break;
          case kContextFineTune:
            if (fine_tune_toggle_)
              fine_tune_toggle_();
            break;
          case kContextAutomateFx:
            if (fx_automation_change_assignment_)
              fx_automation_change_assignment_(parameter_id, false);
            break;
          case kContextRemoveFxAutomation:
            if (fx_automation_change_assignment_)
              fx_automation_change_assignment_(parameter_id, true);
            break;
          default:
            break;
        }
      });
    }

    void resetToDefaultValue() {
      const float default_value = jlimit(static_cast<float>(min_normalized_value_),
                                        static_cast<float>(max_normalized_value_),
                                        parameter_.getDefaultValue());
      beginParameterChange();
      parameter_.setValueNotifyingHost(default_value);
      endParameterChange();
      refresh();
      String value_text = parameter_.getText(parameter_.getValue(), 128);
      if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
        value_text = accessibleParameterText(*bridge, parameter_.getValue());
      postPluginAnnouncement(parameter_.getName(128) + " reset to " + value_text,
                             AccessibilityHandler::AnnouncementPriority::high);
    }

    AudioProcessorParameter& parameter_;
    Label label_;
    OffscreenSlider slider_;
    ParameterComboBox combo_;
    OffscreenToggleButton toggle_;
    std::function<String(double)> text_from_value_;
    std::function<bool()> toggle_state_;
    std::function<float(bool)> toggle_value_;
    std::function<PopupMenu(const String&, std::map<int, String>&, int)> modulation_source_submenu_callback_;
    std::function<void(const String&, const String&, Component&)> modulation_assign_callback_;
    std::function<void(const String&, const String&, Component&)> modulation_edit_callback_;
    std::function<bool(const String&)> is_modulation_destination_;
    std::function<std::vector<std::pair<String, String>>(const String&)> modulation_removal_sources_callback_;
    std::function<void(const String&, const String&, Component&)> modulation_remove_callback_;
    std::function<void(const String&, Component&, bool)> midi_learn_callback_;
    std::function<bool(const String&, const KeyPress&, Component&)> extra_command_callback_;
    std::function<bool()> fine_tune_state_;
    std::function<void()> fine_tune_toggle_;
    std::function<String(const String&)> fx_automation_assignment_name_;
    std::function<void(const String&, bool)> fx_automation_change_assignment_;
    std::function<void(const String&, bool)> fx_automation_gesture_;
    std::vector<int> indexed_value_order_;
    int indexed_value_fallback_ = -1;
    std::vector<float> combo_values_;   // the parameter value of each menu item
    // The name and description the row was built with, before any rename.
    String built_name_;
    String built_description_;
    double min_normalized_value_ = 0.0;
    double max_normalized_value_ = 1.0;
    bool use_toggle_control_ = false;
    bool use_combo_control_ = false;
    bool updating_ = false;
    bool synced_envelope_time_ = false;
    String envelope_time_text_;

    void beginParameterChange() {
      parameter_.beginChangeGesture();
      const String id = parameterId();
      if (id.isNotEmpty() && fx_automation_gesture_)
        fx_automation_gesture_(id, true);
    }

    void endParameterChange() {
      const String id = parameterId();
      if (id.isNotEmpty() && fx_automation_gesture_)
        fx_automation_gesture_(id, false);
      parameter_.endChangeGesture();
    }

    double parameterValueForSliderValue(double slider_value) const {
      if (synced_envelope_time_)
        if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
          return bridge->valueForEnvelopeTimeDivision(static_cast<int>(std::round(slider_value)));
      if (indexed_value_order_.empty())
        return slider_value;

      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      if (bridge == nullptr)
        return slider_value;

      const int index = jlimit(0, static_cast<int>(indexed_value_order_.size()) - 1,
                               static_cast<int>(std::round(slider_value)));
      return bridge->convertToPluginValue(static_cast<float>(indexed_value_order_[static_cast<size_t>(index)]));
    }

    double sliderValueForParameterValue(double parameter_value) const {
      if (synced_envelope_time_)
        if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
          return bridge->envelopeTimeDivision(static_cast<float>(parameter_value));
      if (indexed_value_order_.empty())
        return parameter_value;

      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      if (bridge == nullptr)
        return parameter_value;

      const int engine_value = static_cast<int>(std::round(
          bridge->convertToEngineValue(static_cast<float>(parameter_value))));
      if (indexed_value_fallback_ >= 0 &&
          std::find(indexed_value_order_.begin(), indexed_value_order_.end(), engine_value) == indexed_value_order_.end())
        return static_cast<double>(indexed_value_fallback_);
      int closest_index = 0;
      int closest_distance = std::numeric_limits<int>::max();
      for (int i = 0; i < static_cast<int>(indexed_value_order_.size()); ++i) {
        const int distance = std::abs(indexed_value_order_[static_cast<size_t>(i)] - engine_value);
        if (distance < closest_distance) {
          closest_index = i;
          closest_distance = distance;
        }
      }
      return static_cast<double>(closest_index);
    }

    void updateAccessibleCommand() {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      auto command = [this, parameter_id](const KeyPress& key, Component& target) {
        if (parameter_id.isEmpty())
          return false;

        if (extra_command_callback_ && extra_command_callback_(parameter_id, key, target))
          return true;

        if (isModulationMenuKey(key)) {
          showContextMenu(target);
          return true;
        }

        if (isMidiLearnKey(key) && midi_learn_callback_) {
          midi_learn_callback_(parameter_id, target, false);
          return true;
        }

        if (isMidiClearKey(key) && midi_learn_callback_) {
          midi_learn_callback_(parameter_id, target, true);
          return true;
        }

        return false;
      };

      combo_.onAccessibleCommand = command;
      slider_.onAccessibleCommand = command;
      toggle_.onAccessibleCommand = std::move(command);
    }
};

class AccessibleSectionHeader : public Component, public AccessibleContextMenuTarget {
  public:
    std::function<bool(const KeyPress&, Component&)> onKeyCommand;
    std::function<void(Component&)> onContextMenuCommand;
    std::function<void()> onAccessibilityFocus;

    AccessibleSectionHeader(const String& title, int groupRank, bool topLevel, const String& sectionKey = {}) {
      configure(title, groupRank, topLevel, sectionKey);
    }

    // Whether a rebuild asking for this group can have this header back.
    bool isSameGroup(const String& title, bool topLevel, const String& sectionKey) const {
      return top_level_ == topLevel && section_key_ == sectionKey && (sectionKey.isNotEmpty() || title_ == title);
    }

    // As the constructor leaves it, for a rebuild that takes it back: no
    // children, callbacks or other properties.
    void reuse(const String& title, int groupRank, bool topLevel, const String& sectionKey) {
      removeAllChildren();
      child_focus_order_.clear();
      onKeyCommand = nullptr;
      onContextMenuCommand = nullptr;
      onAccessibilityFocus = nullptr;
      getProperties().clear();   // the explicit focus order too
      configure(title, groupRank, topLevel, sectionKey);
      repaint();
    }

    void paint(Graphics& g) override {
      const auto bounds = getLocalBounds().toFloat().reduced(2.0f);
      g.setColour(top_level_ ? Colour(0xff28313d) : Colour(0xff20262f));
      g.fillRoundedRectangle(bounds, 5.0f);
      g.setColour(Colours::white);
      g.setFont(FontOptions(top_level_ ? 18.0f : 15.0f, top_level_ ? Font::bold : Font::plain));
      g.drawText(title_, getLocalBounds().withHeight(headerHeight()).reduced(top_level_ ? 12 : 22, 0),
                 Justification::centredLeft, true);
    }

    bool keyPressed(const KeyPress& key) override {
      // Shift M opens the heading's menu before any other command claims the
      // key: an effect slot's handler takes the bracket keys for moving it.
      if (onContextMenuCommand && isModulationMenuKey(key)) {
        onContextMenuCommand(*this);
        return true;
      }
      if (onKeyCommand && onKeyCommand(key, *this))
        return true;
      if (onContextMenuCommand && isContextMenuKey(key)) {
        onContextMenuCommand(*this);
        return true;
      }
      return Component::keyPressed(key);
    }

    void mouseDown(const MouseEvent& event) override {
      if (onContextMenuCommand && event.mods.isPopupMenu()) {
        onContextMenuCommand(*this);
        return;
      }
      Component::mouseDown(event);
    }

    void showAccessibleContextMenu() override {
      if (onContextMenuCommand)
        onContextMenuCommand(*this);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class HeaderAccessibilityHandler final : public AccessibilityHandler {
        public:
          explicit HeaderAccessibilityHandler(AccessibleSectionHeader& header) :
              AccessibilityHandler(header, AccessibilityRole::group,
                                   AccessibilityActions().addAction(AccessibilityActionType::showMenu,
                                                                     [&header] {
                                                                       header.showAccessibleContextMenu();
                                                                     })
                                                         .addAction(AccessibilityActionType::focus,
                                                                     [&header] {
                                                                       if (header.onAccessibilityFocus)
                                                                         header.onAccessibilityFocus();
                                                                     })) { }

          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }
      };
      return std::make_unique<HeaderAccessibilityHandler>(*this);
    }

    std::unique_ptr<ComponentTraverser> createFocusTraverser() override {
      return std::make_unique<ChildListTraverser>(child_focus_order_);
    }

    void addAccessibleChild(Component* component) {
      if (component != nullptr)
        child_focus_order_.push_back(component);
    }

    void clearAccessibleChildren() { child_focus_order_.clear(); }

    void setHeaderTitle(const String& title) {
      if (title.isEmpty() || title == title_)
        return;

      title_ = title;
      setTitle(title_);
      setDescription(top_level_ ? "Control group " + title_ : "Control section " + title_);
      if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
      repaint();
    }

    int headerHeight() const { return top_level_ ? 42 : 34; }

  private:
    void configure(const String& title, int groupRank, bool topLevel, const String& sectionKey) {
      title_ = title;
      top_level_ = topLevel;
      section_key_ = sectionKey;
      setTitle(title_);
      setDescription(top_level_ ? "Control group " + title_ : "Control section " + title_);
      setHelpText(top_level_ ? "Group heading" : "Section heading");
      setAccessible(true);
      setWantsKeyboardFocus(true);
      setFocusContainerType(Component::FocusContainerType::focusContainer);
      getProperties().set("ControlGroup", groupRank);
      if (sectionKey.isNotEmpty())
        getProperties().set("SectionName", sectionKey);
    }

    String title_;
    bool top_level_ = false;
    String section_key_;
    std::vector<Component*> child_focus_order_;
};

void SynthEditor::addCpuMeter(Component& parent, int x, int& y, int width, int& focusOrder) {
  if (cpu_meter_ == nullptr)
    cpu_meter_ = std::make_unique<CpuUsageMeter>(synth_);
  cpu_meter_->setBounds(x, y, width, 48);
  cpu_meter_->setExplicitFocusOrder(focusOrder++);
  parent.addAndMakeVisible(*cpu_meter_);
  if (auto* section = dynamic_cast<AccessibleSectionHeader*>(&parent))
    section->addAccessibleChild(cpu_meter_.get());
  row_focus_order_.push_back(cpu_meter_.get());
  cpu_meter_->refresh();
  y += 48;
}

SynthEditor::SynthEditor(SynthPlugin& synth) :
    AudioProcessorEditor(&synth), SynthGuiInterface(&synth, false), synth_(synth),
    last_section_name_(synth.getEditorMemory().section_name),
    last_preset_path_(synth.getEditorMemory().preset_path),
    last_preset_search_(synth.getEditorMemory().preset_search),
    last_preset_library_(synth.getEditorMemory().preset_library),
    last_preset_bank_(synth.getEditorMemory().preset_bank),
    last_preset_category_(synth.getEditorMemory().preset_category),
    last_preset_tag_(synth.getEditorMemory().preset_tag) {
  // A new instance's preset browser starts unfiltered.
  if (last_preset_library_.isEmpty()) last_preset_library_ = kAllLibraries;
  if (last_preset_bank_.isEmpty()) last_preset_bank_ = kAllBanks;
  if (last_preset_category_.isEmpty()) last_preset_category_ = kAllCategories;
  if (last_preset_tag_.isEmpty()) last_preset_tag_ = kAllTags;
  setTitle("Atlas");
  setDescription("Keyboard and VoiceOver accessible editor for Atlas");
  setWantsKeyboardFocus(true);
  setFocusContainerType(Component::FocusContainerType::keyboardFocusContainer);

  title_.setText("Atlas", dontSendNotification);
  title_.setFont(FontOptions(24.0f, Font::bold));
  title_.setTitle("Atlas");
  title_.setColour(Label::textColourId, Colours::white);
  addAndMakeVisible(title_);

  menu_button_.setTitle("Editor menu");
  menu_button_.setDescription("Open tuning, accessibility settings and keyboard help. Patch saving and presets are in the Preset menu");
  menu_button_.setHelpText("Press Enter to save the current patch, initialize the patch, or change accessibility settings");
  menu_button_.setWantsKeyboardFocus(true);
  menu_button_.onClick = [this] { showNavigationMenu(); };
  addAndMakeVisible(menu_button_);

  preset_summary_.setTitle("Preset summary");
  preset_summary_.setDescription("Current preset and Atlas resources path");
  preset_summary_.setColour(Label::textColourId, Colours::white);
  preset_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(preset_summary_);

  preset_menu_.setTitle("Preset menu");
  preset_menu_.setDescription("Save the patch, save it as the default, initialize it, import a preset file or bank, export a bank or the patch, reveal the selected preset, and refresh the list. Loading, favourites, renaming and sorting are in the preset browser and its context menu");
  preset_menu_.setHelpText("Press Enter to open preset browser actions");
  preset_menu_.setWantsKeyboardFocus(true);
  preset_menu_.onClick = [this] { showPresetMenu(); };
  addChildComponent(preset_menu_);

  auto configureFacetTable = [this](PresetTableListBox& table,
                                    std::unique_ptr<PresetFacetTableModel>& model,
                                    int kind, const String& title) {
    model = std::make_unique<PresetFacetTableModel>(*this, kind);
    table.setModel(model.get());
    table.setTitle(title);
    table.setDescription("Filter presets by " + title.toLowerCase() + ". The selected row is applied immediately.");
    table.setHelpText("Use Up and Down to select a " + title.toLowerCase() + ".");
    table.setWantsKeyboardFocus(true);
    table.setRowHeight(30);
    table.setHeaderHeight(32);
    table.setMultipleSelectionEnabled(false);
    table.getHeader().addColumn(title, 1, 170, 90, 420);
    table.getHeader().setStretchToFitActive(true);
    table.onFocusGained = [this] { ensurePresetListLoaded(); };
    table.onKeyPressed = [this, table_pointer = &table, kind](const KeyPress& key) {
      if (key.getKeyCode() == KeyPress::returnKey || key.getKeyCode() == KeyPress::spaceKey) {
        activatePresetFacet(kind, table_pointer->getSelectedRow());
        return true;
      }
      return false;
    };
    table.onSelectionKey = [this, table_pointer = &table, kind] {
      const int row = table_pointer->getSelectedRow();
      const int rows = presetFacetRowCount(kind);
      if (isPositiveAndBelow(row, rows))
        postWindowsReadout(listRowText(presetFacetCellText(kind, row), row, rows));
    };
    addChildComponent(table);
  };
  configureFacetTable(preset_collection_table_, preset_collection_model_, kCollectionFacet, "Collection");
  configureFacetTable(preset_library_table_, preset_library_model_, kLibraryFacet, "Library");
  configureFacetTable(preset_bank_table_, preset_bank_model_, kBankFacet, "Bank");
  configureFacetTable(preset_category_table_, preset_category_model_, kCategoryFacet, "Category");
  configureFacetTable(preset_tag_table_, preset_tag_model_, kTagFacet, "Tags");

  preset_search_.setTitle("Preset search");
  preset_search_.setDescription("Filter presets by name, folder, author, category, or tags");
  preset_search_.setHelpText("Type text to narrow the preset list");
  preset_search_.setTextToShowWhenEmpty("Search presets", Colours::grey);
  preset_search_.setWantsKeyboardFocus(true);
  preset_search_.setText(last_preset_search_, false);
  preset_search_.onTextChange = [this] {
    last_preset_search_ = preset_search_.getText();
    if (!preset_list_loaded_) {
      ensurePresetListLoaded();
      return;
    }
    schedulePresetFilterUpdate(false);
  };
  addChildComponent(preset_search_);

  preset_table_model_ = std::make_unique<PresetResultsTableModel>(*this);
  preset_table_.setModel(preset_table_model_.get());
  preset_table_.setTitle("Preset results table");
  const String command = atlas::keys::commandName();
  preset_table_.setDescription("Sortable preset results with Name first. Press Return to load, F to change favourite, R to rename, E to edit metadata, S to choose the next sort column, Shift S to reverse the sort, or Delete to move an editable preset to Trash. " +
                               command + " S saves the patch, Shift " + command + " S saves it as a new preset.");
  preset_table_.setHelpText("Use arrow keys to select a preset. Return loads, F toggles favourite, R renames, E edits metadata, S chooses the next sort column, Shift S reverses the sort, and column headers can also be pressed to sort. " +
                            command + " S saves the patch and Shift " + command + " S saves it as a new preset.");
  preset_table_.setWantsKeyboardFocus(true);
  preset_table_.setRowHeight(30);
  preset_table_.setHeaderHeight(32);
  preset_table_.setMultipleSelectionEnabled(false);
  auto& preset_header = preset_table_.getHeader();
  preset_header.addColumn("Name", kPresetNameColumn, 220, 120, 480);
  preset_header.addColumn("Author", kPresetAuthorColumn, 140, 80, 260);
  preset_header.addColumn("Favourite", kPresetFavoriteColumn, 90, 70, 120);
  preset_header.addColumn("Tags", kPresetTagsColumn, 190, 100, 420);
  preset_header.addColumn("Category", kPresetCategoryColumn, 150, 90, 300);
  preset_header.addColumn("Bank", kPresetBankColumn, 150, 90, 300);
  preset_header.addColumn("Library", kPresetLibraryColumn, 100, 70, 180);
  preset_header.addColumn("Modified", kPresetModifiedColumn, 160, 110, 230);
  preset_header.setSortColumnId(preset_sort_column_, preset_sort_ascending_);
  preset_table_.onFocusGained = [this] { ensurePresetListLoaded(); };
  preset_table_.onKeyPressed = [this](const KeyPress& key) {
    if (key.getKeyCode() == KeyPress::returnKey) {
      loadSelectedPreset();
      return true;
    }
    const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
    const auto modifiers = key.getModifiers();
    const bool voiceover_character = modifiers.isCtrlDown() && modifiers.isAltDown() &&
                                     !modifiers.isCommandDown();
    const bool character_shortcut = (!modifiers.isCommandDown() && !modifiers.isCtrlDown() &&
                                     !modifiers.isAltDown()) || voiceover_character;
    if (character_shortcut && character == 'f') {
      toggleSelectedPresetFavorite();
      return true;
    }
    if (character_shortcut && character == 'r') {
      renameSelectedPreset();
      return true;
    }
    if (character_shortcut && character == 'e') {
      showPresetMetadataDialog();
      return true;
    }
    if (key.getKeyCode() == KeyPress::deleteKey) {
      deleteSelectedPreset();
      return true;
    }
    if (isContextMenuKey(key)) {
      showSelectedPresetMenu(preset_table_);
      return true;
    }
    if (character_shortcut && character == 's') {
      if (key.getModifiers().isShiftDown())
        preset_sort_ascending_ = !preset_sort_ascending_;
      else
        preset_sort_column_ = preset_sort_column_ >= kPresetModifiedColumn
                                ? kPresetNameColumn : preset_sort_column_ + 1;
      preset_table_.getHeader().setSortColumnId(preset_sort_column_, preset_sort_ascending_);
      postPluginAnnouncement("Sorted by " + preset_table_.getHeader().getColumnName(preset_sort_column_) +
                               (preset_sort_ascending_ ? ", ascending" : ", descending"),
                             AccessibilityHandler::AnnouncementPriority::medium);
      return true;
    }
    return false;
  };
  preset_table_.onSelectionKey = [this] {
    const int row = preset_table_.getSelectedRow();
    if (isPositiveAndBelow(row, filtered_presets_.size()))
      postWindowsReadout(listRowText(presetTableCellText(row, kPresetNameColumn), row, filtered_presets_.size()));
    else
      postWindowsReadout(preset_list_loaded_ ? "No matching presets" : "Preset list loading");
  };
  addChildComponent(preset_table_);

  preset_preview_.setTitle("Autoload preset when scrolling");
  preset_preview_.setDescription("Automatically load presets as you move through the preset list");
  preset_preview_.setHelpText("Turn this on to load presets while scrolling. Leave it off to load only when pressing Enter.");
  preset_preview_.setWantsKeyboardFocus(true);
  preset_preview_.setToggleState(last_preset_preview, dontSendNotification);
  preset_preview_.onClick = [this] {
    last_preset_preview = preset_preview_.getToggleState();
#if JUCE_WINDOWS
    // NVDA says nothing when a check box changes (see SpokenToggleButton), so
    // this is the only answer to the key press.
    postPresetActionAnnouncement(last_preset_preview ? "Preset autoload enabled" : "Preset autoload disabled",
                                 AccessibilityHandler::AnnouncementPriority::medium);
#else
    postPluginAnnouncement(last_preset_preview ? "Preset autoload enabled" : "Preset autoload disabled",
                                           AccessibilityHandler::AnnouncementPriority::medium);
#endif
  };
  addChildComponent(preset_preview_);

  preset_details_.setTitle("Selected preset details");
  preset_details_.setDescription("No preset selected");
  preset_details_.setColour(Label::textColourId, Colours::white);
  preset_details_.setFont(FontOptions(13.0f));
  preset_details_.setMinimumHorizontalScale(0.65f);
  preset_details_.setJustificationType(Justification::centredLeft);
  preset_details_.setAccessible(true);
  preset_details_.setWantsKeyboardFocus(true);
  addChildComponent(preset_details_);

  save_patch_prompt_.setTitle("Save patch");
  save_patch_prompt_.setDescription("Save the current Atlas patch");
  save_patch_prompt_.setColour(Label::textColourId, Colours::white);
  save_patch_prompt_.setJustificationType(Justification::centredLeft);
  addChildComponent(save_patch_prompt_);

  auto configureSaveEditor = [this](TextEditor& editor, const String& title, const String& empty_text) {
    editor.setTitle(title);
    editor.setDescription(title);
    editor.setHelpText("Type " + title.toLowerCase());
    editor.setTextToShowWhenEmpty(empty_text, Colours::grey);
    editor.setSelectAllWhenFocused(true);
    editor.setWantsKeyboardFocus(true);
    // Escape closes the form, as Cancel does; the text box keeps the key
    // from reaching SynthEditor::keyPressed and reports it a moment later.
    editor.onEscapeKey = [this] {
      if (save_patch_dialog_visible_)
        hideSavePresetDialog();
    };
    addChildComponent(editor);
  };
  configureSaveEditor(save_patch_name_, "Patch name", "Name");
  configureSaveEditor(save_patch_author_, "Patch author", "Author");
  configureSaveEditor(save_patch_bank_, "Patch bank", "Bank");
  configureSaveEditor(save_patch_category_, "Patch category", "Category");
  configureSaveEditor(save_patch_tags_, "Patch tags", "Tags separated by commas");
  configureSaveEditor(save_patch_comments_, "Patch comments", "Comments");
  save_patch_ok_.setTitle("Save patch");
  save_patch_ok_.setDescription("Save patch to the user library");
  save_patch_ok_.setHelpText("Press Enter to save the patch");
  save_patch_ok_.setWantsKeyboardFocus(true);
  save_patch_ok_.onClick = [this] { commitSavePresetDialog(); };
  addChildComponent(save_patch_ok_);
  save_patch_cancel_.setTitle("Cancel save patch");
  save_patch_cancel_.setDescription("Close the save patch form without saving");
  save_patch_cancel_.setHelpText("Press Enter or Escape to cancel");
  save_patch_cancel_.setWantsKeyboardFocus(true);
  save_patch_cancel_.onClick = [this] { hideSavePresetDialog(); };
  addChildComponent(save_patch_cancel_);

  routing_summary_.setTitle("Routing summary");
  routing_summary_.setDescription("Summary of oscillator, sample, filter, and effects routing");
  routing_summary_.setColour(Label::textColourId, Colours::white);
  routing_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(routing_summary_);

  routing_mode_.setTitle("Filter routing mode");
  routing_mode_.setDescription("Choose how filter 1 and filter 2 are connected");
  routing_mode_.setHelpText("Choose parallel filters, filter 1 into filter 2, or filter 2 into filter 1");
  routing_mode_.setWantsKeyboardFocus(true);
  routing_mode_.addItemList({ "Parallel filters", "Filter 1 into filter 2", "Filter 2 into filter 1" }, 1);
  routing_mode_.onChange = [this] {
    if (!updating_routing_controls_)
      applyRoutingPreset(routing_mode_.getSelectedItemIndex());
  };
  addChildComponent(routing_mode_);

  routing_default_.setTitle("Default routing");
  routing_default_.setDescription("Set filters to parallel routing and restore the default oscillator and sample destinations");
  routing_default_.setHelpText("Press Enter to apply default routing");
  routing_default_.setWantsKeyboardFocus(true);
  routing_default_.onClick = [this] { applyRoutingPreset(3); };
  addChildComponent(routing_default_);

  routing_serial_forward_.setTitle("Serial filters 1 into 2");
  routing_serial_forward_.setDescription("Send filter 1 into filter 2");
  routing_serial_forward_.setHelpText("Press Enter to route filter 1 into filter 2");
  routing_serial_forward_.setWantsKeyboardFocus(true);
  routing_serial_forward_.onClick = [this] { applyRoutingPreset(1); };
  addChildComponent(routing_serial_forward_);

  routing_serial_backward_.setTitle("Serial filters 2 into 1");
  routing_serial_backward_.setDescription("Send filter 2 into filter 1");
  routing_serial_backward_.setHelpText("Press Enter to route filter 2 into filter 1");
  routing_serial_backward_.setWantsKeyboardFocus(true);
  routing_serial_backward_.onClick = [this] { applyRoutingPreset(2); };
  addChildComponent(routing_serial_backward_);

  for (const auto& source : synth_.getEngine()->getModulationSources())
    modulation_source_ids_.add(source.first);
  std::vector<String> sorted_sources;
  for (const auto& id : modulation_source_ids_)
    sorted_sources.push_back(id);
  std::stable_sort(sorted_sources.begin(), sorted_sources.end(), [](const String& a, const String& b) {
    const int rank_a = modulationSourceSortRank(a);
    const int rank_b = modulationSourceSortRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return modulationSourceLabelForId(a).compareNatural(modulationSourceLabelForId(b)) < 0;
  });
  modulation_source_ids_.clear();
  for (const auto& id : sorted_sources)
    modulation_source_ids_.add(id);

  modulation_amount_prompt_.setTitle("Initial modulation amount");
  modulation_amount_prompt_.setDescription("Type the maximum modulation amount from minus one to one");
  modulation_amount_prompt_.setColour(Label::textColourId, Colours::white);
  modulation_amount_prompt_.setJustificationType(Justification::centredLeft);
  addChildComponent(modulation_amount_prompt_);

  modulation_amount_editor_.setTitle("Maximum modulation amount");
  modulation_amount_editor_.setDescription("Maximum modulation amount from minus one to one");
  modulation_amount_editor_.setHelpText("Type a value from minus one to one, then press Return or OK");
  modulation_amount_editor_.setWantsKeyboardFocus(true);
  modulation_amount_editor_.onReturnKey = [this] { applyInlineTextPrompt(); };
  // The field keeps Escape to itself, so the editor's own Escape key never sees it.
  modulation_amount_editor_.onEscapeKey = [this] {
    if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_)
      cancelInlineTextPrompt();
  };
  addChildComponent(modulation_amount_editor_);

  modulation_amount_ok_.setTitle("Set modulation amount");
  modulation_amount_ok_.setDescription("Apply the typed initial modulation amount");
  modulation_amount_ok_.setWantsKeyboardFocus(true);
  modulation_amount_ok_.onClick = [this] { applyInlineTextPrompt(); };
  addChildComponent(modulation_amount_ok_);

  modulation_amount_cancel_.setTitle("Cancel modulation amount");
  modulation_amount_cancel_.setDescription("Leave the modulation connected without changing the amount");
  modulation_amount_cancel_.setWantsKeyboardFocus(true);
  modulation_amount_cancel_.onClick = [this] { cancelInlineTextPrompt(); };
  addChildComponent(modulation_amount_cancel_);

  viewport_.setTitle("Section controls");
  viewport_.setDescription("Controls for the selected synthesizer section");
  viewport_.setViewedComponent(&rows_container_, false);
  addAndMakeVisible(viewport_);

  buildSections();
  const int imported = synth_.takeDownloadedPresetsImported();
  if (imported > 0)
    postPluginAnnouncement("Imported " + String(imported) + " new presets from Downloads folder",
                           AccessibilityHandler::AnnouncementPriority::high);
  if (section_names_.indexOf(last_section_name_) < 0 && section_names_.size() > 0)
    last_section_name_ = section_names_[0];
  showAllSections(false);
  rebuildFocusOrder();
  setResizable(true, true);
  // The full preset browser needs this height to keep its filters, results,
  // details, and autoload toggle simultaneously visible. Hosts may restore
  // an older, smaller editor size, so enforce the usable minimum here.
  setResizeLimits(900, 720, 1400, 1000);
  setSize(900, 720);
  startTimerHz(20);
}

SynthEditor::~SynthEditor() {
  stopTimer();
  // Key releases can no longer reach this editor; don't leave notes held.
  allQwertyNotesOff();
  detachQwertyKeyInterceptor();
  if (ir_browser_scan_) ir_browser_scan_->cancelled.store(true);
  if (ir_browser_scan_thread_.joinable()) ir_browser_scan_thread_.join();
  preset_scan_cancelled_->store(true);
  for (auto& scan : preset_scans_)
    if (scan.thread.joinable()) scan.thread.join();
  // Tear down dynamic controls while the focus lists are still alive.
  clearSectionComponents();
  preset_collection_table_.setModel(nullptr);
  preset_library_table_.setModel(nullptr);
  preset_bank_table_.setModel(nullptr);
  preset_category_table_.setModel(nullptr);
  preset_tag_table_.setModel(nullptr);
  preset_table_.setModel(nullptr);
}

void SynthEditor::paint(Graphics& graphics) {
  graphics.fillAll(Colour(0xff17191d));
}

void SynthEditor::resized() {
  auto bounds = getLocalBounds().reduced(16);
  title_.setBounds(bounds.removeFromTop(40));
  auto navigation = bounds.removeFromTop(38);
  menu_button_.setBounds(navigation.removeFromLeft(120).reduced(3));
  if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) {
    bounds.removeFromTop(8);
    modulation_amount_prompt_.setBounds(bounds.removeFromTop(34));
    auto amount_row = bounds.removeFromTop(38);
    modulation_amount_editor_.setBounds(amount_row.removeFromLeft(jmax(180, amount_row.getWidth() - 220)).reduced(3));
    modulation_amount_ok_.setBounds(amount_row.removeFromLeft(100).reduced(3));
    modulation_amount_cancel_.setBounds(amount_row.removeFromLeft(120).reduced(3));
  }
  if (save_patch_dialog_visible_) {
    bounds.removeFromTop(8);
    save_patch_prompt_.setBounds(bounds.removeFromTop(34));
    auto first_row = bounds.removeFromTop(38);
    save_patch_name_.setBounds(first_row.removeFromLeft(first_row.getWidth() / 2).reduced(3));
    save_patch_author_.setBounds(first_row.reduced(3));
    auto second_row = bounds.removeFromTop(38);
    save_patch_bank_.setBounds(second_row.removeFromLeft(second_row.getWidth() / 2).reduced(3));
    save_patch_category_.setBounds(second_row.reduced(3));
    auto third_row = bounds.removeFromTop(38);
    save_patch_tags_.setBounds(third_row.removeFromLeft(third_row.getWidth() / 2).reduced(3));
    save_patch_comments_.setBounds(third_row.reduced(3));
    auto dialog_actions = bounds.removeFromTop(38);
    save_patch_ok_.setBounds(dialog_actions.removeFromLeft(120).reduced(3));
    save_patch_cancel_.setBounds(dialog_actions.removeFromLeft(120).reduced(3));
  }
  bounds.removeFromTop(10);
  viewport_.setBounds(bounds);
}

void SynthEditor::buildSections() {
  effect_slot_modes_.clear();
  sections_.clear();
  section_names_.clear();
  group_sections_.clear();
  group_names_.clear();
  // A row a rebuild keeps (keepFocusedSwitchingRow) still uses its bridge, so
  // a control that is still listed keeps the bridge it had.
  std::map<String, std::unique_ptr<ValueBridge>> previous_bridges;
  if (kKeepSwitchingFocus) {
    for (auto& bridge : internal_fx_bridges_)
      previous_bridges[bridge->getParameterId()] = std::move(bridge);
  }
  internal_fx_bridges_.clear();
  parameters_by_id_.clear();

  sections_[kPresetSection];
  for (auto* parameter : synth_.getParameters()) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter)) {
      parameters_by_id_[bridge->getParameterId()] = bridge;
      const String section = sectionForParameter(bridge->getParameterId());
      if (effectIdForSection(section).isNotEmpty())
        continue;
      sections_[section].push_back(parameter);
    }
  }


  auto addInternalFxBridge = [this, &previous_bridges](const String& id, const String& section) {
    auto& controls = synth_.getControls();
    auto found = controls.find(id.toStdString());
    if (found == controls.end() || found->second == nullptr || !vital::Parameters::isParameter(id.toStdString()))
      return;

    std::unique_ptr<ValueBridge> bridge;
    if (const auto previous = previous_bridges.find(id); previous != previous_bridges.end() &&
        previous->second != nullptr && previous->second->engineControl() == found->second)
      bridge = std::move(previous->second);
    else {
      bridge = std::make_unique<ValueBridge>(id.toStdString(), found->second);
      bridge->setListener(&synth_);
      if (vital::suppressor::isFrequency(id.toStdString()))
        bridge->suppressorSampleRate = [this] { return synth_.getFxSampleRate(); };
    }
    auto* bridge_ptr = bridge.get();
    parameters_by_id_[id] = bridge_ptr;
    sections_[section].push_back(bridge_ptr);
    internal_fx_bridges_.push_back(std::move(bridge));
  };

  // Chain-order controls are part of the internal FX model too. They are no
  // longer enumerated to the host, but the accessible editor still needs the
  // same bridges for ordering and compatibility with old presets.
  for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
    const String prefix = chain == 0 ? String() : "bus_" + String(chain) + "_";
    const String section = chain == 0 ? String(kEffectsChainSection)
                                      : "Bus " + String(chain) + " - " + kEffectsChainSection;
    addInternalFxBridge(prefix + "effect_chain_order", section);
    addInternalFxBridge(prefix + "post_effect_order", section);
    for (int slot = 0; slot < vital::constants::kNumEffects; ++slot)
      addInternalFxBridge(prefix + "effect_chain_slot_" + String(slot + 1), section);
  }


  auto addDynamicFxSection = [this, &addInternalFxBridge](const String& section) {
    for (int slot = 0; slot < vital::constants::kMaxEffectSlots; ++slot) {
      const int type = effectSlotType(section, slot);
      if (type <= 0)
        continue;

      const String slot_prefix = effectSlotPrefix(section, slot);
      const String effect_prefix = slot_prefix + strings::kEffectOrder[type - 1] + "_";
      addInternalFxBridge(slot_prefix + "enabled", section);
      for (const auto& control : synth_.getControls()) {
        const String id(control.first);
        if (id.startsWith(effect_prefix) &&
            shouldExposeEffectSlotParameter(strings::kEffectOrder[type - 1],
                                            id.substring(effect_prefix.length())))
          addInternalFxBridge(id, section);
      }
    }
  };

  addDynamicFxSection(kEffectsChainSection);
  for (int bus = 1; bus <= vital::kNumBuses; ++bus)
    addDynamicFxSection("Bus " + String(bus) + " - " + kEffectsChainSection);


  // Oscillator slot parameters the host doesn't see individually (they are
  // automated through the oscillator automation proxies) still need rows.
  for (const auto& control : synth_.getControls()) {
    const String id(control.first);
    const int slot = sourceSlotForId(id);
    if (slot < 0 || parameters_by_id_.count(id))
      continue;
    addInternalFxBridge(id, "Oscillator " + String(slot + 1));
  }

  // Rank each parameter once: computing ranks inside the comparison made the
  // sort the costliest part of every preset load.
  for (auto& section : sections_) {
    std::vector<std::pair<int, AudioProcessorParameter*>> ranked;
    ranked.reserve(section.second.size());
    for (auto* parameter : section.second) {
      auto* bridge = dynamic_cast<ValueBridge*>(parameter);
      ranked.push_back({ bridge != nullptr ? parameterRank(bridge->getParameterId()) : std::numeric_limits<int>::max(),
                         parameter });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < ranked.size(); ++i)
      section.second[i] = ranked[i].second;
  }


  std::vector<String> names;
  for (const auto& section : sections_)
    names.push_back(section.first);
  auto dynamic_section_rank = [this](const String& section) {
    const String effect_id = effectIdForSection(section);
    if (effect_id.isEmpty())
      return sectionRank(section);

    int effect_index = -1;
    for (int i = 0; i < vital::constants::kNumEffects; ++i) {
      if (effect_id == String(strings::kEffectOrder[i])) {
        effect_index = i;
        break;
      }
    }
    if (effect_index < 0)
      return sectionRank(section);

    int order[vital::constants::kNumEffects];
    readEffectChainOrder(section, order);
    int position = vital::constants::kNumEffects;
    for (int i = 0; i < vital::constants::kNumEffects; ++i) {
      if (order[i] == effect_index) {
        position = i;
        break;
      }
    }

    if (section.startsWith("Bus 1 - "))
      return 220 + sectionRank(kEffectsChainSection) + 1 + position;
    if (section.startsWith("Bus 2 - "))
      return 250 + sectionRank(kEffectsChainSection) + 1 + position;
    if (section.startsWith("Bus 3 - "))
      return 280 + sectionRank(kEffectsChainSection) + 1 + position;
    return sectionRank(kEffectsChainSection) + 1 + position;
  };
  std::stable_sort(names.begin(), names.end(), [&](const String& a, const String& b) {
    const int rank_a = dynamic_section_rank(a);
    const int rank_b = dynamic_section_rank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return a < b;
  });
  for (const auto& name : names)
    section_names_.add(name);
  buildGroups();
}

void SynthEditor::buildGroups() {
  group_sections_.clear();
  group_names_.clear();

  for (const auto& section : section_names_)
    group_sections_[groupForSection(section)].add(section);

  std::vector<String> names;
  for (const auto& group : group_sections_)
    names.push_back(group.first);
  std::stable_sort(names.begin(), names.end(), [](const String& a, const String& b) {
    const int rank_a = groupRank(a);
    const int rank_b = groupRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return a < b;
  });
  for (const auto& name : names)
    group_names_.add(name);

  // Rebuilding (after a preset load or an effect change) keeps the group the
  // user is in instead of jumping back to the first group.
  current_group_ = groupForSection(last_section_name_);
  if (!group_names_.contains(current_group_))
    current_group_ = group_names_.isEmpty() ? String() : group_names_[0];
}

void SynthEditor::selectSectionByName(const String& section, bool announce) {
  if (!section_names_.contains(section))
    return;

  last_section_name_ = section;
  if (row_focus_order_.empty())
    showAllSections(false);
  // On Windows a bus's chain is reached at its Bus heading (the group search
  // below) and called by the bus: the chain is all the Bus group holds, and
  // its own name, "Chain and routing", is the same on every bus.
  const bool bus_heading = kBusChainsStepToTheirHeading && isEffectChainSection(section) &&
                           fxBusForSection(section) > 0;
  Component* target = nullptr;
  if (!bus_heading) {
    for (auto* component : row_focus_order_) {
      if (component != nullptr && component->getProperties()["SectionName"].toString() == section) {
        target = component;
        break;
      }
    }
  }
  if (target == nullptr && !bus_heading) {
    const String title = accessibleSectionTitle(section);
    for (auto* component : row_focus_order_) {
      if (component != nullptr && component->getTitle() == title) {
        target = component;
        break;
      }
    }
  }
  if (target == nullptr) {
    const String group = groupForSection(section);
    for (auto* component : row_focus_order_) {
      if (component != nullptr && component->getTitle() == group) {
        target = component;
        break;
      }
    }
  }
  if (target != nullptr) {
    ensureComponentVisible(target);
    target->grabKeyboardFocus();
  }
  if (announce) {
    if (bus_heading)
      postNavigationAnnouncement(fxBusNameForSection(section));
    else
      postPluginAnnouncement(accessibleSectionTitle(section), AccessibilityHandler::AnnouncementPriority::high);
  }
}

std::unique_ptr<AccessibleParameterRow> SynthEditor::createAccessibleParameterRow(AudioProcessorParameter& parameter,
                                                                                  const String& sectionName) {
  // Built even when a kept row takes its place, for what building records
  // (the choices the editor watches for changes).
  auto row = buildAccessibleParameterRow(parameter, sectionName);
  if (kept_switching_row_ == nullptr || row == nullptr || &row->parameter() != kept_switching_parameter_ ||
      row->parameterId() != kept_switching_parameter_id_)
    return row;
  if (!kept_switching_row_->readsLike(*row)) {
    // The choice reads differently now: its fresh row replaces it.
    kept_switching_control_ = nullptr;
    kept_switching_row_.reset();
    return row;
  }
  kept_switching_row_->onValueChange = row->onValueChange;
  kept_switching_row_->restoreBuiltName();
  kept_switching_row_->refresh();
  return std::move(kept_switching_row_);
}

std::unique_ptr<AccessibleParameterRow> SynthEditor::buildAccessibleParameterRow(AudioProcessorParameter& parameter,
                                                                                 const String& sectionName) {
  auto* bridge = dynamic_cast<ValueBridge*>(&parameter);
  if (bridge == nullptr)
    return std::make_unique<AccessibleParameterRow>(parameter);

  const String parameter_id = bridge->getParameterId();
  const String legacy_id = legacySourceId(parameter_id);
  if (parameter_id == "volume")
    return std::make_unique<AccessibleParameterRow>(parameter, "Global Gain");
  const String suffix = filterSuffixForParameter(parameter_id);
  if (arpeggiatorForId(parameter_id) > 0) {
    String title = arpeggiatorControlTitle(parameter_id);
    if (title.isNotEmpty()) {
      // "Destination oscillator 5, sample": the slot's type says what the arp plays.
      if (parameter_id.contains("_destination_osc_")) {
        const int slot = parameter_id.fromLastOccurrenceOf("_", false, false).getIntValue() - 1;
        if (isPositiveAndBelow(slot, vital::kNumOscillators))
          title += ", " + String(strings::kOscillatorTypeNames[sourceSlotType(slot)]).toLowerCase();
      }
      return std::make_unique<AccessibleParameterRow>(parameter, title);
    }
  }

  if (parameter_id == "stereo_mode") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Stereo mode",
        [](double value) { return value >= 0.5 ? "Rotate" : "Spread"; }, 1.0, 0.0,
        String(), String(),
        "Choose how the final stereo routing control treats the left and right channels", true);
  }

  if (isTransposeQuantizeParameter(parameter_id)) {
    return std::make_unique<AccessibleParameterRow>(parameter, "Transpose quantize",
                                                    [bridge](double value) {
                                                      const int quantize = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return transposeQuantizeDescription(quantize);
                                                    });
  }

  if (isTransposeQuantizeModeParameter(parameter_id)) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, transposeQuantizeModeTitleForParameter(parameter_id),
        [](double value) { return value >= 0.5 ? "Global scale" : "Local scale"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether transpose quantize uses this source's scale or the global scale", true);
  }

  if (isEffectChoiceParameter(parameter_id, "eq_low_mode")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "EQ low band mode",
        [](double value) { return value >= 0.5 ? "High pass" : "Shelf"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether the low EQ band is a shelf or high pass filter", true);
  }

  if (isEffectChoiceParameter(parameter_id, "eq_band_mode")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "EQ band 1 mode",
        [](double value) { return value >= 0.5 ? "Notch" : "Shelf"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether EQ band 1 is a shelf or notch filter", true);
  }

  for (int band = 2; band <= 4; ++band) {
    if (isEffectChoiceParameter(parameter_id, "eq_band_" + String(band) + "_mode")) {
      return std::make_unique<AccessibleParameterRow>(
          parameter, "EQ band " + String(band) + " mode",
          [](double value) { return value >= 0.5 ? "Notch" : "Shelf"; }, 1.0, 0.0,
          String(), String(),
          "Choose whether EQ band " + String(band) + " is a shelf or notch filter", true);
    }
  }

  if (isEffectChoiceParameter(parameter_id, "eq_high_mode")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "EQ high band mode",
        [](double value) { return value >= 0.5 ? "Low pass" : "Shelf"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether the high EQ band is a shelf or low pass filter", true);
  }

  if (isEffectChoiceParameter(parameter_id, "waveshaper_mode")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Waveshaper mode",
        [](double value) { return value >= 0.5 ? "Bipolar" : "Unipolar"; }, 1.0, 0.0,
        String(), String(),
        "Choose symmetric unipolar shaping or full-range bipolar shaping for asymmetric distortion", true);
  }

  if (parameter_id == "release_velocity_mode" || parameter_id == "release_velocity_target" ||
      isEffectChoiceParameter(parameter_id, "dimension_expander_mode") ||
      isEffectChoiceParameter(parameter_id, "erosion_mode") ||
      isEffectChoiceParameter(parameter_id, "resonator_note_priority")) {
    // These are named two-way choices, not On/Off switches.
    return std::make_unique<AccessibleParameterRow>(parameter, String(),
        std::function<String(double)>(), 1.0, 0.0, String(), String(), String(), true);
  }

  if (isEffectChoiceParameter(parameter_id, "utility_filter_slope") ||
      legacy_id == "noise_filter_slope" || legacy_id == "sample_filter_slope") {
    return std::make_unique<AccessibleParameterRow>(parameter, String(),
        std::function<String(double)>(), 1.0, 0.0, String(), String(),
        "Choose a slope from 6 to 48 dB per octave for the low cut and high cut filters", true);
  }

  if (legacy_id == "granular_mode") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Granular playback mode",
        [](double value) { return value >= 0.5 ? "Manual position" : "Play-through"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether the granular oscillator plays through the sample or uses the position control", true);
  }

  if (legacy_id == "granular_size_mode") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Granular size mode",
        [](double value) { return value >= 0.5 ? "Grid steps" : "Milliseconds"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether grain size is set in milliseconds or as a multiple of the tempo grid step", true);
  }

  if (legacy_id == "granular_midi_density") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Granular MIDI density control",
        [](double value) { return value >= 0.5 ? "MIDI note rate" : "Density knob"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether grain density follows the density knob or the incoming MIDI note rate", true);
  }

  if (legacy_id == "sample_root_key" || legacy_id == "granular_root_key") {
    const String title = legacy_id == "sample_root_key" ? "Sample root key" : "Granular root key";
    return std::make_unique<AccessibleParameterRow>(
        parameter, title,
        [](double value) { return midiRootKeyText(value); }, 1.0, 0.0,
        String(), String(),
        "Set the MIDI note that plays the loaded sample at its original pitch", true);
  }

  if (isSampleLoopPointParameter(parameter_id)) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, bridge->getName(128),
        [this, bridge](double value) -> String {
          const int length = sampleLoopPointLength(bridge->getParameterId());
          if (fine_tune_sample_loop_ && length > 0)
            return String(juce::roundToInt(value * length));
          return accessibleParameterText(*bridge, value);
        });
  }

  if (parameter_id.startsWith("random_")) {
    const String suffix = parameter_id.fromFirstOccurrenceOf("random_", false, false)
                                      .fromFirstOccurrenceOf("_", false, false);
    if (suffix == "style")
      return std::make_unique<AccessibleParameterRow>(parameter, "Style");
    if (suffix == "frequency")
      return std::make_unique<AccessibleParameterRow>(parameter, "Rate");
    if (suffix == "rate_x10")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "10x rate",
          [bridge](double value) { return bridge->getText(static_cast<float>(value), 128); }, 1.0, 0.0,
          String(), String(),
          "Multiply this random LFO rate by ten", true);
    if (suffix == "sync")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "Tempo sync type",
          [bridge](double value) { return bridge->getText(static_cast<float>(value), 128); }, 1.0, 0.0,
          String(), String(),
          "Choose seconds, tempo, dotted tempo, triplet tempo, or keytrack timing", true);
    if (suffix == "tempo")
      return std::make_unique<AccessibleParameterRow>(parameter, "Tempo");
    if (suffix == "sync_type")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "Transport sync",
          [](double value) { return value >= 0.5 ? "Host synced" : "Free running"; }, 1.0, 0.0,
          String(), String(),
          "Choose whether the random LFO follows host transport sync or runs freely", true);
    if (suffix == "stereo")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "Stereo mode",
          [](double value) { return value >= 0.5 ? "Independent stereo" : "Mono"; }, 1.0, 0.0,
          String(), String(),
          "Choose whether random values are shared or independent between stereo channels", true);
    if (suffix == "keytrack_transpose")
      return std::make_unique<AccessibleParameterRow>(parameter, "Keytrack transpose");
    if (suffix == "keytrack_tune")
      return std::make_unique<AccessibleParameterRow>(parameter, "Keytrack tune");
  }

  if (parameter_id == "filter_1_filter_input") {
    auto row = std::make_unique<AccessibleParameterRow>(
        parameter, "Filter 1 input from Filter 2",
        [](double value) {
          return value >= 0.5 ? "Filter 2 feeds Filter 1" : "Filter 1 does not receive Filter 2";
        }, 1.0, 0.0,
        String(), String(),
        "Route Filter 2 into Filter 1. Sources assigned to both filters enter Filter 2 once; only Filter 1 reaches the output destination.", true);
    row->onValueChange = [this, &parameter] {
      if (parameter.getValue() >= 0.5f) setParameterEngineValue("filter_2_filter_input", 0.0f);
      for (auto& row : rows_) row->refresh();
      refreshRoutingControls();
    };
    return row;
  }

  if (parameter_id == "filter_2_filter_input") {
    auto row = std::make_unique<AccessibleParameterRow>(
        parameter, "Filter 2 input from Filter 1",
        [](double value) {
          return value >= 0.5 ? "Filter 1 feeds Filter 2" : "Filter 2 does not receive Filter 1";
        }, 1.0, 0.0,
        String(), String(),
        "Route Filter 1 into Filter 2. Sources assigned to both filters enter Filter 1 once; only Filter 2 reaches the output destination.", true);
    row->onValueChange = [this, &parameter] {
      if (parameter.getValue() >= 0.5f) setParameterEngineValue("filter_1_filter_input", 0.0f);
      for (auto& row : rows_) row->refresh();
      refreshRoutingControls();
    };
    return row;
  }

  const int macro_index = macroIndexForControlId(parameter_id);
  if (macro_index >= 0) {
    const String macro_name = synth_.getMacroName(macro_index);
    return std::make_unique<AccessibleParameterRow>(parameter, macro_name,
                                                    [this, macro_index, bridge](double value) {
                                                      if (isMacroBipolar(macro_index)) {
                                                        const int percent = roundToInt((value * 2.0 - 1.0) * 100.0);
                                                        return String(percent) + "%";
                                                      }
                                                      return bridge->getText(static_cast<float>(value), 128);
                                                    });
  }

  if (parameter_id.startsWith("osc_") && parameter_id.endsWith("_distortion_type")) {
    const int oscillator = parameter_id.substring(4).getIntValue() - 1;
    const bool secondary = parameter_id.contains("_secondary_");
    const String title = secondary ? "Secondary distortion mode" : "Distortion mode";
    if (isPositiveAndBelow(oscillator, vital::kNumOscillators) && isAnalogOscillator(oscillator)) {
      const int waveform = oscillatorModeKey(oscillator) % 64;
      auto text = [bridge](double value) {
        const int type = roundToInt(bridge->convertToEngineValue(static_cast<float>(value)));
        if (type == vital::SynthOscillator::kSync)
          return String("Hard Sync");
        return accessibleParameterText(*bridge, value);
      };
      auto row = std::make_unique<AccessibleParameterRow>(
          parameter, title, text, 1.0, 0.0, String(), String(),
          "Choose Hard Sync, an FM source, or a ring modulation source. FM drives the oscillator's phase, "
          "Hard Sync restarts it with its own note, and ring modulation shapes its output; the sub is unaffected. "
          "For Hard Sync, Distortion Amount 50% keeps the pitch and 100% syncs four octaves up. "
          "Wavetable warps stay stored for the Wavetable type and read as None here.");
      // Warps kept from the Wavetable type do nothing here, so they read as None.
      row->setIndexedValueOrder(analogDistortionOrder(waveform), 0);
      return row;
    }
    const int slot_type = sourceSlotType(oscillator);
    if (isPositiveAndBelow(oscillator, vital::kNumOscillators) &&
        !vital::source_types::describe(slot_type).uses_wavetable) {
      const bool fm = vital::source_types::supportsFm(slot_type);
      auto text = [bridge, fm](double value) {
        const int type = roundToInt(bridge->convertToEngineValue(static_cast<float>(value)));
        if (vital::SynthOscillator::modulatorPosition(type) < 0 || (!fm && vital::SynthOscillator::isFmType(type)))
          return String("None");
        return accessibleParameterText(*bridge, value);
      };
      const String type_name = String(strings::kOscillatorTypeNames[slot_type]).toLowerCase();
      const String help = slot_type == vital::source_types::kSpeech
          ? String("Choose an FM source or a ring modulation source from another oscillator. FM bends the "
                   "voice's pitch at audio rate while its formants and the syllable's timing stay put, and "
                   "ring modulation multiplies its output by the source. Wavetable warps stay stored for the "
                   "Wavetable type and read as None here.")
          : fm
          ? "Choose an FM source or a ring modulation source from another oscillator. FM modulates this " +
            type_name + " oscillator's playback rate at audio rate, running it backwards at high amounts, and "
            "ring modulation multiplies its output by the source. Wavetable warps stay stored for the "
            "Wavetable type and read as None here."
          : "Choose a ring modulation source from another oscillator: this " + type_name +
            " oscillator's output is multiplied by the source. A " + type_name + " oscillator cannot be "
            "frequency modulated, so FM modes and wavetable warps read as None here.";
      auto row = std::make_unique<AccessibleParameterRow>(
          parameter, title, text, 1.0, 0.0, String(), String(), help);
      row->setIndexedValueOrder(slotDistortionOrder(slot_type), 0);
      return row;
    }
    auto row = std::make_unique<AccessibleParameterRow>(
        parameter, parameter_id.contains("_secondary_") ? "Secondary distortion mode" : "Distortion mode",
        std::function<String(double)>(), 1.0, 0.0,
        String(), String(),
        "Choose a native warp, an FM source, or a ring modulation source");
    row->setIndexedValueOrder(std::vector<int>(vital::SynthOscillator::kDistortionPresentationOrder.begin(),
                                               vital::SynthOscillator::kDistortionPresentationOrder.end()));
    return row;
  }

  if (parameter_id.startsWith("osc_")) {
    const String prefix = parameter_id.upToFirstOccurrenceOf("_", true, false) +
                          parameter_id.fromFirstOccurrenceOf("_", false, false).upToFirstOccurrenceOf("_", true, false);
    const String suffix = parameter_id.substring(prefix.length());
    const int oscillator = prefix.fromFirstOccurrenceOf("osc_", false, false).getIntValue() - 1;
    if (isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
      if (suffix == "type") {
        last_oscillator_modes_[prefix] = oscillatorModeKey(oscillator);
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "Choose this oscillator's type: Wavetable, Analog, Sample, Granular, Spectral, Noise, Multi-sample, Braids, "
            "OpenWurli or OpenB3. "
            "Each type keeps its own settings, wavetable and sample for when you switch back.", true);
      }
      if (suffix == "analog_waveform")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "Choose Sine, Triangle, Saw, Square, White Noise or Pitched Noise. Pitched Noise is chip-style noise that follows the note.");
      if (suffix == "analog_shape") {
        const int waveform = roundToInt(engineValueFor(parameterBridge(prefix + "analog_waveform")));
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            analogShapeDescription(waveform));
      }
      if (suffix == "sub_level")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "Level of the sub oscillator, one or two octaves below. It stays centred, ignores unison, "
            "and follows this oscillator's Level and Pan.");
      if (suffix == "sub_octave")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "Play the sub oscillator one or two octaves below the note.", true);
      if (suffix == "sub_waveform")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "Choose Sine, Triangle or Square for the sub oscillator.");
      if (suffix == "analog_drift")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "How much the pitch wanders over seconds, with a little fast jitter, as an old oscillator "
            "does when it warms and cools. Each voice and each unison oscillator drifts on its own. "
            "At 0% the pitch is steady.");
      if (suffix == "analog_voice_spread")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "How far each voice is off from the others in pitch, level and sub level, like the voice cards "
            "of an old polysynth. The offsets stay the same each time the preset loads. "
            "At 0% every voice matches.");
      if (suffix == "analog_wear")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "How worn the waveform circuits are. Edges soften, most on high notes, the two halves of the "
            "wave become uneven, and the square's pulse width and the triangle's slopes drift off true. "
            "At 0% the waveforms are clean.");
      if (suffix == "analog_sag")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "How much a tired power supply droops when many voices play. Big chords go a little flat "
            "and quiet, then recover as voices end. At 0% the supply holds steady.");
      if (suffix == "analog_noise")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "Hiss under each note. In the top half, dirty contacts add crackle. It follows this "
            "oscillator's Level and the note's envelope. At 0% there is no noise.");
      if (suffix == "analog_hum")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "Mains hum under each note, mostly at twice the Hum Frequency, as from a worn power supply. "
            "It follows this oscillator's Level and the note's envelope. At 0% there is no hum.");
      if (suffix == "analog_hum_frequency")
        return std::make_unique<AccessibleParameterRow>(
            parameter, String(), std::function<String(double)>(), 1.0, 0.0, String(), String(),
            "The mains frequency of the hum: 50 Hz as in Europe, or 60 Hz as in North America. "
            "Hum sets how loud it is.", true);
    }
  }

  if (parameter_id.startsWith("osc_") && parameter_id.endsWith("_spectral_morph_type"))
    last_random_amplitude_modes_[parameter_id] =
        static_cast<int>(engineValueFor(bridge)) == vital::SynthOscillator::kRandomAmplitudes;

  if (parameter_id.startsWith("osc_") && parameter_id.endsWith("_spectral_morph_seed"))
    return std::make_unique<AccessibleParameterRow>(parameter, bridge->getDetails().local_description);

  if (parameter_id.startsWith("osc_") && parameter_id.contains("_secondary_"))
    return std::make_unique<AccessibleParameterRow>(parameter, bridge->getDetails().local_description);

  if (isEffectChoiceParameter(parameter_id, "delay_style")) {
    auto row = std::make_unique<AccessibleParameterRow>(parameter, "Style");
    // Presentation only: saved/automated values 3 (Mid) and 4 (L-R) stay intact.
    row->setIndexedValueOrder({ vital::StereoDelay::kMono, vital::StereoDelay::kStereo,
                               vital::StereoDelay::kPingPong, vital::StereoDelay::kPingPongLR,
                               vital::StereoDelay::kMidPingPong });
    return row;
  }

  if ((parameter_id.startsWith("osc_") && parameter_id.endsWith("_destination")) ||
      legacy_id == "sample_destination" || legacy_id == "granular_destination" ||
      legacy_id == "spectral_destination" || legacy_id == "noise_destination") {
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kBus3));
    auto row = std::make_unique<AccessibleParameterRow>(parameter, "Destination",
                                                        [bridge](double value) {
                                                          const int destination = static_cast<int>(std::round(
                                                              bridge->convertToEngineValue(static_cast<float>(value))));
                                                          return routingDestinationText(destination);
                                                        },
                                                        max_normalized);
    row->setIndexedValueOrder(std::vector<int>(
        std::begin(vital::constants::kSourceDestinationPresentationOrder),
        std::end(vital::constants::kSourceDestinationPresentationOrder)));
    return row;
  }

  if (parameter_id.startsWith("bus_") && parameter_id.endsWith("_destination")) {
    const String number = parameter_id.fromFirstOccurrenceOf("bus_", false, false)
                          .upToFirstOccurrenceOf("_", false, false);
    const int bus_index = jlimit(0, vital::kNumBuses - 1, number.getIntValue() - 1);
    return std::make_unique<AccessibleParameterRow>(parameter, "Destination",
                                                    [bridge, bus_index](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return busRoutingDestinationText(bus_index, destination);
                                                    });
  }

  if (parameter_id == "filter_1_destination" || parameter_id == "filter_2_destination") {
    const double min_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kEffects));
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kBus3));
    const String filter_name = parameter_id == "filter_1_destination" ? "Filter 1 destination"
                                                                      : "Filter 2 destination";
    auto row = std::make_unique<AccessibleParameterRow>(parameter, filter_name,
                                                    [bridge](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return routingDestinationText(destination);
                                                    },
                                                    max_normalized, min_normalized);
    // Keep bus choices together without changing saved or automated values.
    row->setIndexedValueOrder({ vital::constants::kEffects, vital::constants::kBus1,
                               vital::constants::kBus2, vital::constants::kBus3,
                               vital::constants::kDirectOut });
    return row;
  }

  if (suffix.isEmpty())
    return std::make_unique<AccessibleParameterRow>(parameter,
                                                    accessibleParameterTitle(sectionName, parameter.getName(128)));

  const String prefix = filterPrefixForParameter(parameter_id);
  int model = 0;
  if (auto* model_bridge = parameterBridge(prefix + "model"))
    model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));

  if (suffix == "model") {
    const int style = static_cast<int>(std::round(engineValueFor(parameterBridge(prefix + "style"))));
    last_filter_states_[prefix] = { model, style };
    return std::make_unique<AccessibleParameterRow>(parameter, "Filter model");
  }

  if (suffix == "style") {
    const bool filter_fx = prefix.contains("filter_fx");
    const int max_style = jmax(0, numFilterStylesForModel(model, filter_fx) - 1);
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(max_style));
    const String title = model == vital::constants::kFilterTable ? "Phase mode"
                       : model == vital::constants::kDistortionFilter ? "Distortion type"
                                                                        : "Filter style";
    return std::make_unique<AccessibleParameterRow>(parameter, title,
                                                    [bridge, model, filter_fx](double value) {
                                                      const int style = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return filterStyleNameForModel(model, style, filter_fx);
                                                    },
                                                    max_normalized);
  }
  if (suffix == "table_frame")
    return std::make_unique<AccessibleParameterRow>(parameter, "Wavetable frame");

  if (suffix == "blend") {
    const String name = model == vital::constants::kComb ? "Comb blend"
                      : model == vital::constants::kSampleHoldFilter ? "Bit crush"
                                                                     : "Filter blend, low pass to high pass";
    return std::make_unique<AccessibleParameterRow>(parameter, name,
                                                    [model](double value) { return filterBlendText(model, value); });
  }

  return std::make_unique<AccessibleParameterRow>(parameter,
                                                  accessibleParameterTitle(sectionName, parameter.getName(128)));
}

int SynthEditor::sampleLoopPointLength(const String& parameterId) const {
  const int slot = sourceSlotForId(parameterId);
  auto* sample = isPositiveAndBelow(slot, vital::kNumOscillators) ? synth_.getSample(slot) : nullptr;
  return sample != nullptr ? sample->originalLength() : 0;
}

void SynthEditor::toggleSampleLoopFineTune() {
  fine_tune_sample_loop_ = !fine_tune_sample_loop_;
  for (auto& row : rows_) {
    if (row != nullptr && isSampleLoopPointParameter(row->parameterId()))
      row->notifyDisplayChanged();
  }
  postPluginAnnouncement(String("Sample loop fine tune ") +
                             (fine_tune_sample_loop_ ? "on, sample numbers" : "off, percent"),
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::wireSampleLoopFineTune(AccessibleParameterRow& row) {
  if (!isSampleLoopPointParameter(row.parameterId()))
    return;
  row.setFineTuneCallbacks([this] { return fine_tune_sample_loop_; },
                           [this] { toggleSampleLoopFineTune(); });
}

bool SynthEditor::shouldShowParameterInSection(const String& sectionName, AudioProcessorParameter* parameter) const {
  auto* bridge = dynamic_cast<ValueBridge*>(parameter);
  if (bridge == nullptr)
    return true;

  const String parameter_id = bridge->getParameterId();
  if (parameter_id == "beats_per_minute" || parameter_id == "view_spectrogram" ||
      parameter_id.endsWith("_view_2d") || isEffectChoiceParameter(parameter_id, "utility_input_gain"))
    return false;

  // The zone crossfade only applies while glide zone crossing is enabled.
  if (parameter_id == "zone_crossfade") {
    if (auto* glide_bridge = parameterBridge("portamento_glide_zones"))
      return glide_bridge->convertToEngineValue(glide_bridge->getValue()) != 0.0f;
    return false;
  }

  if (isMacroBipolarParameterId(bridge->getParameterId()))
    return false;

  // Envelope 1's analog mode uses RC curves instead of the Power settings,
  // and only shows its Age while it is on.
  if (parameter_id.startsWith("env_1_")) {
    bool analog = false;
    if (auto* analog_bridge = parameterBridge("env_1_analog"))
      analog = analog_bridge->convertToEngineValue(analog_bridge->getValue()) >= 0.5f;
    if (parameter_id == "env_1_analog_age")
      return analog;
    if (parameter_id.endsWith("_power"))
      return !analog;
  }

  for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
    const String prefix = "osc_" + String(osc) + "_";
    if (!parameter_id.startsWith(prefix))
      continue;
    const String suffix = parameter_id.substring(prefix.length());
    const int type = sourceSlotType(osc - 1);
    // Each type shows only its own parameters; slot-level controls always show.
    const int owner = vital::source_types::typeForParameterSuffix(suffix.toStdString());
    if (owner >= 0)
      return vital::source_types::shareModule(owner, type);
    if (isSlotLevelOscillatorSuffix(suffix))
      return true;
    // Sample, Granular, Spectral and Noise read the slot's distortion mode and
    // amount for FM/RM from other oscillators; unison spread is wavetable-only.
    // Types without slot FM or RM (Multi-sample) have no distortion rows.
    if (!vital::source_types::describe(type).uses_wavetable)
      return isOscillatorDistortionSuffix(suffix) && !suffix.endsWith("distortion_spread") &&
             (vital::source_types::supportsFm(type) || vital::source_types::supportsRm(type));
    const bool analog = isAnalogOscillator(osc - 1);
    if (isAnalogOnlyOscillatorSuffix(suffix))
      return analog;
    if (analog && isWavetableOnlyOscillatorSuffix(suffix))
      return false;
    if (analog && isOscillatorDistortionSuffix(suffix))
      return oscillatorModeKey(osc - 1) % 64 != vital::analog::kWhiteNoise;
    break;
  }

  if (parameter_id.startsWith("osc_") && parameter_id.endsWith("_spectral_morph_seed")) {
    const String mode_id = parameter_id.dropLastCharacters(4) + "type";
    return static_cast<int>(engineValueFor(parameterBridge(mode_id))) == vital::SynthOscillator::kRandomAmplitudes;
  }

  const String suffix = filterSuffixForParameter(parameter_id);
  if (suffix.isEmpty())
    return true;
  if (!isFilterParameterSuffix(suffix))
    return false;

  const String prefix = filterPrefixForParameter(parameter_id);
  int model = 0;
  int style = 0;
  if (auto* model_bridge = parameterBridge(prefix + "model"))
    model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));
  else if (const auto found = synth_.getControls().find((prefix + "model").toStdString());
           found != synth_.getControls().end() && found->second != nullptr)
    model = static_cast<int>(std::round(found->second->value()));   // effect slot filters have no host bridge
  if (auto* style_bridge = parameterBridge(prefix + "style"))
    style = static_cast<int>(std::round(style_bridge->convertToEngineValue(style_bridge->getValue())));
  else if (const auto found = synth_.getControls().find((prefix + "style").toStdString());
           found != synth_.getControls().end() && found->second != nullptr)
    style = static_cast<int>(std::round(found->second->value()));
  if (model == vital::constants::kFormant)
    style = jlimit(0, vital::FormantFilter::kNumFormantStyles - 1, style);

  const bool reverb = model == vital::constants::kReverbFilter;
  const bool scream = model == vital::constants::kScreamFilter;
  // Wavetable model: Cutoff, Resonance, Key Track, Phase mode (Style), Wavetable Frame and Mix.
  const bool table = model == vital::constants::kFilterTable;
  if (suffix == "table_frame")
    return table;
  if (table && (suffix == "drive" || suffix == "blend" || suffix == "blend_transpose"))
    return false;
  if (suffix == "scream_amount" || suffix == "scream_input_gain" || suffix == "scream_output_gain" || suffix == "scream_autogain")
    return scream;
  if (scream && (suffix == "style" || suffix == "drive" || suffix == "blend"))
    return false;
  if (suffix == "reverb_cutoff" || suffix == "reverb_damp" || suffix == "reverb_output" || suffix == "reverb_bass_preserve")
    return reverb;
  if (reverb && (suffix == "cutoff" || suffix == "style" || suffix == "drive" || suffix == "blend"))
    return false;
  // Ring Mod uses Cutoff as its carrier frequency and Style as its waveform.
  if (model == vital::constants::kRingModFilter &&
      (suffix == "resonance" || suffix == "drive" || suffix == "blend" || suffix == "blend_transpose"))
    return false;
  // Sample & Hold uses Cutoff as its hold rate and Blend as bit crush.
  if (model == vital::constants::kSampleHoldFilter &&
      (suffix == "resonance" || suffix == "drive" || suffix == "blend_transpose"))
    return false;
  // Distortion has its own drive; Cutoff, Resonance and Blend set its filter.
  const bool distortion = model == vital::constants::kDistortionFilter;
  if (suffix == "distortion_drive" || suffix == "distortion_filter_order")
    return distortion;
  if (distortion && (suffix == "drive" || suffix == "blend_transpose"))
    return false;
  const bool formant = model == vital::constants::kFormant;
  const bool vocal_tract = formant && style == vital::FormantFilter::kVocalTract;
  const bool comb = model == vital::constants::kComb;

  if (suffix == "cutoff" || suffix == "resonance" || suffix == "keytrack")
    return !formant;
  if (suffix == "drive")
    return !formant && !comb;
  if (suffix == "blend")
    return !formant || vocal_tract;
  if (suffix == "blend_transpose")
    return comb;
  if (suffix == "formant_x" || suffix == "formant_y" ||
      suffix == "formant_resonance" || suffix == "formant_spread")
    return formant;
  if (suffix == "formant_transpose")
    return formant && !vocal_tract;

  return true;
}

Component* SynthEditor::controlForParameter(const String& parameterId) const {
  for (const auto& row : rows_)
    if (row != nullptr && row->parameterId() == parameterId)
      return row->focusableControl();
  return nullptr;
}

String SynthEditor::focusedParameterId() const {
  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl()->hasKeyboardFocus(true))
      return row->parameterId();
  }
  return {};
}

String SynthEditor::focusedAccessibleTitle() const {
  auto* focused = Component::getCurrentlyFocusedComponent();
  if (focused == nullptr)
    return {};

  const String title = focused->getTitle();
  if (title.isNotEmpty())
    return title;
  return focused->getName();
}

String SynthEditor::focusedSectionKey() const {
  for (auto* component = Component::getCurrentlyFocusedComponent(); component != nullptr;
       component = component->getParentComponent()) {
    const String key = component->getProperties()["SectionName"].toString();
    if (key.isNotEmpty())
      return key;
  }
  return {};
}

Component* SynthEditor::persistentFocusedComponent() const {
  auto* focused = Component::getCurrentlyFocusedComponent();
  if (focused == nullptr)
    return nullptr;

  for (Component* component = focused; component != nullptr; component = component->getParentComponent()) {
    if (component == &rows_container_)
      return nullptr;
  }

  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl() == focused)
      return nullptr;
  }
  return focused;
}

std::unique_ptr<AccessibleParameterRow> SynthEditor::createConfiguredSectionRow(AudioProcessorParameter& parameter,
                                                                               const String& section_name) {
  auto row = createAccessibleParameterRow(parameter, section_name);
  if (auto* row_bridge = dynamic_cast<ValueBridge*>(&parameter);
      row_bridge != nullptr && synth_.hasAutomationProxy(row_bridge->getParameterId().toStdString()))
    setAutomationProxyCallbacks(*row);
  row->setModulationSourceSubmenuCallback([this](const String& destinationId,
                                                 std::map<int, String>& choices, int firstItemId) {
    return createModulationSourceSubmenu(destinationId, choices, firstItemId);
  });
  row->setModulationAssignCallback([this](const String& sourceId, const String& destinationId, Component& target) {
    connectModulationAndPromptForAmount(sourceId, destinationId, target);
  });
  row->setModulationEditCallback([this](const String& sourceId, const String& destinationId, Component& target) {
    promptForInitialModulationAmount(sourceId, destinationId, false, target);
  });
  row->setModulationDestinationPredicate([this](const String& id) { return isModulationDestinationId(id); });
  row->setModulationRemovalCallbacks(
      [this](const String& destinationId) { return modulationSourcesForDestination(destinationId); },
      [this](const String& sourceId, const String& destinationId, Component& target) {
        removeModulationFromParameter(sourceId, destinationId, target);
      });
  row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
    if (clear) {
      synth_.clearMidiLearn(parameterId.toStdString());
      postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                             AccessibilityHandler::AnnouncementPriority::high);
    }
    else {
      armOrCancelMidiLearn(parameterId);
    }
  });
  row->setValueEntryCallback([this](const String& parameterId, Component& target) {
    promptForParameterValue(parameterId, target);
  });
  row->setExtraCommandCallback([this, section_name](const String& parameterId, const KeyPress& key,
                                                    Component& target) {
    return handleMacroShortcut(parameterId, key, target) || handleEffectShortcut(section_name, key, target);
  });
  wireSampleLoopFineTune(*row);
  return row;
}

void SynthEditor::addOscillatorSectionContent(int oscillator, const String& section_name, Component& parent,
                                              int& y, int x, int width, int& focus_order) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent);
  const int slot_type = sourceSlotType(oscillator);
  const auto& type = vital::source_types::describe(slot_type);
  // Braids: Timbre and Color are named after the selected synthesis mode,
  // "Plucking position (Color)", and say what they do there.
  const int braids_model = slot_type == vital::source_types::kBraids ? braidsModel(oscillator) : -1;
  const String braids_mode_name = braids_model >= 0 ? String(strings::kBraidsModelNames[braids_model]) : String();
  auto braids_suffix = [&](AudioProcessorParameter& parameter) {
    auto* bridge = dynamic_cast<ValueBridge*>(&parameter);
    if (braids_model < 0 || bridge == nullptr)
      return String();
    const String id = bridge->getParameterId();
    const String prefix_id = "osc_" + String(oscillator + 1) + "_braids_";
    return id.startsWith(prefix_id) ? id.substring(prefix_id.length()) : String();
  };
  auto braidsModeRowName = [&](AudioProcessorParameter& parameter) {
    const String suffix = braids_suffix(parameter);
    if (suffix == "timbre")
      return String(strings::kBraidsTimbreNames[braids_model]) + " (Timbre)";
    if (suffix == "color")
      return String(strings::kBraidsColorNames[braids_model]) + " (Color)";
    return String();
  };
  auto braidsModeRowDescription = [&](AudioProcessorParameter& parameter) {
    const String suffix = braids_suffix(parameter);
    const bool timbre = suffix == "timbre";
    const String meaning = timbre ? String(strings::kBraidsTimbreNames[braids_model])
                                  : String(strings::kBraidsColorNames[braids_model]);
    return String(timbre ? "Timbre" : "Color") + " sets " + meaning.toLowerCase() + " in the " +
           braids_mode_name + " mode.";
  };

  std::map<OscillatorRowGroup, std::vector<AudioProcessorParameter*>> groups;
  const String prefix = "osc_" + String(oscillator + 1) + "_";
  for (auto* parameter : sections_[section_name]) {
    auto* bridge = dynamic_cast<ValueBridge*>(parameter);
    if (bridge == nullptr || !shouldShowParameterInSection(section_name, parameter))
      continue;
    const String id = bridge->getParameterId();
    const OscillatorRowGroup group = id.startsWith(prefix) ? oscillatorRowGroup(id.substring(prefix.length()))
                                                           : OscillatorRowGroup::kOutput;
    groups[group].push_back(parameter);
  }

  // One row (and its Randomize seed button) inside `row_parent`; `group` is the
  // accessibility group that lists it, null at the section's own level in the
  // single-section view.
  // The section names the oscillator, so its rows keep only its number:
  // "Oscillator 1 Sample Loop" reads "1 Sample Loop", "Destination" "1 Destination".
  const String row_number = String(oscillator + 1) + " ";
  const String row_title_prefix = "Oscillator " + row_number;
  auto place_row = [&](AudioProcessorParameter& parameter, Component& row_parent, AccessibleSectionHeader* group,
                       int& row_y, int row_x, int row_width) {
    auto row = createConfiguredSectionRow(parameter, section_name);
    const String name = row->focusableControl()->getTitle();
    if (const String braids = braidsModeRowName(parameter); braids.isNotEmpty())
      row->setAccessibleName(row_number + braids, braidsModeRowDescription(parameter));
    else
      row->setAccessibleName(row_number + (name.startsWith(row_title_prefix) ? name.substring(row_title_prefix.length())
                                                                             : name));
    row->setExplicitFocusOrder(focus_order++);
    row->setControlFocusOrder(focus_order++);
    row->setBounds(row_x, row_y, row_width, 48);
    row_parent.addAndMakeVisible(row.get());
    if (group != nullptr)
      group->addAccessibleChild(row->focusableControl());
    row_focus_order_.push_back(row->focusableControl());
    rows_.push_back(std::move(row));
    row_y += 48;

    auto* bridge = dynamic_cast<ValueBridge*>(&parameter);
    if (bridge == nullptr || !bridge->getParameterId().endsWith("_spectral_morph_seed"))
      return;
    const String id = bridge->getParameterId();
    const String title = id.contains("_secondary_") ? "Randomize secondary amplitudes seed" : "Randomize amplitudes seed";
    const String description = "Choose a new random amplitudes pattern for this morph slot";
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->setWantsKeyboardFocus(true);
    button_ptr->onClick = [this, id] { randomizeAmplitudeSeed(id); };
    button_ptr->setExplicitFocusOrder(focus_order++);
    button_ptr->setBounds(row_x, row_y, row_width, 38);
    row_parent.addAndMakeVisible(button_ptr);
    if (group != nullptr)
      group->addAccessibleChild(button_ptr);
    row_focus_order_.push_back(button_ptr);
    section_headers_.push_back(std::move(button));
    row_y += 44;
  };

  auto open_group = [&](const String& title, const String& key, const String& description, Component& group_parent,
                        AccessibleSectionHeader* parent_group, int& parent_y, int group_x, int group_width) {
    auto group = makeSectionHeader(title, groupRank("Oscillators"), false, section_name + " " + key);
    auto* group_ptr = group.get();
    group_ptr->setDescription(description);
    group_ptr->setExplicitFocusOrder(focus_order++);
    group_ptr->setBounds(group_x, parent_y, group_width, group_ptr->headerHeight());
    group_parent.addAndMakeVisible(group_ptr);
    if (parent_group != nullptr)
      parent_group->addAccessibleChild(group_ptr);
    row_focus_order_.push_back(group_ptr);
    section_headers_.push_back(std::move(group));
    return group_ptr;
  };
  auto close_group = [](AccessibleSectionHeader* group, int inner_y, int& parent_y, int group_x, int group_width) {
    group->setBounds(group_x, parent_y, group_width, inner_y + 2);
    parent_y += inner_y + 10;
  };
  auto add_rows_to = [&](AccessibleSectionHeader* group, const std::vector<AudioProcessorParameter*>& parameters,
                         int& inner_y, int inner_width) {
    for (auto* parameter : parameters)
      place_row(*parameter, *group, group, inner_y, 10, jmax(580, inner_width - 20));
  };
  auto rows_in = [&](OscillatorRowGroup which) -> const std::vector<AudioProcessorParameter*>& {
    static const std::vector<AudioProcessorParameter*> none;
    const auto found = groups.find(which);
    return found == groups.end() ? none : found->second;
  };
  auto add_group = [&](OscillatorRowGroup which, const String& title, const String& key, const String& description) {
    const auto& parameters = rows_in(which);
    if (parameters.empty())
      return;
    auto* group = open_group(title, key, description, parent, accessible_parent, y, x, width);
    int inner_y = group->headerHeight() + 8;
    add_rows_to(group, parameters, inner_y, width);
    close_group(group, inner_y, y, x, width);
  };

  // Every type reads in the same order: output rows, the type's own controls,
  // Pitch and unison, then Warp.
  // 1. Output rows, ungrouped: switch, solo, type, level, pan, destination.
  auto output_rows = rows_in(OscillatorRowGroup::kOutput);
  const auto output_rank = [&](AudioProcessorParameter* parameter) {
    static const StringArray order { "on", "solo", "type", "level", "pan", "destination" };
    auto* bridge = dynamic_cast<ValueBridge*>(parameter);
    const String id = bridge != nullptr ? bridge->getParameterId() : String();
    const int rank = id.startsWith(prefix) ? order.indexOf(id.substring(prefix.length())) : -1;
    return rank < 0 ? order.size() : rank;
  };
  std::stable_sort(output_rows.begin(), output_rows.end(), [&](AudioProcessorParameter* a, AudioProcessorParameter* b) {
    return output_rank(a) < output_rank(b);
  });
  for (auto* parameter : output_rows)
    place_row(*parameter, parent, accessible_parent, y, x, width);

  // 2. Controls: the wavetable or sample the type plays, then the type's own settings.
  if (type.uses_wavetable && !isAnalogOscillator(oscillator)) {
    addWavetableControlsGroup(oscillator, parent, y, x, width, focus_order, section_name,
                              [&](Component& group, int& inner_y, int inner_width) {
                                add_rows_to(dynamic_cast<AccessibleSectionHeader*>(&group),
                                            rows_in(OscillatorRowGroup::kWavetable), inner_y, inner_width);
                              });
  }
  if (type.uses_sample) {
    addSampleControlsGroup(oscillator, parent, y, x, width, focus_order, section_name,
                           [&](Component& group, int& inner_y, int inner_width) {
                             add_rows_to(dynamic_cast<AccessibleSectionHeader*>(&group),
                                         rows_in(OscillatorRowGroup::kSample), inner_y, inner_width);
                           });
  }
  add_group(OscillatorRowGroup::kAnalog, "Analog controls", "analog controls",
            "Waveform, shape and sub oscillator of " + section_name);
  const String type_name = String(strings::kOscillatorTypeNames[slot_type]);
  if (slot_type == vital::source_types::kMultiSample) {
    addMultiSampleControlsGroup(oscillator, parent, y, x, width, focus_order, section_name,
                                [&](Component& group, int& inner_y, int inner_width) {
                                  add_rows_to(dynamic_cast<AccessibleSectionHeader*>(&group),
                                              rows_in(OscillatorRowGroup::kType), inner_y, inner_width);
                                });
  }
  else if (slot_type == vital::source_types::kSpeech) {
    addSpeechControlsGroup(oscillator, parent, y, x, width, focus_order, section_name,
                           [&](Component& group, int& inner_y, int inner_width) {
                             add_rows_to(dynamic_cast<AccessibleSectionHeader*>(&group),
                                         rows_in(OscillatorRowGroup::kType), inner_y, inner_width);
                           });
  }
  else {
    add_group(OscillatorRowGroup::kType, type_name + " controls", type_name.toLowerCase() + " controls",
              type_name + " settings of " + section_name);
  }
  // The selected mode's own parameters, in a group named after the mode. The
  // key stays the same across modes so the group keeps its place.
  if (braids_model >= 0) {
    add_group(OscillatorRowGroup::kTypeMode, braids_mode_name + " parameters", "braids mode parameters",
              "Timbre and Color of the " + braids_mode_name + " mode in " + section_name);
  }

  // 3. Pitch and unison, with Scale lock inside.
  const auto& pitch_rows = rows_in(OscillatorRowGroup::kPitch);
  const auto& scale_rows = rows_in(OscillatorRowGroup::kScaleLock);
  if (!pitch_rows.empty() || !scale_rows.empty()) {
    auto* pitch = open_group("Pitch and unison", "pitch and unison",
                             "Transpose, tune, unison and phase of " + section_name,
                             parent, accessible_parent, y, x, width);
    int inner_y = pitch->headerHeight() + 8;
    add_rows_to(pitch, pitch_rows, inner_y, width);
    if (!scale_rows.empty()) {
      const int scale_width = jmax(600, width - 20);
      auto* scale = open_group("Scale lock", "scale lock", "Snap transposition to a scale for " + section_name,
                               *pitch, pitch, inner_y, 10, scale_width);
      int scale_y = scale->headerHeight() + 8;
      add_rows_to(scale, scale_rows, scale_y, scale_width);
      close_group(scale, scale_y, inner_y, 10, scale_width);
    }
    close_group(pitch, inner_y, y, x, width);
  }

  // 4. Warp.
  add_group(OscillatorRowGroup::kWarp, "Warp", "warp",
            "Distortion, FM, ring modulation and frequency morph modes of " + section_name);
}

bool SynthEditor::restoreFocusAfterRebuild(const String& parameterId, Component* persistentComponent,
                                           const String& accessibleTitle, const String& preferredSection,
                                           bool refocusScreenReader) {
  // A queued rebuild (an effect slot variant) asks for this before it runs.
  refocusScreenReader = std::exchange(screen_reader_refocus_requested_, false) || refocusScreenReader;
  Component* initial = findRebuildFocusTarget(parameterId, persistentComponent, accessibleTitle, preferredSection);
  if (initial == nullptr)
    return false;
  // The rebuild kept this control (keepFocusedSwitchingRow): the screen
  // reader never left it.
  if (initial == kept_switching_control_)
    refocusScreenReader = false;

  // Focus the new control now, so keys pressed while a choice is stepped
  // (an arrow held on a Type menu) keep reaching it rather than the window.
  if (initial->isShowing()) {
    ensureComponentVisible(initial);
    initial->grabKeyboardFocus();
  }

  // Resolve again when the grab runs: another rebuild may have replaced the
  // control in between, and the fresh one is the right target.
  Component::SafePointer<SynthEditor> safe_this(this);
  Component::SafePointer<Component> safe_persistent(persistentComponent);
  MessageManager::callAsync([safe_this, safe_persistent, parameterId, accessibleTitle, preferredSection] {
    if (safe_this == nullptr)
      return;
    Component* target = safe_this->findRebuildFocusTarget(parameterId, safe_persistent.getComponent(),
                                                          accessibleTitle, preferredSection);
    if (target != nullptr && target->isShowing()) {
      safe_this->ensureComponentVisible(target);
      target->grabKeyboardFocus();
    }
  });

  // The rebuild deleted the control the screen reader was on, and its focus
  // notice for the new one can arrive before the reader has the new rows, so
  // it loses its place while keyboard focus is fine. Once the rows have
  // settled, move the reader's focus to the control again.
  // Only the last of several quick rebuilds (a held arrow) does this.
  const int generation = ++screen_reader_refocus_generation_;
  if (!refocusScreenReader)
    return true;
  holdAnnouncements();
  Timer::callAfterDelay(kScreenReaderRefocusDelayMs, [safe_this, generation] {
    auto* focused = Component::getCurrentlyFocusedComponent();
    if (safe_this != nullptr && safe_this->screen_reader_refocus_generation_ == generation &&
        focused != nullptr && safe_this->isParentOf(focused) && focused->isShowing()) {
      if (auto* handler = focused->getAccessibilityHandler()) {
        handler->giveAwayFocus();
        handler->grabFocus();
      }
    }
    releaseAnnouncements();
  });
  return true;
}

Component* SynthEditor::findRebuildFocusTarget(const String& parameterId, Component* persistentComponent,
                                               const String& accessibleTitle, const String& preferredSection) const {
  Component* target = nullptr;
  if (parameterId.isNotEmpty()) {
    for (const auto& row : rows_) {
      if (row != nullptr && row->parameterId() == parameterId) {
        target = row->focusableControl();
        break;
      }
    }
  }

  if (target == nullptr && persistentComponent != nullptr && persistentComponent->isShowing())
    target = persistentComponent;

  if (target == nullptr && accessibleTitle.isNotEmpty() && preferredSection.isNotEmpty()) {
    for (auto* component : focus_order_) {
      if (component == nullptr || !component->isShowing())
        continue;
      auto* section = component;
      while (section != nullptr && section->getProperties()["SectionName"].toString() != preferredSection)
        section = section->getParentComponent();
      if (section == nullptr)
        continue;
      if (component->getTitle() == accessibleTitle || component->getName() == accessibleTitle) {
        target = component;
        break;
      }
    }
  }

  if (target == nullptr && accessibleTitle.isNotEmpty()) {
    for (auto* component : focus_order_) {
      if (component == nullptr || !component->isShowing())
        continue;
      if (component->getTitle() == accessibleTitle || component->getName() == accessibleTitle) {
        target = component;
        break;
      }
    }
  }

  return target;
}

SynthEditor::FocusReturn SynthEditor::focusToReturnTo() const {
  Component* focused = Component::getCurrentlyFocusedComponent();
  if ((focused == nullptr || !isParentOf(focused)) && getPeer() != nullptr)
    focused = getPeer()->getLastFocusedSubcomponent();
  FocusReturn result;
  if (focused == nullptr || !isParentOf(focused))
    return result;

  for (const auto& row : rows_) {
    if (row != nullptr && (row->focusableControl() == focused || row->focusableControl()->isParentOf(focused))) {
      result.parameter_id = row->parameterId();
      break;
    }
  }
  result.title = focused->getTitle().isNotEmpty() ? focused->getTitle() : focused->getName();
  for (auto* component = focused; component != nullptr; component = component->getParentComponent()) {
    const String key = component->getProperties()["SectionName"].toString();
    if (key.isNotEmpty()) {
      result.section = key;
      break;
    }
  }
  result.component = focused;
  return result;
}

void SynthEditor::launchFileChooser(FileChooser& chooser, int flags,
                                    std::function<void(const FileChooser&)> callback) {
  // The chooser takes the window's focus, and the host does not always give
  // it back to the control that opened it; a screen reader can end up at the
  // top of the window. Rows rebuilt meanwhile (a loaded preset) replace it.
  const FocusReturn focus = focusToReturnTo();
  Component::SafePointer<SynthEditor> safe_this(this);
  // Counts the chooser as open until it calls back, or is deleted without
  // calling back (replaced by another).
  struct OpenChooser {
    Component::SafePointer<SynthEditor> editor;
    bool open = true;
    ~OpenChooser() { close(); }
    void close() {
      if (std::exchange(open, false) && editor != nullptr)
        --editor->open_file_choosers_;
    }
  };
  ++open_file_choosers_;
  auto open = std::make_shared<OpenChooser>();
  open->editor = safe_this;
  chooser.launchAsync(flags, [safe_this, focus, open, callback = std::move(callback)](const FileChooser& result) {
    open->close();
    const ScopedAnnouncementHold hold;
    callback(result);
    // A second chooser the callback opened (a bank export asks for the folder,
    // then the file) gives focus back when it closes.
    if (safe_this != nullptr && safe_this->open_file_choosers_ == 0 && !focus.isEmpty()) {
      // Leave focus alone if the callback moved it on, to a prompt it opened say.
      auto* focused = Component::getCurrentlyFocusedComponent();
      auto* target = safe_this->findRebuildFocusTarget(focus.parameter_id, focus.component.getComponent(),
                                                       focus.title, focus.section);
      const bool moved_on = focused != nullptr && focused != safe_this.getComponent() && focused != target &&
                            safe_this->isParentOf(focused);
      if (!moved_on) {
        // Back on the control VoiceOver was on before the chooser: it follows
        // focus to a new element, not back to that one (see returnFocusFromPrompt).
        if (target != nullptr && target == focus.component.getComponent())
          target->invalidateAccessibilityHandler();
        safe_this->restoreFocusAfterRebuild(focus.parameter_id, focus.component.getComponent(), focus.title,
                                            focus.section, true);
      }
    }
  });
}

void SynthEditor::focusOfChildComponentChanged(FocusChangeType cause) {
  // The window losing focus to a file chooser, a menu or the host comes as a
  // mouse click. Remember the control it had: rows rebuilt before focus comes
  // back replace it.
  if (!hasKeyboardFocus(true)) {
    if (cause == focusChangedByMouseClick)
      window_focus_return_ = focusToReturnTo();
    return;
  }
  if (!hasKeyboardFocus(false))
    window_focus_return_ = {};
}

void SynthEditor::focusGained(FocusChangeType) {
  // Focus came back to a window whose control was replaced meanwhile, so JUCE
  // focused the editor itself, which a screen reader reads as the top of the
  // window. Go to the replacement instead.
  if (window_focus_return_.isEmpty())
    return;
  const FocusReturn focus = std::exchange(window_focus_return_, {});
  restoreFocusAfterRebuild(focus.parameter_id, focus.component.getComponent(), focus.title, focus.section, true);
}

void SynthEditor::keepFocusedSwitchingRow() {
  releaseKeptFocus();
  if (!kKeepSwitchingFocus)
    return;
  const auto found = std::find_if(rows_.begin(), rows_.end(), [this](const auto& row) {
    return row != nullptr && row->focusableControl()->hasKeyboardFocus(true) &&
           (isRowSwitchingChoice(row->parameterId()) || isRowSwitchingToggle(row->parameterId())) &&
           rows_container_.isParentOf(row.get());
  });
  if (found == rows_.end())
    return;

  // The groups around it, outermost first, leave the old view with it.
  std::vector<std::unique_ptr<AccessibleSectionHeader>> groups;
  for (auto* parent = (*found)->getParentComponent(); parent != nullptr && parent != &rows_container_;
       parent = parent->getParentComponent()) {
    auto* header = dynamic_cast<AccessibleSectionHeader*>(parent);
    const auto owned = std::find_if(section_headers_.begin(), section_headers_.end(),
                                    [parent](const auto& component) { return component.get() == parent; });
    if (header == nullptr || owned == section_headers_.end())
      continue;
    owned->release();
    section_headers_.erase(owned);
    groups.insert(groups.begin(), std::unique_ptr<AccessibleSectionHeader>(header));
  }

  // Focus goes first, so that taking the row out moves it nowhere else.
  if (rows_container_.hasKeyboardFocus(true))
    rows_container_.giveAwayKeyboardFocus();
  kept_switching_parameter_id_ = (*found)->parameterId();
  kept_switching_parameter_ = &(*found)->parameter();
  kept_switching_control_ = (*found)->focusableControl();
  kept_switching_row_ = std::move(*found);
  rows_.erase(found);
  if (auto* parent = kept_switching_row_->getParentComponent())
    parent->removeChildComponent(kept_switching_row_.get());
  // Innermost first. Their other children are about to be deleted.
  for (auto group = groups.rbegin(); group != groups.rend(); ++group) {
    (*group)->clearAccessibleChildren();
    if (auto* parent = (*group)->getParentComponent())
      parent->removeChildComponent(group->get());
  }
  kept_switching_groups_ = std::move(groups);
  kept_switching_groups_taken_ = 0;
}

void SynthEditor::releaseKeptFocus() {
  // Whatever the rebuild did not take back.
  kept_switching_control_ = nullptr;
  kept_switching_parameter_ = nullptr;
  kept_switching_parameter_id_.clear();
  kept_switching_row_.reset();
  kept_switching_groups_.clear();
  kept_switching_groups_taken_ = 0;
}

std::unique_ptr<AccessibleSectionHeader> SynthEditor::makeSectionHeader(const String& title, int groupRank,
                                                                       bool topLevel, const String& sectionKey) {
  // The rebuild meets the kept groups in order, outermost first.
  if (kept_switching_groups_taken_ < kept_switching_groups_.size()) {
    auto& kept = kept_switching_groups_[kept_switching_groups_taken_];
    if (kept->isSameGroup(title, topLevel, sectionKey)) {
      ++kept_switching_groups_taken_;
      kept->reuse(title, groupRank, topLevel, sectionKey);
      return std::move(kept);
    }
  }
  return std::make_unique<AccessibleSectionHeader>(title, groupRank, topLevel, sectionKey);
}

bool SynthEditor::refreshFilterRowsIfNeeded() {
  bool changed = false;
  for (auto& filter : last_filter_states_) {
    const String& prefix = filter.first;
    int model = -1;
    int style = -1;
    if (auto* model_bridge = parameterBridge(prefix + "model"))
      model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));
    if (auto* style_bridge = parameterBridge(prefix + "style"))
      style = static_cast<int>(std::round(style_bridge->convertToEngineValue(style_bridge->getValue())));

    const std::pair<int, int> state { model, style };
    if (filter.second != state) {
      filter.second = state;
      changed = true;
    }
  }

  if (!changed)
    return false;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();

  keepFocusedSwitchingRow();
  showAllSections(false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title, {}, true);
  releaseKeptFocus();

  return true;
}

bool SynthEditor::refreshRandomAmplitudeRowsIfNeeded() {
  bool changed = false;
  for (auto& mode : last_random_amplitude_modes_) {
    const bool random = static_cast<int>(engineValueFor(parameterBridge(mode.first))) ==
                        vital::SynthOscillator::kRandomAmplitudes;
    changed |= mode.second != random;
    mode.second = random;
  }
  if (!changed)
    return false;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();
  keepFocusedSwitchingRow();
  showAllSections(false);
  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title, {}, true);
  releaseKeptFocus();
  return true;
}

bool SynthEditor::isAnalogOscillator(int oscillator) const {
  // Only the Analog type: the engine's isAnalogType treats every non-wavetable type as analog.
  return sourceSlotType(oscillator) == vital::source_types::kAnalog;
}

int SynthEditor::oscillatorModeKey(int oscillator) const {
  // Any type change rebuilds the oscillator's rows; analog adds its waveform.
  const int type = sourceSlotType(oscillator);
  // A Multi-sample slot's rows follow what is loaded (SF2 presets, SFZ buttons).
  if (type == vital::source_types::kMultiSample)
    return type * 64 + multiSampleUiState(oscillator);
  if (type == vital::source_types::kBraids)
    return type * 64 + braidsModel(oscillator);
  if (!isAnalogOscillator(oscillator))
    return type * 64 - 1;
  return type * 64 + roundToInt(engineValueFor(parameterBridge("osc_" + String(oscillator + 1) + "_analog_waveform")));
}

int SynthEditor::braidsModel(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return 0;
  const std::string id = vital::source_types::slotPrefix(oscillator) + "braids_model";
  float value = 0.0f;
  if (auto* bridge = parameterBridge(id))
    value = engineValueFor(bridge);
  else {
    const auto& controls = synth_.getControls();
    const auto found = controls.find(id);
    if (found != controls.end() && found->second != nullptr)
      value = found->second->value();
  }
  return jlimit(0, vital::braids_engine::kNumModels - 1, roundToInt(value));
}

int SynthEditor::sourceSlotType(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return vital::source_types::kWavetable;
  const std::string id = vital::source_types::slotPrefix(oscillator) + "type";
  if (auto* bridge = parameterBridge(id))
    return jlimit(0, vital::source_types::kNumTypes - 1, roundToInt(engineValueFor(bridge)));
  const auto& controls = synth_.getControls();
  const auto found = controls.find(id);
  if (found == controls.end() || found->second == nullptr)
    return vital::source_types::kWavetable;
  return jlimit(0, vital::source_types::kNumTypes - 1, roundToInt(found->second->value()));
}

bool SynthEditor::refreshOscillatorModeRowsIfNeeded() {
  bool changed = std::exchange(instrument_preset_menu_stale_, false);
  for (auto& mode : last_oscillator_modes_) {
    const int oscillator = mode.first.fromFirstOccurrenceOf("osc_", false, false).getIntValue() - 1;
    const int key = oscillatorModeKey(oscillator);
    changed |= mode.second != key;
    mode.second = key;
  }
  if (!changed)
    return false;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  // Every Multi-sample slot has a Load instrument button: stay in this one.
  const String section_key = focusedSectionKey();
  auto* persistent_focus = persistentFocusedComponent();
  keepFocusedSwitchingRow();
  showAllSections(false);
  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title, section_key, true);
  releaseKeptFocus();
  return true;
}

bool SynthEditor::refreshEnvelopeAnalogRowsIfNeeded() {
  int analog = 0;
  if (auto* bridge = parameterBridge("env_1_analog"))
    analog = static_cast<int>(std::round(bridge->convertToEngineValue(bridge->getValue())));

  if (last_envelope_analog_ == analog)
    return false;
  const bool first_check = last_envelope_analog_ < 0;
  last_envelope_analog_ = analog;

  // Analog mode swaps Envelope 1's Power rows for its Age row.
  if (first_check)
    return false;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();

  keepFocusedSwitchingRow();
  showAllSections(false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title, {}, true);
  releaseKeptFocus();

  return true;
}

int SynthEditor::currentGlideZones() const {
  if (auto* bridge = parameterBridge("portamento_glide_zones"))
    return static_cast<int>(std::round(bridge->convertToEngineValue(bridge->getValue())));
  return 0;
}

bool SynthEditor::refreshZoneCrossfadeRowIfNeeded() {
  const int glide_zones = currentGlideZones();
  if (last_glide_zones_ == glide_zones)
    return false;
  last_glide_zones_ = glide_zones;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();

  showAllSections(false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title, {}, true);

  return true;
}

void SynthEditor::clearSectionComponents() {
  // JUCE can synchronously traverse focus when a focused child is removed.
  // Invalidate every borrowed control list before destroying any components.
  focus_order_.clear();
  accessibility_focus_order_.clear();
  row_focus_order_.clear();
  top_level_accessibility_order_.clear();
  for (auto& component : section_headers_)
    if (auto* header = dynamic_cast<AccessibleSectionHeader*>(component.get()))
      header->clearAccessibleChildren();
  if (rows_container_.hasKeyboardFocus(true))
    rows_container_.giveAwayKeyboardFocus();

  fx_bus_toggles_.clear();
  section_headers_.clear();
  rows_.clear();
  last_filter_states_.clear();
  last_random_amplitude_modes_.clear();
  last_oscillator_modes_.clear();
  instrument_preset_menu_stale_ = false;
  rows_container_.removeAllChildren();
}

void SynthEditor::showAllSections(bool announce) {
  clearSectionComponents();
  // The rows built here already follow the glide setting; the timer rebuilds
  // only when it changes. Never recorded, it rebuilt every row on the
  // editor's first tick.
  last_glide_zones_ = currentGlideZones();

  showPresetControls();
  active_oscillator_ = -1;
  active_modulation_remap_connection_ = -1;
  refreshVisibleModulationLabels();

  int y = 0;
  const int width = jmax(760, viewport_.getMaximumVisibleWidth());
  int focus_order = 10;

  for (const auto& group : group_names_) {
    auto group_component = makeSectionHeader(group, groupRank(group), true);
    auto* group_ptr = group_component.get();
    group_ptr->setExplicitFocusOrder(focus_order++);
    rows_container_.addAndMakeVisible(group_ptr);
    row_focus_order_.push_back(group_ptr);
    top_level_accessibility_order_.push_back(group_ptr);
    section_headers_.push_back(std::move(group_component));

    const auto found = group_sections_.find(group);
    if (found == group_sections_.end())
      continue;

    int group_y = group_ptr->headerHeight();
    if (group == kPresetSection) {
      group_ptr->onAccessibilityFocus = [this, group_ptr, revealing = false]() mutable {
        auto* handler = group_ptr->getAccessibilityHandler();
        if (revealing || (handler != nullptr && handler->hasFocus(true)))
          return;
        ScopedValueSetter<bool> guard(revealing, true);
        ensurePresetListLoaded();
        // VoiceOver navigation does not use OrderedFocusTraverser's scrolling.
        // Reveal the native preset controls before it queries this group's children.
        ensureComponentVisible(group_ptr);
        // Focusing does not change the child structure. A layout notification
        // here makes VoiceOver request focus again and can create a feedback loop.
      };
      preset_summary_.setBounds(16, group_y, jmax(620, width - 32), 38);
      group_ptr->addAndMakeVisible(preset_summary_);
      group_y += 42;

      auto addPresetControl = [&](Component& component, int height = 38) {
        component.setBounds(16, group_y, jmax(620, width - 32), height);
        group_ptr->addAndMakeVisible(component);
        group_ptr->addAccessibleChild(&component);
        row_focus_order_.push_back(&component);
        group_y += height + 6;
      };

      const int available_width = jmax(620, width - 32);
      preset_menu_.setBounds(16, group_y, 150, 38);
      preset_search_.setBounds(172, group_y, available_width - 156, 38);
      group_ptr->addAndMakeVisible(preset_menu_);
      group_ptr->addAndMakeVisible(preset_search_);
      group_ptr->addAccessibleChild(&preset_menu_);
      group_ptr->addAccessibleChild(&preset_search_);
      row_focus_order_.push_back(&preset_menu_);
      row_focus_order_.push_back(&preset_search_);
      group_y += 42;
      std::array<PresetTableListBox*, 5> facet_tables {
        &preset_collection_table_, &preset_library_table_, &preset_bank_table_,
        &preset_category_table_, &preset_tag_table_
      };
      const int facet_width = available_width / static_cast<int>(facet_tables.size());
      int facet_x = 16;
      for (int index = 0; index < static_cast<int>(facet_tables.size()); ++index) {
        auto* table = facet_tables[static_cast<size_t>(index)];
        const int table_width = index == static_cast<int>(facet_tables.size()) - 1
                                  ? 16 + available_width - facet_x : facet_width;
        table->setBounds(facet_x, group_y, table_width, 100);
        group_ptr->addAndMakeVisible(table);
        group_ptr->addAccessibleChild(table);
        row_focus_order_.push_back(table);
        facet_x += facet_width;
      }
      group_y += 104;
      preset_table_.setBounds(16, group_y, available_width, 220);
      group_ptr->addAndMakeVisible(preset_table_);
      group_ptr->addAccessibleChild(&preset_table_);
      row_focus_order_.push_back(&preset_table_);
      group_y += 224;
      addPresetControl(preset_details_, 42);
      addPresetControl(preset_preview_);
    }

    for (const auto& section_name : found->second) {
      const bool flatten_section = found->second.size() == 1;
      AccessibleSectionHeader* section_ptr = group_ptr;
      int section_y = flatten_section ? group_y : 0;

      if (!flatten_section) {
        auto section_component = makeSectionHeader(sourceStatusTitleForSection(section_name), groupRank(group),
                                                   false, section_name);
        section_ptr = section_component.get();
        if (effectIdForSection(section_name).isNotEmpty()) {
          section_ptr->onKeyCommand = [this, section_name](const KeyPress& key, Component& target) {
            return handleEffectShortcut(section_name, key, target);
          };
        }
        if (const int oscillator = oscillatorIndexForSectionName(section_name);
            isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
          section_ptr->onContextMenuCommand = [this, oscillator](Component& target) {
            showOscillatorContextMenu(oscillator, target);
          };
        }
        section_ptr->setExplicitFocusOrder(focus_order++);
        group_ptr->addAndMakeVisible(section_ptr);
        group_ptr->addAccessibleChild(section_ptr);
        row_focus_order_.push_back(section_ptr);
        section_headers_.push_back(std::move(section_component));
        section_y = section_ptr->headerHeight();
      }
      else if (const int oscillator = oscillatorIndexForSectionName(section_name);
               isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
        section_ptr->onContextMenuCommand = [this, oscillator](Component& target) {
          showOscillatorContextMenu(oscillator, target);
        };
      }

      if (section_name == "Modulation routing") {
        const int slot_width = jmax(620, width - (flatten_section ? 32 : 34));
        for (int slot = 1; slot <= vital::kMaxModulationConnections; ++slot) {
          const String title = modulationSlotTitle(slot);

          auto slot_component = makeSectionHeader(title, groupRank(group), false);
          auto* slot_ptr = slot_component.get();
          slot_ptr->getProperties().set("ModulationSlot", slot);
          slot_ptr->setExplicitFocusOrder(focus_order++);
          slot_ptr->setBounds(flatten_section ? 16 : 10, section_y, slot_width, slot_ptr->headerHeight());
          section_ptr->addAndMakeVisible(slot_ptr);
          section_ptr->addAccessibleChild(slot_ptr);
          row_focus_order_.push_back(slot_ptr);
          section_headers_.push_back(std::move(slot_component));

          int slot_y = slot_ptr->headerHeight();
          const String prefix = "modulation_" + String(slot) + "_";
          for (auto* parameter : sections_[section_name]) {
            auto* bridge = dynamic_cast<ValueBridge*>(parameter);
            if (bridge == nullptr || !bridge->getParameterId().startsWith(prefix))
              continue;

            auto row = std::make_unique<AccessibleParameterRow>(*parameter);
            row->setModulationSourceSubmenuCallback([this](const String& destinationId,
                                                           std::map<int, String>& choices, int firstItemId) {
              return createModulationSourceSubmenu(destinationId, choices, firstItemId);
            });
            row->setModulationAssignCallback([this](const String& sourceId, const String& destinationId,
                                                    Component& target) {
              connectModulationAndPromptForAmount(sourceId, destinationId, target);
            });
            row->setModulationEditCallback([this](const String& sourceId, const String& destinationId,
                                                  Component& target) {
              promptForInitialModulationAmount(sourceId, destinationId, false, target);
            });
            row->setModulationDestinationPredicate([this](const String& id) { return isModulationDestinationId(id); });
            row->setModulationRemovalCallbacks(
                [this](const String& destinationId) { return modulationSourcesForDestination(destinationId); },
                [this](const String& sourceId, const String& destinationId, Component& target) {
                  removeModulationFromParameter(sourceId, destinationId, target);
                });
            row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
              if (clear) {
                synth_.clearMidiLearn(parameterId.toStdString());
                postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                                       AccessibilityHandler::AnnouncementPriority::high);
              }
              else {
                armOrCancelMidiLearn(parameterId);
              }
            });
            row->setValueEntryCallback([this](const String& parameterId, Component& target) {
              promptForParameterValue(parameterId, target);
            });
            row->setExtraCommandCallback([this, section_name](const String& parameterId, const KeyPress& key,
                                                              Component& target) {
              return handleMacroShortcut(parameterId, key, target) ||
                     handleEffectShortcut(section_name, key, target);
            });
            row->setAccessibleName(modulationControlTitle(bridge->getParameterId()));
            row->setExplicitFocusOrder(focus_order++);
            row->setControlFocusOrder(focus_order++);
            row->setBounds(10, slot_y, jmax(600, slot_width - 20), 48);
            slot_ptr->addAndMakeVisible(row.get());
            slot_ptr->addAccessibleChild(row->focusableControl());
            row_focus_order_.push_back(row->focusableControl());
            rows_.push_back(std::move(row));
            slot_y += 48;
          }

          auto* connection = synth_.getModulationBank().atIndex(slot - 1);
          if (connection != nullptr && !connection->source_name.empty() &&
              !connection->destination_name.empty() && connection->modulation_processor != nullptr) {
            auto remap = std::make_unique<ModulationRemapKeyboardComponent>();
            auto* remap_ptr = remap.get();
            remap_ptr->setTitle(modulationSlotHeaderTitle(slot) + " remap editor");
            remap_ptr->setDescription(modulationRemapStatusTextForSlot(slot));
            remap_ptr->onKeyPressed = [this, slot](const KeyPress& key) {
              if (!selectModulationRemapSlot(slot)) {
                postPluginAnnouncement(modulationSlotHeaderTitle(slot) + " has no modulation route",
                                       AccessibilityHandler::AnnouncementPriority::high);
                return true;
              }
              return handleModulationRemapShortcut(key);
            };
            remap_ptr->getStatusText = [this, slot] { return modulationRemapStatusTextForSlot(slot); };
            remap_ptr->setExplicitFocusOrder(focus_order++);
            remap_ptr->setBounds(10, slot_y, jmax(600, slot_width - 20), 82);
            slot_ptr->addAndMakeVisible(remap_ptr);
            slot_ptr->addAccessibleChild(remap_ptr);
            row_focus_order_.push_back(remap_ptr);
            section_headers_.push_back(std::move(remap));
            slot_y += 88;
          }

          // Only a slot with a source or destination has something to clear.
          if (connection != nullptr && (!connection->source_name.empty() || !connection->destination_name.empty())) {
            auto clear = std::make_unique<OffscreenTextButton>("Clear modulation slot");
            auto* clear_ptr = clear.get();
            clear_ptr->setTitle("Clear modulation slot");
            clear_ptr->setDescription("Disconnect " + modulationSlotHeaderTitle(slot) +
                                      " and reset its amount, options and remap");
            clear_ptr->setHelpText("Press Enter to empty this modulation slot");
            clear_ptr->setWantsKeyboardFocus(true);
            clear_ptr->onClick = [this, slot] { clearModulationSlot(slot); };
            clear_ptr->setExplicitFocusOrder(focus_order++);
            clear_ptr->setBounds(10, slot_y, jmax(600, slot_width - 20), 38);
            slot_ptr->addAndMakeVisible(clear_ptr);
            slot_ptr->addAccessibleChild(clear_ptr);
            row_focus_order_.push_back(clear_ptr);
            section_headers_.push_back(std::move(clear));
            slot_y += 44;
          }

          slot_ptr->setBounds(flatten_section ? 16 : 10, section_y, slot_width, slot_y);
          section_y += slot_y + 8;
        }

        if (flatten_section)
          group_y = section_y + 8;
        else {
          section_ptr->setBounds(16, group_y, jmax(620, width - 24), section_y);
          group_y += section_y + 8;
        }
        continue;
      }

      auto addActionButton = [&](const String& title, const String& description, std::function<void()> action) {
        auto button = std::make_unique<OffscreenTextButton>(title);
        auto* button_ptr = button.get();
        button_ptr->setTitle(title);
        button_ptr->setDescription(description);
        button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
        button_ptr->setWantsKeyboardFocus(true);
        button_ptr->onClick = std::move(action);
        button_ptr->setExplicitFocusOrder(focus_order++);
        button_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
        section_ptr->addAndMakeVisible(button_ptr);
        section_ptr->addAccessibleChild(button_ptr);
        row_focus_order_.push_back(button_ptr);
        section_headers_.push_back(std::move(button));
        section_y += 44;
      };

      const int oscillator_section = oscillatorIndexForSectionName(section_name);
      if (oscillator_section >= 0) {
        addOscillatorSectionContent(oscillator_section, section_name, *section_ptr, section_y,
                                    flatten_section ? 16 : 10, jmax(620, width - (flatten_section ? 32 : 34)),
                                    focus_order);
      }
      else if (section_name == "Filter 1" || section_name == "Filter 2") {
        const String filter_prefix = section_name == "Filter 1" ? "filter_1" : "filter_2";
        if (filterModelForPrefix(filter_prefix) == vital::constants::kFilterTable)
          addFilterTableControlsGroup(filter_prefix, *section_ptr, section_y, flatten_section ? 16 : 10,
                                      jmax(620, width - (flatten_section ? 32 : 34)), focus_order, section_name);
      }
      else if (section_name == kSignalRoutingSection) {
        refreshRoutingControls();
        routing_summary_.setVisible(true);
        routing_mode_.setVisible(true);
        routing_default_.setVisible(true);
        routing_serial_forward_.setVisible(true);
        routing_serial_backward_.setVisible(true);

        routing_summary_.setBounds(flatten_section ? 16 : 10, section_y,
                                   jmax(620, width - (flatten_section ? 32 : 34)), 54);
        section_ptr->addAndMakeVisible(routing_summary_);
        section_y += 60;

        auto addRoutingControl = [&](Component& component) {
          component.setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(component);
          section_ptr->addAccessibleChild(&component);
          row_focus_order_.push_back(&component);
          section_y += 44;
        };

        addRoutingControl(routing_mode_);
        addRoutingControl(routing_default_);
        addRoutingControl(routing_serial_forward_);
        addRoutingControl(routing_serial_backward_);
      }
      else if (section_name.startsWith("LFO ")) {
        const int lfo = section_name.fromFirstOccurrenceOf("LFO ", false, false).getIntValue() - 1;
        if (isPositiveAndBelow(lfo, vital::kNumLfos)) {
          addActionButton("Import LFO preset", "Load an LFO shape into " + section_name,
                          [this, lfo] { chooseLfoPresetFile(lfo); });
          addActionButton("Save LFO preset", "Save the current shape from " + section_name,
                          [this, lfo] { saveLfoPresetFile(lfo); });

          auto shape = std::make_unique<OffscreenComboBox>();
          auto* shape_ptr = shape.get();
          shape_ptr->setTitle(section_name + " shape preset");
          shape_ptr->setDescription("Choose a standard shape for " + section_name);
          shape_ptr->setHelpText("Choose a shape, then use the apply shape button");
          shape_ptr->setWantsKeyboardFocus(true);
          shape_ptr->addItemList(lfoShapePresetNames(), 1);
          shape_ptr->setSelectedItemIndex(lfo_shape_preset_index_, dontSendNotification);
          // G and H in the keyboard editor step this menu's choice.
          shape_ptr->getProperties().set("LfoShapePreset", lfo);
          shape_ptr->setExplicitFocusOrder(focus_order++);
          shape_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                               jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(shape_ptr);
          section_ptr->addAccessibleChild(shape_ptr);
          row_focus_order_.push_back(shape_ptr);
          section_headers_.push_back(std::move(shape));
          section_y += 44;

          auto apply_shape = std::make_unique<OffscreenTextButton>("Apply shape");
          auto* apply_shape_ptr = apply_shape.get();
          apply_shape_ptr->setTitle(section_name + " apply shape");
          apply_shape_ptr->setDescription("Replace " + section_name + " with the selected shape preset");
          apply_shape_ptr->setHelpText("Press Enter to apply the selected shape to " + section_name);
          apply_shape_ptr->setWantsKeyboardFocus(true);
          apply_shape_ptr->onClick = [this, lfo, shape_ptr] {
            active_lfo_index_ = lfo;
            lfo_shape_preset_index_ = shape_ptr->getSelectedItemIndex();
            applyLfoShapePreset();
          };
          apply_shape_ptr->setExplicitFocusOrder(focus_order++);
          apply_shape_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                                     jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(apply_shape_ptr);
          section_ptr->addAccessibleChild(apply_shape_ptr);
          row_focus_order_.push_back(apply_shape_ptr);
          section_headers_.push_back(std::move(apply_shape));
          section_y += 44;

          auto keyboard = std::make_unique<LfoMsegKeyboardComponent>();
          auto* keyboard_ptr = keyboard.get();
          keyboard_ptr->setTitle(section_name + " accessible editor");
          keyboard_ptr->setDescription("Keyboard editor for " + section_name + " points, values, and curves");
          keyboard_ptr->onKeyPressed = [this, lfo](const KeyPress& key) {
            active_lfo_index_ = lfo;
            syncLfoEditorState();
            return handleLfoMsegShortcut(key);
          };
          keyboard_ptr->getStatusText = [this, lfo] { return lfoMsegStatusTextFor(lfo); };
          keyboard_ptr->setExplicitFocusOrder(focus_order++);
          keyboard_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                                  jmax(620, width - (flatten_section ? 32 : 34)), 74);
          section_ptr->addAndMakeVisible(keyboard_ptr);
          section_ptr->addAccessibleChild(keyboard_ptr);
          row_focus_order_.push_back(keyboard_ptr);
          section_headers_.push_back(std::move(keyboard));
          section_y += 82;
        }
      }
      else if (effectIdForSection(section_name).isNotEmpty()) {
        addActionButton("Import FX preset", "Load an FX preset into " + accessibleSectionTitle(section_name),
                        [this, section_name] { chooseEffectPresetFile(section_name); });
        addActionButton("Save FX preset", "Save the current " + accessibleSectionTitle(section_name) + " settings",
                        [this, section_name] { saveEffectPresetFile(section_name); });
      }

      // An arpeggiator's destination rows nest in a Destinations group that
      // VoiceOver can enter, after its other rows.
      AccessibleSectionHeader* arp_destinations = nullptr;
      int destinations_y = 0;
      const int row_x = flatten_section ? 16 : 10;
      const int row_width = jmax(620, width - (flatten_section ? 32 : 34));
      auto close_arp_destinations = [&] {
        if (arp_destinations == nullptr)
          return;
        arp_destinations->setBounds(row_x, section_y, row_width, destinations_y + 2);
        section_y += destinations_y + 10;
        arp_destinations = nullptr;
      };
      for (auto* parameter : sections_[section_name]) {
        if (oscillator_section >= 0)
          break;  // Oscillator rows were grouped above.
        if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
          if (bridge->getParameterId().endsWith("effect_chain_order") ||
              bridge->getParameterId().endsWith("post_effect_order") ||
              bridge->getParameterId().contains("_effect_chain_slot_") ||
              bridge->getParameterId().startsWith("effect_chain_slot_") ||
              isEffectSlotParameter(section_name, bridge->getParameterId()))
            continue;
        if (!shouldShowParameterInSection(section_name, parameter))
          continue;

        auto* parameter_bridge = dynamic_cast<ValueBridge*>(parameter);
        const String parameter_id = parameter_bridge != nullptr ? parameter_bridge->getParameterId() : String();
        if (!isArpeggiatorDestination(section_name, parameter_id))
          close_arp_destinations();
        else if (arp_destinations == nullptr) {
          auto header = makeSectionHeader("Destinations", groupRank(group), false, section_name + " destinations");
          arp_destinations = header.get();
          arp_destinations->setDescription("Oscillators " + section_name + " plays");
          arp_destinations->setExplicitFocusOrder(focus_order++);
          arp_destinations->setBounds(row_x, section_y, row_width, arp_destinations->headerHeight());
          section_ptr->addAndMakeVisible(arp_destinations);
          section_ptr->addAccessibleChild(arp_destinations);
          row_focus_order_.push_back(arp_destinations);
          section_headers_.push_back(std::move(header));
          destinations_y = arp_destinations->headerHeight() + 8;
        }

        if (parameter_id == "volume")
          addCpuMeter(*section_ptr, row_x, section_y, row_width, focus_order);

        auto row = createConfiguredSectionRow(*parameter, section_name);
        row->setExplicitFocusOrder(focus_order++);
        row->setControlFocusOrder(focus_order++);
        AccessibleSectionHeader& row_group = arp_destinations != nullptr ? *arp_destinations : *section_ptr;
        if (arp_destinations != nullptr) {
          row->setBounds(10, destinations_y, jmax(580, row_width - 20), 48);
          destinations_y += 48;
        }
        else {
          row->setBounds(row_x, section_y, row_width, 48);
          section_y += 48;
        }
        row_group.addAndMakeVisible(row.get());
        row_group.addAccessibleChild(row->focusableControl());
        row_focus_order_.push_back(row->focusableControl());
        rows_.push_back(std::move(row));

        if (auto* bridge = dynamic_cast<ValueBridge*>(parameter);
            bridge != nullptr && bridge->getParameterId().startsWith("osc_") &&
            bridge->getParameterId().endsWith("_spectral_morph_seed")) {
          const String id = bridge->getParameterId();
          addActionButton(id.contains("_secondary_") ? "Randomize secondary amplitudes seed" : "Randomize amplitudes seed",
                          "Choose a new random amplitudes pattern for this morph slot",
                          [this, id] { randomizeAmplitudeSeed(id); });
        }
      }
      close_arp_destinations();

      if (isEffectChainSection(section_name)) {
        auto summary = std::make_unique<Label>();
        auto* summary_ptr = summary.get();
        summary_ptr->setTitle("Effects chain order");
        summary_ptr->setDescription("Current order of the effects chain");
        summary_ptr->getProperties().set("SectionName", section_name);
        summary_ptr->getProperties().set("EffectChainSummary", true);
        summary_ptr->setColour(Label::textColourId, Colours::white);
        summary_ptr->setJustificationType(Justification::centredLeft);
        summary_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                               jmax(620, width - (flatten_section ? 32 : 34)), 54);
        section_ptr->addAndMakeVisible(summary_ptr);
        section_ptr->addAccessibleChild(summary_ptr);
        section_headers_.push_back(std::move(summary));
        section_y += 60;

        addFxBusControlsGroup(section_name, *section_ptr, section_y, flatten_section ? 16 : 10,
                              jmax(620, width - (flatten_section ? 32 : 34)), focus_order);

        auto add_effect = std::make_unique<AccessibleTextButton>("Add effect");
        auto* add_effect_ptr = add_effect.get();
        add_effect_ptr->setTitle("Add effect");
        add_effect_ptr->setDescription("Open the list of effect modules to add to this chain");
        add_effect_ptr->setHelpText("Press Enter to choose an effect module to add");
        add_effect_ptr->getProperties().set("SectionName", section_name);
        add_effect_ptr->setWantsKeyboardFocus(true);
        add_effect_ptr->setExplicitFocusOrder(focus_order++);
        add_effect_ptr->onClick = [this, section_name, add_effect_ptr] {
          showAddEffectMenu(section_name, *add_effect_ptr);
        };
        add_effect_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                                  jmax(620, width - (flatten_section ? 32 : 34)), 38);
        section_ptr->addAndMakeVisible(add_effect_ptr);
        section_ptr->addAccessibleChild(add_effect_ptr);
        row_focus_order_.push_back(add_effect_ptr);
        section_y += 44;
        section_headers_.push_back(std::move(add_effect));

        addEffectSlotGroups(section_name, *section_ptr, section_y, flatten_section ? 16 : 10,
                            jmax(620, width - (flatten_section ? 32 : 34)), focus_order);
      }

      if (flatten_section)
        group_y = section_y + 8;
      else {
        section_ptr->setBounds(16, group_y, jmax(620, width - 24), section_y);
        group_y += section_y + 8;
      }
    }
    group_ptr->setBounds(0, y, width, group_y + 8);
    y += group_y + 12;
  }

  refreshEffectChainSummaries();
  rows_container_.setSize(width, y);
  viewport_.setViewPosition(0, 0);
  rebuildFocusOrder();

  if (announce)
    postPluginAnnouncement("All controls visible, grouped like Surge, " +
                                             String(rows_.size()) + " controls",
                                             AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::armOrCancelMidiLearn(const String& parameterId) {
  // Pressing Shift L again on the same control cancels: an armed learn binds
  // whatever controller moves next, such as a mod wheel touched by accident.
  if (synth_.midiLearnArmedName() == parameterId.toStdString()) {
    synth_.cancelMidiLearn();
    postPluginAnnouncement("MIDI learn cancelled for " + modulationDestinationLabelForId(parameterId),
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  synth_.armMidiLearn(parameterId.toStdString());
  postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId) +
                             ". Move a MIDI control to map it, or press Shift L again to cancel",
                         AccessibilityHandler::AnnouncementPriority::high);
}

PopupMenu SynthEditor::createMidiLearnMenu() {
  PopupMenu menu;
  const auto mappings = synth_.midiLearnMappings();
  if (mappings.empty()) {
    menu.addItem("No MIDI learn mappings", false, false, nullptr);
    return menu;
  }
  for (const auto& mapping : mappings) {
    const int controller = mapping.first;
    const String destination(mapping.second);
    menu.addItem("Remove " + midiControlName(controller) + " to " + modulationDestinationLabelForId(destination),
                 [this, destination, controller] {
                   synth_.clearMidiLearn(destination.toStdString());
                   postPluginAnnouncement("Removed " + midiControlName(controller) + " to " +
                                              modulationDestinationLabelForId(destination),
                                          AccessibilityHandler::AnnouncementPriority::high);
                 });
  }
  menu.addSeparator();
  menu.addItem("Remove all MIDI learn mappings", [this] {
    synth_.clearAllMidiLearn();
    postPluginAnnouncement("Removed all MIDI learn mappings", AccessibilityHandler::AnnouncementPriority::high);
  });
  return menu;
}

void SynthEditor::showNavigationMenu() {
  PopupMenu menu;
  menu.addSectionHeader("Editor");
  menu.addItem(6, "Load tuning file...");
  if (!synth_.getTuning()->isDefault())
    menu.addItem(7, "Clear tuning: " + String(synth_.getTuning()->getName()));
  menu.addSeparator();
  const int mapping_count = static_cast<int>(synth_.midiLearnMappings().size());
  menu.addSubMenu("MIDI learn mappings (" + String(mapping_count) + ")", createMidiLearnMenu());
  menu.addItem(4, "Accessibility settings");
  menu.addItem(8, "Keyboard help");

  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(menu_button_),
                     [this](int result) {
    if (result == 6)
      loadTuningFile();
    else if (result == 7)
      clearTuning();
    else if (result == 4)
      showAccessibilitySettingsMenu();
    else if (result == 8)
      showKeyboardHelp();
  });
}

void SynthEditor::showAccessibilitySettingsMenu() {
  struct Entry {
    const char* label;
    bool AccessibilitySpeechSettings::* field;
  };

  static const Entry kEntries[] = {
    { "Use speech feedback",               &AccessibilitySpeechSettings::speech_feedback  },
    { "Navigation announcements",          &AccessibilitySpeechSettings::navigation       },
    { "Preset announcements",              &AccessibilitySpeechSettings::presets          },
    { "LFO and Trance gate editor announcements",          &AccessibilitySpeechSettings::lfo_editor       },
    { "Wavetable editor announcements",    &AccessibilitySpeechSettings::wavetable_editor },
    { "Sample and granular announcements", &AccessibilitySpeechSettings::sample_browser   },
    { "Modulation announcements",          &AccessibilitySpeechSettings::modulation       },
    { "Parameter value announcements",     &AccessibilitySpeechSettings::parameters       },
    { "Other announcements",               &AccessibilitySpeechSettings::other            },
    { "Frequency values in Hz",            &AccessibilitySpeechSettings::frequency_values_in_hz },
  };

  class DialogContent : public Component {
  public:
    DialogContent() {
      auto& settings = accessibilitySpeechSettings();
      for (const auto& e : kEntries) {
        auto btn = std::make_unique<SpokenToggleButton>(e.label);
        btn->setToggleState(settings.*(e.field), dontSendNotification);
        auto* btn_ptr = btn.get();
        auto field = e.field;
        btn->onStateChange = [btn_ptr, field]() {
          accessibilitySpeechSettings().*field = btn_ptr->getToggleState();
          saveAccessibilitySpeechSettings();
        };
        addAndMakeVisible(*btn);
        buttons_.push_back(std::move(btn));
      }
      setSize(360, static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0])) * 28 + 16);
    }

    void resized() override {
      int y = 8;
      for (auto& btn : buttons_) {
        btn->setBounds(8, y, getWidth() - 16, 24);
        y += 28;
      }
    }

  private:
    std::vector<std::unique_ptr<ToggleButton>> buttons_;
  };

  DialogWindow::LaunchOptions opts;
  opts.dialogTitle = "Accessibility Settings";
  opts.content.setOwned(new DialogContent());
  opts.componentToCentreAround = this;
  opts.useNativeTitleBar = true;
  opts.resizable = false;
  opts.escapeKeyTriggersCloseButton = true;
  auto* dlg = opts.launchAsync();
  if (dlg) {
    Component::SafePointer<DialogWindow> safe_dlg(dlg);
    MessageManager::callAsync([safe_dlg]() {
      if (safe_dlg && safe_dlg->getContentComponent()) {
        if (auto* first = safe_dlg->getContentComponent()->getChildComponent(0))
          first->grabKeyboardFocus();
      }
    });
  }
}

namespace {
  // The keyboard help with this platform's key names: {Cmd} is Command on
  // macOS and Ctrl elsewhere. Windows opens context menus with Shift F10 as
  // well as Shift M, and not with the right bracket (kRightBracketOpensMenus),
  // and adds comma and period stepping through effect slots.
  String keyboardHelpForPlatform(const char* text) {
#if JUCE_MAC
    const String command = "Cmd", context_menu = "Right bracket ( ] ) or Shift+M", heading_menu = "Shift+M";
#else
    const String command = "Ctrl", context_menu = "Shift+M or Shift+F10", heading_menu = context_menu;
#endif
    const String effect_slot_steps = kSectionKeysStepThroughEffectSlots
        ? "  On an effects chain, Comma / Period also step through its effects (Slot 1, Slot 2 and so on)\n"
        : "";
    return String(text).replace("{Cmd}", command).replace("{ContextMenuKeys}", context_menu)
                       .replace("{HeadingMenuKeys}", heading_menu)
                       .replace("{EffectSlotSteps}", effect_slot_steps);
  }
}

void SynthEditor::showKeyboardHelp() {
  // NOTE: This text is the single source of on-screen keyboard help. Whenever a keyboard shortcut
  // is added or changed anywhere in the plugin (focusShortcutTarget, OffscreenSlider::keyPressed,
  // SynthEditor::keyPressed, handleLfoMsegShortcut, handleWavetableShortcut, handleMacroShortcut, and the preset/popup
  // browser keyPressed overrides), add or update its entry below under the matching group.
  static const char* const kHelpText =
      "ATLAS keyboard shortcuts\n"
      "\nTrance gate editor\n"
      "  Left/Right move between steps. Up/Down or brackets change values; Shift uses 0.05 instead of 0.01.\n"
      "  Comma/Period decrease/increase the curve by 1%; Shift uses 5%. I/K also increase/decrease the curve.\n"
      "  G/H choose one of the five StepGate shapes; A applies it; M/Shift M cycle the current step shape forward/backward.\n"
      "  Minus/Equals change speed. Semicolon/Apostrophe shorten/lengthen the pattern range.\n"
      "  Delete clears the current step to silence. Backspace clears all steps. Global controls use Atlas slider shortcuts.\n"
      "  Navigation announces position first. Edits announce only the changed value.\n"
      "\n"
      "Most single-letter shortcuts work when a parameter or the editor has focus, but not while a "
      "text field is focused.\n"
      "\n"
      "Section navigation\n"
      "  O - Oscillators\n"
      "  X - Mixer and routing\n"
      "  F - Filters\n"
      "  E - Envelopes\n"
      "  L - LFOs\n"
      "  M - Modulation routing\n"
      "  P - Effects\n"
      "  R - Signal routing\n"
      "  Z - Zones\n"
      "  G - Master and global\n"
      "  Ctrl+1 / Ctrl+2 / Ctrl+3 - Jump to Bus 1 / 2 / 3\n"
      "  1 to 9 - Jump to subsection within the current group\n"
      "  Comma ( , ) - Previous section\n"
      "  Period ( . ) - Next section\n"
      "{EffectSlotSteps}"
      "\n"
      "Headings with a menu\n"
      "  {HeadingMenuKeys} on an oscillator heading - Copy an oscillator or paste one into this one\n"
      "  {HeadingMenuKeys} on an effect slot heading - Copy the effect, paste it to another bus, replace or remove it\n"
      "\n"
      "Adjusting a parameter (when a slider has focus)\n"
      "  Up / Right / Page Up - Increase value\n"
      "  Down / Left / Page Down - Decrease value\n"
      "  Shift + arrow - Fine adjustment\n"
      "  Page Up / Page Down - Larger adjustment\n"
      "  Home - Maximum value\n"
      "  End - Minimum value\n"
      "  Enter - Type a value\n"
      "  Backspace / Delete - Reset to default\n"
      "  {ContextMenuKeys} - Open context menu\n"
      "  Shift+L - MIDI learn: the next MIDI control moved is mapped; Shift+L again cancels\n"
      "  Shift+C - Clear MIDI learn (Editor menu, MIDI learn mappings, lists and removes them all)\n"
      "  Levels and modulation amounts read and accept percentages: 0% to 100%, or -100% to 100%\n"
      "\n"
      "Choosing from a menu (Type, Model, Style and other choices that change which controls are shown)\n"
      "  Down / Right - Next choice; Up / Left - Previous choice\n"
      "  Page Down / Page Up - Move ten choices\n"
      "  Home - First choice; End - Last choice\n"
      "  Enter / Space - Open the list of choices; arrows move, Enter chooses\n"
      "  Escape - Close the list and keep the current choice\n"
      "  Backspace / Delete - Reset to default\n"
      "  {ContextMenuKeys} - Open context menu\n"
      "\n"
      "Virtual musical keyboard\n"
      "  Alt+K - Toggle the computer-keyboard piano on or off\n"
      "  While active:\n"
      "    C - Octave up\n"
      "    X - Octave down\n"
      "    B - Velocity up (Shift for +1)\n"
      "    V - Velocity down (Shift for -1)\n"
      "    Note keys: A W S E D F T G Y H U J K O L P ; [ ' ]\n"
      "\n"
      "Macros (when a macro has focus)\n"
      "  Shift+Enter - Rename macro\n"
      "  B - Toggle unipolar / bipolar\n"
      "\n"
      "Effects chain (when an effect has focus)\n"
      "  Left bracket ( [ ) - Move effect earlier in the chain\n"
      "  Right bracket ( ] ) - Move effect later in the chain\n"
      "  Shift+M on an effect's heading - Slot menu: copy, paste, replace or remove the effect\n"
      "  On an FX parameter, open its context menu and choose Automate to use the first free host slot\n"
      "\n"
      "LFO segment (MSEG) editor\n"
      "  Shift+1 to Shift+0 - Select LFO 1 to 10; use the LFO selector for LFO 11 and 12\n"
      "  LFO mode: Left / Right moves in time; Shift+Left / Right moves the selected point\n"
      "  LFO mode: Comma ( , ) / Period ( . ) selects the previous / next point\n"
      "  Performer: Comma / Period selects complete shapes; A inserts the selected preset\n"
      "  Performer: Up / Down moves the whole shape; I / K changes all curves in that shape\n"
      "  Performer: E or Delete removes every point belonging to the current shape\n"
      "  Step Sequencer: Left / Right selects a step; Up / Down changes its held value\n"
      "  Step Sequencer: E or Delete clears the step; curve and shape commands are ignored\n"
      "  Up / Down - Change point, shape, or step value by 1% (Shift for 5%)\n"
      "  Left bracket ( [ ) / Right bracket ( ] ) - Change value by 1% (Shift for 5%)\n"
      "  A - Add a point in LFO mode or insert the selected preset in Performer mode\n"
      "  S - Toggle point selection\n"
      "  Shift+S - Toggle multi-selection mode\n"
      "  R - Clear all selections\n"
      "  E or Delete - Delete the current point, complete performer shape, or step\n"
      "  I / K - Cycle curve type in LFO/Performer; ignored in Step Sequencer\n"
      "  {Cmd} + Up / Down - Cycle curve type up / down\n"
      "  J / L - Move point in time left / right\n"
      "  G / H - Cycle shape presets backward / forward\n"
      "  Shift+A - Apply selected shape preset\n"
      "  Semicolon ( ; ) / Apostrophe ( ' ) - Cycle LFO cycle length\n"
      "  Minus ( - ) / Equals ( = ) - Make grid coarser / finer\n"
      "  C / V - Copy / paste selected points\n"
      "  Shift+C / Shift+V - Copy / paste the whole shape\n"
      "  Shift+D - Duplicate shape to the next LFO\n"
      "  Backspace - Clear the LFO shape\n"
      "\n"
      "Modulation remap editor (when its keyboard editor has focus)\n"
      "  Left / Right or Comma ( , ) / Period ( . ) - Previous / next point\n"
      "  Shift + Left / Right - Move an interior point's input by 1% (Alt for 5%)\n"
      "  Up / Down - Change point output by 1% (Shift for 5%)\n"
      "  A - Add a point in the largest gap\n"
      "  E or Delete - Delete the selected interior point\n"
      "  I / K - Cycle the following segment's curve forward / backward\n"
      "  H / V - Flip the remap horizontally / vertically\n"
      "  Backspace - Reset the remap to linear\n"
      "  Navigation announces point details. Edits announce only the changed value or curve.\n"
      "\n"
      "Wavetable keyboard editor (when its editor row has focus)\n"
      "  Left / Right - Previous / next frame\n"
      "  Shift + Left / Right - Move and extend frame selection\n"
      "  Home / End - First / last frame\n"
      "  S - Toggle current frame selection\n"
      "  R - Clear frame selection\n"
      "  {Cmd}+A or Shift+A - Select all frames\n"
      "  Comma ( , ) / Period ( . ) - Previous / next harmonic\n"
      "  Page Up / Page Down - Move by ten harmonics\n"
      "  Up / Down - Change harmonic level by 1% (Shift for 5%)\n"
      "  Left bracket ( [ ) / Right bracket ( ] ) - Change harmonic level by 1% (Shift for 5%)\n"
      "  J / L - Change harmonic phase by 1 degree (Shift for 15 degrees)\n"
      "  Backspace - Clear current harmonic\n"
      "  F - Remove fundamental\n"
      "  0 - Remove DC offset\n"
      "  N - Normalize selected or current frames\n"
      "  A - Add a copy of the current frame\n"
      "  E or Delete - Delete selected or current frames\n"
      "  C / V - Copy / paste selected or current frames\n"
      "  1 to 6 - Apply Sine, Saturated sine, Triangle, Square, Pulse, or Saw\n"
      "  I - Cycle None, waveform, spectral, smooth waveform, and smooth spectral interpolation\n"
      "  Frame timeline position creates space between source frames for interpolation\n"
      "  Synthesize preset to wavetable - Capture four seconds of the full patch into this oscillator\n"
      "\n"
      "Presets and browsers\n"
      "  Collection, Library, Bank, Category, and Tags tables: Up / Down selects and applies a value\n"
      "  Preset table: Up / Down chooses a preset; Enter loads it; F changes its favourite state\n"
      "  S - Sort the preset table by the next column\n"
      "  Shift+S - Reverse the preset table sort direction\n"
      "  R - Rename the selected editable user preset\n"
      "  E - Edit the selected editable preset's metadata\n"
      "  {Cmd}+S - Save the patch over the loaded user preset, or ask where to save it\n"
      "  Shift+{Cmd}+S - Save the patch as a new preset\n"
      "  Delete - Ask before moving the selected editable user preset to Trash\n"
      "  Esc - Close the browser\n"
      "\n"
      "This dialog\n"
      "  Tab - Move to the Close button\n"
      "  Enter (on Close) or Esc - Close this dialog\n";

  class DialogContent : public Component, public TextEditor::Listener {
  public:
    DialogContent() {
      text_.setMultiLine(true, true);
      // Editable (not read-only) on purpose: a read-only JUCE TextEditor is exposed to the OS
      // accessibility layer as static text with no navigable caret, so screen readers re-read the
      // same line on every arrow press. An editable field exposes a movable caret and gives proper
      // line/word/character navigation. This mirrors the Airwindows Consolidated documentation
      // viewer's fix; edits are harmless since the text is reset on every open and discarded on
      // close. See the keyboard-help-dialog memory.
      text_.setReadOnly(false);
      text_.setScrollbarsShown(true);
      text_.setCaretVisible(true);
      text_.setAccessible(true);
      text_.setTabKeyUsedAsCharacter(false);
      text_.setWantsKeyboardFocus(true);
      text_.setExplicitFocusOrder(1);
      text_.setTitle("Keyboard shortcuts");
      text_.setText(keyboardHelpForPlatform(kHelpText), dontSendNotification);
      // The editable TextEditor swallows Escape, so it never reaches the DialogWindow's
      // escapeKeyTriggersCloseButton. Catch it here and close ourselves.
      text_.addListener(this);
      addAndMakeVisible(text_);

      close_.setWantsKeyboardFocus(true);
      close_.setExplicitFocusOrder(2);
      close_.setTitle("Close");
      close_.onClick = [this] { closeDialog(); };
      addAndMakeVisible(close_);

      setSize(520, 560);
    }

    void resized() override {
      const int margin = 8;
      const int button_height = 28;
      const int button_width = 100;
      text_.setBounds(margin, margin, getWidth() - 2 * margin,
                      getHeight() - button_height - 3 * margin);
      close_.setBounds((getWidth() - button_width) / 2,
                       getHeight() - button_height - margin, button_width, button_height);
    }

    bool keyPressed(const KeyPress& key) override {
      if (key.getKeyCode() == KeyPress::returnKey && close_.hasKeyboardFocus(false)) {
        closeDialog();
        return true;
      }
      return Component::keyPressed(key);
    }

    void textEditorEscapeKeyPressed(TextEditor&) override { closeDialog(); }

  private:
    void closeDialog() {
      if (auto* dw = findParentComponentOfClass<DialogWindow>())
        dw->exitModalState(0);
    }

    TextEditor text_;
    TextButton close_{"Close"};
  };

  DialogWindow::LaunchOptions opts;
  opts.dialogTitle = "Keyboard help";
  opts.content.setOwned(new DialogContent());
  opts.componentToCentreAround = this;
  opts.useNativeTitleBar = true;
  opts.resizable = true;
  opts.escapeKeyTriggersCloseButton = true;
  auto* dlg = opts.launchAsync();
  if (dlg) {
    Component::SafePointer<DialogWindow> safe_dlg(dlg);
    MessageManager::callAsync([safe_dlg]() {
      if (safe_dlg && safe_dlg->getContentComponent()) {
        if (auto* first = safe_dlg->getContentComponent()->getChildComponent(0))
          first->grabKeyboardFocus();
      }
    });
  }
}

File SynthEditor::defaultPatchFile() const {
  return LoadSave::getUserPresetDirectory().getChildFile("Default").withFileExtension(vital::kPresetExtension);
}

void SynthEditor::initializePatch() {
  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();
  const File default_file = defaultPatchFile();
  std::string error;
  if (default_file.existsAsFile() && synth_.loadFromFile(default_file, error)) {
    last_preset_path_ = default_file.getFullPathName();
    synth_.updateHostDisplay();
    updateFullGui();
    selectPresetFile(default_file);
    updatePresetSummary();
    restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);
    postPluginAnnouncement("Loaded default patch", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  synth_.loadInitPreset();
  synth_.updateHostDisplay();
  updateFullGui();
  refreshPresetList();
  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);
  postPluginAnnouncement("Initialized patch", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::savePatchAsDefault() {
  const File destination = defaultPatchFile().withFileExtension(vital::kPresetExtension);
  if (!synth_.saveCopyToFile(destination)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save default patch",
                                          "The default patch could not be written.");
    postPluginAnnouncement("Unable to save default patch", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  refreshPresetList();
  updatePresetSummary();
  postPluginAnnouncement("Saved patch as default", AccessibilityHandler::AnnouncementPriority::high);
}


void SynthEditor::rebuildFocusOrder() {
  focus_order_.clear();
  accessibility_focus_order_.clear();
  focus_order_.push_back(&menu_button_);
  accessibility_focus_order_.push_back(&menu_button_);
  if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) {
    focus_order_.push_back(&modulation_amount_editor_);
    focus_order_.push_back(&modulation_amount_ok_);
    focus_order_.push_back(&modulation_amount_cancel_);
    accessibility_focus_order_.push_back(&modulation_amount_editor_);
    accessibility_focus_order_.push_back(&modulation_amount_ok_);
    accessibility_focus_order_.push_back(&modulation_amount_cancel_);
  }
  if (save_patch_dialog_visible_) {
    focus_order_.push_back(&save_patch_name_);
    focus_order_.push_back(&save_patch_author_);
    focus_order_.push_back(&save_patch_bank_);
    focus_order_.push_back(&save_patch_category_);
    focus_order_.push_back(&save_patch_tags_);
    focus_order_.push_back(&save_patch_comments_);
    focus_order_.push_back(&save_patch_ok_);
    focus_order_.push_back(&save_patch_cancel_);
    accessibility_focus_order_.push_back(&save_patch_name_);
    accessibility_focus_order_.push_back(&save_patch_author_);
    accessibility_focus_order_.push_back(&save_patch_bank_);
    accessibility_focus_order_.push_back(&save_patch_category_);
    accessibility_focus_order_.push_back(&save_patch_tags_);
    accessibility_focus_order_.push_back(&save_patch_comments_);
    accessibility_focus_order_.push_back(&save_patch_ok_);
    accessibility_focus_order_.push_back(&save_patch_cancel_);
  }
  for (auto* component : row_focus_order_)
    focus_order_.push_back(component);
  for (auto* component : top_level_accessibility_order_)
    accessibility_focus_order_.push_back(component);
}

std::unique_ptr<ComponentTraverser> SynthEditor::createFocusTraverser() {
  return std::make_unique<OrderedFocusTraverser>(*this, false);
}

std::unique_ptr<ComponentTraverser> SynthEditor::createKeyboardFocusTraverser() {
  return std::make_unique<OrderedFocusTraverser>(*this, true);
}

void SynthEditor::ensureComponentVisible(Component* component) {
  if (component == nullptr)
    return;

  Component* parent = component;
  bool inside_rows = false;
  while (parent != nullptr) {
    if (parent == &rows_container_) {
      inside_rows = true;
      break;
    }
    parent = parent->getParentComponent();
  }
  if (!inside_rows)
    return;

  const auto area = rows_container_.getLocalArea(component, component->getLocalBounds()).expanded(0, 12);
  const int current_y = viewport_.getViewPositionY();
  const int visible_height = viewport_.getViewHeight();
  int target_y = current_y;
  if (area.getY() < current_y)
    target_y = area.getY();
  else if (area.getBottom() > current_y + visible_height)
    target_y = area.getBottom() - visible_height;
  target_y = jlimit(0, jmax(0, rows_container_.getHeight() - visible_height), target_y);
  if (target_y != current_y)
    viewport_.setViewPosition(0, target_y);
}

std::vector<std::pair<String, String>> SynthEditor::modulationSourcesForDestination(const String& destinationId) const {
  std::vector<std::pair<String, String>> sources;
  StringArray seen_sources;
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name.empty() || route->destination_name != destinationId.toStdString())
      continue;

    const String source_id(route->source_name);
    if (seen_sources.contains(source_id))
      continue;

    seen_sources.add(source_id);
    sources.push_back({ source_id, modulationSourceLabelForId(source_id) });
  }

  std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
    return a.second.compareNatural(b.second) < 0;
  });
  return sources;
}

void SynthEditor::removeModulationFromParameter(const String& sourceId, const String& destinationId, Component& target) {
  if (sourceId.isEmpty() || destinationId.isEmpty())
    return;

  const String source_label = modulationSourceLabelForId(sourceId);
  const String destination_label = modulationDestinationLabelForId(destinationId);
  String restore_parameter;
  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl() == &target) {
      restore_parameter = row->parameterId();
      break;
    }
  }
  const String restore_title = target.getTitle();
  const String restore_section = restore_parameter.isNotEmpty() ? sectionForParameter(restore_parameter) : String();
  // Refreshing the routes can rebuild the modulation rows, deleting target.
  // A control outside the rows outlives the rebuild, though its title may
  // change now that the route is gone, so keep a pointer to it as well.
  Component::SafePointer<Component> persistent(rows_container_.isParentOf(&target) ? nullptr : &target);
  synth_.disconnectModulation(sourceId.toStdString(), destinationId.toStdString());
  refreshVisibleModulationLabels();
  showAllSections(false);
  if (!restoreFocusAfterRebuild(restore_parameter, persistent.getComponent(), restore_title, restore_section) &&
      destinationId != restore_parameter)
    restoreFocusAfterRebuild(destinationId, nullptr, {}, sectionForParameter(destinationId));
  postPluginAnnouncement("Removed modulation from " + source_label + " to " + destination_label,
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::clearModulationSlot(int slot) {
  auto* connection = isPositiveAndBelow(slot - 1, vital::kMaxModulationConnections)
      ? synth_.getModulationBank().atIndex(slot - 1) : nullptr;
  if (connection == nullptr || (connection->source_name.empty() && connection->destination_name.empty()))
    return;

  synth_.disconnectModulation(connection);
  // A slot holding only a source or only a destination was never connected.
  connection->resetConnection("", "");
  // Leave the slot as a new route would find it.
  const String prefix = "modulation_" + String(slot) + "_";
  for (const char* suffix : { "amount", "power", "bipolar", "stereo", "bypass", "random_mode" })
    setParameterEngineValue(prefix + suffix, 0.0f);
  if (connection->modulation_processor != nullptr)
    connection->modulation_processor->lineMapGenerator()->initLinear();

  refreshVisibleModulationLabels();
  const String announcement = modulationSlotHeaderTitle(slot) + " cleared";
  // The emptied slot loses its remap editor and the Clear button that called
  // this, so rebuild once the click has returned and land on the slot's heading.
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this, slot, announcement] {
    if (safe_this == nullptr)
      return;
    safe_this->showAllSections(false);
    // Find the heading by its slot: its title is the whole route summary, and
    // the timer may have refreshed it since this was queued.
    Component* heading = nullptr;
    for (auto& header : safe_this->section_headers_) {
      if (header != nullptr && header->getProperties().contains("ModulationSlot") &&
          static_cast<int>(header->getProperties()["ModulationSlot"]) == slot) {
        heading = header.get();
        break;
      }
    }
    safe_this->restoreFocusAfterRebuild({}, heading, safe_this->modulationSlotTitle(slot));
    postPluginAnnouncement(announcement, AccessibilityHandler::AnnouncementPriority::high);
  });
}

String SynthEditor::modulationSlotTitle(int slot) const {
  // One slot is one route. Announce it on its own: source, destination, amount,
  // and where it sits among the source's routes, so no counting is needed.
  const String title = modulationSlotHeaderTitle(slot);
  auto* connection = synth_.getModulationBank().atIndex(slot - 1);
  if (connection == nullptr || (connection->source_name.empty() && connection->destination_name.empty()))
    return title + ", empty";

  const String source = connection->source_name.empty() ? String()
                                                        : modulationSourceLabelForId(String(connection->source_name));
  if (connection->destination_name.empty())
    return title + ", " + source + ", no destination";
  const String destination = modulationDestinationLabelForId(String(connection->destination_name));
  if (connection->source_name.empty())
    return title + ", no source, to " + destination;

  const String prefix = "modulation_" + String(slot) + "_";
  String amount_text;
  if (auto* amount_bridge = parameterBridge(prefix + "amount")) {
    const String percent = amount_bridge->getText(amount_bridge->getValue(), 128);
    amount_text = percent.startsWith("-") || percent == "0%" ? percent : "+" + percent;
  }
  String bypass_text;
  if (auto* bypass_bridge = parameterBridge(prefix + "bypass")) {
    if (bypass_bridge->convertToEngineValue(bypass_bridge->getValue()) >= 0.5f)
      bypass_text = ", bypassed";
  }

  // Position of this route among the routes from the same source, in slot order.
  int position = 0, count = 0;
  for (int other = 1; other <= vital::kMaxModulationConnections; ++other) {
    auto* candidate = synth_.getModulationBank().atIndex(other - 1);
    if (candidate == nullptr || candidate->source_name != connection->source_name || candidate->destination_name.empty())
      continue;
    ++count;
    if (other == slot)
      position = count;
  }
  String route_text;
  if (count > 1)
    route_text = ", route " + String(position) + " of " + String(count) + " from " + source;

  String result = title + ", " + source + " to " + destination;
  if (amount_text.isNotEmpty())
    result += ", " + amount_text;
  return result + bypass_text + route_text;
}

String SynthEditor::modulationSlotHeaderTitle(int slot) const {
  return "Modulation " + numberWord(slot);
}

String SynthEditor::modulationControlTitle(const String& parameterId) const {
  const String rest = parameterId.fromFirstOccurrenceOf("modulation_", false, false);
  const String suffix = rest.fromFirstOccurrenceOf("_", false, false);
  if (suffix == "amount")
    return "Amount";
  if (suffix == "power")
    return "Power";
  if (suffix == "bipolar")
    return "Bipolar";
  if (suffix == "stereo")
    return "Stereo";
  if (suffix == "bypass")
    return "Bypass";
  if (suffix == "random_mode")
    return "Random correlation";
  return readableId(suffix);
}

void SynthEditor::showPresetControls() {
  preset_summary_.setVisible(true);
  preset_details_.setVisible(true);
  preset_menu_.setVisible(true);
  preset_search_.setVisible(true);
  preset_collection_table_.setVisible(true);
  preset_library_table_.setVisible(true);
  preset_bank_table_.setVisible(true);
  preset_category_table_.setVisible(true);
  preset_tag_table_.setVisible(true);
  preset_table_.setVisible(true);
  preset_preview_.setVisible(true);
  updatePresetSummary();
  resized();
  // VoiceOver can enter the tables without triggering their keyboard focus callbacks.
  ensurePresetListLoaded();
}

void SynthEditor::ensurePresetListLoaded() {
  if (preset_list_loaded_ || preset_list_loading_)
    return;

  startPresetListLoad(false);
}

void SynthEditor::refreshPresetList(bool announce) {
  startPresetListLoad(announce, true);
}

void SynthEditor::startPresetListLoad(bool announce, bool forceRefresh) {
  const int generation = ++preset_list_generation_;

  if (!forceRefresh) {
    std::vector<PresetBrowserItem> cached_items;
    bool cache_valid = false;
    {
      const ScopedLock lock(presetBrowserCacheLock());
      cache_valid = cachedPresetBrowserItemsValid();
      if (cache_valid)
        cached_items = cachedPresetBrowserItems();
    }

    if (cache_valid) {
      applyPresetList(std::move(cached_items), announce, generation);
      return;
    }
  }

  // A refresh keeps the current list on screen until the new one is ready;
  // only a first load starts from empty tables.
  const bool keep_current_list = preset_list_loaded_;
  preset_list_loading_ = true;
  preset_index_progress_.clear();
  if (!keep_current_list) {
    preset_list_loaded_ = false;
    filtered_presets_.clear();
    preset_facets_.clear();
    preset_collection_table_.updateContent();
    preset_library_table_.updateContent();
    preset_bank_table_.updateContent();
    preset_category_table_.updateContent();
    preset_tag_table_.updateContent();
    preset_table_.updateContent();
  }
  updatePresetSummary();

  Component::SafePointer<SynthEditor> safe_this(this);
  // The scan is joined when the editor closes: a thread that posts to the
  // message queue after the editor (or the whole plugin) is gone crashes.
  for (auto it = preset_scans_.begin(); it != preset_scans_.end();) {
    if (it->finished->load()) {
      it->thread.join();
      it = preset_scans_.erase(it);
    }
    else
      ++it;
  }
  auto cancelled = preset_scan_cancelled_;
  auto finished = std::make_shared<std::atomic<bool>>(false);
  PresetScan scan;
  scan.finished = finished;
  scan.thread = std::thread([safe_this, generation, announce, forceRefresh, keep_current_list, cancelled, finished] {
    struct Finish { std::shared_ptr<std::atomic<bool>> flag; ~Finish() { flag->store(true); } } finish { finished };
    auto post = [&cancelled](std::function<void()> message) {
      if (!cancelled->load())
        MessageManager::callAsync(std::move(message));
    };
    // The index is reused even on a refresh: a preset whose size and date are
    // unchanged is not read again, so a refresh only indexes new or edited files.
    auto persisted_items = loadPresetIndexDatabase();
    bool displayed_persisted_database = false;
    if (!forceRefresh && !keep_current_list && !persisted_items.empty()) {
      std::vector<PresetBrowserItem> database_items;
      database_items.reserve(persisted_items.size());
      for (const auto& [path, item] : persisted_items)
        database_items.push_back(item);

      {
        const ScopedLock lock(presetBrowserCacheLock());
        cachedPresetBrowserItems() = database_items;
        cachedPresetBrowserItemsValid() = true;
      }

      displayed_persisted_database = true;
      post([safe_this, generation, database_items = std::move(database_items)]() mutable {
        if (safe_this != nullptr)
          safe_this->applyPresetList(std::move(database_items), false, generation, true);
      });
    }

    Array<File> presets;
    LoadSave::getAllPresets(presets);
    LoadSave::FileSorterAscending sorter;
    presets.sort(sorter);

    const File data_directory = LoadSave::getDataDirectory();
    std::vector<PresetBrowserItem> items;
    items.reserve(static_cast<size_t>(presets.size()));
    bool database_changed = false;
    int to_index = 0;
    for (const auto& preset : presets) {
      if (cancelled->load())
        return;
      auto found = persisted_items.find(presetPathKey(preset).toStdString());
      if (found == persisted_items.end() || !presetFileStampMatches(found->second, preset))
        ++to_index;
    }
    int indexed = 0;
    uint32 last_report = Time::getMillisecondCounter();
    for (const auto& preset : presets) {
      if (cancelled->load())
        return;
      const String key = presetPathKey(preset);
      auto found = persisted_items.find(key.toStdString());
      if (found != persisted_items.end() && presetFileStampMatches(found->second, preset)) {
        items.push_back(found->second);
        continue;
      }

      items.push_back(buildPresetBrowserItem(preset, data_directory));
      database_changed = true;
      ++indexed;
      if (Time::getMillisecondCounter() - last_report > 500) {
        last_report = Time::getMillisecondCounter();
        const String progress = "Indexing presets, " + String(indexed) + " of " + String(to_index) + " new or changed";
        post([safe_this, generation, progress] {
          if (safe_this != nullptr && generation == safe_this->preset_list_generation_) {
            safe_this->preset_index_progress_ = progress;
            safe_this->updatePresetSummary();
          }
        });
      }
    }
    if (persisted_items.size() != static_cast<size_t>(presets.size()))
      database_changed = true;
    if (database_changed)
      savePresetIndexDatabase(items);

    {
      const ScopedLock lock(presetBrowserCacheLock());
      cachedPresetBrowserItems() = items;
      cachedPresetBrowserItemsValid() = true;
    }

    if (displayed_persisted_database && !database_changed && !announce) {
      post([safe_this, generation] {
        if (safe_this != nullptr && generation == safe_this->preset_list_generation_) {
          safe_this->preset_list_loading_ = false;
          safe_this->preset_index_progress_.clear();
          safe_this->updatePresetSummary();
        }
      });
      return;
    }

    post([safe_this, generation, announce, items = std::move(items)]() mutable {
      if (safe_this != nullptr)
        safe_this->applyPresetList(std::move(items), announce, generation);
    });
  });
  preset_scans_.push_back(std::move(scan));
}

void SynthEditor::applyPresetList(std::vector<PresetBrowserItem> items, bool announce, int generation,
                                  bool stillScanning) {
  if (generation != preset_list_generation_)
    return;

  preset_index_ = std::move(items);
  preset_favorites_ = LoadSave::getFavorites();
  preset_list_loading_ = stillScanning;
  if (!stillScanning)
    preset_index_progress_.clear();
  preset_list_loaded_ = true;
  all_presets_.clear();
  preset_index_by_path_.clear();
  preset_index_by_path_.reserve(preset_index_.size());
  for (size_t i = 0; i < preset_index_.size(); ++i) {
    const auto& item = preset_index_[i];
    all_presets_.add(item.file);
    preset_index_by_path_[item.file.getFullPathName().toStdString()] = i;
  }
  populatePresetFilters();
  filterPresetList();
  updatePresetSummary();
  if (announce)
    postPluginAnnouncement("Preset list refreshed, " + String(all_presets_.size()) + " presets found",
                                           AccessibilityHandler::AnnouncementPriority::medium);
}

const PresetBrowserItem* SynthEditor::presetItemForFile(const File& file) const {
  const auto found = preset_index_by_path_.find(file.getFullPathName().toStdString());
  if (found == preset_index_by_path_.end() || found->second >= preset_index_.size())
    return nullptr;
  return &preset_index_[found->second];
}

void SynthEditor::populatePresetFilters() {
  ScopedValueSetter<bool> guard(updating_preset_list_, true);

  preset_libraries_.clear();
  preset_libraries_.add(kAllLibraries);
  std::set<String> libraries;
  for (const auto& item : preset_index_) {
    if (item.library.isNotEmpty())
      libraries.insert(item.library);
  }
  for (const auto& library : libraries) {
    if (library != kAllLibraries)
      preset_libraries_.add(library);
  }
  if (!preset_libraries_.contains(last_preset_library_))
    last_preset_library_ = kAllLibraries;

  preset_banks_.clear();
  preset_banks_.add(kAllBanks);
  std::set<String> banks;
  for (const auto& item : preset_index_) {
    if (last_preset_library_ != kAllLibraries && item.library != last_preset_library_)
      continue;
    if (item.bank.isNotEmpty())
      banks.insert(item.bank);
  }
  for (const auto& bank : banks) {
    if (bank != kAllBanks)
      preset_banks_.add(bank);
  }
  if (!preset_banks_.contains(last_preset_bank_))
    last_preset_bank_ = kAllBanks;

  preset_categories_.clear();
  preset_categories_.add(kAllCategories);
  std::set<String> categories;
  for (const auto& item : preset_index_) {
    if (last_preset_library_ != kAllLibraries && item.library != last_preset_library_)
      continue;
    if (last_preset_bank_ != kAllBanks && item.bank != last_preset_bank_)
      continue;
    if (item.category.isNotEmpty())
      categories.insert(item.category);
  }
  for (const auto& category : categories) {
    if (category != kAllCategories)
      preset_categories_.add(category);
  }
  if (!preset_categories_.contains(last_preset_category_))
    last_preset_category_ = kAllCategories;

  preset_tags_.clear();
  preset_tags_.add(kAllTags);
  std::set<String> tags;
  for (const auto& item : preset_index_) {
    if (last_preset_library_ != kAllLibraries && item.library != last_preset_library_)
      continue;
    if (last_preset_bank_ != kAllBanks && item.bank != last_preset_bank_)
      continue;
    if (last_preset_category_ != kAllCategories && item.category != last_preset_category_)
      continue;
    for (const auto& tag : item.tags) {
      if (tag.isNotEmpty())
        tags.insert(tag);
    }
  }
  for (const auto& tag : tags) {
    if (tag != kAllTags)
      preset_tags_.add(tag);
  }
  if (!preset_tags_.contains(last_preset_tag_))
    last_preset_tag_ = kAllTags;

  auto matchesLibrary = [](const PresetBrowserItem& item, const String& library) {
    return library == kAllLibraries || item.library == library;
  };
  auto matchesBank = [](const PresetBrowserItem& item, const String& bank) {
    return bank == kAllBanks || item.bank == bank;
  };
  auto matchesCategory = [](const PresetBrowserItem& item, const String& category) {
    return category == kAllCategories || item.category == category;
  };
  auto matchesTag = [](const PresetBrowserItem& item, const String& tag) {
    return tag == kAllTags || item.tags.contains(tag);
  };
  auto countMatching = [this](auto predicate) {
    int count = 0;
    for (const auto& item : preset_index_) {
      if (predicate(item))
        ++count;
    }
    return count;
  };
  auto addFacet = [this](int kind, const String& section, const String& value, int count, bool active) {
    preset_facets_.push_back({ kind, section, value, count, active });
  };

  const int all_matching_count = countMatching([&](const PresetBrowserItem& item) {
    return matchesLibrary(item, last_preset_library_) && matchesBank(item, last_preset_bank_) &&
           matchesCategory(item, last_preset_category_) && matchesTag(item, last_preset_tag_);
  });
  const int favorite_matching_count = countMatching([&](const PresetBrowserItem& item) {
    return matchesLibrary(item, last_preset_library_) && matchesBank(item, last_preset_bank_) &&
           matchesCategory(item, last_preset_category_) && matchesTag(item, last_preset_tag_) &&
           preset_favorites_.count(item.file.getFullPathName().toStdString()) != 0;
  });

  preset_facets_.clear();
  addFacet(kCollectionFacet, "Collection", "All presets", all_matching_count, !preset_favorites_only_);
  addFacet(kCollectionFacet, "Collection", "Favourites", favorite_matching_count, preset_favorites_only_);
  for (const auto& library : preset_libraries_) {
    const int count = countMatching([&](const PresetBrowserItem& item) {
      return matchesLibrary(item, library);
    });
    addFacet(kLibraryFacet, "Library", library, count, library == last_preset_library_);
  }
  for (const auto& bank : preset_banks_) {
    const int count = countMatching([&](const PresetBrowserItem& item) {
      return matchesLibrary(item, last_preset_library_) && matchesBank(item, bank);
    });
    addFacet(kBankFacet, "Bank", bank, count, bank == last_preset_bank_);
  }
  for (const auto& category : preset_categories_) {
    const int count = countMatching([&](const PresetBrowserItem& item) {
      return matchesLibrary(item, last_preset_library_) && matchesBank(item, last_preset_bank_) &&
             matchesCategory(item, category);
    });
    addFacet(kCategoryFacet, "Category", category, count, category == last_preset_category_);
  }
  for (const auto& tag : preset_tags_) {
    const int count = countMatching([&](const PresetBrowserItem& item) {
      return matchesLibrary(item, last_preset_library_) && matchesBank(item, last_preset_bank_) &&
             matchesCategory(item, last_preset_category_) && matchesTag(item, tag);
    });
    addFacet(kTagFacet, "Tag", tag, count, tag == last_preset_tag_);
  }

  for (int kind = kLibraryFacet; kind <= kCollectionFacet; ++kind) {
    auto* table = presetFacetTableForKind(kind);
    if (table == nullptr)
      continue;
    table->updateContent();
    SparseSet<int> selected_rows;
    int local_row = 0;
    for (const auto& facet : preset_facets_) {
      if (facet.kind != kind)
        continue;
      if (facet.active) {
        selected_rows.addRange(Range<int>(local_row, local_row + 1));
        break;
      }
      ++local_row;
    }
    table->setSelectedRows(selected_rows, dontSendNotification);
    table->repaint();
  }
}

void SynthEditor::filterPresetList() {
  if (!preset_list_loaded_) {
    filtered_presets_.clear();
    preset_table_.updateContent();
    updatePresetSummary();
    return;
  }

  ScopedValueSetter<bool> guard(updating_preset_list_, true);
  filtered_presets_.clear();
  const String filter = preset_search_.getText().trim().toLowerCase();
  std::vector<File> matches;
  for (const auto& item : preset_index_) {
    if (last_preset_library_ != kAllLibraries && item.library != last_preset_library_)
      continue;
    if (last_preset_bank_ != kAllBanks && item.bank != last_preset_bank_)
      continue;
    if (last_preset_category_ != kAllCategories && item.category != last_preset_category_)
      continue;
    if (last_preset_tag_ != kAllTags && !item.tags.contains(last_preset_tag_))
      continue;
    if (preset_favorites_only_ && !isPresetFavorite(item.file))
      continue;

    if (filter.isEmpty() || item.searchable.contains(filter))
      matches.push_back(item.file);
  }

  auto valueForColumn = [this](const PresetBrowserItem& item, int column) {
    switch (column) {
      case kPresetFavoriteColumn: return isPresetFavorite(item.file) ? String("1") : String("0");
      case kPresetCategoryColumn: return item.category;
      case kPresetTagsColumn: return item.tags.joinIntoString(", ");
      case kPresetBankColumn: return item.bank;
      case kPresetAuthorColumn: return item.author;
      case kPresetLibraryColumn: return item.library;
      default: return item.file.getFileNameWithoutExtension();
    }
  };
  std::stable_sort(matches.begin(), matches.end(), [this, &valueForColumn](const File& a, const File& b) {
    const auto* first = presetItemForFile(a);
    const auto* second = presetItemForFile(b);
    if (first == nullptr || second == nullptr)
      return a.getFileName().compareNatural(b.getFileName()) < 0;
    int comparison = 0;
    if (preset_sort_column_ == kPresetModifiedColumn) {
      if (first->modified_time < second->modified_time)
        comparison = -1;
      else if (first->modified_time > second->modified_time)
        comparison = 1;
    }
    else {
      comparison = valueForColumn(*first, preset_sort_column_)
                       .compareNatural(valueForColumn(*second, preset_sort_column_));
    }
    if (comparison == 0)
      comparison = first->file.getFileNameWithoutExtension().compareNatural(second->file.getFileNameWithoutExtension());
    return preset_sort_ascending_ ? comparison < 0 : comparison > 0;
  });
  for (const auto& match : matches)
    filtered_presets_.add(match);

  const String previous_path = last_preset_path_.isNotEmpty() ? last_preset_path_
                             : synth_.getActiveFile().existsAsFile() ? synth_.getActiveFile().getFullPathName()
                             : String();
  preset_table_.updateContent();
  if (!filtered_presets_.isEmpty()) {
    int selected = 0;
    for (int i = 0; i < filtered_presets_.size(); ++i) {
      if (filtered_presets_[i].getFullPathName() == previous_path) {
        selected = i;
        break;
      }
    }
    preset_table_.selectRow(selected, false, true);
    last_preset_path_ = filtered_presets_[selected].getFullPathName();
  }
  else {
    preset_table_.deselectAllRows();
  }
  preset_table_.repaint();
  updatePresetManagementControls();
  updatePresetSummary();
}

int SynthEditor::presetFacetRowCount(int kind) const {
  int count = 0;
  for (const auto& facet : preset_facets_) {
    if (facet.kind == kind)
      ++count;
  }
  return count;
}

const PresetFacetItem* SynthEditor::presetFacetForRow(int kind, int row) const {
  if (row < 0)
    return nullptr;
  int local_row = 0;
  for (const auto& facet : preset_facets_) {
    if (facet.kind != kind)
      continue;
    if (local_row == row)
      return &facet;
    ++local_row;
  }
  return nullptr;
}

PresetTableListBox* SynthEditor::presetFacetTableForKind(int kind) {
  switch (kind) {
    case kCollectionFacet: return &preset_collection_table_;
    case kLibraryFacet: return &preset_library_table_;
    case kBankFacet: return &preset_bank_table_;
    case kCategoryFacet: return &preset_category_table_;
    case kTagFacet: return &preset_tag_table_;
    default: return nullptr;
  }
}

void SynthEditor::activatePresetFacet(int kind, int row) {
  const auto* selected_facet = presetFacetForRow(kind, row);
  if (selected_facet == nullptr)
    return;
  const auto facet = *selected_facet;
  switch (kind) {
    case kLibraryFacet:
      last_preset_library_ = facet.value;
      last_preset_bank_ = kAllBanks;
      last_preset_category_ = kAllCategories;
      last_preset_tag_ = kAllTags;
      break;
    case kBankFacet:
      last_preset_bank_ = facet.value;
      last_preset_category_ = kAllCategories;
      last_preset_tag_ = kAllTags;
      break;
    case kCategoryFacet:
      last_preset_category_ = facet.value;
      last_preset_tag_ = kAllTags;
      break;
    case kTagFacet:
      last_preset_tag_ = facet.value;
      break;
    case kCollectionFacet:
      preset_favorites_only_ = facet.value == "Favourites";
      break;
    default:
      return;
  }

  populatePresetFilters();
  filterPresetList();
}

File SynthEditor::selectedPresetFile() const {
  const int row = preset_table_.getSelectedRow();
  if (!isPositiveAndBelow(row, filtered_presets_.size()))
    return {};
  return filtered_presets_[row];
}

String SynthEditor::presetTableCellText(int row, int columnId) const {
  if (!isPositiveAndBelow(row, filtered_presets_.size()))
    return {};
  const File file = filtered_presets_[row];
  const auto* item = presetItemForFile(file);
  if (item == nullptr)
    return columnId == kPresetNameColumn ? file.getFileNameWithoutExtension() : String();

  switch (columnId) {
    case kPresetFavoriteColumn: return isPresetFavorite(file) ? "Favourite" : "Not favourite";
    case kPresetNameColumn: return file.getFileNameWithoutExtension();
    case kPresetCategoryColumn: return item->category;
    case kPresetTagsColumn: return item->tags.isEmpty() ? String("No tags") : item->tags.joinIntoString(", ");
    case kPresetBankColumn: return item->bank;
    case kPresetAuthorColumn: return item->author.isEmpty() ? String("Unknown") : item->author;
    case kPresetLibraryColumn: return item->library;
    case kPresetModifiedColumn: return Time(item->modified_time).toString(true, true, false, true);
    default: return {};
  }
}

String SynthEditor::presetFacetCellText(int kind, int row) const {
  const auto* facet = presetFacetForRow(kind, row);
  return facet != nullptr ? facet->value : String();
}

bool SynthEditor::isPresetFavorite(const File& file) const {
  return preset_favorites_.count(file.getFullPathName().toStdString()) != 0;
}

bool SynthEditor::isPresetEditable(const File& file) const {
  return file.existsAsFile() && file.isAChildOf(LoadSave::getUserPresetDirectory()) && file.hasWriteAccess();
}

void SynthEditor::presetSelectionChanged(int row, bool allowAutoload) {
  if (!isPositiveAndBelow(row, filtered_presets_.size())) {
    updatePresetManagementControls();
    return;
  }

  last_preset_path_ = filtered_presets_[row].getFullPathName();
  updatePresetManagementControls();
  if (!allowAutoload || !preset_preview_.getToggleState())
    return;
  // A single press loads at once. Holding an arrow fires a selection per key
  // repeat; loading each would queue loads behind the keys, so while the
  // selection keeps moving a preset loads at most every kAutoloadIntervalMs,
  // and the preset the selection rests on always loads last.
  static constexpr double kAutoloadIntervalMs = 250.0;
  const double now = Time::getMillisecondCounterHiRes();
  const double since_last = now - last_preset_autoload_ms_;
  if (since_last >= kAutoloadIntervalMs && !preset_autoload_scheduled_) {
    last_preset_autoload_ms_ = now;
    loadPresetFile(filtered_presets_[row], true, false);
    last_preset_autoload_ms_ = Time::getMillisecondCounterHiRes();
    return;
  }
  if (preset_autoload_scheduled_)
    return;   // the scheduled load picks up the latest selection
  preset_autoload_scheduled_ = true;
  const int delay = jmax(1, roundToInt(kAutoloadIntervalMs - since_last));
  Component::SafePointer<SynthEditor> safe_this(this);
  Timer::callAfterDelay(delay, [safe_this] {
    if (safe_this == nullptr)
      return;
    safe_this->preset_autoload_scheduled_ = false;
    const File file = safe_this->selectedPresetFile();
    if (!safe_this->preset_preview_.getToggleState() || !file.existsAsFile() || file == safe_this->synth_.getActiveFile())
      return;
    safe_this->last_preset_autoload_ms_ = Time::getMillisecondCounterHiRes();
    safe_this->loadPresetFile(file, true, false);
    safe_this->last_preset_autoload_ms_ = Time::getMillisecondCounterHiRes();
    // The selection may have moved again while this one loaded. Compare with
    // the file just tried, not the active one: a preset that failed to load
    // must not be retried every interval.
    if (safe_this->selectedPresetFile() != file)
      safe_this->presetSelectionChanged(safe_this->preset_table_.getSelectedRow(), true);
  });
}

void SynthEditor::updatePresetManagementControls() {
  const File file = selectedPresetFile();
  const bool selected = file.existsAsFile();

  if (!selected) {
    preset_details_.setText("No preset selected", dontSendNotification);
    preset_details_.setDescription("No preset selected");
    return;
  }

  const bool favorite = isPresetFavorite(file);
  const auto* item = presetItemForFile(file);
  String details = file.getFileNameWithoutExtension();
  if (item != nullptr) {
    details += ". " + String(favorite ? "Favourite" : "Not favourite") +
               ". Library: " + item->library + ". Bank: " + item->bank +
               ". Category: " + item->category +
               ". Tags: " + (item->tags.isEmpty() ? String("none") : item->tags.joinIntoString(", ")) +
               ". Author: " + (item->author.isEmpty() ? String("unknown") : item->author);
    if (item->comments.isNotEmpty())
      details += ". Comments: " + item->comments;
  }
  details += ". File: " + file.getFullPathName();
  preset_details_.setText(details, dontSendNotification);
  preset_details_.setDescription(details);
  preset_table_.setDescription(details + ". Press Return to load, F to change favourite, R to rename, E to edit metadata, S to choose the next sort column, Shift S to reverse the sort, or Delete to move an editable preset to Trash.");
  if (auto* handler = preset_details_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

void SynthEditor::toggleSelectedPresetFavorite() {
  const File file = selectedPresetFile();
  if (!file.existsAsFile()) {
    postPresetActionAnnouncement("Choose a preset first", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const std::string path = file.getFullPathName().toStdString();
  if (preset_favorites_.count(path) != 0) {
    preset_favorites_.erase(path);
    LoadSave::removeFavorite(file);
    postPresetActionAnnouncement("Removed " + file.getFileNameWithoutExtension() + " from favourites",
                                 AccessibilityHandler::AnnouncementPriority::medium);
  }
  else {
    preset_favorites_.insert(path);
    LoadSave::addFavorite(file);
    postPresetActionAnnouncement("Added " + file.getFileNameWithoutExtension() + " to favourites",
                                 AccessibilityHandler::AnnouncementPriority::medium);
  }

  populatePresetFilters();
  filterPresetList();
}

void SynthEditor::renameSelectedPreset() {
  const File file = selectedPresetFile();
  if (!isPresetEditable(file)) {
    postPresetActionAnnouncement("Only presets in the user library can be renamed",
                                 AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  Component::SafePointer<SynthEditor> safe_this(this);
  promptForCustomValue("Preset name", file.getFileNameWithoutExtension(), preset_table_,
                       [safe_this, file](const String& value) {
    if (safe_this == nullptr || !file.existsAsFile())
      return;
    const String name = sanitizePresetPathPart(value, file.getFileNameWithoutExtension());
    const File destination = file.getSiblingFile(name + file.getFileExtension());
    if (destination == file)
      return;
    if (destination.existsAsFile()) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to rename preset",
                                            "A preset with that name already exists in this folder.");
      postPresetActionAnnouncement("Unable to rename preset because that name already exists",
                                   AccessibilityHandler::AnnouncementPriority::high);
      return;
    }

    const auto* item = safe_this->presetItemForFile(file);
    const String author = item != nullptr ? item->author : LoadSave::getAuthorFromFile(file);
    const String category = item != nullptr ? item->style : LoadSave::getStyleFromFile(file);
    const String tags = item != nullptr ? item->tags.joinIntoString(", ") : LoadSave::getTagsFromFile(file);
    const String comments = item != nullptr ? item->comments : presetJsonStringField(file, "comments");
    if (!file.moveFileTo(destination)) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to rename preset",
                                            "Atlas could not move the preset file.");
      postPresetActionAnnouncement("Unable to rename preset", AccessibilityHandler::AnnouncementPriority::high);
      return;
    }
    if (!writePresetMetadata(destination, name, author, category, tags, comments)) {
      destination.moveFileTo(file);
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to rename preset",
                                            "Atlas moved the file back because its metadata could not be updated.");
      postPresetActionAnnouncement("Unable to rename preset", AccessibilityHandler::AnnouncementPriority::high);
      return;
    }

    if (safe_this->isPresetFavorite(file)) {
      LoadSave::removeFavorite(file);
      LoadSave::addFavorite(destination);
    }
    if (safe_this->synth_.getActiveFile() == file)
      safe_this->synth_.setActiveFile(destination);
    safe_this->last_preset_path_ = destination.getFullPathName();
    safe_this->refreshPresetList(false);
    postPresetActionAnnouncement("Renamed preset to " + name, AccessibilityHandler::AnnouncementPriority::high);
  });
}

void SynthEditor::showPresetMetadataDialog() {
  const File file = selectedPresetFile();
  if (!isPresetEditable(file)) {
    postPluginAnnouncement("Only presets in the user library can have their metadata edited",
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  if (!save_patch_dialog_visible_)
    save_patch_return_focus_ = Component::getCurrentlyFocusedComponent();
  const auto* item = presetItemForFile(file);
  preset_metadata_edit_mode_ = true;
  preset_metadata_file_ = file;
  save_patch_name_.setText(file.getFileNameWithoutExtension(), false);
  save_patch_author_.setText(item != nullptr ? item->author : LoadSave::getAuthorFromFile(file), false);
  save_patch_bank_.setText(item != nullptr ? item->bank : presetBankName(file), false);
  // The folder, else the stored style, is the category. The browser's
  // "Uncategorized" placeholder is not one: saved back it would move the file
  // into a folder of that name and write it as the style.
  const String path_category = presetPathCategoryName(file);
  save_patch_category_.setText(path_category.isNotEmpty() ? path_category : LoadSave::getStyleFromFile(file), false);
  save_patch_tags_.setText(item != nullptr ? item->tags.joinIntoString(", ") : LoadSave::getTagsFromFile(file), false);
  save_patch_comments_.setText(item != nullptr ? item->comments : presetJsonStringField(file, "comments"), false);
  save_patch_prompt_.setText("Edit preset metadata. Category controls the folder and sound type; tags are independent searchable labels.",
                             dontSendNotification);
  save_patch_prompt_.setTitle("Edit preset metadata");
  save_patch_prompt_.setDescription("Edit name, author, bank, category, tags, and comments. Tags remain separate from categories.");
  save_patch_ok_.setButtonText("Save metadata");
  save_patch_ok_.setTitle("Save preset metadata");
  save_patch_ok_.setDescription("Save metadata and move the preset if its bank or category changed");
  save_patch_dialog_visible_ = true;
  save_patch_prompt_.setVisible(true);
  save_patch_name_.setVisible(true);
  save_patch_author_.setVisible(true);
  save_patch_bank_.setVisible(true);
  save_patch_category_.setVisible(true);
  save_patch_tags_.setVisible(true);
  save_patch_comments_.setVisible(true);
  save_patch_ok_.setVisible(true);
  save_patch_cancel_.setVisible(true);
  rebuildFocusOrder();
  resized();
  save_patch_name_.grabKeyboardFocus();
  save_patch_name_.selectAll();
  postPluginAnnouncement("Edit metadata for " + file.getFileNameWithoutExtension(),
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::revealSelectedPreset() {
  const File file = selectedPresetFile();
  if (!file.existsAsFile()) {
    postPluginAnnouncement("Choose a preset first", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  file.revealToUser();
  postPluginAnnouncement("Revealed " + file.getFileNameWithoutExtension() + " in Finder",
                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::deleteSelectedPreset() {
  const File file = selectedPresetFile();
  if (!isPresetEditable(file)) {
    postPluginAnnouncement("Only presets in the user library can be moved to Trash",
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  Component::SafePointer<SynthEditor> safe_this(this);
  NativeMessageBox::showYesNoBox(MessageBoxIconType::QuestionIcon, "Move preset to Trash?",
                                 "Move “" + file.getFileNameWithoutExtension() +
                                   "” to Trash? It can be recovered from the Trash.",
                                 &preset_table_, ModalCallbackFunction::create([safe_this, file](int result) {
    if (safe_this == nullptr || result != 1)
      return;
    if (!file.moveToTrash()) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to move preset",
                                            "Atlas could not move the preset to Trash.");
      postPluginAnnouncement("Unable to move preset to Trash", AccessibilityHandler::AnnouncementPriority::high);
      return;
    }
    if (safe_this->isPresetFavorite(file))
      LoadSave::removeFavorite(file);
    if (safe_this->synth_.getActiveFile() == file)
      safe_this->synth_.clearActiveFile();
    safe_this->refreshPresetList(false);
    postPluginAnnouncement("Moved " + file.getFileNameWithoutExtension() +
                             " to Trash. It can be recovered from the Trash.",
                           AccessibilityHandler::AnnouncementPriority::high);
  }));
}

void SynthEditor::schedulePresetFilterUpdate(bool rebuildFilters) {
  preset_filter_rebuild_pending_ = preset_filter_rebuild_pending_ || rebuildFilters;
  if (preset_filter_update_pending_)
    return;

  preset_filter_update_pending_ = true;
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this] {
    if (safe_this == nullptr)
      return;

    const bool rebuild = safe_this->preset_filter_rebuild_pending_;
    safe_this->preset_filter_update_pending_ = false;
    safe_this->preset_filter_rebuild_pending_ = false;
    if (!safe_this->preset_list_loaded_)
      return;

    if (rebuild)
      safe_this->populatePresetFilters();
    safe_this->filterPresetList();
  });
}

void SynthEditor::updatePresetSummary() {
  const String current = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();

  if (!preset_list_loaded_) {
    const String summary = "Current preset: " + current +
                           ". Atlas resources path: " + LoadSave::getDataDirectory().getFullPathName() +
                           (preset_list_loading_ ?
                              ". Preset list loading in the background" +
                              (preset_index_progress_.isNotEmpty() ? ", " + preset_index_progress_.toLowerCase() : String()) +
                              ". Autoload preset when scrolling is " :
                              ". Preset list not loaded yet. Open the Preset menu or focus the preset browser to scan presets. Autoload preset when scrolling is ") +
                           (preset_preview_.getToggleState() ? "on." : "off.");
    preset_summary_.setText(summary, dontSendNotification);
    preset_summary_.setDescription(summary);
    if (auto* handler = preset_summary_.getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
    return;
  }

  const String summary = "Current preset: " + current +
                         ". Atlas resources path: " + LoadSave::getDataDirectory().getFullPathName() +
                         ". " + String(all_presets_.size()) + " presets found, " +
                         String(filtered_presets_.size()) + " shown. Library: " +
                         last_preset_library_ + ". Bank: " + last_preset_bank_ +
                         ". Category: " + last_preset_category_ +
                         ". Tag: " + last_preset_tag_ +
                         ". Collection: " + (preset_favorites_only_ ? String("Favourites") : String("All presets")) +
                         (preset_list_loading_ ? ". Refreshing the preset list" +
                            (preset_index_progress_.isNotEmpty() ? ", " + preset_index_progress_.toLowerCase() : String()) :
                            String()) +
                         ". Autoload preset when scrolling is " +
                         (preset_preview_.getToggleState() ? "on." : "off.");
  preset_summary_.setText(summary, dontSendNotification);
  preset_summary_.setDescription(summary);
  if (auto* handler = preset_summary_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

void SynthEditor::showPresetMenu() {
  ensurePresetListLoaded();

  // Patch and library actions only: choosing, loading, favouriting, renaming
  // and sorting presets happen in the browser itself (see its context menu).
  enum Item { kSave = 1, kSaveAs, kSaveDefault, kInitialize, kImportPreset, kImportBank, kExportBank,
              kExportPreset, kReveal, kRefresh, kScanDownloads };
  const bool has_selection = selectedPresetFile().existsAsFile();
  PopupMenu menu;
  menu.addItem(kSave, "Save patch (" + atlas::keys::commandName() + " S)");
  menu.addItem(kSaveAs, "Save patch as... (Shift " + atlas::keys::commandName() + " S)");
  menu.addItem(kSaveDefault, "Save patch as default");
  menu.addItem(kInitialize, "Initialize patch");
  menu.addSeparator();
  menu.addItem(kImportPreset, "Import preset file...");
  menu.addItem(kImportBank, "Import preset bank...");
  menu.addItem(kExportBank, "Export folder as bank...");
  menu.addItem(kExportPreset, "Export patch as preset file...");
  menu.addItem(kReveal, "Reveal selected preset in Finder", has_selection);
  menu.addSeparator();
  menu.addItem(kRefresh, "Refresh preset list");
  menu.addItem(kScanDownloads, LoadSave::shouldScanDownloads() ? "Turn scan Downloads folder on startup off"
                                                              : "Turn scan Downloads folder on startup on");

  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(preset_menu_), [this](int result) {
    switch (result) {
      case kSave: savePatch(); break;
      case kSaveAs: savePresetAs(); break;
      case kSaveDefault: savePatchAsDefault(); break;
      case kInitialize: initializePatch(); break;
      case kImportPreset: choosePresetFile(); break;
      case kImportBank: chooseBankFile(); break;
      case kExportBank: chooseFolderToExportBank(); break;
      case kExportPreset: exportPreset(); break;
      case kReveal: revealSelectedPreset(); break;
      case kRefresh: refreshPresetList(); break;
      case kScanDownloads: toggleScanDownloads(); break;
      default: break;
    }
  });
}

void SynthEditor::showSelectedPresetMenu(Component& target) {
  ensurePresetListLoaded();

  const File selected = selectedPresetFile();
  const bool has_selection = selected.existsAsFile();
  const bool editable = isPresetEditable(selected);
  PopupMenu menu;
  menu.addSectionHeader(has_selection ? selected.getFileNameWithoutExtension() : String("Preset actions"));
  menu.addItem(1, "Load preset (Return)", has_selection);
  menu.addItem(2, isPresetFavorite(selected) ? "Remove from favourites (F)" : "Add to favourites (F)", has_selection);
  menu.addSeparator();
  menu.addItem(3, "Rename preset... (R)", editable);
  menu.addItem(4, "Edit preset metadata... (E)", editable);
  menu.addItem(5, "Reveal preset in Finder", has_selection);
  menu.addItem(6, "Move preset to Trash... (Delete)", editable);

  Component::SafePointer<SynthEditor> safe_this(this);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                     [safe_this](int result) {
    if (safe_this == nullptr)
      return;
    if (result == 1)
      safe_this->loadSelectedPreset();
    else if (result == 2)
      safe_this->toggleSelectedPresetFavorite();
    else if (result == 3)
      safe_this->renameSelectedPreset();
    else if (result == 4)
      safe_this->showPresetMetadataDialog();
    else if (result == 5)
      safe_this->revealSelectedPreset();
    else if (result == 6)
      safe_this->deleteSelectedPreset();
  });
}

void SynthEditor::toggleScanDownloads() {
  const bool enabled = !LoadSave::shouldScanDownloads();
  LoadSave::saveScanDownloads(enabled);

  if (enabled) {
    const int imported = LoadSave::scanDownloadsForPresets();
    refreshPresetList(false);
    String message = "Scan Downloads folder for presets on startup on";
    if (imported > 0)
      message += ", " + String(imported) + " new presets imported";
    postPluginAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
  }
  else {
    postPluginAnnouncement("Scan Downloads folder for presets on startup off",
                           AccessibilityHandler::AnnouncementPriority::high);
  }
}



void SynthEditor::selectPresetFile(const File& file) {
  if (!file.existsAsFile())
    return;
  last_preset_path_ = file.getFullPathName();
  if (!preset_list_loaded_) {
    updatePresetSummary();
    return;
  }

  const auto* item = presetItemForFile(file);
  const String library = item != nullptr ? item->library : presetLibraryName(file);
  if (preset_libraries_.contains(library)) {
    last_preset_library_ = library;
    populatePresetFilters();
  }
  const String bank = item != nullptr ? item->bank : presetBankName(file);
  if (preset_banks_.contains(bank)) {
    last_preset_bank_ = bank;
    populatePresetFilters();
  }
  const String category = item != nullptr ? item->category : presetCategoryName(file);
  if (preset_categories_.contains(category))
    last_preset_category_ = category;
  last_preset_tag_ = kAllTags;
  preset_favorites_only_ = false;
  populatePresetFilters();
  filterPresetList();
}


void SynthEditor::loadSelectedPreset() {
  ensurePresetListLoaded();

  const int index = preset_table_.getSelectedRow();
  if (!isPositiveAndBelow(index, filtered_presets_.size())) {
    postPluginAnnouncement("Choose a preset first", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  loadPresetFile(filtered_presets_[index], false, false);
}

void SynthEditor::choosePresetFile() {
  preset_chooser_ = std::make_unique<FileChooser>("Open Atlas preset", LoadSave::getDataDirectory(),
                                                  vital::kPresetExtensionsList);
  launchFileChooser(*preset_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                      [this](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadPresetFile(file, false, true);
    preset_chooser_.reset();
  });
}

void SynthEditor::loadPresetFile(File file, bool preview, bool updateBrowserFilters) {
  if (!file.existsAsFile()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to load preset",
                                          "The preset file no longer exists. Refresh the preset list.");
    postPluginAnnouncement("Preset file no longer exists", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();
  // The table sits inside the rows, so persistentFocusedComponent never names it.
  const bool loaded_from_table = preset_table_.hasKeyboardFocus(true);
  std::string error;
  if (!synth_.loadFromFile(file, error)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to load preset", error);
    postPluginAnnouncement("Unable to load preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  last_preset_path_ = file.getFullPathName();
  synth_.updateHostDisplay();
  // When this editor is the plugin's active editor, loading already refreshed it.
  if (synth_.getGuiInterface() != this)
    updateFullGui();
  if (updateBrowserFilters)
    selectPresetFile(file);
  else if (selectedPresetFile() == file)
    updatePresetManagementControls();   // loaded from the list: it is already selected
  else
    filterPresetList();
  updatePresetSummary();
  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);
  if (preview || loaded_from_table) {
    Component::SafePointer<SynthEditor> safe_this(this);
    Component::SafePointer<Component> preset_list_focus(&preset_table_);
    MessageManager::callAsync([safe_this, preset_list_focus] {
      if (safe_this != nullptr && preset_list_focus != nullptr && preset_list_focus->isShowing()) {
        safe_this->ensureComponentVisible(preset_list_focus.getComponent());
        preset_list_focus->grabKeyboardFocus();
      }
    });
  }
  postPluginAnnouncement((preview ? "Autoloaded preset " : "Loaded preset ") +
                                           file.getFileNameWithoutExtension(),
                                         preview ? AccessibilityHandler::AnnouncementPriority::medium
                                                 : AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::chooseBankFile() {
  preset_chooser_ = std::make_unique<FileChooser>("Import Atlas bank", LoadSave::getDataDirectory(),
                                                  vital::kBankExtensionsList);
  launchFileChooser(*preset_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                      [this](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      importBankFile(file);
    preset_chooser_.reset();
  });
}

void SynthEditor::importBankFile(const File& file) {
  FileInputStream input_stream(file);
  if (!input_stream.openedOk()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import bank",
                                          "The bank file could not be opened.");
    postPluginAnnouncement("Unable to open bank file", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  File data_directory = LoadSave::getDataDirectory();
  data_directory.createDirectory();
  if (!LoadSave::hasDataDirectory())
    LoadSave::saveDataDirectory(data_directory);

  ZipFile import_zip(input_stream);
  const Result unzip_result = import_zip.uncompressTo(data_directory);
  if (unzip_result.wasOk()) {
    LoadSave::markPackInstalled(file.getFileNameWithoutExtension().toStdString());
    refreshPresetList();
    postPluginAnnouncement("Imported bank " + file.getFileNameWithoutExtension() +
                                             ". Preset list refreshed.",
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  LoadSave::writeErrorLog("Unzipping bank failed: " + unzip_result.getErrorMessage());
  NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import bank",
                                        unzip_result.getErrorMessage());
  postPluginAnnouncement("Unable to import bank", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::chooseFolderToExportBank() {
  preset_chooser_ = std::make_unique<FileChooser>("Choose folder to export as an Atlas bank",
                                                  LoadSave::getUserPresetDirectory());
  launchFileChooser(*preset_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                                      [this](const FileChooser& chooser) {
    const File source_folder = chooser.getResult();
    preset_chooser_.reset();
    if (!source_folder.isDirectory())
      return;

    String bank_name = source_folder.getFileName().removeCharacters("\\/:*?\"<>|");
    if (bank_name.isEmpty())
      bank_name = "Atlas Bank";
    const File default_file = source_folder.getSiblingFile(bank_name).withFileExtension(vital::kBankExtension);
    bank_chooser_ = std::make_unique<FileChooser>("Export Atlas bank", default_file,
                                                  String("*.") + vital::kBankExtension);
    launchFileChooser(*bank_chooser_, FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                          FileBrowserComponent::warnAboutOverwriting,
                                      [this, source_folder](const FileChooser& save_chooser) {
      const File destination = save_chooser.getResult();
      if (destination != File())
        exportFolderAsBank(source_folder, destination);
      bank_chooser_.reset();
    });
  });
}

void SynthEditor::exportFolderAsBank(const File& sourceFolder, const File& destinationFile) {
  if (!sourceFolder.isDirectory()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export bank",
                                          "The selected source is not a folder.");
    return;
  }

  Array<File> files;
  sourceFolder.findChildFiles(files, File::findFiles | File::ignoreHiddenFiles, true,
                              vital::kPresetExtensionsList + ";*." + vital::kWavetableExtension +
                              ";*." + vital::kLegacyWavetableExtension + ";" + vital::kSampleExtensionsList +
                              ";" + vital::kLfoExtensionsList + ";*." + vital::kFxExtension);
  if (files.isEmpty()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export bank",
                                          "No Atlas presets, tables, samples, LFOs, or FX presets were found.");
    postPluginAnnouncement("No bank content found", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const File destination = destinationFile.withFileExtension(vital::kBankExtension);
  String bank_name = destination.getFileNameWithoutExtension().removeCharacters("\\/:*?\"<>|");
  if (bank_name.isEmpty())
    bank_name = "Atlas Bank";

  ZipFile::Builder bank_zip;
  int added = 0;
  int64 total_bytes = 0;
  for (const File& file : files) {
    const String extension = file.getFileExtension().toLowerCase();
    String folder_name;
    if (extension == "." + String(vital::kPresetExtension) ||
        extension == "." + String(vital::kLegacyPresetExtension)) {
      folder_name = LoadSave::kPresetFolderName;
    }
    else if (extension == "." + String(vital::kWavetableExtension) ||
             extension == "." + String(vital::kLegacyWavetableExtension)) {
      folder_name = LoadSave::kWavetableFolderName;
    }
    else if (extension == "." + String(vital::kLfoExtension) ||
             extension == "." + String(vital::kLegacyLfoExtension)) {
      folder_name = LoadSave::kLfoFolderName;
    }
    else if (extension == "." + String(vital::kFxExtension)) {
      folder_name = LoadSave::kFxFolderName;
    }
    else if (extension == ".wav" || extension == ".flac" || extension == ".aif" || extension == ".aiff") {
      folder_name = pathContainsFolderName(file, sourceFolder, LoadSave::kWavetableFolderName) ?
                    LoadSave::kWavetableFolderName : LoadSave::kSampleFolderName;
    }
    else {
      continue;
    }

    const String relative_path = bankRelativePathForFile(file, sourceFolder, folder_name);
    // Presets carrying multi-sample instruments are zips of audio already.
    const int compression = vital::multisample::container::isContainer(file) ? 0 : 9;
    bank_zip.addFile(file, compression, bank_name + "/" + folder_name + "/" + relative_path);
    total_bytes += file.getSize();
    ++added;
  }

  if (total_bytes > 0xF0000000ll) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export bank",
                                          "The bank would be larger than 4 GB, the most a bank can hold.");
    postPluginAnnouncement("Bank too large to export", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  FileOutputStream output(destination);
  if (!output.openedOk() || !bank_zip.writeToStream(output, nullptr)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export bank",
                                          "The bank file could not be written.");
    postPluginAnnouncement("Unable to export bank", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  postPluginAnnouncement("Exported bank " + destination.getFileNameWithoutExtension() +
                                         " with " + String(added) + " files",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::loadTuningFile() {
  tuning_chooser_ = std::make_unique<FileChooser>("Load Tuning", LoadSave::getDataDirectory(),
                                                  Tuning::allFileExtensions());
  launchFileChooser(*tuning_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                      [this](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file.existsAsFile()) {
      synth_.loadTuningFile(file);
      postPluginAnnouncement("Loaded tuning " + String(synth_.getTuning()->getName()),
                             AccessibilityHandler::AnnouncementPriority::high);
    }
    tuning_chooser_.reset();
  });
}

void SynthEditor::clearTuning() {
  synth_.getTuning()->setDefaultTuning();
  synth_.markStateChanged();
  postPluginAnnouncement("Tuning cleared", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::exportPreset() {
  const File default_file = LoadSave::getUserPresetDirectory()
                                .getChildFile(synth_.getPresetName().isEmpty() ? String("Untitled")
                                                                               : synth_.getPresetName())
                                .withFileExtension(vital::kPresetExtension);
  export_chooser_ = std::make_unique<FileChooser>("Export Preset", default_file,
                                                  String("*.") + vital::kPresetExtension);
  launchFileChooser(*export_chooser_, FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                          FileBrowserComponent::warnAboutOverwriting,
                                      [this](const FileChooser& chooser) {
    const File destination = chooser.getResult();
    if (destination != File()) {
      const File preset = destination.withFileExtension(vital::kPresetExtension);
      if (synth_.saveToFile(preset))
        postPluginAnnouncement("Exported preset " + preset.getFileNameWithoutExtension(),
                               AccessibilityHandler::AnnouncementPriority::high);
      else {
        NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export preset",
                                              "The preset could not be written to disk.");
        postPluginAnnouncement("Unable to export preset", AccessibilityHandler::AnnouncementPriority::high);
      }
    }
    export_chooser_.reset();
  });
}

void SynthEditor::savePresetToUserDirectory() {
  String preset_name = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  preset_name = preset_name.removeCharacters("\\/:*?\"<>|");
  if (preset_name.isEmpty())
    preset_name = "Untitled";

  savePresetFile(LoadSave::getUserPresetDirectory().getChildFile(preset_name));
}

void SynthEditor::savePresetAs() {
  showSavePresetDialog();
}

void SynthEditor::savePatch() {
  // Save over the loaded preset when it is one of the user's own; otherwise
  // (factory, bank or never saved) ask for a name and place.
  const File active = synth_.getActiveFile();
  if (active.existsAsFile() && isPresetEditable(active)) {
    savePresetFile(active);
    return;
  }
  savePresetAs();
}

void SynthEditor::showSavePresetDialog() {
  if (!save_patch_dialog_visible_)
    save_patch_return_focus_ = Component::getCurrentlyFocusedComponent();
  preset_metadata_edit_mode_ = false;
  preset_metadata_file_ = File();
  String preset_name = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  save_patch_name_.setText(preset_name, false);
  save_patch_author_.setText(synth_.getAuthor().isNotEmpty() ? synth_.getAuthor() : String(LoadSave::getAuthor()), false);
  save_patch_bank_.setText(last_preset_bank_ != kAllBanks ? last_preset_bank_ : "User", false);
  save_patch_category_.setText(synth_.getStyle().isNotEmpty() ? synth_.getStyle() :
                               (last_preset_category_ != kAllCategories ? last_preset_category_ : ""), false);
  save_patch_tags_.setText(synth_.getTags(), false);
  save_patch_comments_.setText(synth_.getComments(), false);
  save_patch_prompt_.setText("Save patch", dontSendNotification);
  save_patch_prompt_.setTitle("Save patch");
  save_patch_prompt_.setDescription("Save the current Atlas patch");
  save_patch_ok_.setButtonText("Save");
  save_patch_ok_.setTitle("Save patch");
  save_patch_ok_.setDescription("Save patch to the user library");
  save_patch_dialog_visible_ = true;
  save_patch_prompt_.setVisible(true);
  save_patch_name_.setVisible(true);
  save_patch_author_.setVisible(true);
  save_patch_bank_.setVisible(true);
  save_patch_category_.setVisible(true);
  save_patch_tags_.setVisible(true);
  save_patch_comments_.setVisible(true);
  save_patch_ok_.setVisible(true);
  save_patch_cancel_.setVisible(true);
  rebuildFocusOrder();
  resized();
  MessageManager::callAsync([safe_this = Component::SafePointer<SynthEditor>(this)] {
    if (safe_this != nullptr && safe_this->save_patch_name_.isShowing())
      safe_this->save_patch_name_.grabKeyboardFocus();
  });
}

void SynthEditor::hideSavePresetDialog() {
  // Back to the control the form was opened from, before the form's own
  // controls hide and hand the focus to the editor itself.
  if (auto* target = save_patch_return_focus_.getComponent(); target != nullptr && target->isShowing())
    target->grabKeyboardFocus();
  save_patch_return_focus_ = nullptr;
  save_patch_dialog_visible_ = false;
  save_patch_prompt_.setVisible(false);
  save_patch_name_.setVisible(false);
  save_patch_author_.setVisible(false);
  save_patch_bank_.setVisible(false);
  save_patch_category_.setVisible(false);
  save_patch_tags_.setVisible(false);
  save_patch_comments_.setVisible(false);
  save_patch_ok_.setVisible(false);
  save_patch_cancel_.setVisible(false);
  rebuildFocusOrder();
  resized();
}

void SynthEditor::commitSavePresetDialog() {
  String preset_name = sanitizePresetPathPart(save_patch_name_.getText(), "Untitled");
  String bank = sanitizePresetPathPart(save_patch_bank_.getText(), "User");
  String category = save_patch_category_.getText().trim().removeCharacters("\"");
  while (category.contains(".."))
    category = category.replace("..", ".");
  category = category.trimCharactersAtStart("/\\ ").trimCharactersAtEnd("/\\ ");
  String author = save_patch_author_.getText().trim().removeCharacters("\"");
  String tags = save_patch_tags_.getText().trim().removeCharacters("\"");
  String comments = save_patch_comments_.getText().trim().removeCharacters("\"");

  if (category == ".")
    category = "";

  if (preset_metadata_edit_mode_) {
    const File source = preset_metadata_file_;
    if (!isPresetEditable(source)) {
      hideSavePresetDialog();
      postPluginAnnouncement("The selected preset can no longer be edited",
                             AccessibilityHandler::AnnouncementPriority::high);
      return;
    }

    File destination_directory = presetCategoryDirectory(userPresetBankDirectory(bank), category);
    const String extension = source.getFileExtension().isNotEmpty() ? source.getFileExtension()
                                                                    : "." + String(vital::kPresetExtension);
    const File destination = destination_directory.getChildFile(preset_name + extension);
    if (destination != source && destination.existsAsFile()) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save metadata",
                                            "A preset with that name already exists in the chosen bank and category.");
      postPluginAnnouncement("Unable to save metadata because the destination already exists",
                             AccessibilityHandler::AnnouncementPriority::high);
      return;
    }
    bool saved = destination_directory.createDirectory().wasOk();
    if (saved && destination == source)
      saved = writePresetMetadata(source, preset_name, author, category, tags, comments);
    else if (saved) {
      saved = source.moveFileTo(destination);
      if (saved && !writePresetMetadata(destination, preset_name, author, category, tags, comments)) {
        destination.moveFileTo(source);
        saved = false;
      }
    }
    if (!saved) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save metadata",
                                            "Atlas could not update the preset. Any attempted move was rolled back.");
      postPluginAnnouncement("Unable to save preset metadata",
                             AccessibilityHandler::AnnouncementPriority::high);
      return;
    }

    if (isPresetFavorite(source) && destination != source) {
      LoadSave::removeFavorite(source);
      LoadSave::addFavorite(destination);
    }
    if (synth_.getActiveFile() == source) {
      synth_.setPresetName(preset_name);
      synth_.setAuthor(author);
      synth_.setStyle(category);
      synth_.setTags(tags);
      synth_.setComments(comments);
      synth_.setActiveFile(destination);
    }
    last_preset_path_ = destination.getFullPathName();
    hideSavePresetDialog();
    refreshPresetList(false);
    postPluginAnnouncement("Saved metadata for " + preset_name,
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  synth_.setPresetName(preset_name);
  synth_.setAuthor(author);
  synth_.setStyle(category);
  synth_.setTags(tags);
  synth_.setComments(comments);

  File destination = presetCategoryDirectory(userPresetBankDirectory(bank), category);
  destination = destination.getChildFile(preset_name);

  hideSavePresetDialog();
  // Save as must not replace another preset without asking: the dialog does
  // not show what else is in the folder.
  const File existing = destination.withFileExtension(vital::kPresetExtension);
  if (existing.existsAsFile() && existing != synth_.getActiveFile()) {
    Component::SafePointer<SynthEditor> safe_this(this);
    NativeMessageBox::showAsync(MessageBoxOptions()
                                    .withIconType(MessageBoxIconType::QuestionIcon)
                                    .withTitle("Replace preset?")
                                    .withMessage("A preset called " + preset_name + " already exists in " +
                                                 existing.getParentDirectory().getFileName() + ". Replace it?")
                                    .withButton("Replace")
                                    .withButton("Cancel"),
                                [safe_this, destination](int button) {
      if (safe_this == nullptr)
        return;
      if (button == 0)
        safe_this->savePresetFile(destination);
      else
        postPluginAnnouncement("Save cancelled", AccessibilityHandler::AnnouncementPriority::high);
    });
    return;
  }
  savePresetFile(destination);
}

void SynthEditor::savePresetFile(const File& file) {
  if (!synth_.saveToFile(file)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save preset",
                                          "The preset could not be written to disk.");
    postPluginAnnouncement("Unable to save preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  last_preset_path_ = synth_.getActiveFile().getFullPathName();
  refreshPresetList();
  selectPresetFile(synth_.getActiveFile());
  updatePresetSummary();
  postPluginAnnouncement("Saved preset " + file.withFileExtension(vital::kPresetExtension).getFileNameWithoutExtension(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

bool SynthEditor::isModulationDestinationId(const String& id) const {
  // Only parameters registered with the engine as modulation destinations can be connected.
  // Plain base controls (e.g. the envelope power sliders) have no destination, so offering to
  // modulate them would connect to a null destination and crash on the audio thread.
  return !isEffectChoiceParameter(id, "utility_input_gain") &&
         synth_.getEngine()->getMonoModulationDestination(id.toStdString()) != nullptr;
}

LineGenerator* SynthEditor::lfoGeneratorForIndex(int lfoIndex) const {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos))
    return nullptr;
  return synth_.getLfoSource(lfoIndex);
}

LineGenerator* SynthEditor::activeLfoGenerator() const {
  return lfoGeneratorForIndex(active_lfo_index_);
}

int SynthEditor::selectedLfoPointIndex() const {
  if (!isPositiveAndBelow(lfo_editor_item_index_, static_cast<int>(lfo_editor_item_phases_.size())))
    return -1;
  return lfoPointIndexAtPhase(lfo_editor_item_phases_[static_cast<size_t>(lfo_editor_item_index_)]);
}

int SynthEditor::lfoPointIndexAtPhase(float phase) const {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return -1;

  for (int i = 0; i < generator->getNumPoints(); ++i) {
    if (std::abs(generator->getPoint(i).first - phase) <= 0.0005f)
      return i;
  }
  return -1;
}

int SynthEditor::currentLfoPointIndex() const {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return -1;

  const int selected = selectedLfoPointIndex();
  if (isPositiveAndBelow(selected, generator->getNumPoints()) &&
      std::abs(generator->getPoint(selected).first - lfo_cursor_phase_) <= 0.0005f)
    return selected;

  return lfoPointIndexAtPhase(lfo_cursor_phase_);
}

bool SynthEditor::isLfoPointSelected(float phase) const {
  return std::any_of(selected_lfo_point_phases_.begin(), selected_lfo_point_phases_.end(),
                     [phase](float selected) { return std::abs(selected - phase) <= 0.0005f; });
}

std::vector<int> SynthEditor::selectedLfoPointIndices() const {
  std::vector<int> indices;
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return indices;

  for (int i = 0; i < generator->getNumPoints(); ++i) {
    if (isLfoPointSelected(generator->getPoint(i).first))
      indices.push_back(i);
  }
  return indices;
}

void SynthEditor::pruneSelectedLfoPointPhases() {
  selected_lfo_point_phases_.erase(std::remove_if(selected_lfo_point_phases_.begin(),
                                                  selected_lfo_point_phases_.end(),
                                                  [this](float phase) {
                                                    return lfoPointIndexAtPhase(phase) < 0;
                                                  }),
                                   selected_lfo_point_phases_.end());
}

float SynthEditor::lfoCycleQuarterNotes() const {
  const auto& divisions = msegTimeDivisions();
  const int cycle = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_cycle_index_);
  return jmax(0.03125f, divisions[cycle].quarter_notes);
}

float SynthEditor::lfoGridQuarterNotes() const {
  const auto& divisions = msegTimeDivisions();
  const int grid = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_grid_index_);
  return jmax(0.03125f, divisions[grid].quarter_notes);
}

float SynthEditor::lfoGridAmount() const {
  return jlimit(0.000001f, 1.0f, lfoGridQuarterNotes() / lfoCycleQuarterNotes());
}

float SynthEditor::lfoEditorBlockStart(float phase) const {
  const float length = jlimit(0.000001f, 1.0f, lfoGridAmount());
  const float tolerance = jmin(0.0005f, length * 0.01f);
  float clamped = jlimit(0.0f, 1.0f, phase);
  if (clamped >= 1.0f - tolerance)
    clamped = jmax(0.0f, 1.0f - tolerance * 2.0f);
  const float start = std::floor((clamped + tolerance) / length) * length;
  return jlimit(0.0f, jmax(0.0f, 1.0f - tolerance), start);
}

float SynthEditor::lfoEditorBlockEnd(float start) const {
  return jlimit(0.0f, 1.0f, start + jlimit(0.000001f, 1.0f, lfoGridAmount()));
}

float SynthEditor::lfoEditorBlockSamplePhase(float start) const {
  const float end = lfoEditorBlockEnd(start);
  return jlimit(0.0f, 1.0f, start + 0.5f * (end - start));
}

std::vector<int> SynthEditor::lfoEditorBlockPointIndices(LineGenerator* generator, float start) const {
  std::vector<int> indices;
  if (generator == nullptr)
    return indices;

  const float end = lfoEditorBlockEnd(start);
  const float tolerance = jmin(0.0005f, jmax(0.000001f, end - start) * 0.01f);
  const bool final_block = end >= 1.0f - tolerance;
  for (int i = 0; i < generator->getNumPoints(); ++i) {
    const float phase = generator->getPoint(i).first;
    if (phase < start - tolerance)
      continue;
    if (phase < end - tolerance || (final_block && phase <= end + tolerance))
      indices.push_back(i);
  }
  return indices;
}

bool SynthEditor::lfoPerformerBlockHasShape(LineGenerator* generator, float start) const {
  for (int index : lfoEditorBlockPointIndices(generator, start)) {
    if (pointOutputValue(generator, index) > 0.0005f ||
        !LineGenerator::isHoldPower(generator->getPower(index)))
      return true;
  }
  return false;
}

void SynthEditor::writeLfoPoints(LineGenerator* generator, std::vector<CopiedLfoPoint> points, bool smooth) {
  if (generator == nullptr)
    return;

  constexpr float tolerance = 0.000001f;
  const float hold = LineGenerator::kHoldPowerThreshold;
  for (auto& point : points) {
    point.point.first = jlimit(0.0f, 1.0f, point.point.first);
    point.point.second = jlimit(0.0f, 1.0f, point.point.second);
  }
  std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
    return a.point.first < b.point.first;
  });

  std::vector<CopiedLfoPoint> cleaned;
  cleaned.reserve(points.size() + 2);
  for (size_t i = 0; i < points.size();) {
    const float phase = points[i].point.first;
    size_t end = i + 1;
    while (end < points.size() && std::abs(points[end].point.first - phase) <= tolerance)
      ++end;

    auto first = points[i];
    first.point.first = phase;
    cleaned.push_back(first);
    if (end - i > 1) {
      auto last = points[end - 1];
      last.point.first = phase;
      if (std::abs(first.point.second - last.point.second) > tolerance ||
          std::abs(first.power - last.power) > tolerance)
        cleaned.push_back(last);
    }
    i = end;
  }

  if (cleaned.empty()) {
    cleaned.push_back({ { 0.0f, 1.0f }, hold });
    cleaned.push_back({ { 1.0f, 1.0f }, hold });
  }
  else {
    if (cleaned.front().point.first > tolerance)
      cleaned.insert(cleaned.begin(), { { 0.0f, cleaned.front().point.second }, cleaned.front().power });
    else
      cleaned.front().point.first = 0.0f;

    if (cleaned.back().point.first < 1.0f - tolerance)
      cleaned.push_back({ { 1.0f, cleaned.back().point.second }, cleaned.back().power });
    else
      cleaned.back().point.first = 1.0f;
  }

  if (cleaned.size() > LineGenerator::kMaxPoints) {
    const auto final_point = cleaned.back();
    cleaned.resize(LineGenerator::kMaxPoints);
    cleaned.back() = final_point;
    cleaned.back().point.first = 1.0f;
  }

  generator->setNumPoints(static_cast<int>(cleaned.size()));
  for (int i = 0; i < static_cast<int>(cleaned.size()); ++i) {
    generator->setPoint(i, cleaned[static_cast<size_t>(i)].point);
    generator->setPower(i, cleaned[static_cast<size_t>(i)].power);
  }
  generator->setSmooth(smooth);
  generator->render();
  synth_.markStateChanged();
}

void SynthEditor::replaceLfoEditorBlock(const std::vector<CopiedLfoPoint>& replacement,
                                        bool remove_end_point) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  const float start = lfoEditorBlockStart(lfo_cursor_phase_);
  const float end = lfoEditorBlockEnd(start);
  const float tolerance = jmin(0.0005f, jmax(0.000001f, end - start) * 0.01f);
  std::vector<CopiedLfoPoint> points;
  points.reserve(generator->getNumPoints() + replacement.size());
  for (int i = 0; i < generator->getNumPoints(); ++i) {
    const auto point = generator->getPoint(i);
    const bool at_or_after_start = point.first >= start - tolerance;
    const bool before_end = point.first < end - tolerance;
    const bool at_end = std::abs(point.first - end) <= tolerance;
    if (at_or_after_start && (before_end || (remove_end_point && at_end)))
      continue;
    points.push_back({ point, generator->getPower(i) });
  }
  points.insert(points.end(), replacement.begin(), replacement.end());
  writeLfoPoints(generator, std::move(points), false);
}

float SynthEditor::lfoPhaseToQuarterNotes(float phase) const {
  return jmax(0.0f, phase) * lfoCycleQuarterNotes();
}

float SynthEditor::lfoQuarterNotesToPhase(float quarterNotes) const {
  return jlimit(0.0f, 1.0f, quarterNotes / lfoCycleQuarterNotes());
}

float SynthEditor::snapLfoPhaseToGrid(float phase) const {
  const float grid = lfoGridQuarterNotes();
  const float quarter_notes = lfoPhaseToQuarterNotes(jlimit(0.0f, 1.0f, phase));
  const float snapped = std::round(quarter_notes / grid) * grid;
  return lfoQuarterNotesToPhase(snapped);
}

String SynthEditor::lfoPointDescription(int pointIndex) const {
  return lfoPointDescriptionFor(active_lfo_index_, pointIndex);
}

String SynthEditor::lfoPointDescriptionFor(int lfoIndex, int pointIndex) const {
  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr || !isPositiveAndBelow(pointIndex, generator->getNumPoints()))
    return {};

  const auto point = generator->getPoint(pointIndex);
  const float power = pointIndex < generator->getNumPoints() - 1
      ? generator->getPower(pointIndex)
      : generator->getPower(jmax(0, pointIndex - 1));
  return lfoTimeDescriptionFor(lfoIndex, point.first) +
         ", " + percentString(1.0f - point.second) +
         ", " + curveNameForIndex(curveIndexForPower(power, generator->smooth()));
}

String SynthEditor::lfoTimeDescription(float phase) const {
  return lfoTimeDescriptionFor(active_lfo_index_, phase);
}

String SynthEditor::lfoTimeDescriptionFor(int lfo_index, float phase) const {
  // From the parameters, not the key handler's cached index: VoiceOver reads
  // this text without a key press.
  const int cycle_index = lfoCycleIndexFromParameters(lfo_index);
  const auto& divisions = msegTimeDivisions();
  const float cycle_quarter_notes = divisions[jlimit(0, static_cast<int>(divisions.size()) - 1,
                                                      cycle_index)].quarter_notes;
  const double quarter_notes = static_cast<double>(jlimit(0.0f, 1.0f, phase) * cycle_quarter_notes);
  const int total_hundredths = jmax(0, roundToInt(quarter_notes * 100.0));
  const int bar = total_hundredths / 400 + 1;
  const int within_bar = total_hundredths % 400;
  const int beat = within_bar / 100 + 1;
  const int percent = within_bar % 100;
  return "Bar " + String(bar) + " beat " + String(beat) + " " + String(percent) + "%";
}

String SynthEditor::lfoMsegStatusText() const {
  return lfoMsegStatusTextFor(active_lfo_index_);
}

String SynthEditor::lfoMsegStatusTextFor(int lfoIndex) const {
  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr)
    return "LFO editor unavailable";

  if (lfoIndex == active_lfo_index_ && generator->getShapeMode() == LineGenerator::kModeStepSequencer) {
    const float start = lfoEditorBlockStart(lfo_cursor_phase_);
    const float value = generator->valueAtPhase(lfoEditorBlockSamplePhase(start));
    return "Step at " + lfoTimeDescriptionFor(lfoIndex, start) + ", value " + percentString(value) +
           ", grid " + String(msegTimeDivisions()[lfo_grid_index_].name) + ", curves fixed to Hold";
  }

  if (lfoIndex == active_lfo_index_ && generator->getShapeMode() == LineGenerator::kModePerformer) {
    const float start = lfoEditorBlockStart(lfo_cursor_phase_);
    if (!lfoPerformerBlockHasShape(generator, start))
      return "No performer shape at " + lfoTimeDescriptionFor(lfoIndex, start) + ", grid " +
             String(msegTimeDivisions()[lfo_grid_index_].name);

    float minimum = 1.0f;
    float maximum = 0.0f;
    for (int index : lfoEditorBlockPointIndices(generator, start)) {
      const float value = pointOutputValue(generator, index);
      minimum = jmin(minimum, value);
      maximum = jmax(maximum, value);
    }
    return "Performer shape at " + lfoTimeDescriptionFor(lfoIndex, start) + ", range " +
           percentString(minimum) + " to " + percentString(maximum) + ", grid " +
           String(msegTimeDivisions()[lfo_grid_index_].name);
  }

  const int selected = lfoIndex == active_lfo_index_ ? currentLfoPointIndex() : 0;
  if (isPositiveAndBelow(selected, generator->getNumPoints()))
    return lfoPointDescriptionFor(lfoIndex, selected) + ", " + String(msegTimeDivisions()[lfo_grid_index_].name);
  return lfoTimeDescriptionFor(lfoIndex, lfo_cursor_phase_) + ", value " +
         percentString(generator->valueAtPhase(lfo_cursor_phase_)) + ", no point, grid " +
         String(msegTimeDivisions()[lfo_grid_index_].name);
}

void SynthEditor::applyLfoMode(int mode) {
  if (active_lfo_index_ < 0)
    return;

  mode = jlimit(0, LineGenerator::kNumShapeModes - 1, mode);
  if (auto* generator = activeLfoGenerator()) {
    if (mode == LineGenerator::kModeStepSequencer) {
      constexpr float tolerance = 0.0005f;
      std::vector<float> starts;
      starts.push_back(0.0f);
      for (int i = 0; i < generator->getNumPoints(); ++i) {
        const float phase = generator->getPoint(i).first;
        if (phase >= 1.0f - tolerance)
          continue;
        const float start = lfoEditorBlockStart(phase);
        if (std::none_of(starts.begin(), starts.end(), [start](float existing) {
              return std::abs(existing - start) <= 0.0005f;
            }))
          starts.push_back(start);
      }
      std::sort(starts.begin(), starts.end());

      std::vector<CopiedLfoPoint> steps;
      steps.reserve(starts.size() + 1);
      for (float start : starts) {
        const float sample_phase = lfoEditorBlockSamplePhase(start);
        steps.push_back({ { start, 1.0f - generator->valueAtPhase(sample_phase) },
                          LineGenerator::kHoldPowerThreshold });
      }
      const float final_start = lfoEditorBlockStart(1.0f);
      steps.push_back({ { 1.0f, 1.0f - generator->valueAtPhase(lfoEditorBlockSamplePhase(final_start)) },
                        LineGenerator::kHoldPowerThreshold });
      writeLfoPoints(generator, std::move(steps), false);
    }
    generator->setShapeMode(mode);
  }
  syncLfoEditorState();
  postLfoAnnouncement("Mode " + lfoModeNames()[mode] + ". Editing LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::applyLfoCycleLength() {
  if (active_lfo_index_ < 0)
    return;

  const auto& divisions = msegTimeDivisions();
  const int index = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_cycle_index_);
  const String division_name(divisions[index].name);
  float base_quarter_notes = divisions[index].quarter_notes;
  int sync_mode = vital::SynthLfo::kTempo;
  if (division_name.containsIgnoreCase("dotted")) {
    base_quarter_notes /= 1.5f;
    sync_mode = vital::SynthLfo::kDottedTempo;
  }
  else if (division_name.containsIgnoreCase("triplet")) {
    base_quarter_notes *= 1.5f;
    sync_mode = vital::SynthLfo::kTripletTempo;
  }
  const int tempo_index = closestVitalTempoIndex(base_quarter_notes);
  const String prefix = "lfo_" + String(active_lfo_index_ + 1) + "_";
  setParameterEngineValue(prefix + "sync", static_cast<float>(sync_mode));
  setParameterEngineValue(prefix + "tempo", static_cast<float>(tempo_index));
  lfo_cursor_phase_ = snapLfoPhaseToGrid(lfo_cursor_phase_);
  postLfoAnnouncement("Cycle length " + String(divisions[index].name));
}

int SynthEditor::lfoCycleIndexFromParameters(int lfo_index) const {
  if (!isPositiveAndBelow(lfo_index, vital::kNumLfos))
    return lfo_cycle_index_;

  const String prefix = "lfo_" + String(lfo_index + 1) + "_";
  auto* tempo = parameterBridge(prefix + "tempo");
  auto* sync = parameterBridge(prefix + "sync");
  if (tempo == nullptr || sync == nullptr)
    return lfo_cycle_index_;

  const int sync_mode = roundToInt(sync->convertToEngineValue(sync->getValue()));
  if (sync_mode != vital::SynthLfo::kTempo && sync_mode != vital::SynthLfo::kDottedTempo &&
      sync_mode != vital::SynthLfo::kTripletTempo)
    return lfo_cycle_index_;

  const int tempo_index = jlimit(1, vital::constants::kNumSyncedFrequencyRatios - 1,
                                 roundToInt(tempo->convertToEngineValue(tempo->getValue())));
  const float ratio = vital::constants::kSyncedFrequencyRatios[tempo_index];
  if (ratio <= 0.0f)
    return lfo_cycle_index_;

  float quarter_notes = 1.0f / ratio;
  if (sync_mode == vital::SynthLfo::kDottedTempo)
    quarter_notes *= 1.5f;
  else if (sync_mode == vital::SynthLfo::kTripletTempo)
    quarter_notes *= 2.0f / 3.0f;

  const auto& divisions = msegTimeDivisions();
  int closest = 0;
  float closest_distance = std::numeric_limits<float>::max();
  for (int i = 0; i < static_cast<int>(divisions.size()); ++i) {
    const float distance = std::abs(std::log2(jmax(0.000001f, divisions[i].quarter_notes / quarter_notes)));
    if (distance < closest_distance) {
      closest_distance = distance;
      closest = i;
    }
  }
  return closest;
}

bool SynthEditor::syncLfoCycleLengthFromParameters() {
  const int actual = lfoCycleIndexFromParameters(active_lfo_index_);
  if (actual == lfo_cycle_index_)
    return false;
  lfo_cycle_index_ = actual;
  return true;
}

void SynthEditor::syncLfoEditorState() {
  active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, active_lfo_index_);
  syncLfoCycleLengthFromParameters();
  if (activeLfoGenerator() != nullptr) {
    pruneSelectedLfoPointPhases();
    rebuildLfoEditorItems();
  }
}

void SynthEditor::rebuildLfoEditorItems() {
  auto* generator = activeLfoGenerator();
  lfo_editor_item_phases_.clear();
  lfo_editor_item_index_ = -1;
  if (generator == nullptr)
    return;

  // Normal mode lists every point. Performer mode lists each grid block that
  // holds a shape, and Step Sequencer mode lists every grid block.
  const int mode = generator->getShapeMode();
  if (mode == LineGenerator::kModeLfo) {
    for (int i = 0; i < generator->getNumPoints(); ++i)
      lfo_editor_item_phases_.push_back(generator->getPoint(i).first);
  }
  else {
    const float block = lfoGridAmount();
    const int block_count = jmax(1, static_cast<int>(std::ceil(1.0f / block - 0.0005f)));
    for (int block_index = 0; block_index < block_count; ++block_index) {
      const float start = block_index * block;
      if (mode == LineGenerator::kModePerformer && !lfoPerformerBlockHasShape(generator, start))
        continue;
      lfo_editor_item_phases_.push_back(start);
    }
  }

  selectLfoEditorItemAtPhase(lfo_cursor_phase_, false);
}

void SynthEditor::selectLfoEditorItemAtPhase(float phase, bool notify) {
  if (lfo_editor_item_phases_.empty())
    return;

  auto* generator = activeLfoGenerator();
  const float target = generator != nullptr && generator->getShapeMode() != LineGenerator::kModeLfo
      ? lfoEditorBlockStart(phase) : phase;
  int closest = 0;
  float closest_distance = std::numeric_limits<float>::max();
  for (int i = 0; i < static_cast<int>(lfo_editor_item_phases_.size()); ++i) {
    const float distance = std::abs(lfo_editor_item_phases_[static_cast<size_t>(i)] - target);
    if (distance < closest_distance) {
      closest_distance = distance;
      closest = i;
    }
  }
  setLfoEditorItem(closest, notify);
}

void SynthEditor::setLfoEditorItem(int index, bool notify) {
  if (!isPositiveAndBelow(index, static_cast<int>(lfo_editor_item_phases_.size())))
    index = -1;
  const bool changed = index != lfo_editor_item_index_;
  lfo_editor_item_index_ = index;
  // As with a combo box, choosing the item that is already chosen does nothing.
  if (changed && notify)
    lfoEditorItemChosen();
}

void SynthEditor::lfoEditorItemChosen() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr ||
      !isPositiveAndBelow(lfo_editor_item_index_, static_cast<int>(lfo_editor_item_phases_.size())))
    return;

  lfo_cursor_phase_ = lfo_editor_item_phases_[static_cast<size_t>(lfo_editor_item_index_)];
  if (!lfo_multi_selection_mode_) {
    selected_lfo_point_phases_.clear();
    if (generator->getShapeMode() == LineGenerator::kModePerformer) {
      for (int index : lfoEditorBlockPointIndices(generator, lfo_cursor_phase_))
        selected_lfo_point_phases_.push_back(generator->getPoint(index).first);
    }
    else
      selected_lfo_point_phases_.push_back(lfo_cursor_phase_);
  }
}

void SynthEditor::moveLfoCursor(float delta) {
  lfo_cursor_phase_ = snapLfoPhaseToGrid(jlimit(0.0f, 1.0f, lfo_cursor_phase_ + delta));
  auto* generator = activeLfoGenerator();
  if (generator)
    selectLfoEditorItemAtPhase(lfo_cursor_phase_, false);
  postLfoAnnouncement(lfoMsegStatusText());
}

void SynthEditor::moveToLfoPoint(int direction) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0) {
    postLfoAnnouncement("No points");
    return;
  }

  const float tolerance = 0.0005f;
  int next = -1;
  const int current = currentLfoPointIndex();

  if (current >= 0) {
    next = jlimit(0, generator->getNumPoints() - 1, current + direction);
  }
  else if (direction > 0) {
    for (int i = 0; i < generator->getNumPoints(); ++i) {
      if (generator->getPoint(i).first > lfo_cursor_phase_ + tolerance) {
        next = i;
        break;
      }
    }
    if (next < 0)
      next = generator->getNumPoints() - 1;
  }
  else {
    for (int i = generator->getNumPoints() - 1; i >= 0; --i) {
      if (generator->getPoint(i).first < lfo_cursor_phase_ - tolerance) {
        next = i;
        break;
      }
    }
    if (next < 0)
      next = 0;
  }

  lfo_cursor_phase_ = generator->getPoint(next).first;
  if (!lfo_multi_selection_mode_) {
    selected_lfo_point_phases_.clear();
    selected_lfo_point_phases_.push_back(lfo_cursor_phase_);
  }
  setLfoEditorItem(next, true);
  postLfoAnnouncement(lfoMsegStatusText());
}

void SynthEditor::moveToLfoPerformerBlock(int direction) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0) {
    postLfoAnnouncement("No shapes");
    return;
  }

  const float tolerance = 0.0005f;
  std::vector<float> blocks;
  for (int i = 0; i < generator->getNumPoints(); ++i) {
    const float block = lfoEditorBlockStart(generator->getPoint(i).first);
    if (lfoPerformerBlockHasShape(generator, block) &&
        std::none_of(blocks.begin(), blocks.end(), [block](float existing) {
          return std::abs(existing - block) <= 0.0005f;
        }))
      blocks.push_back(block);
  }
  std::sort(blocks.begin(), blocks.end());

  if (blocks.empty()) {
    postLfoAnnouncement("No shapes");
    return;
  }

  int current = -1;
  const float current_block = lfoEditorBlockStart(lfo_cursor_phase_);
  for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
    if (std::abs(blocks[static_cast<size_t>(i)] - current_block) <= tolerance) {
      current = i;
      break;
    }
  }

  int next = 0;
  if (current >= 0)
    next = jlimit(0, static_cast<int>(blocks.size()) - 1, current + direction);
  else if (direction > 0) {
    next = static_cast<int>(blocks.size()) - 1;
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
      if (blocks[static_cast<size_t>(i)] > lfo_cursor_phase_ + tolerance) {
        next = i;
        break;
      }
    }
  }
  else {
    next = 0;
    for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; --i) {
      if (blocks[static_cast<size_t>(i)] < lfo_cursor_phase_ - tolerance) {
        next = i;
        break;
      }
    }
  }

  lfo_cursor_phase_ = blocks[static_cast<size_t>(next)];
  selected_lfo_point_phases_.clear();
  for (int i : lfoEditorBlockPointIndices(generator, lfo_cursor_phase_)) {
    selected_lfo_point_phases_.push_back(generator->getPoint(i).first);
  }

  selectLfoEditorItemAtPhase(lfo_cursor_phase_, false);
  postLfoAnnouncement("Shape " + String(next + 1) + ", " + lfoTimeDescription(lfo_cursor_phase_) +
                      ", " + String(msegTimeDivisions()[lfo_grid_index_].name));
}

void SynthEditor::stepLfoCurve(int direction) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() < 2)
    return;

  const int mode = generator->getShapeMode();
  if (mode == LineGenerator::kModeStepSequencer) {
    postLfoAnnouncement("Curve editing is ignored in Step Sequencer mode; every step uses Hold");
    return;
  }

  std::vector<int> indices;
  if (mode == LineGenerator::kModePerformer) {
    const float start = lfoEditorBlockStart(lfo_cursor_phase_);
    if (!lfoPerformerBlockHasShape(generator, start)) {
      postLfoAnnouncement("No performer shape at " + lfoTimeDescription(start));
      return;
    }
    indices = lfoEditorBlockPointIndices(generator, start);
  }
  else if (lfo_multi_selection_mode_)
    indices = selectedLfoPointIndices();

  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (indices.empty()) {
    postLfoAnnouncement("No point selected. Press A to add a point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }

  const int reference_index = indices.front();
  const int reference_curve_point = isPositiveAndBelow(reference_index, generator->getNumPoints() - 1)
      ? reference_index : generator->getNumPoints() - 2;
  const int current_curve = curveIndexForPower(generator->getPower(reference_curve_point), generator->smooth());
  int next_curve = 0;
  if (mode == LineGenerator::kModePerformer) {
    static const std::array<int, 4> performer_curves {{
      kCurveHold, kCurveLinear, kCurveEaseIn, kCurveEaseOut
    }};
    int current_position = 1;
    for (int i = 0; i < static_cast<int>(performer_curves.size()); ++i) {
      if (performer_curves[static_cast<size_t>(i)] == current_curve) {
        current_position = i;
        break;
      }
    }
    next_curve = performer_curves[static_cast<size_t>(
        (current_position + direction + static_cast<int>(performer_curves.size())) %
        static_cast<int>(performer_curves.size()))];
  }
  else
    next_curve = (current_curve + direction + kNumAccessibleLfoCurves) % kNumAccessibleLfoCurves;

  generator->setSmooth(mode == LineGenerator::kModePerformer ? false : smoothForCurveIndex(next_curve));
  for (int index : indices) {
    if (isPositiveAndBelow(index, generator->getNumPoints() - 1))
      generator->setPower(index, powerForCurveIndex(next_curve));
  }
  generator->render();
  synth_.markStateChanged();

  if (mode == LineGenerator::kModePerformer)
    postLfoAnnouncement("Performer shape curve " + curveNameForIndex(next_curve) + ", " +
                        lfoTimeDescription(lfoEditorBlockStart(lfo_cursor_phase_)));
  else if (indices.size() == 1)
    postLfoAnnouncement("Curve " + curveNameForIndex(next_curve) + ", " + lfoPointDescription(indices.front()));
  else
    postLfoAnnouncement("Curve " + curveNameForIndex(next_curve) + ", " +
                        String(static_cast<int>(indices.size())) + " points");
}

void SynthEditor::stepLfoShapePreset(int direction) {
  if (auto* generator = activeLfoGenerator();
      generator != nullptr && generator->getShapeMode() == LineGenerator::kModeStepSequencer) {
    postLfoAnnouncement("Shape presets are ignored in Step Sequencer mode; use Up and Down to set the current step");
    return;
  }
  const auto& names = lfoShapePresetNames();
  lfo_shape_preset_index_ = jlimit(0, names.size() - 1, lfo_shape_preset_index_ + direction);
  // The LFO's shape preset menu follows, so its Apply shape button applies the same shape.
  for (auto& component : section_headers_) {
    if (component == nullptr || !component->getProperties().contains("LfoShapePreset") ||
        static_cast<int>(component->getProperties()["LfoShapePreset"]) != active_lfo_index_)
      continue;
    if (auto* combo = dynamic_cast<ComboBox*>(component.get()))
      combo->setSelectedItemIndex(lfo_shape_preset_index_, dontSendNotification);
  }
  postLfoAnnouncement("Shape preset " + names[lfo_shape_preset_index_]);
}

void SynthEditor::stepLfoCycleLength(int direction) {
  const int count = static_cast<int>(msegTimeDivisions().size());
  lfo_cycle_index_ = jlimit(0, count - 1, lfo_cycle_index_ + direction);
  applyLfoCycleLength();
}

void SynthEditor::stepLfoGrid(int direction) {
  const int count = static_cast<int>(msegTimeDivisions().size());
  lfo_grid_index_ = jlimit(0, count - 1, lfo_grid_index_ + direction);
  postLfoAnnouncement("Grid " + String(msegTimeDivisions()[static_cast<size_t>(lfo_grid_index_)].name) + ". " +
                      lfoMsegStatusText());
}

void SynthEditor::switchLfoFromShortcut(int lfoIndex) {
  active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, lfoIndex);
  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  syncLfoEditorState();
  postLfoAnnouncement("Editing LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::clearLfoShape() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;
  generator->setNumPoints(2);
  generator->setPoint(0, { 0.0f, 0.5f });
  generator->setPoint(1, { 1.0f, 0.5f });
  const float power = generator->getShapeMode() == LineGenerator::kModeStepSequencer ?
      LineGenerator::kHoldPowerThreshold : 0.0f;
  generator->setPower(0, power);
  generator->setPower(1, power);
  generator->setSmooth(false);
  generator->render();
  synth_.markStateChanged();
  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  syncLfoEditorState();
  postLfoAnnouncement("Cleared LFO " + String(active_lfo_index_ + 1));
}

bool SynthEditor::handleLfoMsegShortcut(const KeyPress& key) {
  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const bool shift = key.getModifiers().isShiftDown();
  const bool command = key.getModifiers().isCommandDown();
  // KeyPress::operator== only matches a key without modifiers, so arrows that
  // take Shift or Command are compared by key code.
  const int code = key.getKeyCode();
  const bool shift_only = shift && !command && !key.getModifiers().isCtrlDown() && !key.getModifiers().isAltDown();

  const int lfo_index = shiftedDigitIndex(key);
  if (lfo_index >= 0 && lfo_index < vital::kNumLfos) {
    switchLfoFromShortcut(lfo_index);
    return true;
  }

  if (isCommandChord(key.getModifiers()) && !shift && (code == KeyPress::upKey || code == KeyPress::downKey)) {
    if (auto* generator = activeLfoGenerator()) {
      // Step from the curve after the current point, or before it for the last point.
      const int point_index = currentLfoPointIndex();
      const int curve_point = isPositiveAndBelow(point_index, generator->getNumPoints() - 1)
          ? point_index : jmax(0, point_index - 1);
      const int current = curveIndexForPower(generator->getPower(curve_point), generator->smooth());
      setLfoPointCurve((current + (code == KeyPress::upKey ? 1 : -1) + kNumAccessibleLfoCurves) %
                       kNumAccessibleLfoCurves);
    }
    return true;
  }
  if (shift_only && (code == KeyPress::leftKey || code == KeyPress::rightKey)) {
    moveLfoPointTime(code == KeyPress::rightKey ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (key == KeyPress::leftKey || key == KeyPress::rightKey) {
    moveLfoCursor(key == KeyPress::rightKey ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if ((key == KeyPress::upKey || key == KeyPress::downKey) ||
      (shift_only && (code == KeyPress::upKey || code == KeyPress::downKey))) {
    moveLfoPointValue(code == KeyPress::upKey ? (shift ? 0.05f : 0.01f) : (shift ? -0.05f : -0.01f));
    return true;
  }
  if (key.getKeyCode() == KeyPress::backspaceKey) {
    clearLfoShape();
    return true;
  }
  if (shift && character == 'c') {
    copyLfoShape();
    return true;
  }
  if (shift && character == 'v') {
    pasteLfoShape();
    return true;
  }
  if (shift && character == 'd') {
    duplicateLfoShapeToNextSlot();
    return true;
  }
  if (shift && character == 'a') {
    applyLfoShapePreset();
    return true;
  }
  if (character == 'a') {
    if (auto* generator = activeLfoGenerator()) {
      if (generator->getShapeMode() == LineGenerator::kModeLfo)
        addLfoPoint();
      else
        applyLfoShapePreset();
    }
    return true;
  }
  if (shift && character == 's') {
    toggleLfoMultiSelectionMode();
    return true;
  }
  if (character == 's') {
    toggleLfoPointSelection();
    return true;
  }
  if (character == 'r') {
    clearLfoPointSelection();
    return true;
  }
  if (character == 'e' || key.getKeyCode() == KeyPress::deleteKey) {
    deleteLfoPoint();
    return true;
  }
  if (character == '[' || character == '{') {
    moveLfoPointValue(shift ? -0.05f : -0.01f);
    return true;
  }
  if (character == ']' || character == '}') {
    moveLfoPointValue(shift ? 0.05f : 0.01f);
    return true;
  }
  if (character == 'i' || character == 'k') {
    stepLfoCurve(character == 'i' ? 1 : -1);
    return true;
  }
  if (character == 'j' || character == 'l') {
    moveLfoPointTime(character == 'l' ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (character == ',') {
    if (auto* generator = activeLfoGenerator()) {
      if (generator->getShapeMode() == LineGenerator::kModeStepSequencer)
        moveLfoCursor(-lfoGridAmount());
      else if (generator->getShapeMode() == LineGenerator::kModePerformer)
        moveToLfoPerformerBlock(-1);
      else
        moveToLfoPoint(-1);
    }
    return true;
  }
  if (character == '.') {
    if (auto* generator = activeLfoGenerator()) {
      if (generator->getShapeMode() == LineGenerator::kModeStepSequencer)
        moveLfoCursor(lfoGridAmount());
      else if (generator->getShapeMode() == LineGenerator::kModePerformer)
        moveToLfoPerformerBlock(1);
      else
        moveToLfoPoint(1);
    }
    return true;
  }
  if (character == 'g' || character == 'h') {
    stepLfoShapePreset(character == 'h' ? 1 : -1);
    return true;
  }
  if (character == 'm') {
    if (auto* generator = activeLfoGenerator())
      applyLfoMode((jlimit(0, LineGenerator::kNumShapeModes - 1, generator->getShapeMode()) + 1) %
                   LineGenerator::kNumShapeModes);
    return true;
  }
  if (character == ';' || character == '\'') {
    stepLfoCycleLength(character == '\'' ? 1 : -1);
    return true;
  }
  if (character == '-' || character == '_') {
    stepLfoGrid(-1);
    return true;
  }
  if (character == '=' || character == '+') {
    stepLfoGrid(1);
    return true;
  }
  if (character == 'c') {
    copyLfoPoints();
    return true;
  }
  if (character == 'v') {
    pasteLfoPoints();
    return true;
  }
  return false;
}

void SynthEditor::toggleLfoPointSelection() {
  auto* generator = activeLfoGenerator();
  const int index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (generator == nullptr || index < 0) {
    postLfoAnnouncement("No point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }

  const float phase = generator->getPoint(index).first;
  const auto existing = std::find_if(selected_lfo_point_phases_.begin(), selected_lfo_point_phases_.end(),
                                     [phase](float selected) { return std::abs(selected - phase) <= 0.0005f; });
  if (existing != selected_lfo_point_phases_.end()) {
    selected_lfo_point_phases_.erase(existing);
    postLfoAnnouncement("Removed point from selection, " + lfoPointDescription(index));
  }
  else {
    if (!lfo_multi_selection_mode_)
      selected_lfo_point_phases_.clear();
    selected_lfo_point_phases_.push_back(phase);
    postLfoAnnouncement("Added point to selection, " + lfoPointDescription(index));
  }
}

void SynthEditor::toggleLfoMultiSelectionMode() {
  lfo_multi_selection_mode_ = !lfo_multi_selection_mode_;
  if (!lfo_multi_selection_mode_) {
    selected_lfo_point_phases_.clear();
    const int index = lfoPointIndexAtPhase(lfo_cursor_phase_);
    if (auto* generator = activeLfoGenerator())
      if (isPositiveAndBelow(index, generator->getNumPoints()))
        selected_lfo_point_phases_.push_back(generator->getPoint(index).first);
  }
  postLfoAnnouncement(lfo_multi_selection_mode_ ? "Multi selection on" : "Multi selection off");
}

void SynthEditor::clearLfoPointSelection() {
  selected_lfo_point_phases_.clear();
  postLfoAnnouncement("Point selection cleared");
}

void SynthEditor::copyLfoPoints() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (indices.empty()) {
    postLfoAnnouncement("No points selected");
    return;
  }

  std::sort(indices.begin(), indices.end());
  copied_lfo_points_.clear();
  for (int index : indices) {
    copied_lfo_points_.push_back({ generator->getPoint(index), generator->getPower(index) });
  }
  postLfoAnnouncement("Copied " + String(static_cast<int>(copied_lfo_points_.size())) + " point" +
                      (copied_lfo_points_.size() == 1 ? "" : "s"));
}

void SynthEditor::pasteLfoPoints() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;
  if (copied_lfo_points_.empty()) {
    postLfoAnnouncement("No points copied");
    return;
  }

  std::vector<CopiedLfoPoint> source = copied_lfo_points_;
  std::sort(source.begin(), source.end(), [](const auto& a, const auto& b) {
    return a.point.first < b.point.first;
  });

  const float source_start = source.front().point.first;
  const float source_end = source.back().point.first;
  const float span = jmax(0.0f, source_end - source_start);
  const float paste_start = span >= 1.0f ? 0.0f : jlimit(0.0f, 1.0f - span, lfo_cursor_phase_);

  std::vector<CopiedLfoPoint> points;
  points.reserve(generator->getNumPoints() + static_cast<int>(source.size()));
  for (int i = 0; i < generator->getNumPoints(); ++i)
    points.push_back({ generator->getPoint(i), generator->getPower(i) });

  selected_lfo_point_phases_.clear();
  std::vector<CopiedLfoPoint> pasted;
  pasted.reserve(source.size());
  for (auto point : source) {
    point.point.first = jlimit(0.0f, 1.0f, paste_start + (point.point.first - source_start));
    pasted.push_back(point);
    selected_lfo_point_phases_.push_back(point.point.first);
  }
  // Existing points at a pasted phase give way; pasted points that share a
  // phase (the two points of a step) all stay, in their copied order.
  points.erase(std::remove_if(points.begin(), points.end(), [&pasted](const auto& existing) {
    return std::any_of(pasted.begin(), pasted.end(), [&existing](const auto& point) {
      return std::abs(existing.point.first - point.point.first) <= 0.0005f;
    });
  }), points.end());
  points.insert(points.end(), pasted.begin(), pasted.end());

  std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
    return a.point.first < b.point.first;
  });
  const int count = jmin(static_cast<int>(points.size()), LineGenerator::kMaxPoints);

  generator->setNumPoints(count);
  for (int i = 0; i < count; ++i) {
    generator->setPoint(i, points[static_cast<size_t>(i)].point);
    generator->setPower(i, points[static_cast<size_t>(i)].power);
  }
  generator->render();
  synth_.markStateChanged();

  lfo_cursor_phase_ = paste_start;
  syncLfoEditorState();
  postLfoAnnouncement("Pasted " + String(static_cast<int>(source.size())) + " point" +
                      (source.size() == 1 ? "" : "s") + " at " + lfoTimeDescription(paste_start));
}

void SynthEditor::copyLfoShape() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  copied_lfo_shape_.clear();
  for (int i = 0; i < generator->getNumPoints(); ++i)
    copied_lfo_shape_.push_back({ generator->getPoint(i), generator->getPower(i) });
  // Smooth and S Curve segments live in the smooth flag, not in the points.
  copied_lfo_shape_smooth_ = generator->smooth();
  copied_lfo_shape_mode_ = generator->getShapeMode();
  postLfoAnnouncement("Copied LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::pasteLfoShape() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;
  if (copied_lfo_shape_.empty()) {
    postLfoAnnouncement("No pattern copied");
    return;
  }

  const int count = jmin(static_cast<int>(copied_lfo_shape_.size()), LineGenerator::kMaxPoints);
  generator->setNumPoints(count);
  for (int i = 0; i < count; ++i) {
    generator->setPoint(i, copied_lfo_shape_[static_cast<size_t>(i)].point);
    generator->setPower(i, copied_lfo_shape_[static_cast<size_t>(i)].power);
  }
  generator->setShapeMode(copied_lfo_shape_mode_);
  generator->setSmooth(copied_lfo_shape_smooth_);
  generator->render();
  synth_.markStateChanged();

  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  syncLfoEditorState();
  postLfoAnnouncement("Pasted to LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::duplicateLfoShapeToNextSlot() {
  auto* source = activeLfoGenerator();
  if (source == nullptr)
    return;

  std::vector<CopiedLfoPoint> duplicate;
  for (int i = 0; i < source->getNumPoints(); ++i)
    duplicate.push_back({ source->getPoint(i), source->getPower(i) });
  const bool smooth = source->smooth();
  const int mode = source->getShapeMode();

  const int destination = (active_lfo_index_ + 1) % vital::kNumLfos;
  active_lfo_index_ = destination;
  if (auto* target = activeLfoGenerator()) {
    const int count = jmin(static_cast<int>(duplicate.size()), LineGenerator::kMaxPoints);
    target->setNumPoints(count);
    for (int i = 0; i < count; ++i) {
      target->setPoint(i, duplicate[static_cast<size_t>(i)].point);
      target->setPower(i, duplicate[static_cast<size_t>(i)].power);
    }
    target->setShapeMode(mode);
    target->setSmooth(smooth);
    target->render();
  }
  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  syncLfoEditorState();
  postLfoAnnouncement("Duplicated to LFO " + String(destination + 1));
}

void SynthEditor::applyLfoShapePreset() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  const int shape = lfo_shape_preset_index_;
  if (shape == 0) {
    postLfoAnnouncement("Custom shape selected. No change applied.");
    return;
  }
  const int mode = generator->getShapeMode();
  if (mode == LineGenerator::kModeStepSequencer) {
    postLfoAnnouncement("Shape presets are ignored in Step Sequencer mode; use Up and Down to set the current step");
    return;
  }
  if (mode == LineGenerator::kModeLfo && shape == 1)
    generator->initSin();
  else if (mode == LineGenerator::kModeLfo && shape == 2)
    generator->initTriangle();
  else if (mode == LineGenerator::kModeLfo && shape == 3)
    generator->initSquare();
  else if (mode == LineGenerator::kModeLfo && shape == 4)
    generator->initSawUp();
  else if (mode == LineGenerator::kModeLfo && shape == 5)
    generator->initSawDown();
  else {
    struct ShapePoint {
      float phase = 0.0f;
      float value = 0.0f;
      float power = 0.0f;
    };

    auto shape_points_for = [shape, mode]() {
      std::vector<ShapePoint> points;
      auto add = [&points](float phase, float value, float power = 0.0f) {
        points.push_back({ phase, value, power });
      };
      const float hold = LineGenerator::kHoldPowerThreshold;
      constexpr float pi = 3.14159265358979323846f;

      if (shape == 1) {
        add(0.0f, 0.5f);
        add(0.5f, 1.0f);
        add(1.0f, 0.5f);
      }
      else if (shape == 2) {
        add(0.0f, 0.0f);
        add(0.5f, 1.0f);
        add(1.0f, 0.0f);
      }
      else if (shape == 3) {
        add(0.0f, 1.0f, hold);
        add(0.5f, 1.0f, hold);
        add(0.5f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 4) {
        add(0.0f, 0.0f);
        add(1.0f, 1.0f);
      }
      else if (shape == 5) {
        add(0.0f, 1.0f);
        add(1.0f, 0.0f);
      }
      else if (shape == 6) {
        add(0.0f, 1.0f, hold);
        add(0.25f, 1.0f, hold);
        add(0.25f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 7) {
        add(0.0f, 0.0f, hold);
        add(0.25f, 0.3f, hold);
        add(0.5f, 0.7f, hold);
        add(0.75f, 1.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 8 || shape == 9) {
        for (int i = 0; i <= 8; ++i) {
          const float value = i / 8.0f;
          add(i / 8.0f, shape == 8 ? value : 1.0f - value, hold);
        }
      }
      else if (shape == 10) {
        add(0.0f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 11) {
        add(0.0f, 0.5f, hold);
        add(1.0f, 0.5f, hold);
      }
      else if (shape == 12) {
        add(0.0f, 1.0f, hold);
        add(1.0f, 1.0f, hold);
      }
      else if (shape == 13) {
        add(0.0f, 1.0f, hold);
        add(0.5f, 1.0f, hold);
        add(0.5f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 14) {
        add(0.0f, 1.0f, hold);
        add(0.25f, 1.0f, hold);
        add(0.25f, 0.0f, hold);
        add(0.5f, 0.0f, hold);
        add(0.5f, 1.0f, hold);
        add(0.75f, 1.0f, hold);
        add(0.75f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 15) {
        add(0.0f, 1.0f, hold);
        add(0.125f, 1.0f, hold);
        add(0.125f, 0.0f, hold);
        add(0.25f, 0.0f, hold);
        add(0.25f, 1.0f, hold);
        add(0.375f, 1.0f, hold);
        add(0.375f, 0.0f, hold);
        add(0.5f, 0.0f, hold);
        add(0.5f, 1.0f, hold);
        add(0.625f, 1.0f, hold);
        add(0.625f, 0.0f, hold);
        add(0.75f, 0.0f, hold);
        add(0.75f, 1.0f, hold);
        add(0.875f, 1.0f, hold);
        add(0.875f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 16) {
        add(0.0f, 0.0f);
        add(0.5f, 1.0f);
        add(1.0f, 0.0f);
      }
      else if (shape == 17) {
        add(0.0f, 1.0f);
        add(0.5f, 0.0f);
        add(1.0f, 1.0f);
      }
      else if (shape == 18) {
        add(0.0f, 0.0f);
        add(1.0f, 1.0f);
      }
      else if (shape == 19) {
        add(0.0f, 1.0f);
        add(1.0f, 0.0f);
      }
      else if (shape == 20) {
        add(0.0f, 1.0f, hold);
        add(0.125f, 1.0f, hold);
        add(0.125f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 21) {
        add(0.0f, 0.0f, hold);
        add(0.5f, 0.0f, hold);
        add(0.5f, 1.0f, hold);
        add(0.75f, 1.0f, hold);
        add(0.75f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 22) {
        for (int i = 0; i < 3; ++i) {
          const float start = i / 3.0f;
          const float end = start + 1.0f / 6.0f;
          add(start, 1.0f, hold);
          add(end, 1.0f, hold);
          add(end, 0.0f, hold);
        }
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 23) {
        add(0.0f, 1.0f);
        add(0.25f, 0.0f);
        add(1.0f, 1.0f);
      }
      else if (shape == 24) {
        add(0.0f, 0.0f);
        add(0.75f, 1.0f);
        add(1.0f, 0.0f);
      }
      else if (shape == 25 || shape == 26) {
        const int steps = shape == 25 ? 4 : 8;
        for (int i = 0; i < steps; ++i) {
          const float start = i / static_cast<float>(steps);
          const float mid = start + 0.5f / steps;
          add(start, 1.0f, hold);
          add(mid, 1.0f, hold);
          add(mid, 0.0f, hold);
        }
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 27 || shape == 28) {
        for (int i = 0; i <= 4; ++i) {
          const float phase = i / 4.0f;
          const float value = i / 4.0f;
          add(phase, shape == 27 ? value : 1.0f - value, hold);
        }
      }
      else if (shape == 29) {
        for (int i = 0; i <= 8; ++i)
          add(i / 8.0f, (i % 2) ? 1.0f : 0.0f, hold);
      }
      else if (shape == 30) {
        add(0.0f, 0.0f);
        add(0.75f, 1.0f);
        add(0.75f, 1.0f, hold);
        add(1.0f, 1.0f, hold);
      }
      else if (shape == 31) {
        add(0.0f, 1.0f);
        add(0.75f, 0.0f);
        add(0.75f, 0.0f, hold);
        add(1.0f, 0.0f, hold);
      }
      else if (shape == 32) {
        for (int i = 0; i <= 8; ++i) {
          const float phase = i / 8.0f;
          add(phase, phase * phase);
        }
      }
      else if (shape == 33) {
        for (int i = 0; i <= 8; ++i) {
          const float phase = i / 8.0f;
          add(phase, 1.0f - (1.0f - phase) * (1.0f - phase));
        }
      }
      else if (shape == 34) {
        for (int i = 0; i <= 8; ++i) {
          const float phase = i / 8.0f;
          add(phase, 1.0f - std::sin(phase * pi));
        }
      }
      else {
        for (int i = 0; i <= 8; ++i) {
          const float phase = i / 8.0f;
          add(phase, std::sin(phase * pi));
        }
      }

      if (mode == LineGenerator::kModeStepSequencer) {
        for (auto& point : points)
          point.power = LineGenerator::kHoldPowerThreshold;
      }
      return points;
    };

    auto setPoints = [this, generator](std::vector<ShapePoint> points) {
      constexpr float tolerance = 0.0005f;
      for (auto& point : points) {
        point.phase = jlimit(0.0f, 1.0f, point.phase);
        point.value = jlimit(0.0f, 1.0f, point.value);
      }
      std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.phase < b.phase;
      });

      std::vector<ShapePoint> cleaned;
      cleaned.reserve(points.size() + 2);
      if (points.empty() || points.front().phase > tolerance)
        cleaned.push_back({ 0.0f, points.empty() ? 0.0f : points.front().value,
                            points.empty() ? 0.0f : points.front().power });

      for (size_t i = 0; i < points.size();) {
        const float phase = points[i].phase;
        size_t end = i + 1;
        while (end < points.size() && std::abs(points[end].phase - phase) <= tolerance)
          ++end;

        if (end - i == 1) {
          cleaned.push_back(points[i]);
        }
        else {
          ShapePoint first = points[i];
          ShapePoint last = points[end - 1];
          first.phase = phase;
          last.phase = phase;
          cleaned.push_back(first);
          if (std::abs(first.value - last.value) > tolerance || std::abs(first.power - last.power) > tolerance)
            cleaned.push_back(last);
        }
        i = end;
      }

      if (cleaned.empty() || cleaned.back().phase < 1.0f - tolerance)
        cleaned.push_back({ 1.0f, cleaned.empty() ? 0.0f : cleaned.back().value,
                            cleaned.empty() ? 0.0f : cleaned.back().power });
      else
        cleaned.back().phase = 1.0f;

      if (cleaned.size() > LineGenerator::kMaxPoints) {
        cleaned.resize(LineGenerator::kMaxPoints);
        cleaned.back().phase = 1.0f;
      }

      const int count = jmin(static_cast<int>(cleaned.size()), LineGenerator::kMaxPoints);
      generator->setNumPoints(count);
      int index = 0;
      for (const auto& point : cleaned) {
        if (index >= count)
          break;
        generator->setPoint(index, { point.phase, 1.0f - point.value });
        generator->setPower(index, point.power);
        index++;
      }
      generator->setSmooth(false);
      generator->render();
      synth_.markStateChanged();
    };

    auto insertPoints = [this, generator](const std::vector<ShapePoint>& source) {
      const float start = lfoEditorBlockStart(lfo_cursor_phase_);
      const float end = lfoEditorBlockEnd(start);
      const float length = jmax(0.001f, end - start);
      constexpr float tolerance = 0.0005f;
      constexpr float tail_hold_margin = 0.03f;

      std::vector<ShapePoint> source_with_tail = source;
      if (!source_with_tail.empty()) {
        auto last = source_with_tail.back();
        ShapePoint tail = last;
        tail.phase = jlimit(0.0f, 1.0f - tolerance * 2.0f, 1.0f - tail_hold_margin);
        tail.power = LineGenerator::kHoldPowerThreshold;

        source_with_tail.erase(std::remove_if(source_with_tail.begin(), source_with_tail.end(),
                                              [tail](const auto& point) {
                                                return point.phase > tail.phase - 0.0005f;
                                              }),
                               source_with_tail.end());
        source_with_tail.push_back(tail);
      }

      std::vector<CopiedLfoPoint> replacement;
      replacement.reserve(source_with_tail.size() + 1);
      for (const auto& point : source_with_tail) {
        replacement.push_back({ { start + point.phase * length, 1.0f - point.value }, point.power });
      }

      const bool final_block = end >= 1.0f - tolerance;
      const bool next_has_shape = !final_block && lfoPerformerBlockHasShape(generator, end);
      if (final_block && !source.empty())
        replacement.push_back({ { 1.0f, 1.0f - source.back().value }, LineGenerator::kHoldPowerThreshold });
      else if (!next_has_shape)
        replacement.push_back({ { end, 1.0f }, LineGenerator::kHoldPowerThreshold });

      replaceLfoEditorBlock(replacement, final_block || !next_has_shape);
      lfo_cursor_phase_ = start;
    };

    const auto points = shape_points_for();
    if (mode == LineGenerator::kModeLfo)
      setPoints(points);
    else
      insertPoints(points);

    if (mode == LineGenerator::kModeLfo && (shape == 1 || shape == 18 || shape == 19)) {
      generator->setSmooth(true);
      generator->render();
      synth_.markStateChanged();
    }
  }
  generator->setShapeMode(mode);

  syncLfoEditorState();
  postLfoAnnouncement((mode == LineGenerator::kModeLfo ? "Applied shape " : "Inserted shape ") +
                      lfoShapePresetNames()[shape]);
}

void SynthEditor::addLfoPoint() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() >= LineGenerator::kMaxPoints)
    return;

  lfo_cursor_phase_ = snapLfoPhaseToGrid(lfo_cursor_phase_);
  int insert_index = generator->getNumPoints();
  for (int i = 0; i < generator->getNumPoints(); ++i) {
    const float point_time = generator->getPoint(i).first;
    if (std::abs(point_time - lfo_cursor_phase_) <= 0.0005f) {
      setLfoEditorItem(i, true);
      postLfoAnnouncement("Point already exists, " + lfoMsegStatusText());
      return;
    }
    if (point_time > lfo_cursor_phase_) {
      insert_index = i;
      break;
    }
  }

  const float output_value = generator->valueAtPhase(lfo_cursor_phase_);
  generator->addPoint(insert_index, { lfo_cursor_phase_, 1.0f - output_value });
  generator->render();
  synth_.markStateChanged();
  syncLfoEditorState();
  setLfoEditorItem(insert_index, false);
  selected_lfo_point_phases_.clear();
  selected_lfo_point_phases_.push_back(lfo_cursor_phase_);
  postLfoAnnouncement("Added point, " + lfoPointDescription(insert_index));
}

void SynthEditor::deleteLfoPoint() {
  auto* generator = activeLfoGenerator();
  if (generator != nullptr && generator->getShapeMode() != LineGenerator::kModeLfo) {
    clearLfoPerformerBlock();
    return;
  }

  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (generator == nullptr || indices.empty()) {
    postLfoAnnouncement("No point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }
  if (generator->getNumPoints() <= 1) {
    postLfoAnnouncement("No point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }
  {
    const int last_index = generator->getNumPoints() - 1;
    indices.erase(std::remove_if(indices.begin(), indices.end(), [last_index](int index) {
      return index <= 0 || index >= last_index;
    }), indices.end());
    if (indices.empty()) {
      postLfoAnnouncement("The first and last points cannot be deleted");
      return;
    }
  }

  std::sort(indices.begin(), indices.end(), std::greater<int>());
  const String deleted = indices.size() == 1 ? lfoPointDescription(indices.front()) : String();
  for (int index : indices) {
    if (generator->getNumPoints() <= 1)
      break;
    generator->removePoint(index);
  }
  generator->render();
  synth_.markStateChanged();
  selected_lfo_point_phases_.clear();
  syncLfoEditorState();
  if (indices.size() == 1)
    postLfoAnnouncement("Deleted point, " + deleted);
  else
    postLfoAnnouncement("Deleted " + String(static_cast<int>(indices.size())) + " selected points");
}

void SynthEditor::clearLfoPerformerBlock() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  constexpr float tolerance = 0.0005f;
  const int mode = generator->getShapeMode();
  const float start = lfoEditorBlockStart(lfo_cursor_phase_);
  const float end = lfoEditorBlockEnd(start);
  if (mode == LineGenerator::kModePerformer && !lfoPerformerBlockHasShape(generator, start)) {
    postLfoAnnouncement("No performer shape at " + lfoTimeDescription(start));
    return;
  }

  const bool final_block = end >= 1.0f - tolerance;
  const bool next_has_shape = mode == LineGenerator::kModePerformer && !final_block &&
                              lfoPerformerBlockHasShape(generator, end);
  const float clear_power = LineGenerator::kHoldPowerThreshold;
  std::vector<CopiedLfoPoint> replacement {{ { start, 1.0f }, clear_power }};
  const bool remove_end_point = final_block || (mode == LineGenerator::kModePerformer && !next_has_shape);
  if (remove_end_point)
    replacement.push_back({ { end, 1.0f }, clear_power });

  replaceLfoEditorBlock(replacement, remove_end_point);

  lfo_cursor_phase_ = start;
  selected_lfo_point_phases_.clear();
  selected_lfo_point_phases_.push_back(start);
  syncLfoEditorState();
  postLfoAnnouncement((mode == LineGenerator::kModeStepSequencer ? "Cleared step, " :
                       "Cleared shape, ") + lfoTimeDescription(start) + ", " +
                      String(msegTimeDivisions()[lfo_grid_index_].name));
}

void SynthEditor::moveLfoPointTime(float delta) {
  auto* generator = activeLfoGenerator();
  if (generator != nullptr && generator->getShapeMode() != LineGenerator::kModeLfo) {
    postLfoAnnouncement(generator->getShapeMode() == LineGenerator::kModePerformer
                            ? "Performer shapes are anchored to their grid blocks; use Comma and Period to select a shape"
                            : "Step Sequencer steps are anchored to the grid; use Left and Right to select a step");
    return;
  }
  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (generator == nullptr || indices.empty()) {
    postLfoAnnouncement("No point selected. Press A to add a point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }
  // The first and last points pin the cycle's ends, as every other writer of
  // the shape keeps them.
  const int last_index = generator->getNumPoints() - 1;
  indices.erase(std::remove_if(indices.begin(), indices.end(), [last_index](int index) {
    return index <= 0 || index >= last_index;
  }), indices.end());
  if (indices.empty()) {
    postLfoAnnouncement("The first and last points stay at the start and end of the cycle");
    return;
  }

  std::vector<CopiedLfoPoint> points;
  for (int i = 0; i < generator->getNumPoints(); ++i)
    points.push_back({ generator->getPoint(i), generator->getPower(i) });

  selected_lfo_point_phases_.clear();
  for (int index : indices) {
    points[static_cast<size_t>(index)].point.first =
        snapLfoPhaseToGrid(jlimit(0.0f, 1.0f, points[static_cast<size_t>(index)].point.first + delta));
    selected_lfo_point_phases_.push_back(points[static_cast<size_t>(index)].point.first);
  }
  std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
    return a.point.first < b.point.first;
  });
  for (int i = 0; i < static_cast<int>(points.size()); ++i) {
    generator->setPoint(i, points[static_cast<size_t>(i)].point);
    generator->setPower(i, points[static_cast<size_t>(i)].power);
  }
  generator->render();
  synth_.markStateChanged();
  lfo_cursor_phase_ = selected_lfo_point_phases_.empty() ? lfo_cursor_phase_ : selected_lfo_point_phases_.front();
  syncLfoEditorState();
  const int moved_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.size() == 1 && moved_index >= 0)
    postLfoAnnouncement("Moved point, " + lfoPointDescription(moved_index));
  else
    postLfoAnnouncement("Moved " + String(static_cast<int>(indices.size())) + " selected points");
}

void SynthEditor::moveLfoPointValue(float delta) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  const int mode = generator->getShapeMode();
  if (mode == LineGenerator::kModeStepSequencer) {
    constexpr float tolerance = 0.0005f;
    const float start = lfoEditorBlockStart(lfo_cursor_phase_);
    const float end = lfoEditorBlockEnd(start);
    const float current = generator->valueAtPhase(lfoEditorBlockSamplePhase(start));
    const float output = jlimit(0.0f, 1.0f, current + delta);
    std::vector<CopiedLfoPoint> replacement {{
      { start, 1.0f - output }, LineGenerator::kHoldPowerThreshold
    }};
    const bool final_block = end >= 1.0f - tolerance;
    if (final_block)
      replacement.push_back({ { 1.0f, 1.0f - output }, LineGenerator::kHoldPowerThreshold });

    replaceLfoEditorBlock(replacement, final_block);
    lfo_cursor_phase_ = start;
    selected_lfo_point_phases_.clear();
    selected_lfo_point_phases_.push_back(start);
    syncLfoEditorState();
    postLfoAnnouncement("Step value " + percentString(output) + ", " + lfoTimeDescription(start));
    return;
  }

  if (mode == LineGenerator::kModePerformer) {
    const float start = lfoEditorBlockStart(lfo_cursor_phase_);
    if (!lfoPerformerBlockHasShape(generator, start)) {
      postLfoAnnouncement("No performer shape at " + lfoTimeDescription(start));
      return;
    }
    const auto block_indices = lfoEditorBlockPointIndices(generator, start);
    float minimum = 1.0f;
    float maximum = 0.0f;
    for (int index : block_indices) {
      const float output = pointOutputValue(generator, index);
      minimum = jmin(minimum, output);
      maximum = jmax(maximum, output);
    }
    const float applied_delta = jlimit(-minimum, 1.0f - maximum, delta);
    selected_lfo_point_phases_.clear();
    for (int index : block_indices) {
      auto point = generator->getPoint(index);
      point.second = 1.0f - jlimit(0.0f, 1.0f, 1.0f - point.second + applied_delta);
      generator->setPoint(index, point);
      selected_lfo_point_phases_.push_back(point.first);
    }
    generator->render();
    synth_.markStateChanged();
    lfo_cursor_phase_ = start;
    syncLfoEditorState();
    postLfoAnnouncement("Performer shape range " + percentString(minimum + applied_delta) + " to " +
                        percentString(maximum + applied_delta) + ", " + lfoTimeDescription(start));
    return;
  }

  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (indices.empty()) {
    postLfoAnnouncement("No point selected. Press A to add a point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }

  float output_value = 0.0f;
  for (int index : indices) {
    const auto point = generator->getPoint(index);
    output_value = jlimit(0.0f, 1.0f, pointOutputValue(generator, index) + delta);
    generator->setPoint(index, { point.first, 1.0f - output_value });
  }
  generator->render();
  synth_.markStateChanged();
  syncLfoEditorState();
  if (indices.size() == 1)
    postLfoAnnouncement("Value " + percentString(output_value) + ", " + lfoPointDescription(indices.front()));
  else
    postLfoAnnouncement("Changed " + String(static_cast<int>(indices.size())) +
                        " selected points by " + percentString(std::abs(delta)));
}

void SynthEditor::setLfoPointCurve(int curve_index) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() < 2)
    return;

  const int mode = generator->getShapeMode();
  if (mode == LineGenerator::kModeStepSequencer) {
    postLfoAnnouncement("Curve editing is ignored in Step Sequencer mode; every step uses Hold");
    return;
  }

  auto indices = mode == LineGenerator::kModePerformer
      ? lfoEditorBlockPointIndices(generator, lfoEditorBlockStart(lfo_cursor_phase_))
      : (lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>());
  const int point_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && point_index >= 0)
    indices.push_back(point_index);
  if (indices.empty()) {
    postLfoAnnouncement("No point selected. Press A to add a point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }
  if (mode == LineGenerator::kModePerformer && smoothForCurveIndex(curve_index)) {
    postLfoAnnouncement("Smooth and S Curve are whole-LFO options. Use Hold, Linear, Ease In, or Ease Out for one performer shape");
    return;
  }
  generator->setSmooth(mode == LineGenerator::kModePerformer ? false : smoothForCurveIndex(curve_index));
  for (int index : indices) {
    if (isPositiveAndBelow(index, generator->getNumPoints() - 1))
      generator->setPower(index, powerForCurveIndex(curve_index));
  }
  generator->render();
  synth_.markStateChanged();
  if (mode == LineGenerator::kModePerformer)
    postLfoAnnouncement("Performer shape curve " + curveNameForIndex(curve_index) + ", " +
                        lfoTimeDescription(lfoEditorBlockStart(lfo_cursor_phase_)));
  else if (indices.size() == 1)
    postLfoAnnouncement("Curve " + curveNameForIndex(curve_index) + ", " + lfoPointDescription(indices.front()));
  else
    postLfoAnnouncement("Changed curve for " + String(static_cast<int>(indices.size())) + " selected points");
}

ValueBridge* SynthEditor::parameterBridge(const String& id) const {
  const auto found = parameters_by_id_.find(id);
  return found == parameters_by_id_.end() ? nullptr : found->second;
}

void SynthEditor::setParameterEngineValue(const String& id, float engineValue) {
  if (auto* bridge = parameterBridge(id)) {
    synth_.beginFxAutomationGesture(id.toStdString());
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(bridge->convertToPluginValue(engineValue));
    bridge->endChangeGesture();
    synth_.endFxAutomationGesture(id.toStdString());
    return;
  }

  auto& controls = synth_.getControls();
  auto found = controls.find(id.toStdString());
  if (found != controls.end() && found->second != nullptr)
    synth_.valueChanged(id.toStdString(), engineValue);
}

void SynthEditor::randomizeAmplitudeSeed(const String& id) {
  auto* bridge = parameterBridge(id);
  if (bridge == nullptr)
    return;
  const int previous = static_cast<int>(engineValueFor(bridge));
  // Always select a different, non-legacy pattern with a single host gesture.
  const int choices = vital::RandomValues::kMaxSeed;
  int seed = 1 + Random::getSystemRandom().nextInt(previous > 0 ? choices - 1 : choices);
  if (previous > 0 && seed >= previous)
    ++seed;
  setParameterEngineValue(id, static_cast<float>(seed));
  for (auto& row : rows_)
    if (row->parameterId() == id)
      row->refresh();
  postPluginAnnouncement(bridge->getName(128) + ": " + String(seed),
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::setInternalParameterEngineValue(const String& id, float engineValue) {
  auto& controls = synth_.getControls();
  auto found = controls.find(id.toStdString());
  if (found != controls.end() && found->second != nullptr)
    synth_.valueChanged(id.toStdString(), engineValue);
}

int SynthEditor::oscillatorIndexForSectionName(const String& sectionName) const {
  if (!sectionName.startsWith("Oscillator "))
    return -1;

  const int oscillator = sectionName.fromFirstOccurrenceOf("Oscillator ", false, false).getIntValue() - 1;
  return isPositiveAndBelow(oscillator, vital::kNumOscillators) ? oscillator : -1;
}

void SynthEditor::showOscillatorContextMenu(int targetOscillator, Component& target) {
  if (!isPositiveAndBelow(targetOscillator, vital::kNumOscillators))
    return;

  enum {
    kPasteItem = 1,
    kCopyBase = 100,
    kCopyWithModulationsBase = 200,
  };

  PopupMenu menu;
  menu.addItem(kPasteItem, "Paste to Oscillator " + String(targetOscillator + 1),
               !copied_oscillator_.parameters.empty());
  menu.addSeparator();
  for (int oscillator = 0; oscillator < vital::kNumOscillators; ++oscillator) {
    const String name = "Oscillator " + String(oscillator + 1);
    menu.addItem(kCopyBase + oscillator, "Copy from " + name);
    menu.addItem(kCopyWithModulationsBase + oscillator, "Copy from " + name + " with modulations");
  }

  Component::SafePointer<Component> target_pointer(&target);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                     [this, targetOscillator, target_pointer](int result) {
    if (result == kPasteItem) {
      if (target_pointer != nullptr)
        pasteOscillatorClipboard(targetOscillator, *target_pointer);
      return;
    }
    if (result >= kCopyWithModulationsBase &&
        result < kCopyWithModulationsBase + vital::kNumOscillators) {
      copyOscillatorToClipboard(result - kCopyWithModulationsBase, true);
      return;
    }
    if (result >= kCopyBase && result < kCopyBase + vital::kNumOscillators)
      copyOscillatorToClipboard(result - kCopyBase, false);
  });
}

void SynthEditor::copyOscillatorToClipboard(int sourceOscillator, bool includeModulations) {
  if (!isPositiveAndBelow(sourceOscillator, vital::kNumOscillators))
    return;

  copied_oscillator_ = {};
  copied_oscillator_.source_oscillator = sourceOscillator;
  copied_oscillator_.include_modulations = includeModulations;

  if (auto* creator = synth_.getWavetableCreator(sourceOscillator))
    copied_oscillator_.wavetable = creator->stateToJson();
  // Sample, Granular and Spectral slots carry their sample along.
  if (vital::source_types::describe(sourceSlotType(sourceOscillator)).uses_sample) {
    if (auto* sample = synth_.getSample(sourceOscillator))
      copied_oscillator_.sample = sample->stateToJson();
  }
  if (sourceSlotType(sourceOscillator) == vital::source_types::kMultiSample) {
    if (auto* manager = synth_.getMultiSampleManager()) {
      const auto spec = manager->spec(sourceOscillator);
      if (!spec.empty())
        copied_oscillator_.instrument = spec.toJson(false);
    }
  }

  const String prefix = "osc_" + String(sourceOscillator + 1) + "_";
  for (const auto& entry : parameters_by_id_) {
    const String id = entry.first;
    auto* bridge = entry.second;
    if (bridge == nullptr || !id.startsWith(prefix))
      continue;

    CopiedOscillatorParameter parameter;
    parameter.suffix = id.substring(prefix.length());
    parameter.engine_value = bridge->convertToEngineValue(bridge->getValue());
    copied_oscillator_.parameters.push_back(parameter);
  }

  if (includeModulations) {
    for (auto* route : synth_.getModulationConnections()) {
      if (route == nullptr || route->source_name.empty() || route->destination_name.empty())
        continue;

      const String destination(route->destination_name);
      if (!destination.startsWith(prefix))
        continue;

      const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
      if (connection < 0)
        continue;

      auto readModulationParameter = [this, connection](const String& suffix) {
        if (auto* bridge = parameterBridge("modulation_" + String(connection + 1) + "_" + suffix))
          return bridge->convertToEngineValue(bridge->getValue());
        return 0.0f;
      };

      CopiedModulation modulation;
      modulation.source_id = String(route->source_name);
      modulation.destination_suffix = destination.substring(prefix.length());
      modulation.amount = readModulationParameter("amount");
      modulation.power = readModulationParameter("power");
      modulation.bipolar = readModulationParameter("bipolar");
      modulation.stereo = readModulationParameter("stereo");
      modulation.bypass = readModulationParameter("bypass");
      modulation.random_mode = readModulationParameter("random_mode");
      copied_oscillator_.modulations.push_back(modulation);
    }
  }

  String message = "Copied Oscillator " + String(sourceOscillator + 1);
  if (includeModulations)
    message += " with modulations";
  postPluginAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::pasteOscillatorClipboard(int targetOscillator, Component& target) {
  ignoreUnused(target);
  if (!isPositiveAndBelow(targetOscillator, vital::kNumOscillators) ||
      copied_oscillator_.parameters.empty())
    return;

  const String target_prefix = "osc_" + String(targetOscillator + 1) + "_";
  synth_.pauseProcessing(true);
  if (!copied_oscillator_.wavetable.is_null()) {
    if (auto* creator = synth_.getWavetableCreator(targetOscillator)) {
      creator->jsonToState(copied_oscillator_.wavetable);
      creator->render();
      synth_.markStateChanged();
    }
  }
  for (const auto& parameter : copied_oscillator_.parameters)
    setParameterEngineValue(target_prefix + parameter.suffix, parameter.engine_value);
  if (!copied_oscillator_.sample.is_null()) {
    if (auto* sample = synth_.getSample(targetOscillator))
      sample->jsonToState(copied_oscillator_.sample);
  }
  synth_.pauseProcessing(false);

  if (copied_oscillator_.include_modulations) {
    for (const auto& modulation : copied_oscillator_.modulations) {
      const String destination = target_prefix + modulation.destination_suffix;
      if (isInvalidModulationPair(modulation.source_id, destination))
        continue;

      synth_.connectModulation(modulation.source_id.toStdString(), destination.toStdString());
      const int connection = synth_.getConnectionIndex(modulation.source_id.toStdString(),
                                                       destination.toStdString());
      if (connection < 0)
        continue;

      const String modulation_prefix = "modulation_" + String(connection + 1) + "_";
      setParameterEngineValue(modulation_prefix + "amount", modulation.amount);
      setParameterEngineValue(modulation_prefix + "power", modulation.power);
      setParameterEngineValue(modulation_prefix + "bipolar", modulation.bipolar);
      setParameterEngineValue(modulation_prefix + "stereo", modulation.stereo);
      setParameterEngineValue(modulation_prefix + "bypass", modulation.bypass);
      setParameterEngineValue(modulation_prefix + "random_mode", modulation.random_mode);
    }
  }

  refreshVisibleModulationLabels();
  if (!copied_oscillator_.wavetable.is_null()) {
    wavetable_keyboard_state_[targetOscillator] = {};
    if (wavetable_editor_visible_[targetOscillator])
      rebuildWavetableEditor(targetOscillator, focusedAccessibleTitle());
  }
  if (!copied_oscillator_.sample.is_null())
    refreshSampleNameLabel(targetOscillator);
  if (!copied_oscillator_.instrument.is_null()) {
    if (auto* manager = synth_.getMultiSampleManager())
      manager->applySpec(targetOscillator, vital::multisample::InstrumentSpec::fromJson(copied_oscillator_.instrument),
                         File());
  }
  postPluginAnnouncement("Pasted to Oscillator " + String(targetOscillator + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

int SynthEditor::filterRoutingMode(bool activeOnly) const {
  auto on = [this](const char* id) {
    const auto* bridge = parameterBridge(id);
    return bridge && bridge->getValue() > 0.5f;
  };
  return static_cast<int>(vital::filterRouting(!activeOnly || on("filter_1_on"),
                                              !activeOnly || on("filter_2_on"),
                                              on("filter_1_filter_input"), on("filter_2_filter_input")));
}

void SynthEditor::updateRoutingSummary() {
  auto textFor = [this](const String& id) {
    if (auto* bridge = parameterBridge(id))
      return bridge->getText(bridge->getValue(), 128);
    return String("Unavailable");
  };

  String summary;
  for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
    if (summary.isNotEmpty())
      summary += "; ";
    summary += "Oscillator " + String(osc) + " to " + textFor("osc_" + String(osc) + "_destination");
  }
  const int active_mode = filterRoutingMode(true);
  summary +=
      "; Filter 1 to " + (active_mode == 1 ? String("Filter 2") : textFor("filter_1_destination")) +
      "; Filter 2 to " + (active_mode == 2 ? String("Filter 1") : textFor("filter_2_destination")) +
      ". Active filter routing: " +
      (active_mode == 1 ? "filter 1 into filter 2" : active_mode == 2 ? "filter 2 into filter 1" : "parallel filters");
  if (active_mode != 0)
    summary += ". Sources assigned to Filter 1 and 2 enter the chain once; only the last filter reaches its output destination.";
  else if (filterRoutingMode(false) != 0)
    summary += ". The serial link is inactive while its receiving filter is bypassed.";
  const bool changed = routing_summary_.getText() != summary;
  routing_summary_.setText(summary, dontSendNotification);
  routing_summary_.setDescription(summary);
  if (changed) {
    if (auto* handler = routing_summary_.getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  }
}

void SynthEditor::refreshRoutingControls() {
  updateRoutingSummary();

  const int mode = filterRoutingMode(false);

  ScopedValueSetter<bool> guard(updating_routing_controls_, true);
  routing_mode_.setSelectedItemIndex(mode, dontSendNotification);
}

void SynthEditor::applyRoutingPreset(int preset) {
  if (preset == 0) {
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    postPluginAnnouncement("Filters routed in parallel", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 1) {
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 1.0f);
    postPluginAnnouncement("Filter 1 routed into filter 2", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 2) {
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    setParameterEngineValue("filter_1_filter_input", 1.0f);
    postPluginAnnouncement("Filter 2 routed into filter 1", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 3) {
    setParameterEngineValue("osc_1_destination", vital::constants::kFilter1);
    setParameterEngineValue("osc_2_destination", vital::constants::kFilter2);
    setParameterEngineValue("osc_3_destination", vital::constants::kEffects);
    setParameterEngineValue("osc_4_destination", vital::constants::kEffects);
    for (int osc = 5; osc <= vital::kNumOscillators; ++osc)
      setParameterEngineValue("osc_" + String(osc) + "_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_1_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_2_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    postPluginAnnouncement("Default routing applied", AccessibilityHandler::AnnouncementPriority::high);
  }
  refreshRoutingControls();
  timerCallback();
}

int SynthEditor::effectSlotType(const String& sectionName, int slot) const {
  if (!isPositiveAndBelow(slot, vital::constants::kMaxEffectSlots))
    return 0;

  const String id = effectSlotPrefix(sectionName, slot) + "type";
  if (auto* bridge = parameterBridge(id))
    return jlimit(0, static_cast<int>(vital::constants::kNumEffects),
                  static_cast<int>(std::round(bridge->convertToEngineValue(bridge->getValue()))));

  const auto& controls = synth_.getControls();
  const auto found = controls.find(id.toStdString());
  if (found == controls.end() || found->second == nullptr)
    return 0;
  return jlimit(0, static_cast<int>(vital::constants::kNumEffects),
                static_cast<int>(std::round(found->second->value())));
}

int SynthEditor::firstEmptyEffectSlot(const String& sectionName) const {
  for (int slot = 0; slot < vital::constants::kMaxEffectSlots; ++slot) {
    if (effectSlotType(sectionName, slot) == 0)
      return slot;
  }
  return -1;
}

std::vector<int> SynthEditor::activeEffectSlots(const String& sectionName) const {
  std::vector<int> slots;
  for (int slot = 0; slot < vital::constants::kMaxEffectSlots; ++slot) {
    if (effectSlotType(sectionName, slot) > 0)
      slots.push_back(slot);
  }
  return slots;
}

String SynthEditor::effectSlotPrefix(const String& sectionName, int slot) const {
  return effectChainPrefixForSection(sectionName) + "fx_slot_" + String(slot + 1) + "_";
}

String SynthEditor::effectSlotTitle(const String& sectionName, int slot) const {
  const int type = effectSlotType(sectionName, slot);
  const String name = type > 0 ? effectName(type - 1) : String("Empty");
  return "Slot " + String(slot + 1) + " (" + name + ")";
}

void SynthEditor::swapEffectSlots(const String& sectionName, int firstSlot, int secondSlot) {
  if (!isPositiveAndBelow(firstSlot, vital::constants::kMaxEffectSlots) ||
      !isPositiveAndBelow(secondSlot, vital::constants::kMaxEffectSlots) ||
      firstSlot == secondSlot)
    return;

  const String first_prefix = effectSlotPrefix(sectionName, firstSlot);
  const String second_prefix = effectSlotPrefix(sectionName, secondSlot);
  std::vector<std::pair<String, float>> first_values;
  std::vector<std::pair<String, float>> second_values;
  float first_type = 0.0f;
  float second_type = 0.0f;
  struct SlotModulationRoute {
    String source;
    String old_destination;
    String new_destination;
    float amount = 0.0f;
    float power = 0.0f;
    float bipolar = 0.0f;
    float stereo = 0.0f;
    float bypass = 0.0f;
    float random_mode = 0.0f;
  };

  auto readModulationValue = [this](int connection, const String& suffix) {
    if (auto* bridge = parameterBridge("modulation_" + String(connection + 1) + "_" + suffix))
      return bridge->convertToEngineValue(bridge->getValue());
    return 0.0f;
  };

  auto swappedDestination = [&first_prefix, &second_prefix](const String& destination) {
    if (destination.startsWith(first_prefix))
      return second_prefix + destination.substring(first_prefix.length());
    if (destination.startsWith(second_prefix))
      return first_prefix + destination.substring(second_prefix.length());
    return String();
  };

  std::vector<SlotModulationRoute> modulations;
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name.empty() || route->destination_name.empty())
      continue;

    const String destination(route->destination_name);
    const String new_destination = swappedDestination(destination);
    if (new_destination.isEmpty())
      continue;

    const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
    if (connection < 0)
      continue;

    SlotModulationRoute modulation;
    modulation.source = String(route->source_name);
    modulation.old_destination = destination;
    modulation.new_destination = new_destination;
    modulation.amount = readModulationValue(connection, "amount");
    modulation.power = readModulationValue(connection, "power");
    modulation.bipolar = readModulationValue(connection, "bipolar");
    modulation.stereo = readModulationValue(connection, "stereo");
    modulation.bypass = readModulationValue(connection, "bypass");
    modulation.random_mode = readModulationValue(connection, "random_mode");
    modulations.push_back(modulation);
  }

  for (const auto& control : synth_.getControls()) {
    const String id(control.first);
    if (control.second == nullptr)
      continue;
    if (id.startsWith(first_prefix)) {
      const String suffix = id.substring(first_prefix.length());
      first_values.push_back({ suffix, control.second->value() });
      if (suffix == "type")
        first_type = control.second->value();
    }
    else if (id.startsWith(second_prefix)) {
      const String suffix = id.substring(second_prefix.length());
      second_values.push_back({ suffix, control.second->value() });
      if (suffix == "type")
        second_type = control.second->value();
    }
  }

  synth_.pauseProcessing(true);
  for (const auto& modulation : modulations)
    synth_.disconnectModulation(modulation.source.toStdString(), modulation.old_destination.toStdString());

  setInternalParameterEngineValue(first_prefix + "type", 0.0f);
  setInternalParameterEngineValue(second_prefix + "type", 0.0f);
  for (const auto& value : first_values) {
    if (value.first == "type")
      continue;
    setInternalParameterEngineValue(second_prefix + value.first, value.second);
  }
  for (const auto& value : second_values) {
    if (value.first == "type")
      continue;
    setInternalParameterEngineValue(first_prefix + value.first, value.second);
  }
  setInternalParameterEngineValue(second_prefix + "type", first_type);
  setInternalParameterEngineValue(first_prefix + "type", second_type);
  synth_.swapFxAutomationPrefixes(first_prefix.toStdString(), second_prefix.toStdString());
  swapEffectSlotModuleState(first_prefix, second_prefix);

  for (const auto& modulation : modulations) {
    synth_.connectModulation(modulation.source.toStdString(), modulation.new_destination.toStdString());
    const int connection = synth_.getConnectionIndex(modulation.source.toStdString(),
                                                     modulation.new_destination.toStdString());
    if (connection < 0)
      continue;

    const String modulation_prefix = "modulation_" + String(connection + 1) + "_";
    setInternalParameterEngineValue(modulation_prefix + "amount", modulation.amount);
    setInternalParameterEngineValue(modulation_prefix + "power", modulation.power);
    setInternalParameterEngineValue(modulation_prefix + "bipolar", modulation.bipolar);
    setInternalParameterEngineValue(modulation_prefix + "stereo", modulation.stereo);
    setInternalParameterEngineValue(modulation_prefix + "bypass", modulation.bypass);
    setInternalParameterEngineValue(modulation_prefix + "random_mode", modulation.random_mode);
  }
  synth_.pauseProcessing(false);
  refreshVisibleModulationLabels();
}

void SynthEditor::swapEffectSlotModuleState(const String& firstPrefix, const String& secondPrefix) {
  // A Convolution Reverb's impulse response lives in the slot's module and a
  // Filter FX table in a creator named after the slot; both stay behind when
  // the parameters move unless they are exchanged too, as are the MIDI learn
  // mappings, which are keyed by control name.
  auto* first_convolution = convolutionModuleForPrefix(firstPrefix);
  auto* second_convolution = convolutionModuleForPrefix(secondPrefix);
  if (first_convolution != nullptr && second_convolution != nullptr) {
    const ScopedLock lock(synth_.getCallbackLock());
    const json first_ir = first_convolution->hasCustomIr() ? first_convolution->irStateToJson() : json();
    const json second_ir = second_convolution->hasCustomIr() ? second_convolution->irStateToJson() : json();
    const std::string first_file = first_convolution->getLastBrowsedFile();
    const std::string second_file = second_convolution->getLastBrowsedFile();
    const auto apply = [](vital::ConvolutionReverbModule& module, const json& ir, const std::string& file) {
      if (!ir.is_null())
        module.irStateFromJson(ir);
      else if (module.hasCustomIr())
        module.setDefaultIr();
      module.setLastBrowsedFile(file);
    };
    apply(*first_convolution, second_ir, second_file);
    apply(*second_convolution, first_ir, first_file);
  }

  const std::string first_table = (firstPrefix + "filter_fx").toStdString();
  const std::string second_table = (secondPrefix + "filter_fx").toStdString();
  WavetableCreator* first_creator = synth_.getFilterTableCreator(first_table, false);
  WavetableCreator* second_creator = synth_.getFilterTableCreator(second_table, false);
  if (first_creator != nullptr || second_creator != nullptr) {
    const json first_state = first_creator != nullptr ? first_creator->stateToJson() : json();
    const json second_state = second_creator != nullptr ? second_creator->stateToJson() : json();
    const auto applyTable = [this](const std::string& prefix, const json& state) {
      if (state.is_null()) {
        synth_.resetFilterTable(prefix);
        return;
      }
      if (WavetableCreator* creator = synth_.getFilterTableCreator(prefix, true)) {
        try {
          creator->jsonToState(state);
          creator->render();
        }
        catch (const json::exception&) {
          synth_.resetFilterTable(prefix);
        }
      }
    };
    applyTable(first_table, second_state);
    applyTable(second_table, first_state);
  }

  synth_.swapMidiLearnPrefixes(firstPrefix.toStdString(), secondPrefix.toStdString());
}

void SynthEditor::clearEffectSlot(const String& sectionName, int slot) {
  if (!isPositiveAndBelow(slot, vital::constants::kMaxEffectSlots))
    return;

  const String prefix = effectSlotPrefix(sectionName, slot);
  synth_.clearFxAutomationForPrefix(prefix.toStdString());

  std::vector<String> ids;
  ids.reserve(64);
  for (const auto& control : synth_.getControls()) {
    const String id(control.first);
    if (control.second == nullptr || !id.startsWith(prefix) || !vital::Parameters::isParameter(control.first) ||
        id == prefix + "type" || id == prefix + "enabled")
      continue;
    ids.push_back(id);
  }
  // Routes into the removed effect would otherwise linger invisibly in the
  // modulation list, occupying connection slots.
  std::vector<std::pair<std::string, std::string>> routes;
  for (auto* route : synth_.getModulationConnections()) {
    if (route != nullptr && String(route->destination_name).startsWith(prefix))
      routes.push_back({ route->source_name, route->destination_name });
  }

  synth_.pauseProcessing(true);
  for (const auto& route : routes)
    synth_.disconnectModulation(route.first, route.second);
  setInternalParameterEngineValue(prefix + "type", 0.0f);
  for (const auto& id : ids) {
    setInternalParameterEngineValue(id, vital::Parameters::getDetails(id.toStdString()).default_value);
  }
  setInternalParameterEngineValue(prefix + "enabled", 1.0f);
  // The slot module's impulse response and Filter FX table, and the MIDI
  // learn mappings onto the slot, belong to the removed effect: left behind
  // they would be saved with every preset and reappear in the next effect.
  if (auto* module = convolutionModuleForPrefix(prefix)) {
    const ScopedLock lock(synth_.getCallbackLock());
    if (module->hasCustomIr())
      module->setDefaultIr();
    module->setLastBrowsedFile({});
  }
  synth_.resetFilterTable((prefix + "filter_fx").toStdString());
  synth_.pauseProcessing(false);
  synth_.clearMidiLearnPrefix(prefix.toStdString());
  if (!routes.empty()) {
    refreshVisibleModulationLabels();
  }
}

void SynthEditor::removeEffectSlot(const String& sectionName, int slot, const String& focusedTitle) {
  if (effect_chain_rebuild_queued_)
    return;

  const auto slots = activeEffectSlots(sectionName);
  if (std::find(slots.begin(), slots.end(), slot) == slots.end())
    return;

  const String removed = effectSlotTitle(sectionName, slot);
  clearEffectSlot(sectionName, slot);
  scheduleEffectChainRebuild(sectionName, focusedTitle);
  postPluginAnnouncement("Removed " + removed, AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::replaceEffectSlot(const String& sectionName, int slot, int effect) {
  if (effect_chain_rebuild_queued_ || !isPositiveAndBelow(effect, vital::constants::kNumEffects))
    return;

  const auto slots = activeEffectSlots(sectionName);
  const auto found = std::find(slots.begin(), slots.end(), slot);
  if (found == slots.end())
    return;

  const int index = static_cast<int>(std::distance(slots.begin(), found));
  const String replaced = effectName(effectSlotType(sectionName, slot) - 1);
  const String slot_prefix = effectSlotPrefix(sectionName, slot);
  clearEffectSlot(sectionName, slot);
  synth_.pauseProcessing(true);
  setInternalParameterEngineValue(slot_prefix + "enabled", 0.0f);
  setInternalParameterEngineValue(slot_prefix + "type", static_cast<float>(effect + 1));
  setInternalParameterEngineValue(slot_prefix + "enabled", 1.0f);
  synth_.pauseProcessing(false);
  scheduleEffectChainRebuild(sectionName, "Slot " + String(index + 1) + " (" + effectName(effect) + ")");
  postPluginAnnouncement("Replaced " + replaced + " with " + effectName(effect),
                         AccessibilityHandler::AnnouncementPriority::high);
}

std::vector<String> SynthEditor::effectChainSectionNames() const {
  std::vector<String> sections { kEffectsChainSection };
  for (int bus = 1; bus <= vital::kNumBuses; ++bus)
    sections.push_back("Bus " + String(bus) + " - Chain and routing");
  return sections;
}

PopupMenu SynthEditor::createEffectSlotContextMenu(const String& sectionName, int slot, const String& focusedTitle) {
  PopupMenu menu;
  const int type = effectSlotType(sectionName, slot);
  if (type <= 0)
    return menu;

  Component::SafePointer<SynthEditor> safe_this(this);
  const String name = effectName(type - 1);
  menu.addItem("Copy " + name, [safe_this, sectionName, slot] {
    if (safe_this != nullptr)
      safe_this->copyEffectSlotToClipboard(sectionName, slot, false);
  });
  menu.addItem("Copy " + name + " with modulations", [safe_this, sectionName, slot] {
    if (safe_this != nullptr)
      safe_this->copyEffectSlotToClipboard(sectionName, slot, true);
  });
  menu.addSeparator();

  const bool can_paste = copied_effect_slot_.type > 0;
  PopupMenu paste_menu;
  if (can_paste) {
    const std::vector<String> sections = effectChainSectionNames();
    for (int target_bus = 0; target_bus < static_cast<int>(sections.size()); ++target_bus) {
      if (target_bus == copied_effect_slot_.source_bus)
        continue;
      const String target_section = sections[static_cast<size_t>(target_bus)];
      paste_menu.addItem(fxBusNameForSection(target_section), firstEmptyEffectSlot(target_section) >= 0, false,
                         [safe_this, target_section, sectionName, focusedTitle] {
        if (safe_this != nullptr)
          safe_this->pasteEffectSlotClipboard(target_section, sectionName, focusedTitle);
      });
    }
  }
  menu.addSubMenu(can_paste ? "Paste " + effectName(copied_effect_slot_.type - 1) + " to" : "Paste to",
                  paste_menu, can_paste);
  menu.addSeparator();

  PopupMenu replace_menu;
  addGroupedEffectItems(replace_menu, type - 1, [safe_this, sectionName, slot](int effect) {
    if (safe_this != nullptr)
      safe_this->replaceEffectSlot(sectionName, slot, effect);
  });
  menu.addSubMenu("Replace " + name + " with", replace_menu);
  menu.addSeparator();
  menu.addItem("Remove " + name, [safe_this, sectionName, slot] {
    if (safe_this != nullptr)
      safe_this->removeEffectSlot(sectionName, slot, "Add effect");
  });
  return menu;
}

void SynthEditor::showEffectSlotContextMenu(const String& sectionName, int slot, Component& target) {
  PopupMenu menu = createEffectSlotContextMenu(sectionName, slot, target.getTitle());
  if (menu.getNumItems() == 0)
    return;
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target));
}

void SynthEditor::copyEffectSlotToClipboard(const String& sectionName, int slot, bool includeModulations) {
  const int type = effectSlotType(sectionName, slot);
  if (type <= 0)
    return;

  const String slot_prefix = effectSlotPrefix(sectionName, slot);
  const String effect_prefix = slot_prefix + strings::kEffectOrder[type - 1] + "_";
  copied_effect_slot_ = {};
  copied_effect_slot_.source_bus = fxBusForSection(sectionName);
  copied_effect_slot_.type = type;
  copied_effect_slot_.include_modulations = includeModulations;

  const auto& controls = synth_.getControls();
  const auto enabled = controls.find((slot_prefix + "enabled").toStdString());
  copied_effect_slot_.enabled = enabled == controls.end() || enabled->second == nullptr ||
                                enabled->second->value() >= 0.5f;
  for (const auto& control : controls) {
    const String id(control.first);
    if (control.second == nullptr || !id.startsWith(effect_prefix) || !vital::Parameters::isParameter(control.first))
      continue;
    copied_effect_slot_.parameters.push_back({ id.substring(slot_prefix.length()), control.second->value() });
  }

  if (type - 1 == vital::constants::kConvolutionReverb) {
    if (auto* module = convolutionModuleForPrefix(slot_prefix)) {
      if (module->hasCustomIr())
        copied_effect_slot_.impulse_response = module->irStateToJson();
    }
  }
  if (type - 1 == vital::constants::kFilterFx) {
    if (WavetableCreator* creator = synth_.getFilterTableCreator((slot_prefix + "filter_fx").toStdString(), false))
      copied_effect_slot_.filter_table = creator->stateToJson();
  }

  if (includeModulations) {
    for (auto* route : synth_.getModulationConnections()) {
      if (route == nullptr || route->source_name.empty() || route->destination_name.empty())
        continue;

      const String destination(route->destination_name);
      if (!destination.startsWith(effect_prefix))
        continue;

      const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
      if (connection < 0)
        continue;

      auto readModulationParameter = [this, connection](const String& suffix) {
        if (auto* bridge = parameterBridge("modulation_" + String(connection + 1) + "_" + suffix))
          return bridge->convertToEngineValue(bridge->getValue());
        return 0.0f;
      };

      CopiedModulation modulation;
      modulation.source_id = String(route->source_name);
      modulation.destination_suffix = destination.substring(slot_prefix.length());
      modulation.amount = readModulationParameter("amount");
      modulation.power = readModulationParameter("power");
      modulation.bipolar = readModulationParameter("bipolar");
      modulation.stereo = readModulationParameter("stereo");
      modulation.bypass = readModulationParameter("bypass");
      modulation.random_mode = readModulationParameter("random_mode");
      copied_effect_slot_.modulations.push_back(modulation);
    }
  }

  String message = "Copied " + effectName(type - 1) + " from " + fxBusNameForSection(sectionName);
  if (includeModulations)
    message += " with modulations";
  postPluginAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::pasteEffectSlotClipboard(const String& targetSection, const String& focusSection,
                                           const String& focusedTitle) {
  const int type = copied_effect_slot_.type;
  if (!isEffectChainSection(targetSection) || type <= 0 || effect_chain_rebuild_queued_)
    return;

  const String name = effectName(type - 1);
  const int slot = firstEmptyEffectSlot(targetSection);
  if (slot < 0) {
    postPluginAnnouncement(fxBusNameForSection(targetSection) + " effects chain full",
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const String slot_prefix = effectSlotPrefix(targetSection, slot);
  synth_.pauseProcessing(true);
  setInternalParameterEngineValue(slot_prefix + "enabled", 0.0f);
  setInternalParameterEngineValue(slot_prefix + "type", static_cast<float>(type));
  for (const auto& parameter : copied_effect_slot_.parameters)
    setInternalParameterEngineValue(slot_prefix + parameter.first, parameter.second);
  if (type - 1 == vital::constants::kConvolutionReverb) {
    // Processing is paused, so no callback lock: the host's audio thread
    // would wait while the impulse response decodes.
    if (auto* module = convolutionModuleForPrefix(slot_prefix)) {
      if (!copied_effect_slot_.impulse_response.is_null())
        module->irStateFromJson(copied_effect_slot_.impulse_response);
      else if (module->hasCustomIr())
        module->setDefaultIr();
      module->setLastBrowsedFile({});
    }
  }
  if (type - 1 == vital::constants::kFilterFx) {
    const std::string table_prefix = (slot_prefix + "filter_fx").toStdString();
    if (copied_effect_slot_.filter_table.is_null())
      synth_.resetFilterTable(table_prefix);
    else if (WavetableCreator* creator = synth_.getFilterTableCreator(table_prefix, true)) {
      try {
        creator->jsonToState(copied_effect_slot_.filter_table);
        creator->render();
      }
      catch (const json::exception&) {
        synth_.resetFilterTable(table_prefix);
      }
    }
  }
  setInternalParameterEngineValue(slot_prefix + "enabled", copied_effect_slot_.enabled ? 1.0f : 0.0f);

  if (copied_effect_slot_.include_modulations) {
    for (const auto& modulation : copied_effect_slot_.modulations) {
      const String destination = slot_prefix + modulation.destination_suffix;
      if (isInvalidModulationPair(modulation.source_id, destination))
        continue;

      synth_.connectModulation(modulation.source_id.toStdString(), destination.toStdString());
      const int connection = synth_.getConnectionIndex(modulation.source_id.toStdString(),
                                                       destination.toStdString());
      if (connection < 0)
        continue;

      const String modulation_prefix = "modulation_" + String(connection + 1) + "_";
      setInternalParameterEngineValue(modulation_prefix + "amount", modulation.amount);
      setInternalParameterEngineValue(modulation_prefix + "power", modulation.power);
      setInternalParameterEngineValue(modulation_prefix + "bipolar", modulation.bipolar);
      setInternalParameterEngineValue(modulation_prefix + "stereo", modulation.stereo);
      setInternalParameterEngineValue(modulation_prefix + "bypass", modulation.bypass);
      setInternalParameterEngineValue(modulation_prefix + "random_mode", modulation.random_mode);
    }
  }
  synth_.pauseProcessing(false);
  refreshVisibleModulationLabels();

  scheduleEffectChainRebuild(focusSection.isNotEmpty() ? focusSection : targetSection, focusedTitle);
  postPluginAnnouncement("Pasted " + name + " to " + fxBusNameForSection(targetSection),
                         AccessibilityHandler::AnnouncementPriority::high);
}

bool SynthEditor::isFxBusEnabled(const String& sectionName) const {
  const auto slots = activeEffectSlots(sectionName);
  if (slots.empty())
    return true;

  const auto& controls = synth_.getControls();
  for (int slot : slots) {
    const auto found = controls.find((effectSlotPrefix(sectionName, slot) + "enabled").toStdString());
    if (found != controls.end() && found->second != nullptr && found->second->value() >= 0.5f)
      return true;
  }
  return false;
}

String SynthEditor::effectChainSummaryText(const String& sectionName) const {
  const auto slots = activeEffectSlots(sectionName);
  if (slots.empty())
    return "Current effects chain: empty";

  String summary = "Current effects chain: ";
  for (size_t i = 0; i < slots.size(); ++i)
    summary += (i == 0 ? "" : ", ") + effectSlotTitle(sectionName, slots[i]);
  return summary;
}

void SynthEditor::refreshEffectChainSummaries() {
  for (auto& component : section_headers_) {
    auto* label = dynamic_cast<Label*>(component.get());
    if (label == nullptr || !label->getProperties().contains("EffectChainSummary"))
      continue;

    const String summary = effectChainSummaryText(label->getProperties()["SectionName"].toString());
    if (label->getText() == summary)
      continue;
    label->setText(summary, dontSendNotification);
    label->setDescription(summary);
    if (auto* handler = label->getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  }
}

void SynthEditor::refreshFxBusToggles() {
  for (auto& toggle : fx_bus_toggles_) {
    if (toggle.second == nullptr)
      continue;
    const bool enabled = isFxBusEnabled(toggle.first);
    if (toggle.second->getToggleState() != enabled)
      toggle.second->setToggleState(enabled, dontSendNotification);
  }
}

void SynthEditor::addEffectToChain(const String& sectionName, int effect, Component& target) {
  if (!isEffectChainSection(sectionName) || !isPositiveAndBelow(effect, vital::constants::kNumEffects))
    return;
  if (effect_chain_rebuild_queued_)
    return;

  const int slot = firstEmptyEffectSlot(sectionName);
  if (slot < 0) {
    postPluginAnnouncement("Effects chain full", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const String slot_prefix = effectSlotPrefix(sectionName, slot);
  synth_.pauseProcessing(true);
  setInternalParameterEngineValue(slot_prefix + "enabled", 0.0f);
  setInternalParameterEngineValue(slot_prefix + "type", static_cast<float>(effect + 1));
  setInternalParameterEngineValue(slot_prefix + "enabled", 1.0f);
  synth_.pauseProcessing(false);
  scheduleEffectChainRebuild(sectionName, "Slot " + String(slot + 1) + " (" + effectName(effect) + ")");
  postPluginAnnouncement("Added " + effectName(effect), AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showAddEffectMenu(const String& sectionName, Component& target) {
  PopupMenu menu;
  menu.addSectionHeader("Add effect");
  addGroupedEffectItems(menu, -1);

  Component::SafePointer<Component> safe_target(&target);
  Component::SafePointer<SynthEditor> safe_this(this);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(target),
                     [safe_this, safe_target, sectionName](int result) {
    if (safe_this == nullptr || safe_target == nullptr || result <= 0)
      return;
    safe_this->addEffectToChain(sectionName, result - 1, *safe_target);
  });
}

bool SynthEditor::isEffectSlotParameter(const String& sectionName, const String& parameterId) const {
  return isEffectChainSection(sectionName) &&
         parameterId.startsWith(effectChainPrefixForSection(sectionName) + "fx_slot_");
}

void SynthEditor::addFxBusControlsGroup(const String& sectionName, Component& parent, int& y, int x,
                                       int width, int& focusOrder) {
  auto group = makeSectionHeader("Bus controls", groupRank(sectionName), false, sectionName);
  auto* group_ptr = group.get();
  group_ptr->setDescription("Effects bus actions for " + fxBusNameForSection(sectionName));
  group_ptr->setExplicitFocusOrder(focusOrder++);
  parent.addAndMakeVisible(group_ptr);
  if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
    accessible_parent->addAccessibleChild(group_ptr);
  row_focus_order_.push_back(group_ptr);
  section_headers_.push_back(std::move(group));

  int group_y = group_ptr->headerHeight() + 8;
  auto add_button = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->getProperties().set("SectionName", sectionName);
    button_ptr->setWantsKeyboardFocus(true);
    button_ptr->setExplicitFocusOrder(focusOrder++);
    button_ptr->onClick = std::move(action);
    button_ptr->setBounds(10, group_y, jmax(200, width - 20), 38);
    group_ptr->addAndMakeVisible(button_ptr);
    group_ptr->addAccessibleChild(button_ptr);
    row_focus_order_.push_back(button_ptr);
    section_headers_.push_back(std::move(button));
    group_y += 44;
  };
  {
    const String title = "Bus enabled";
    auto toggle = std::make_unique<OffscreenToggleButton>();
    auto* toggle_ptr = toggle.get();
    toggle_ptr->setButtonText(title);
    toggle_ptr->setTitle(title);
    toggle_ptr->setDescription("Turn all effects in " + fxBusNameForSection(sectionName) + " on or off");
    toggle_ptr->setHelpText("Press Space or Enter to bypass or enable every effect in " +
                            fxBusNameForSection(sectionName));
    toggle_ptr->getProperties().set("SectionName", sectionName);
    toggle_ptr->getProperties().set("accessibleOnText", "Enabled");
    toggle_ptr->getProperties().set("accessibleOffText", "Bypassed");
    toggle_ptr->setClickingTogglesState(true);
    toggle_ptr->setToggleState(isFxBusEnabled(sectionName), dontSendNotification);
    toggle_ptr->setWantsKeyboardFocus(true);
    toggle_ptr->setExplicitFocusOrder(focusOrder++);
    toggle_ptr->onClick = [this, sectionName, toggle_ptr, title] {
      const bool enabled = toggle_ptr->getToggleState();
      if (effect_chain_rebuild_queued_) {
        toggle_ptr->setToggleState(!enabled, dontSendNotification);
        return;
      }
      synth_.setFxBusEnabled(fxBusForSection(sectionName), enabled);
      scheduleEffectChainRebuild(sectionName, title);
      postPluginAnnouncement(fxBusNameForSection(sectionName) + (enabled ? " enabled" : " bypassed"),
                             AccessibilityHandler::AnnouncementPriority::high);
    };
    toggle_ptr->setBounds(10, group_y, jmax(200, width - 20), 38);
    group_ptr->addAndMakeVisible(toggle_ptr);
    group_ptr->addAccessibleChild(toggle_ptr);
    row_focus_order_.push_back(toggle_ptr);
    fx_bus_toggles_.push_back({ sectionName, toggle_ptr });
    section_headers_.push_back(std::move(toggle));
    group_y += 44;
  }
  add_button("Clear bus", "Remove all effects from " + fxBusNameForSection(sectionName),
              [this, sectionName] {
    if (effect_chain_rebuild_queued_)
      return;
    synth_.clearFxBus(fxBusForSection(sectionName));
    scheduleEffectChainRebuild(sectionName, "Clear bus");
    postPluginAnnouncement(fxBusNameForSection(sectionName) + " cleared",
                           AccessibilityHandler::AnnouncementPriority::high);
  });
  add_button("Load bus FX preset", "Replace this bus's effects with an Atlas FX bus preset",
              [this, sectionName] { chooseFxBusPresetFile(sectionName, false); });
  add_button("Save bus FX preset", "Save this bus's effect order, settings, and impulses as an atfx file",
              [this, sectionName] { chooseFxBusPresetFile(sectionName, true); });
  group_ptr->setBounds(x, y, width, group_y + 4);
  y += group_y + 12;
}

void SynthEditor::addEffectSlotGroups(const String& sectionName, Component& parent, int& y, int x, int width,
                                      int& focusOrder) {
  const auto slots = activeEffectSlots(sectionName);
  for (int position = 0; position < static_cast<int>(slots.size()); ++position) {
    const int slot = slots[static_cast<size_t>(position)];
    const int type = effectSlotType(sectionName, slot);
    if (type <= 0)
      continue;

    const String title = "Slot " + String(position + 1) + " (" + effectName(type - 1) + ")";
    auto slot_component = makeSectionHeader(title, groupRank(sectionName), false);
    auto* slot_ptr = slot_component.get();
    slot_ptr->getProperties().set("SectionName", sectionName);
    slot_ptr->getProperties().set("FxSlot", slot);
    slot_ptr->setExplicitFocusOrder(focusOrder++);
    slot_ptr->onContextMenuCommand = [this, sectionName, slot](Component& target) {
      showEffectSlotContextMenu(sectionName, slot, target);
    };
    slot_ptr->onKeyCommand = [this, sectionName, slot, title, type](const KeyPress& key, Component& target) {
      const bool is_move_key = isEffectMoveEarlierKey(key) || isEffectMoveLaterKey(key);
      const bool is_remove_key = key.getKeyCode() == KeyPress::backspaceKey || key.getKeyCode() == KeyPress::deleteKey;
      if ((is_move_key || is_remove_key) && effect_chain_rebuild_queued_)
        return true;

      const auto slots_now = activeEffectSlots(sectionName);
      auto found = std::find(slots_now.begin(), slots_now.end(), slot);
      if (found == slots_now.end())
        return false;

      const int index = static_cast<int>(std::distance(slots_now.begin(), found));
      int direction = 0;
      if (isEffectMoveEarlierKey(key))
        direction = -1;
      else if (isEffectMoveLaterKey(key))
        direction = 1;
      else if (key.getKeyCode() == KeyPress::backspaceKey || key.getKeyCode() == KeyPress::deleteKey) {
        removeEffectSlot(sectionName, slot, "Add effect");
        return true;
      }
      else {
        return false;
      }

      const int next = index + direction;
      if (!isPositiveAndBelow(next, static_cast<int>(slots_now.size())))
        return true;

      const String moved = effectSlotTitle(sectionName, slot);
      const String moved_title = "Slot " + String(next + 1) + " (" + effectName(type - 1) + ")";
      swapEffectSlots(sectionName, slot, slots_now[static_cast<size_t>(next)]);
      scheduleEffectChainRebuild(sectionName, moved_title);
      postPluginAnnouncement(moved + " moved to position " + String(next + 1),
                                             AccessibilityHandler::AnnouncementPriority::high);
      return true;
    };

    slot_ptr->setBounds(x, y, width, slot_ptr->headerHeight());
    parent.addAndMakeVisible(slot_ptr);
    if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
      accessible_parent->addAccessibleChild(slot_ptr);
    row_focus_order_.push_back(slot_ptr);
    section_headers_.push_back(std::move(slot_component));

    int slot_y = slot_ptr->headerHeight();
    const String slot_prefix = effectSlotPrefix(sectionName, slot);
    const String effect_id = strings::kEffectOrder[type - 1];
    const String effect_prefix = slot_prefix + effect_id + "_";
    const String enabled_id = slot_prefix + "enabled";
    int dimension_mode = 0;
    if (effect_id == "dimension_expander") {
      const auto found = synth_.getControls().find((effect_prefix + "mode").toStdString());
      if (found != synth_.getControls().end()) dimension_mode = found->second->value() >= 0.5f ? 1 : 0;
      effect_slot_modes_[effect_prefix + "mode"] = dimension_mode;
    }
    int resonator_model = 0;
    if (effect_id == "resonator") {
      const auto found = synth_.getControls().find((effect_prefix + "model").toStdString());
      if (found != synth_.getControls().end()) resonator_model = roundToInt(found->second->value());
      effect_slot_modes_[effect_prefix + "model"] = effectSlotRowVariant(effect_prefix + "model", float(resonator_model));
    }
    int analogue_rotary_model = 0;
    if (effect_id == "analoguerotary") {
      const auto found = synth_.getControls().find((effect_prefix + "model").toStdString());
      if (found != synth_.getControls().end()) analogue_rotary_model = roundToInt(found->second->value());
      effect_slot_modes_[effect_prefix + "model"] = effectSlotRowVariant(effect_prefix + "model", float(analogue_rotary_model));
    }
    int analogue_wah_model = 0;
    if (effect_id == "analoguewah") {
      const auto found = synth_.getControls().find((effect_prefix + "model").toStdString());
      if (found != synth_.getControls().end()) analogue_wah_model = roundToInt(found->second->value());
      effect_slot_modes_[effect_prefix + "model"] = effectSlotRowVariant(effect_prefix + "model", float(analogue_wah_model));
    }
    int analogue_echo_medium = 0;
    if (effect_id == "analogueecho") {
      const auto found = synth_.getControls().find((effect_prefix + "medium").toStdString());
      if (found != synth_.getControls().end()) analogue_echo_medium = roundToInt(found->second->value());
      effect_slot_modes_[effect_prefix + "medium"] = effectSlotRowVariant(effect_prefix + "medium", float(analogue_echo_medium));
    }
    int convolution_predelay_synced = 0;
    if (effect_id == "convolution_reverb") {
      const auto found = synth_.getControls().find((effect_prefix + "predelay_sync").toStdString());
      if (found != synth_.getControls().end())
        convolution_predelay_synced = effectSlotRowVariant(effect_prefix + "predelay_sync", found->second->value());
      effect_slot_modes_[effect_prefix + "predelay_sync"] = convolution_predelay_synced;
    }
    int spectral_lock_mode = 0;
    if (effect_id == "spectral_lock") {
      const auto found = synth_.getControls().find((effect_prefix + "mode").toStdString());
      if (found != synth_.getControls().end()) spectral_lock_mode = found->second->value() >= 0.5f ? 1 : 0;
      effect_slot_modes_[effect_prefix + "mode"] = spectral_lock_mode;
    }
    int glitch_delay_tap = 0;
    bool glitch_delay_synced = false;
    if (effect_id == "glitch_delay") {
      const auto tap = synth_.getControls().find((effect_prefix + "tap_view").toStdString());
      if (tap != synth_.getControls().end()) glitch_delay_tap = jlimit(0, 3, roundToInt(tap->second->value()));
      effect_slot_modes_[effect_prefix + "tap_view"] = effectSlotRowVariant(effect_prefix + "tap_view", float(glitch_delay_tap));
      const auto sync = synth_.getControls().find((effect_prefix + "sync").toStdString());
      glitch_delay_synced = sync != synth_.getControls().end() && sync->second->value() >= 0.5f;
      effect_slot_modes_[effect_prefix + "sync"] = glitch_delay_synced ? 1 : 0;
    }
    int analogue_chorus_model = 0;
    if (effect_id == "analoguechorus") {
      const auto found = synth_.getControls().find((effect_prefix + "model").toStdString());
      if (found != synth_.getControls().end()) analogue_chorus_model = roundToInt(found->second->value());
      effect_slot_modes_[effect_prefix + "model"] = effectSlotRowVariant(effect_prefix + "model", float(analogue_chorus_model));
    }

    struct EffectParameterEntry {
      AudioProcessorParameter* parameter = nullptr;
      String id;
      String suffix;
    };
    AudioProcessorParameter* enabled_parameter = nullptr;
    std::vector<EffectParameterEntry> effect_parameters;
    for (auto* parameter : sections_[sectionName]) {
      auto* bridge = dynamic_cast<ValueBridge*>(parameter);
      if (bridge == nullptr)
        continue;

      const String id = bridge->getParameterId();
      if (id == enabled_id) {
        enabled_parameter = parameter;
        continue;
      }
      if (!id.startsWith(effect_prefix))
        continue;

      const String suffix = id.substring(effect_prefix.length());
      if (effect_id == "dimension_expander" && suffix != "mode" && suffix != "on" &&
          (suffix.startsWith("quorax_") != (dimension_mode == 1)))
        continue;
      if (effect_id == "resonator" && !isResonatorParameterRelevant(suffix, resonator_model))
        continue;
      if (effect_id == "analoguechorus" && !isAnalogueChorusParameterRelevant(suffix, analogue_chorus_model))
        continue;
      if (effect_id == "analogueecho" && !isAnalogueEchoParameterRelevant(suffix, analogue_echo_medium))
        continue;
      if (effect_id == "analoguerotary" && !isAnalogueRotaryParameterRelevant(suffix, analogue_rotary_model))
        continue;
      if (effect_id == "analoguewah" && !isAnalogueWahParameterRelevant(suffix, analogue_wah_model))
        continue;
      if (effect_id == "spectral_lock" && !isSpectralLockParameterRelevant(suffix, spectral_lock_mode))
        continue;
      if (effect_id == "convolution_reverb" && !isConvolutionReverbParameterRelevant(suffix, convolution_predelay_synced))
        continue;
      if (effect_id == "glitch_delay" && !isGlitchDelayParameterRelevant(suffix, glitch_delay_tap, glitch_delay_synced))
        continue;
      if (shouldExposeEffectSlotParameter(effect_id, suffix) &&
          shouldShowParameterInSection(sectionName, parameter))
        effect_parameters.push_back({ parameter, id, suffix });
    }
    std::stable_sort(effect_parameters.begin(), effect_parameters.end(), [&effect_id](const auto& a, const auto& b) {
      const int rank_a = effectSlotParameterRank(effect_id, a.suffix);
      const int rank_b = effectSlotParameterRank(effect_id, b.suffix);
      if (rank_a != rank_b)
        return rank_a < rank_b;
      return false;
    });

    auto add_parameter_row = [&](AudioProcessorParameter& parameter, Component& row_parent,
                                 AccessibleSectionHeader& accessible_parent, int& row_y, int row_width,
                                 bool floaty_mode = false) {
      auto* parameter_bridge = dynamic_cast<ValueBridge*>(&parameter);
      auto row = floaty_mode
          ? std::make_unique<AccessibleParameterRow>(parameter, "Time Sync", std::function<String(double)>(),
                1.0, 0.0, "Free", "Sync", "Choose Free time in milliseconds or Sync to the host tempo", false, true)
          : createAccessibleParameterRow(parameter, sectionName);
      if (floaty_mode) {
        const String id = parameter_bridge->getParameterId();
        floaty_sync_durations_.try_emplace(id, 8.0f / 64.0f); // Quarter note when first enabling sync.
        row->setToggleMapping([this, parameter_bridge, id] {
          const float value = parameter_bridge->getValue();
          if (value > 0) floaty_sync_durations_[id] = value;
          return value > 0;
        }, [this, parameter_bridge, id](bool on) {
          const float value = parameter_bridge->getValue();
          if (value > 0) floaty_sync_durations_[id] = value;
          return on ? floaty_sync_durations_.at(id) : 0.0f;
        });
      }
      row->setModulationSourceSubmenuCallback([this](const String& destinationId,
                                                     std::map<int, String>& choices, int firstItemId) {
        return createModulationSourceSubmenu(destinationId, choices, firstItemId);
      });
      row->setModulationAssignCallback([this](const String& sourceId, const String& destinationId,
                                              Component& target) {
        connectModulationAndPromptForAmount(sourceId, destinationId, target);
      });
      row->setModulationEditCallback([this](const String& sourceId, const String& destinationId,
                                            Component& target) {
        promptForInitialModulationAmount(sourceId, destinationId, false, target);
      });
      row->setModulationDestinationPredicate([this](const String& id) { return isModulationDestinationId(id); });
      row->setModulationRemovalCallbacks(
          [this](const String& destinationId) { return modulationSourcesForDestination(destinationId); },
          [this](const String& sourceId, const String& destinationId, Component& target) {
            removeModulationFromParameter(sourceId, destinationId, target);
          });
      row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
        if (clear) {
          synth_.clearMidiLearn(parameterId.toStdString());
          postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        }
        else {
          armOrCancelMidiLearn(parameterId);
        }
      });
      row->setValueEntryCallback([this](const String& parameterId, Component& target) {
        promptForParameterValue(parameterId, target);
      });
      row->setExtraCommandCallback([this, sectionName](const String& parameterId, const KeyPress& key,
                                                       Component& target) {
        return handleMacroShortcut(parameterId, key, target) ||
               handleEffectShortcut(sectionName, key, target);
      });
      setAutomationProxyCallbacks(*row);

      auto* bridge = dynamic_cast<ValueBridge*>(&parameter);
      const String id = bridge == nullptr ? String() : bridge->getParameterId();
      const auto& details = vital::Parameters::getDetails(id.toStdString());
      String local_name = details.local_description.empty()
          ? modulationDestinationLabelForId(id) : String(details.local_description);
      // The slot header already announces the effect, so a long effect name
      // on every row is just noise for screen readers.
      const String effect_name_prefix = effectName(type - 1) + " ";
      if (local_name.startsWith(effect_name_prefix) && !id.endsWith("suppressor_amount"))
        local_name = local_name.fromFirstOccurrenceOf(effect_name_prefix, false, false);
      const String filter_suffix = filterSuffixForParameter(id);
      if (filter_suffix != "model" && filter_suffix != "style" && filter_suffix != "blend")
        row->setAccessibleName(floaty_mode ? "Time Sync" : id == enabled_id ? "Enabled" : local_name);
      row->setExplicitFocusOrder(focusOrder++);
      row->setControlFocusOrder(focusOrder++);
      row->setBounds(10, row_y, jmax(580, row_width - 20), 48);
      row_parent.addAndMakeVisible(row.get());
      accessible_parent.addAccessibleChild(row->focusableControl());
      row_focus_order_.push_back(row->focusableControl());
      rows_.push_back(std::move(row));
      row_y += 48;
    };

    // Every inserted effect begins with its one functional bypass control.
    if (enabled_parameter != nullptr)
      add_parameter_row(*enabled_parameter, *slot_ptr, *slot_ptr, slot_y, width);

    if (type - 1 == vital::constants::kConvolutionReverb) {
      const String chain_prefix = effectChainPrefixForSection(sectionName);
      const String module_prefix = chain_prefix + "fx_slot_" + String(slot + 1) + "_";
      auto* module = convolutionModuleForPrefix(module_prefix);
      const bool custom_ir = module != nullptr && module->hasCustomIr() && !module->irName().empty();
      const String browse_title = custom_ir
          ? "Browse impulse response folders, current: " + String(module->irName())
          : "Browse impulse response folders";

      auto ir_group = makeSectionHeader(
          "Impulse response controls", groupRank(sectionName), false,
          sectionName + " slot " + String(slot + 1) + " impulse response controls");
      auto* ir_group_ptr = ir_group.get();
      ir_group_ptr->setDescription(custom_ir
          ? "Impulse response loading, browsing, navigation, editing, and saving. Current: " + String(module->irName())
          : "Impulse response loading, browsing, navigation, editing, and saving");
      ir_group_ptr->setExplicitFocusOrder(focusOrder++);
      const int ir_width = jmax(600, width - 20);
      ir_group_ptr->setBounds(10, slot_y, ir_width, ir_group_ptr->headerHeight());
      slot_ptr->addAndMakeVisible(ir_group_ptr);
      slot_ptr->addAccessibleChild(ir_group_ptr);
      row_focus_order_.push_back(ir_group_ptr);
      section_headers_.push_back(std::move(ir_group));

      int ir_y = ir_group_ptr->headerHeight() + 8;

      auto addIrActionButton = [&](const String& button_title, const String& description,
                                   std::function<void()> action) {
        auto button = std::make_unique<OffscreenTextButton>(button_title);
        auto* button_ptr = button.get();
        button_ptr->setTitle(button_title);
        button_ptr->setDescription(description);
        button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
        button_ptr->setWantsKeyboardFocus(true);
        button_ptr->onClick = std::move(action);
        button_ptr->setExplicitFocusOrder(focusOrder++);
        button_ptr->setBounds(10, ir_y, jmax(580, ir_width - 20), 38);
        ir_group_ptr->addAndMakeVisible(button_ptr);
        ir_group_ptr->addAccessibleChild(button_ptr);
        row_focus_order_.push_back(button_ptr);
        section_headers_.push_back(std::move(button));
        ir_y += 44;
        return button_ptr;
      };

      addIrActionButton("Load impulse response file",
                        "Load an impulse response from an audio or " + String(vital::kIrExtension) + " file",
                        [this, module_prefix] { chooseConvolutionIrFile(module_prefix); });
      auto* browse_button = addIrActionButton(browse_title,
                        "Choose an impulse response from the Impulse Responses folders",
                        [this, module_prefix, ir_group_ptr] {
                          showConvolutionIrBrowserMenu(module_prefix, *ir_group_ptr);
                        });
      ir_controls_[module_prefix] = { ir_group_ptr, browse_button };
      if (!ir_browser_cache_valid_) refreshIrBrowserCache();
      addIrActionButton("Previous impulse response",
                        "Load the previous impulse response from the folders",
                        [this, module_prefix] { loadShiftedConvolutionIr(module_prefix, -1); });
      addIrActionButton("Next impulse response",
                        "Load the next impulse response from the folders",
                        [this, module_prefix] { loadShiftedConvolutionIr(module_prefix, 1); });
      addIrActionButton("Save impulse response",
                        "Save the current impulse response as an " + String(vital::kIrExtension) + " file",
                        [this, module_prefix] { saveConvolutionIrFile(module_prefix); });

      for (const auto& entry : effect_parameters) {
        if (entry.suffix.startsWith("ir_") || entry.suffix.startsWith("fade_"))
          add_parameter_row(*entry.parameter, *ir_group_ptr, *ir_group_ptr, ir_y, ir_width);
      }

      ir_group_ptr->setBounds(10, slot_y, ir_width, ir_y);
      slot_y += ir_y + 8;
    }
    if (effect_id == "filter_fx" && filterModelForPrefix(slot_prefix + "filter_fx") == vital::constants::kFilterTable) {
      int group_y = slot_y;
      addFilterTableControlsGroup(slot_prefix + "filter_fx", *slot_ptr, group_y, 10, jmax(600, width - 20), focusOrder,
                                  sectionName + " slot " + String(slot + 1));
      slot_y = group_y;
    }

    // Some effects nest their rows in groups. Each group lists its rows by
    // suffix, then its subgroups; a group with no rows to show is left out.
    struct RowGroup {
      String title, description;
      StringArray suffixes;
      std::vector<RowGroup> children;
    };
    std::map<String, AudioProcessorParameter*> by_suffix;
    for (const auto& entry : effect_parameters)
      by_suffix[entry.suffix] = entry.parameter;

    const String group_key = sectionName + " slot " + String(slot + 1) + " " + effectName(type - 1).toLowerCase() + " ";
    const auto has_rows = [&by_suffix](const RowGroup& group, const auto& self) -> bool {
      for (const auto& suffix : group.suffixes)
        if (by_suffix.count(suffix)) return true;
      for (const auto& child : group.children)
        if (self(child, self)) return true;
      return false;
    };
    const auto place_group = [&](const RowGroup& group, AccessibleSectionHeader& parent, int& parent_y,
                                 int group_width, const String& key, const auto& self) -> void {
      if (!has_rows(group, has_rows))
        return;
      const String group_id = key + group.title.toLowerCase();
      auto header = makeSectionHeader(group.title, groupRank(sectionName), false, group_id);
      auto* header_ptr = header.get();
      header_ptr->setDescription(group.description);
      header_ptr->setExplicitFocusOrder(focusOrder++);
      header_ptr->setBounds(10, parent_y, group_width, header_ptr->headerHeight());
      parent.addAndMakeVisible(header_ptr);
      parent.addAccessibleChild(header_ptr);
      row_focus_order_.push_back(header_ptr);
      section_headers_.push_back(std::move(header));

      int inner_y = header_ptr->headerHeight() + 8;
      for (const auto& suffix : group.suffixes) {
        const auto found = by_suffix.find(suffix);
        if (found != by_suffix.end())
          add_parameter_row(*found->second, *header_ptr, *header_ptr, inner_y, group_width);
      }
      for (const auto& child : group.children)
        self(child, *header_ptr, inner_y, jmax(600, group_width - 20), group_id + " ", self);
      header_ptr->setBounds(10, parent_y, group_width, inner_y + 2);
      parent_y += inner_y + 10;
    };
    const int group_width = jmax(600, width - 20);
    bool grouped_rows = false;

    // Glitch Delay nests its rows as Astralay groups them: Global (timing, tape
    // stop, glitch engine, output), then the shown tap's delay controls and its
    // glitches, one group per glitch type.
    if (effect_id == "glitch_delay") {
      const String tap = "tap_" + String(glitch_delay_tap + 1) + "_";
      const auto tap_rows = [&tap](std::initializer_list<const char*> controls) {
        StringArray result;
        for (const auto* control : controls)
          result.add(tap + control);
        return result;
      };
      const RowGroup global { "Global", "Timing, tape stop, glitch engine and output, shared by every tap", {}, {
        { "Timing", "Host sync, glide and freeze", { "sync", "glide", "freeze", "freeze_sustain" }, {} },
        { "Tape stop", "Slowing the repeats to a halt and back", { "stop_tape", "stop_time", "start_time" }, {} },
        { "Glitch engine", "Where glitches act, the chunk grid they start on, how long they last and their randomness",
          { "placement", "buffer_size", "buffer_sync", "max_glitches", "length_min", "length_max", "reproducible", "seed" }, {} },
        { "Output", "Smear, mix and output level", { "smear_amount", "smear_size", "mix", "output_gain" }, {} } } };
      const RowGroup tap_group { "Tap " + String(glitch_delay_tap + 1),
                                 "Delay and glitch controls for tap " + String(glitch_delay_tap + 1), {}, {
        { "Delay", "On, time, level, pan, feedback and the feedback filters",
          tap_rows({ "enabled", "time", "time_sync", "volume", "pan", "feedback", "low_cut", "high_cut" }), {} },
        { "Glitches", "Nine glitch types, each with its own probability and ranges", {}, {
          { "Reverse", "Plays the audio backwards", tap_rows({ "reverse_prob" }), {} },
          { "Stutter", "Repeats a short slice",
            tap_rows({ "stutter_prob", "stutter_min", "stutter_max", "stutter_sync_min", "stutter_sync_max" }), {} },
          { "Granularize", "Overlapping grains",
            tap_rows({ "grain_prob", "grain_size_min", "grain_size_max", "grain_density_min", "grain_density_max" }), {} },
          { "Pitch", "Pitch sweeps or varispeed",
            tap_rows({ "pitch_prob", "pitch_mode", "pitch_min", "pitch_max", "pitch_speed_min", "pitch_speed_max" }), {} },
          { "LPC formant", "Formants moved with linear prediction", tap_rows({ "lpc_prob", "lpc_min", "lpc_max" }), {} },
          { "Cepstral formant", "Formants moved with a cepstral envelope", tap_rows({ "ceps_prob", "ceps_min", "ceps_max" }), {} },
          { "Ring modulation", "Ring modulation", tap_rows({ "ring_prob", "ring_min", "ring_max" }), {} },
          { "Frequency modulation", "Frequency modulation around the detected pitch",
            tap_rows({ "fm_prob", "fm_ratio_min", "fm_ratio_max", "fm_index_min", "fm_index_max" }), {} },
          { "Bit crusher", "Bit depth and sample rate reduction",
            tap_rows({ "crush_prob", "crush_bits_min", "crush_bits_max", "crush_rate_min", "crush_rate_max" }), {} } } } } };

      place_group(global, *slot_ptr, slot_y, group_width, group_key, place_group);
      if (const auto view = by_suffix.find("tap_view"); view != by_suffix.end())
        add_parameter_row(*view->second, *slot_ptr, *slot_ptr, slot_y, width);
      place_group(tap_group, *slot_ptr, slot_y, group_width, group_key, place_group);
      grouped_rows = true;
    }

    // The EQ gives each band its own group. Mid Side EQ has the same six bands
    // for each channel, so they sit in a Mid group and a Side group.
    if (effect_id == "eq" || effect_id == "mid_side_eq") {
      const auto equalizer_bands = [](const String& channel) {
        const auto band = [&channel](const String& title, const String& modes, const String& band_prefix) {
          StringArray suffixes;
          for (const auto* control : { "mode", "cutoff", "gain", "resonance" })
            suffixes.add(channel + band_prefix + control);
          return RowGroup { title, modes + ", with cutoff, gain and resonance", suffixes, {} };
        };
        return std::vector<RowGroup> {
          band("Low band", "Shelf or high pass", "low_"),
          band("Band 1", "Shelf or notch", "band_"),
          band("Band 2", "Shelf or notch", "band_2_"),
          band("Band 3", "Shelf or notch", "band_3_"),
          band("Band 4", "Shelf or notch", "band_4_"),
          band("High band", "Shelf or low pass", "high_") };
      };
      const std::vector<RowGroup> groups = effect_id == "eq" ? equalizer_bands("") : std::vector<RowGroup> {
        { "Mid", "The six bands for the mid channel, what the left and right channels share", {},
          equalizer_bands("mid_eq_") },
        { "Side", "The six bands for the side channel, the difference between left and right", {},
          equalizer_bands("side_eq_") } };
      for (const auto& group : groups)
        place_group(group, *slot_ptr, slot_y, group_width, group_key, place_group);
      grouped_rows = true;
    }

    // The compressor keeps its global controls at the top of the slot, then
    // gives each band its own group. The crossovers sit at 120 Hz and 2.5 kHz;
    // Enabled Bands decides whether the Low and High bands run, and the Mid
    // band takes whatever they leave.
    if (effect_id == "compressor") {
      for (const auto* control : { "enabled_bands", "attack", "release", "mix" }) {
        if (const auto found = by_suffix.find(control); found != by_suffix.end())
          add_parameter_row(*found->second, *slot_ptr, *slot_ptr, slot_y, width);
      }
      const auto band = [](const String& title, const String& range, const String& band_prefix) {
        StringArray suffixes;
        for (const auto* control : { "upper_threshold", "upper_ratio", "lower_threshold", "lower_ratio", "gain" })
          suffixes.add(band_prefix + control);
        return RowGroup { title, range + ", with upper and lower thresholds and ratios, and gain", suffixes, {} };
      };
      for (const auto& group : { band("Low band", "Below 120 Hz, in Multiband and Low Band modes", "low_"),
                                 band("Mid band", "Always on: 120 Hz to 2.5 kHz in Multiband mode, everything above 120 Hz "
                                                  "in Low Band mode, everything below 2.5 kHz in High Band mode, and the "
                                                  "whole signal in Single Band mode", "band_"),
                                 band("High band", "Above 2.5 kHz, in Multiband and High Band modes", "high_") })
        place_group(group, *slot_ptr, slot_y, group_width, group_key, place_group);
      grouped_rows = true;
    }

    for (const auto& entry : effect_parameters) {
      if (grouped_rows)
        break;
      if (type - 1 == vital::constants::kConvolutionReverb &&
          (entry.suffix.startsWith("ir_") || entry.suffix.startsWith("fade_")))
        continue;
      // Two views of the original parameter: 0 is Free, 1..64 are durations.
      // This preserves existing presets, automation and the original JSFX DSP.
      if (effect_id == "floaty_delay" && entry.suffix == "sync")
        add_parameter_row(*entry.parameter, *slot_ptr, *slot_ptr, slot_y, width, true);
      add_parameter_row(*entry.parameter, *slot_ptr, *slot_ptr, slot_y, width);
    }

    if (effect_id == "trance_gate")
      addTranceGateEditor(effect_prefix, sectionName, *slot_ptr, slot_y, 10, width - 20, focusOrder);
    slot_ptr->setBounds(x, y, width, slot_y);
    y += slot_y + 8;
  }
}

void SynthEditor::addTranceGateEditor(const String& prefix, const String& section, Component& parent,
                                      int& y, int x, int width, int& focusOrder) {
  auto editor = std::make_unique<TranceGateEditor>(trance_gate_editor_states_[prefix]);
  auto* keyboard = editor.get();
  keyboard->getProperties().set("TranceGatePrefix", prefix);
  keyboard->getProperties().set("SectionName", section);
  keyboard->read = [this, prefix](const String& suffix) {
    const auto found = synth_.getControls().find((prefix + suffix).toStdString());
    return found == synth_.getControls().end() ? 0.0f : found->second->value();
  };
  keyboard->write = [this, prefix](const TranceGateEditor::Changes& changes) {
    synth_.pauseProcessing(true);
    for (const auto& change : changes) setParameterEngineValue(prefix + change.first, change.second);
    synth_.pauseProcessing(false);
    for (auto& row : rows_) row->refresh();
  };
  keyboard->announce = [](const String& message) { postLfoAnnouncement(message); };
  keyboard->setExplicitFocusOrder(focusOrder++);
  keyboard->setBounds(x, y, width, 82);
  parent.addAndMakeVisible(keyboard);
  if (auto* group = dynamic_cast<AccessibleSectionHeader*>(&parent)) group->addAccessibleChild(keyboard);
  row_focus_order_.push_back(keyboard);
  section_headers_.push_back(std::move(editor));
  y += 90;
}

void SynthEditor::setAutomationProxyCallbacks(AccessibleParameterRow& row) {
  row.setFxAutomationCallbacks(
      [this](const String& parameterId) {
        return synth_.fxAutomationSlotNameForParameter(parameterId.toStdString());
      },
      [this](const String& parameterId, bool remove) {
        const String parameter_name = modulationDestinationLabelForId(parameterId);
        if (remove) {
          const String slot_name = synth_.fxAutomationSlotNameForParameter(parameterId.toStdString());
          if (synth_.removeFxParameterAutomation(parameterId.toStdString()))
            postPluginAnnouncement(parameter_name + " removed from " + slot_name,
                                   AccessibilityHandler::AnnouncementPriority::high);
          return;
        }

        if (synth_.assignFxParameterToAutomationSlot(parameterId.toStdString()) < 0) {
          postPluginAnnouncement(sourceSlotForId(parameterId) >= 0 ? "No free automation slots for this oscillator"
                                                                   : "No free automation slots in this effects chain",
                                 AccessibilityHandler::AnnouncementPriority::high);
          return;
        }
        const String slot_name = synth_.fxAutomationSlotNameForParameter(parameterId.toStdString());
        postPluginAnnouncement(parameter_name + " assigned to " + slot_name,
                               AccessibilityHandler::AnnouncementPriority::high);
      },
      [this](const String& parameterId, bool begin) {
        if (begin)
          synth_.beginFxAutomationGesture(parameterId.toStdString());
        else
          synth_.endFxAutomationGesture(parameterId.toStdString());
      });
}

void SynthEditor::readEffectChainOrder(const String& sectionName, int* order) const {
  const String prefix = effectChainPrefixForSection(sectionName);
  bool slots_are_default = true;
  bool slots_are_valid = true;
  bool seen[vital::constants::kNumEffects] = {};

  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    const String slot_id = prefix + "effect_chain_slot_" + String(i + 1);
    auto* slot = parameterBridge(slot_id);
    if (slot == nullptr) {
      slots_are_valid = false;
      break;
    }

    const int effect = jlimit(0, vital::constants::kNumEffects - 1,
                              static_cast<int>(std::round(slot->convertToEngineValue(slot->getValue()))));
    order[i] = effect;
    slots_are_default = slots_are_default && effect == i;
    if (seen[effect])
      slots_are_valid = false;
    seen[effect] = true;
  }

  if (slots_are_valid && !slots_are_default)
    return;

  for (int i = 0; i < vital::constants::kNumEffects; ++i)
    order[i] = i;

  if (auto* bridge = parameterBridge(prefix + "effect_chain_order")) {
    const float max_order = vital::utils::factorial(vital::constants::kNumReorderableEffects) - 1;
    const float chain_order = jlimit(0.0f, max_order, std::round(bridge->convertToEngineValue(bridge->getValue())));
    vital::utils::decodeFloatToOrder(order, chain_order, vital::constants::kNumReorderableEffects);
  }

  if (auto* post_bridge = parameterBridge(prefix + "post_effect_order")) {
    if (post_bridge->convertToEngineValue(post_bridge->getValue()) >= 0.5f)
      std::swap(order[vital::constants::kFrequencyShifter], order[vital::constants::kLimiter]);
  }
}

void SynthEditor::writeEffectChainOrder(const String& sectionName, const int* order) {
  synth_.pauseProcessing(true);
  const String prefix = effectChainPrefixForSection(sectionName);
  bool legacy_seen[vital::constants::kNumReorderableEffects] = {};
  bool can_sync_legacy_order = true;
  for (int i = 0; i < vital::constants::kNumReorderableEffects; ++i) {
    if (!isPositiveAndBelow(order[i], vital::constants::kNumReorderableEffects) || legacy_seen[order[i]]) {
      can_sync_legacy_order = false;
      break;
    }
    legacy_seen[order[i]] = true;
  }
  for (int i = vital::constants::kNumReorderableEffects; i < vital::constants::kNumEffects; ++i)
    can_sync_legacy_order = can_sync_legacy_order && order[i] == i;

  if (can_sync_legacy_order) {
    int legacy_order[vital::constants::kNumReorderableEffects];
    for (int i = 0; i < vital::constants::kNumReorderableEffects; ++i)
      legacy_order[i] = order[i];
    const float encoded_order = vital::utils::encodeOrderToFloat(legacy_order,
                                                                 vital::constants::kNumReorderableEffects);
    if (auto* chain_order = parameterBridge(prefix + "effect_chain_order")) {
      if (chain_order->convertToEngineValue(chain_order->getValue()) != encoded_order)
        setParameterEngineValue(prefix + "effect_chain_order", encoded_order);
    }
  }

  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    const String slot_id = prefix + "effect_chain_slot_" + String(i + 1);
    auto* slot = parameterBridge(slot_id);
    if (slot == nullptr)
      continue;

    const int current = static_cast<int>(std::round(slot->convertToEngineValue(slot->getValue())));
    if (current != order[i])
      setParameterEngineValue(slot_id, static_cast<float>(order[i]));
  }

  if (auto* post_order = parameterBridge(prefix + "post_effect_order")) {
    if (post_order->convertToEngineValue(post_order->getValue()) != 0.0f)
      setParameterEngineValue(prefix + "post_effect_order", 0.0f);
  }
  synth_.pauseProcessing(false);
}

void SynthEditor::scheduleEffectChainRebuild(const String& preferredSection, const String& focusedTitle,
                                             const String& focusedParameter) {
  if (effect_chain_rebuild_queued_) {
    // Coalesce into the queued rebuild. The first explicit focus target wins:
    // a slot move names the control it wants focused, and a timer tick that
    // notices the moved slot's variant a moment later only knows the stale
    // title of the control the move is about to destroy.
    const bool queued_explicit = queued_effect_chain_rebuild_title_.isNotEmpty() ||
                                 queued_effect_chain_rebuild_parameter_.isNotEmpty();
    if (!queued_explicit) {
      queued_effect_chain_rebuild_title_ = focusedTitle;
      queued_effect_chain_rebuild_parameter_ = focusedParameter;
    }
    if (queued_effect_chain_rebuild_section_.isEmpty())
      queued_effect_chain_rebuild_section_ = preferredSection;
    // If the queued rebuild has already run, this request needs one of its own.
    queued_effect_chain_rebuild_pending_ = true;
    return;
  }

  queued_effect_chain_rebuild_section_ = preferredSection;
  queued_effect_chain_rebuild_title_ = focusedTitle;
  queued_effect_chain_rebuild_parameter_ = focusedParameter;
  queued_effect_chain_rebuild_pending_ = false;
  effect_chain_rebuild_queued_ = true;
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this] {
    if (safe_this == nullptr)
      return;

    const String section = safe_this->queued_effect_chain_rebuild_section_;
    const String title = safe_this->queued_effect_chain_rebuild_title_;
    const String parameter = safe_this->queued_effect_chain_rebuild_parameter_;
    safe_this->queued_effect_chain_rebuild_section_.clear();
    safe_this->queued_effect_chain_rebuild_title_.clear();
    safe_this->queued_effect_chain_rebuild_parameter_.clear();
    safe_this->queued_effect_chain_rebuild_pending_ = false;

    safe_this->rebuildSectionsAfterEffectOrderChange(section, title, parameter);
    // The flag clears one message later so key repeats cannot start another
    // move before focus has landed. A request that arrived in between was
    // stored, not posted; give it its own rebuild now.
    MessageManager::callAsync([safe_this] {
      if (safe_this == nullptr)
        return;
      safe_this->effect_chain_rebuild_queued_ = false;
      if (safe_this->queued_effect_chain_rebuild_pending_) {
        safe_this->queued_effect_chain_rebuild_pending_ = false;
        safe_this->scheduleEffectChainRebuild(safe_this->queued_effect_chain_rebuild_section_,
                                              safe_this->queued_effect_chain_rebuild_title_,
                                              safe_this->queued_effect_chain_rebuild_parameter_);
      }
    });
  });
}

void SynthEditor::rebuildSectionsAfterEffectOrderChange(const String& preferredSection, const String& focusedTitle,
                                                        const String& focusedParameter) {
  const String section = preferredSection.isNotEmpty() ? preferredSection : last_section_name_;
  // An explicit title names the control that must end up focused (a button
  // whose action caused the rebuild); only without one do we fall back to
  // whatever is focused now, which may be the window itself mid-action.
  const bool explicit_target = focusedTitle.isNotEmpty() || focusedParameter.isNotEmpty();
  const String accessible_title = focusedTitle.isNotEmpty() ? focusedTitle : focusedAccessibleTitle();
  const String parameter_id = focusedParameter.isNotEmpty() ? focusedParameter
                            : (focusedTitle.isNotEmpty() ? String() : focusedParameterId());
  auto* persistent_focus = explicit_target ? nullptr : persistentFocusedComponent();
  // An effect's mode changed: keep its focused choice through the rebuild.
  if (std::exchange(keep_switching_row_on_rebuild_, false))
    keepFocusedSwitchingRow();
  buildSections();

  showAllSections(false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title, section);
  releaseKeptFocus();
}

void SynthEditor::connectModulationAndPromptForAmount(const String& sourceId, const String& destinationId,
                                                      Component& target) {
  if (sourceId.isEmpty() || destinationId.isEmpty())
    return;
  if (arpeggiatorForId(destinationId) > 0 && !sourceFitsArpeggiator(sourceId)) {
    postPluginAnnouncement(modulationSourceLabelForId(sourceId) +
                           " cannot modulate the arpeggiator: every step it plays is a new note",
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  if (isInvalidModulationPair(sourceId, destinationId)) {
    postPluginAnnouncement(modulationSourceLabelForId(sourceId) + " cannot modulate its own controls",
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  if (const int slot = vital::source_types::slotForOutputSource(sourceId.toStdString());
      slot >= 0 && !synth_.acceptsOscillatorModulation(slot, destinationId.toStdString())) {
    postPluginAnnouncement(modulationSourceLabelForId(sourceId) + " can only modulate audio-rate controls",
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const bool created = synth_.connectModulation(sourceId.toStdString(), destinationId.toStdString());
  const int connection = synth_.getConnectionIndex(sourceId.toStdString(), destinationId.toStdString());
  if (connection < 0) {
    // Every modulation slot is taken, or the engine refused the route.
    postPluginAnnouncement("No free modulation slot: remove a modulation first",
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  if (created && connection >= 0) {
    const String prefix = "modulation_" + String(connection + 1) + "_";
    setParameterEngineValue(prefix + "amount", 0.0f);
    setParameterEngineValue(prefix + "power", 0.0f);
    setParameterEngineValue(prefix + "bipolar",
                            vital::ModulationConnection::isModulationSourceDefaultBipolar(
                                sourceId.toStdString()) ? 1.0f : 0.0f);
    setParameterEngineValue(prefix + "stereo", 0.0f);
    setParameterEngineValue(prefix + "bypass", 0.0f);
    setParameterEngineValue(prefix + "random_mode", 0.0f);
  }
  const int macro_index = macroIndexForControlId(sourceId);
  if (macro_index >= 0) {
    if (connection >= 0)
      setParameterEngineValue("modulation_" + String(connection + 1) + "_bipolar",
                              isMacroBipolar(macro_index) ? 1.0f : 0.0f);
  }
  Component::SafePointer<Component> safe_target(&target);
  refreshVisibleModulationLabels();
  Component& prompt_focus = safe_target != nullptr ? *safe_target.getComponent() : static_cast<Component&>(*this);
  promptForInitialModulationAmount(sourceId, destinationId, created, prompt_focus);
}

void SynthEditor::promptForInitialModulationAmount(const String& sourceId, const String& destinationId,
                                                   bool created, Component& target) {
  parameter_value_prompt_visible_ = false;
  pending_parameter_value_id_.clear();
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  rememberPromptFocus(target);
  pending_modulation_source_ = sourceId;
  pending_modulation_destination_ = destinationId;
  pending_modulation_created_ = created;
  modulation_amount_prompt_visible_ = true;
  modulation_amount_prompt_.setText("Amount for " + modulationSourceLabelForId(sourceId) + " to " +
                                    modulationDestinationLabelForId(destinationId) +
                                    ": the value it moves to", dontSendNotification);
  const String reach_help = "Type the value the destination moves to from where it is now, such as 12 semitones, 50%, "
                            "1000 Hz or C4. A unipolar source moves all of that distance; a bipolar source, such as "
                            "an LFO, moves half of it each way. Type raw followed by a percentage, such as raw 25%, "
                            "to set the amount itself, from -100% to 100%.";
  modulation_amount_prompt_.setTitle("Initial modulation amount");
  modulation_amount_prompt_.setDescription(reach_help);
  modulation_amount_editor_.setTitle("Modulation amount, as a destination value");
  modulation_amount_editor_.setDescription(reach_help);
  modulation_amount_editor_.setHelpText(reach_help);
  modulation_amount_ok_.setTitle("Set modulation amount");
  modulation_amount_ok_.setDescription("Apply the typed initial modulation amount");
  modulation_amount_cancel_.setTitle("Cancel modulation amount");
  modulation_amount_cancel_.setDescription("Leave the modulation connected without changing the amount");
  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  String peak_value = "1.0";
  if (auto* bridge = parameterBridge(destinationId)) {
    peak_value = accessibleParameterText(*bridge, bridge->getValue());
    const int connection = synth_.getConnectionIndex(sourceId.toStdString(), destinationId.toStdString());
    if (connection >= 0) {
      if (auto* amount_bridge = parameterBridge("modulation_" + String(connection + 1) + "_amount")) {
        const auto& details = bridge->getDetails();
        const float current_engine = bridge->convertToEngineValue(bridge->getValue());
        const float amount = amount_bridge->convertToEngineValue(amount_bridge->getValue());
        const float peak_engine = jlimit(details.min, details.max,
                                         current_engine + amount * (details.max - details.min));
        peak_value = accessibleParameterText(*bridge, bridge->convertToPluginValue(peak_engine));
      }
    }
  }
  modulation_amount_editor_.setText(peak_value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type the value the modulation moves to",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::promptForParameterValue(const String& parameterId, Component& target) {
  auto* bridge = parameterBridge(parameterId);
  if (bridge == nullptr)
    return;

  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  parameter_value_prompt_visible_ = true;
  pending_parameter_value_id_ = parameterId;
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  rememberPromptFocus(target);

  const String name = bridge->getName(128);
  String value = accessibleParameterText(*bridge, bridge->getValue());
  const int macro_index = macroIndexForControlId(parameterId);
  if (macro_index >= 0 && isMacroBipolar(macro_index))
    value = String(roundToInt((bridge->getValue() * 2.0f - 1.0f) * 100.0f)) + "%";
  modulation_amount_prompt_.setText("Set " + name + ". Current value " + value, dontSendNotification);
  modulation_amount_prompt_.setTitle("Set " + name);
  modulation_amount_prompt_.setDescription("Type a value for " + name);
  modulation_amount_editor_.setTitle(name + " value");
  modulation_amount_editor_.setDescription("Type a value for " + name);
  modulation_amount_editor_.setHelpText("Type a value and press Return. Units like percent, ms, seconds, dB, note names, and named options are accepted.");
  if (isTransposeQuantizeParameter(parameterId)) {
    modulation_amount_editor_.setDescription("Type a scale name such as major or minor, custom notes such as C D E F G A B, or add global for global snap");
    modulation_amount_editor_.setHelpText("Examples: major, minor, global major, C D E F G A B, C D E flat F G A flat B flat, chromatic, or off.");
  }
  modulation_amount_ok_.setTitle("Set " + name);
  modulation_amount_ok_.setDescription("Apply the typed value for " + name);
  modulation_amount_cancel_.setTitle("Cancel value entry");
  modulation_amount_cancel_.setDescription("Cancel value entry for " + name);

  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  modulation_amount_editor_.setText(value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type value for " + name,
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::promptForMacroName(int macroIndex, Component& target) {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return;

  Component::SafePointer<Component> target_pointer(&target);
  promptForCustomValue("Macro " + String(macroIndex + 1) + " name", synth_.getMacroName(macroIndex), target,
                       [this, macroIndex, target_pointer](const String& text) {
                         String name = text.trim();
                         if (name.isEmpty())
                           name = "MACRO " + String(macroIndex + 1);

                         synth_.setMacroName(macroIndex, name);
                         if (target_pointer != nullptr) {
                           target_pointer->setTitle(name);
                           target_pointer->setDescription("Adjust " + name);
                         }
                       });
}

void SynthEditor::promptForCustomValue(const String& title, const String& value, Component& target,
                                       std::function<void(const String&)> apply) {
  if (!apply)
    return;

  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  parameter_value_prompt_visible_ = true;
  pending_parameter_value_id_.clear();
  pending_custom_value_title_ = title;
  pending_custom_value_apply_ = std::move(apply);
  rememberPromptFocus(target);

  modulation_amount_prompt_.setText("Set " + title + ". Current value " + value, dontSendNotification);
  modulation_amount_prompt_.setTitle("Set " + title);
  modulation_amount_prompt_.setDescription("Type a value for " + title);
  modulation_amount_editor_.setTitle(title + " value");
  modulation_amount_editor_.setDescription("Type a value for " + title);
  modulation_amount_editor_.setHelpText("Type a value and press Return or OK");
  modulation_amount_ok_.setTitle("Set " + title);
  modulation_amount_ok_.setDescription("Apply the typed value for " + title);
  modulation_amount_cancel_.setTitle("Cancel value entry");
  modulation_amount_cancel_.setDescription("Cancel value entry for " + title);

  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  modulation_amount_editor_.setText(value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this] {
    if (safe_this == nullptr || !safe_this->parameter_value_prompt_visible_)
      return;
    safe_this->modulation_amount_editor_.grabKeyboardFocus();
    safe_this->modulation_amount_editor_.selectAll();
    if (auto* handler = safe_this->modulation_amount_editor_.getAccessibilityHandler())
      handler->grabFocus();
  });
  if (title == "Preset name")
    postPresetActionAnnouncement("Rename preset. Type a new preset name",
                                 AccessibilityHandler::AnnouncementPriority::high);
  else
    postPluginAnnouncement("Type value for " + title,
                           AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::rememberPromptFocus(Component& target) {
  pending_parameter_value_focus_ = &target;
  pending_prompt_focus_title_ = target.getTitle();
  pending_prompt_focus_parameter_.clear();
  pending_prompt_focus_section_.clear();
  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl() == &target) {
      pending_prompt_focus_parameter_ = row->parameterId();
      pending_prompt_focus_section_ = sectionForParameter(pending_prompt_focus_parameter_);
      break;
    }
  }
}

void SynthEditor::returnFocusFromPrompt(bool rebuild, const String& fallbackParameterId) {
  Component::SafePointer<Component> target = pending_parameter_value_focus_;
  const String parameter_id = pending_prompt_focus_parameter_;
  const String title = pending_prompt_focus_title_;
  const String section = pending_prompt_focus_section_;
  pending_parameter_value_focus_ = nullptr;
  pending_prompt_focus_parameter_.clear();
  pending_prompt_focus_title_.clear();
  pending_prompt_focus_section_.clear();

  // Controls outside the rows (the wavetable and LFO controls) outlive a
  // rebuild, and their titles can change with the modulation just added.
  Component::SafePointer<Component> persistent;
  if (target != nullptr && !rows_container_.isParentOf(target.getComponent()))
    persistent = target.getComponent();

  if (rebuild) {
    showAllSections(false);
  }
  else if (target != nullptr && target->isShowing()) {
    persistent = target.getComponent();
    // VoiceOver in a plugin host does not follow focus back to the element it
    // was on before the prompt opened, but does follow it to a new one (as
    // after a rebuild), so give the control a new accessibility element.
    target->invalidateAccessibilityHandler();
  }

  // The control, or after a rebuild (a new route, or one while the prompt was
  // open) its replacement, or else the row the prompt changed. The caller
  // hides the field next, and hiding the field the screen reader is on can
  // send the reader to the top of the window, so move it onto the control
  // again once things settle.
  if (restoreFocusAfterRebuild(parameter_id, persistent.getComponent(), title, section, true))
    return;
  if (fallbackParameterId.isNotEmpty() && fallbackParameterId != parameter_id)
    restoreFocusAfterRebuild(fallbackParameterId, nullptr, {}, sectionForParameter(fallbackParameterId), true);
}

void SynthEditor::hideInlineTextPrompt() {
  // Focus has just gone back to the control, but the screen reader is still on
  // the field and follows only once it catches up. Taking the field away before
  // then leaves VoiceOver on an element that no longer exists, and in a plugin
  // host it stays there, saying nothing, until the user moves it from the top
  // of the window. So the field goes once the reader has moved.
  const int generation = ++inline_prompt_hide_generation_;
  Component::SafePointer<SynthEditor> safe_this(this);
  Timer::callAfterDelay(kInlinePromptHideDelayMs, [safe_this, generation] {
    if (safe_this != nullptr && safe_this->inline_prompt_hide_generation_ == generation)
      safe_this->hideInlineTextPromptNow();
  });
}

void SynthEditor::hideInlineTextPromptNow() {
  // A new prompt opened meanwhile.
  if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_)
    return;
  modulation_amount_prompt_.setVisible(false);
  modulation_amount_editor_.setVisible(false);
  modulation_amount_ok_.setVisible(false);
  modulation_amount_cancel_.setVisible(false);
  rebuildFocusOrder();
  resized();
}

void SynthEditor::applyInlineTextPrompt() {
  if (parameter_value_prompt_visible_)
    applyParameterValue();
  else if (modulation_amount_prompt_visible_)
    applyInitialModulationAmount();
}

void SynthEditor::applyInitialModulationAmount() {
  const String source_id = pending_modulation_source_;
  const String destination_id = pending_modulation_destination_;
  const bool rebuild_modulation_slots = pending_modulation_created_;
  const float amount = modulationAmountFromPeakText(destination_id, modulation_amount_editor_.getText());
  const int connection = synth_.getConnectionIndex(source_id.toStdString(), destination_id.toStdString());
  if (connection >= 0) {
    setParameterEngineValue("modulation_" + String(connection + 1) + "_amount", amount);
    refreshVisibleModulationLabels();
  }

  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  returnFocusFromPrompt(rebuild_modulation_slots, destination_id);
  hideInlineTextPrompt();

  postPluginAnnouncement(modulationSourceLabelForId(source_id) + " assigned to " +
                             modulationDestinationLabelForId(destination_id) + ", " +
                             modulationAmountSummary(source_id, destination_id, amount),
                         AccessibilityHandler::AnnouncementPriority::high);
}

String SynthEditor::modulationAmountSummary(const String& sourceId, const String& destinationId, float amount) const {
  const int connection = synth_.getConnectionIndex(sourceId.toStdString(), destinationId.toStdString());
  const auto found = connection >= 0 ?
      synth_.getControls().find("modulation_" + std::to_string(connection + 1) + "_bipolar") : synth_.getControls().end();
  const bool bipolar = found != synth_.getControls().end() && found->second->value() >= 0.5f;
  auto* bridge = parameterBridge(destinationId);
  if (bridge == nullptr)
    return "amount " + ValueBridge::rangePercentText(amount);
  const auto& details = bridge->getDetails();
  const bool linear = details.value_scale == vital::ValueDetails::kLinear ||
                      details.value_scale == vital::ValueDetails::kIndexed;
  if (!linear || details.string_lookup != nullptr)
    return String(bipolar ? "bipolar" : "unipolar") + ", amount " + ValueBridge::rangePercentText(amount);
  // Linear controls: say how far the destination moves, in its own units.
  const bool percent_destination = ValueBridge::usesRangePercentDisplay(details);
  const float distance = amount * (details.max - details.min) * (percent_destination ? 100.0f : details.display_multiply);
  const String units = percent_destination ? String("%") : String(details.display_units).trim();
  auto value = [&](float v) {
    String text = String(v, std::abs(v) < 10.0f ? 2 : 1);
    if (text.contains("."))
      text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    const bool semitones = units.isEmpty() && destinationId.endsWith("transpose");
    return text + (semitones ? String(" semitones") : units.isEmpty() ? String() :
                   (units == "%" ? units : " " + units));
  };
  if (bipolar)
    return "amount " + value(distance) + ", bipolar: " + value(std::abs(distance) * 0.5f) + " each way";
  return "amount " + value(distance) + ", unipolar";
}

float SynthEditor::modulationAmountFromPeakText(const String& destinationId, const String& text) const {
  const String trimmed = text.trim();
  const String lower = trimmed.toLowerCase();
  if (lower.startsWith("raw ") || lower.startsWith("normalized ")) {
    // The amount itself, -100% to 100%: "raw 25%", "raw 25" or "raw 0.25".
    const String value = trimmed.fromFirstOccurrenceOf(" ", false, false);
    float amount = static_cast<float>(value.getDoubleValue());
    if (value.contains("%") || std::abs(amount) > 1.0f)
      amount *= 0.01f;
    return jlimit(-1.0f, 1.0f, amount);
  }

  auto* bridge = parameterBridge(destinationId);
  if (bridge == nullptr)
    return jlimit(-1.0f, 1.0f, static_cast<float>(trimmed.getDoubleValue()));

  const auto& details = bridge->getDetails();
  const float range = details.max - details.min;
  if (range <= 0.0f)
    return 0.0f;
  // Modulation is continuous even on stepped controls: Transpose moves by
  // fractions of a semitone. Converting through the control's own value
  // would round 0.5 semitones to a whole step (or to nothing).
  const float target_normalized = static_cast<float>(accessibleParameterValueForText(*bridge, trimmed));
  const float target_engine = jlimit(details.min, details.max, details.min + target_normalized * range);
  const float current_engine = bridge->convertToEngineValue(bridge->getValue());

  return jlimit(-1.0f, 1.0f, (target_engine - current_engine) / range);
}

void SynthEditor::applyParameterValue() {
  const String parameter_id = pending_parameter_value_id_;
  const String custom_title = pending_custom_value_title_;
  if (parameter_id.isEmpty() && pending_custom_value_apply_) {
    auto apply = std::move(pending_custom_value_apply_);
    apply(modulation_amount_editor_.getText());

    parameter_value_prompt_visible_ = false;
    pending_custom_value_title_.clear();
    returnFocusFromPrompt(false, {});
    hideInlineTextPrompt();

    postPluginAnnouncement(custom_title + " " + modulation_amount_editor_.getText(),
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  auto* bridge = parameterBridge(parameter_id);
  if (bridge != nullptr) {
    float value;
    if (fine_tune_sample_loop_ && isSampleLoopPointParameter(parameter_id)) {
      const int length = sampleLoopPointLength(parameter_id);
      const double normalized = length > 0
          ? modulation_amount_editor_.getText().trim().getDoubleValue() / length
          : bridge->getValue();
      value = static_cast<float>(jlimit(0.0, 1.0, normalized));
    }
    else {
      value = static_cast<float>(accessibleParameterValueForText(*bridge, modulation_amount_editor_.getText()));
    }
    const int macro_index = macroIndexForControlId(parameter_id);
    if (macro_index >= 0) {
      if (isMacroBipolar(macro_index)) {
        const String text = modulation_amount_editor_.getText().trim();
        float percent = text.getFloatValue();
        if (text.contains("%") || std::abs(percent) > 1.0f)
          percent *= 0.01f;
        value = jlimit(0.0f, 1.0f, (jlimit(-1.0f, 1.0f, percent) + 1.0f) * 0.5f);
      }
      else if (bridge->convertToEngineValue(value) < 0.0f) {
        value = bridge->convertToPluginValue(0.0f);
      }
    }
    if (isTransposeQuantizeParameter(parameter_id)) {
      const int current_value = static_cast<int>(std::round(bridge->convertToEngineValue(bridge->getValue())));
      int quantize_value = current_value;
      if (parseTransposeQuantizeText(modulation_amount_editor_.getText(), current_value, quantize_value)) {
        value = bridge->convertToPluginValue(static_cast<float>(quantize_value));
        if (parameter_id.startsWith("osc_")) {
          const String prefix = parameter_id.upToLastOccurrenceOf("transpose_quantize", false, false) +
                                "transpose_quantize_";
          if (auto* scale_bridge = parameterBridge(prefix + "scale")) {
            scale_bridge->beginChangeGesture();
            scale_bridge->setValueNotifyingHost(scale_bridge->convertToPluginValue(14.0f));
            scale_bridge->endChangeGesture();
          }
        }
      }
    }
    synth_.beginFxAutomationGesture(parameter_id.toStdString());
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(value);
    bridge->endChangeGesture();
    synth_.endFxAutomationGesture(parameter_id.toStdString());
  }

  parameter_value_prompt_visible_ = false;
  pending_parameter_value_id_.clear();
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  returnFocusFromPrompt(false, parameter_id);
  hideInlineTextPrompt();

  if (bridge != nullptr) {
    const int macro_index = macroIndexForControlId(parameter_id);
    if (macro_index >= 0 && isMacroBipolar(macro_index)) {
      postPluginAnnouncement(synth_.getMacroName(macro_index) + " " +
                                             String(roundToInt((bridge->getValue() * 2.0f - 1.0f) * 100.0f)) + "%",
                                             AccessibilityHandler::AnnouncementPriority::high);
    }
    else {
      postPluginAnnouncement(bridge->getName(128) + " " + accessibleParameterText(*bridge, bridge->getValue()),
                                             AccessibilityHandler::AnnouncementPriority::high);
    }
  }
}

bool SynthEditor::isMacroBipolar(int macroIndex) const {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return false;

  if (auto* bridge = parameterBridge(macroBipolarParameterId(macroIndex)))
    return bridge->getValue() >= 0.5f;
  return false;
}

void SynthEditor::setMacroBipolar(int macroIndex, bool bipolar) {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return;

  if (auto* bridge = parameterBridge(macroBipolarParameterId(macroIndex))) {
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(bipolar ? 1.0f : 0.0f);
    bridge->endChangeGesture();
  }

  const String source_id = "macro_control_" + String(macroIndex + 1);
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name != source_id.toStdString() || route->destination_name.empty())
      continue;

    const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
    if (connection >= 0)
      setParameterEngineValue("modulation_" + String(connection + 1) + "_bipolar", bipolar ? 1.0f : 0.0f);
  }
  refreshVisibleModulationLabels();
}

bool SynthEditor::moveEffectInSection(const String& sectionName, const String& effectId, int direction,
                                      const String& focusedTitle) {
  if (sectionName.isEmpty() || effectId.isEmpty())
    return false;
  if (effect_chain_rebuild_queued_)
    return true;

  int order[vital::constants::kNumEffects];
  readEffectChainOrder(sectionName, order);

  int selected = -1;
  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    if (strings::kEffectOrder[order[i]] == effectId) {
      selected = i;
      break;
    }
  }

  const int next = selected + direction;
  if (!isPositiveAndBelow(selected, vital::constants::kNumEffects) ||
      !isPositiveAndBelow(next, vital::constants::kNumEffects))
    return true;

  std::swap(order[selected], order[next]);
  const String moved = effectName(order[next]);
  const String parameter_id = focusedParameterId();
  writeEffectChainOrder(sectionName, order);
  scheduleEffectChainRebuild(sectionName, focusedTitle, parameter_id);
  postPluginAnnouncement(moved + " moved to position " + String(next + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
  return true;
}

bool SynthEditor::handleEffectShortcut(const String& sectionName, const KeyPress& key, Component& target) {
  const String effect_id = effectIdForSection(sectionName);
  if (effect_id.isEmpty())
    return false;

  if (isEffectMoveEarlierKey(key))
    return moveEffectInSection(sectionName, effect_id, -1, target.getTitle());
  if (isEffectMoveLaterKey(key))
    return moveEffectInSection(sectionName, effect_id, 1, target.getTitle());
  return false;
}

bool SynthEditor::handleMacroShortcut(const String& parameterId, const KeyPress& key, Component& target) {
  const int macro_index = macroIndexForControlId(parameterId);
  if (macro_index < 0)
    return false;

  if (key.getModifiers().isShiftDown() && key.getKeyCode() == KeyPress::returnKey) {
    promptForMacroName(macro_index, target);
    return true;
  }

  const bool is_b_key = CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'b' ||
                        key.getKeyCode() == 'b' || key.getKeyCode() == 'B';
  if (key.getModifiers().isAnyModifierKeyDown() || !is_b_key) {
    return false;
  }

  const bool bipolar = !isMacroBipolar(macro_index);
  setMacroBipolar(macro_index, bipolar);

  if (auto* bridge = parameterBridge(parameterId)) {
    if (auto* slider = dynamic_cast<Slider*>(&target)) {
      const double interval = slider->getInterval();
      slider->setRange(0.0, 1.0, interval);
      slider->setValue(jlimit(0.0, 1.0, static_cast<double>(bridge->getValue())), dontSendNotification);
      if (auto* handler = slider->getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }
  }

  postPluginAnnouncement(synth_.getMacroName(macro_index) +
                                         (bipolar ? " bipolar, minus 100 to 100"
                                                  : " unipolar, 0 to 100"),
                                         AccessibilityHandler::AnnouncementPriority::high);
  return true;
}

void SynthEditor::cancelInlineTextPrompt() {
  if (parameter_value_prompt_visible_) {
    parameter_value_prompt_visible_ = false;
    const String parameter_id = pending_parameter_value_id_;
    pending_parameter_value_id_.clear();
    pending_custom_value_title_.clear();
    pending_custom_value_apply_ = nullptr;
    returnFocusFromPrompt(false, parameter_id);
    hideInlineTextPrompt();
    postPluginAnnouncement("Value entry cancelled",
                                           AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  if (modulation_amount_prompt_visible_)
    cancelInitialModulationAmount();
}

void SynthEditor::cancelInitialModulationAmount() {
  const String source_id = pending_modulation_source_;
  const String destination_id = pending_modulation_destination_;
  const bool created = pending_modulation_created_;
  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  returnFocusFromPrompt(created, destination_id);
  hideInlineTextPrompt();

  postPluginAnnouncement(modulationSourceLabelForId(source_id) + " assigned to " +
                                         modulationDestinationLabelForId(destination_id) +
                                         (created ? "" : ", already assigned"),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

PopupMenu SynthEditor::createModulationSourceSubmenu(const String& destinationId, std::map<int, String>& choices,
                                                     int firstItemId) {
  struct SourceChoice {
    String id;
    String label;
  };

  auto addSource = [](std::map<String, std::vector<SourceChoice>>& groups,
                      const String& group, const String& id) {
    groups[group].push_back({ id, readableId(id) });
  };

  std::map<String, std::vector<SourceChoice>> groups;
  for (const auto& id : modulation_source_ids_) {
    if (isInvalidModulationPair(id, destinationId))
      continue;

    if (id.startsWith("lfo_"))
      addSource(groups, "LFOs", id);
    else if (id.startsWith("env_"))
      addSource(groups, "Envelopes", id);
    else if (id == "random" || id.startsWith("random_"))
      addSource(groups, "Random", id);
    else if (id.startsWith("macro_"))
      addSource(groups, "Macros", id);
    else if (const int slot = vital::source_types::slotForOutputSource(id.toStdString()); slot >= 0) {
      // Only audio-rate, per-voice controls follow an oscillator's audio.
      if (synth_.acceptsOscillatorModulation(slot, destinationId.toStdString()))
        addSource(groups, "Oscillators", id);
    }
    else if (const int slot = sourceSlotForId(id); slot >= 0 && id.endsWith("_playhead")) {
      // Only Sample and Granular slots have a playhead to read.
      if (vital::source_types::describe(sourceSlotType(slot)).has_playhead)
        addSource(groups, "Oscillator playheads", id);
    }
    else if (id == "aftertouch" || id == "velocity" || id == "lift" || id == "mod_wheel" ||
             id == "pitch_wheel" || id == "slide" || id == "note" || id == "note_in_octave" ||
             id == "stereo")
      addSource(groups, "MIDI and expression", id);
  }

  PopupMenu menu;
  int item_id = firstItemId;
  for (const auto& group_name : StringArray{ "LFOs", "Envelopes", "Random", "Macros", "Oscillators",
                                             "Oscillator playheads", "MIDI and expression" }) {
    const auto found = groups.find(group_name);
    if (found == groups.end() || found->second.empty())
      continue;

    auto sources = found->second;
    std::stable_sort(sources.begin(), sources.end(), [](const SourceChoice& a, const SourceChoice& b) {
      const int rank_a = modulationSourceSortRank(a.id);
      const int rank_b = modulationSourceSortRank(b.id);
      if (rank_a != rank_b)
        return rank_a < rank_b;
      return a.label.compareNatural(b.label) < 0;
    });

    PopupMenu source_menu;
    for (const auto& source : sources) {
      choices[item_id] = source.id;
      const int macro_index = macroIndexForControlId(source.id);
      const String label = macro_index >= 0 ? synth_.getMacroName(macro_index)
                                            : modulationSourceLabelForId(source.id);
      source_menu.addItem(item_id++, label);
    }
    menu.addSubMenu(group_name, source_menu, true);
  }

  if (choices.empty())
    menu.addItem(firstItemId, "No modulation sources available", false);

  return menu;
}

void SynthEditor::refreshVisibleModulationLabels() {
  for (auto& header : section_headers_) {
    auto* section = dynamic_cast<AccessibleSectionHeader*>(header.get());
    if (section == nullptr || !section->getProperties().contains("ModulationSlot"))
      continue;

    const int slot = static_cast<int>(section->getProperties()["ModulationSlot"]);
    section->setHeaderTitle(modulationSlotTitle(slot));
  }

  for (auto& row : rows_) {
    if (!row)
      continue;

    const String id = row->parameterId();
    if (!id.startsWith("modulation_"))
      continue;

    const String title = modulationControlTitle(id);
    row->setAccessibleName(title, "Adjust " + title);
  }
}

LineGenerator* SynthEditor::activeModulationRemapGenerator() const {
  if (!isPositiveAndBelow(active_modulation_remap_connection_, vital::kMaxModulationConnections))
    return nullptr;

  auto* route = synth_.getModulationBank().atIndex(active_modulation_remap_connection_);
  if (route == nullptr || route->source_name.empty() || route->destination_name.empty() ||
      route->modulation_processor == nullptr)
    return nullptr;
  return route->modulation_processor->lineMapGenerator();
}

int SynthEditor::selectedModulationRemapPointIndex() const {
  auto* generator = activeModulationRemapGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0)
    return -1;
  return jlimit(0, generator->getNumPoints() - 1, modulation_remap_point_index_);
}

bool SynthEditor::selectModulationRemapSlot(int slot) {
  if (!isPositiveAndBelow(slot - 1, vital::kMaxModulationConnections))
    return false;

  auto* connection = synth_.getModulationBank().atIndex(slot - 1);
  if (connection == nullptr || connection->source_name.empty() || connection->destination_name.empty() ||
      connection->modulation_processor == nullptr)
    return false;

  bool connected = false;
  for (auto* route : synth_.getModulationConnections())
    connected = connected || route == connection;
  if (!connected)
    return false;

  if (active_modulation_remap_connection_ != slot - 1)
    modulation_remap_point_index_ = 0;
  active_modulation_remap_connection_ = slot - 1;
  return true;
}

String SynthEditor::modulationRemapPointDescriptionFor(LineGenerator* generator, int point_index) const {
  if (generator == nullptr || !isPositiveAndBelow(point_index, generator->getNumPoints()))
    return "No remap point";

  const auto point = generator->getPoint(point_index);
  String description = "Point " + String(point_index + 1) + " of " + String(generator->getNumPoints()) +
      ", input " + compactFloat(point.first * 100.0f, 1) + "%" +
      ", output " + compactFloat((1.0f - point.second) * 100.0f, 1) + "%";
  if (point_index < generator->getNumPoints() - 1) {
    const int curve = curveIndexForPower(generator->getPower(point_index), generator->smooth());
    description += ", next segment " + curveNameForIndex(curve);
  }
  else
    description += ", final point";
  return description;
}

String SynthEditor::modulationRemapPointDescription(int point_index) const {
  return modulationRemapPointDescriptionFor(activeModulationRemapGenerator(), point_index);
}

String SynthEditor::modulationRemapStatusTextForSlot(int slot) const {
  if (!isPositiveAndBelow(slot - 1, vital::kMaxModulationConnections))
    return "No modulation remap selected";

  const auto* route = synth_.getModulationBank().atIndex(slot - 1);
  if (route == nullptr || route->source_name.empty() || route->destination_name.empty() ||
      route->modulation_processor == nullptr)
    return modulationSlotHeaderTitle(slot) + ", no modulation route";

  auto* generator = route->modulation_processor->lineMapGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0)
    return modulationSlotHeaderTitle(slot) + ", remap unavailable";

  const String destination = vital::Parameters::isParameter(route->destination_name)
      ? vital::Parameters::getDisplayName(route->destination_name) : readableId(route->destination_name);
  const String route_name = modulationSlotHeaderTitle(slot) + ", " +
      modulationSourceLabelForId(route->source_name) + " to " + destination;
  const String shape = generator->linear() ? "linear" : "custom";
  const int point_index = active_modulation_remap_connection_ == slot - 1
      ? jlimit(0, generator->getNumPoints() - 1, modulation_remap_point_index_) : 0;
  return route_name + ", " + shape + " remap, " +
      String(generator->getNumPoints()) + " points. " +
      modulationRemapPointDescriptionFor(generator, point_index);
}

void SynthEditor::finishModulationRemapEdit(const String& announcement) {
  auto* generator = activeModulationRemapGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0)
    return;

  modulation_remap_point_index_ = jlimit(0, generator->getNumPoints() - 1, modulation_remap_point_index_);
  // As in the trance gate editor, the explicit announcement supplies feedback.
  // A group notification would also repeat the full route/point description.
  if (announcement.isNotEmpty())
    postModulationRemapAnnouncement(announcement);
}

void SynthEditor::selectModulationRemapPoint(int direction) {
  auto* generator = activeModulationRemapGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0)
    return;
  modulation_remap_point_index_ = jlimit(0, generator->getNumPoints() - 1,
                                         selectedModulationRemapPointIndex() + direction);
  finishModulationRemapEdit(modulationRemapPointDescription(modulation_remap_point_index_));
}

void SynthEditor::addModulationRemapPoint() {
  auto* generator = activeModulationRemapGenerator();
  if (generator == nullptr || generator->getNumPoints() >= LineGenerator::kMaxPoints)
    return;

  int insertion_index = 1;
  float largest_gap = -1.0f;
  for (int i = 1; i < generator->getNumPoints(); ++i) {
    const float gap = generator->getPoint(i).first - generator->getPoint(i - 1).first;
    if (gap > largest_gap) {
      largest_gap = gap;
      insertion_index = i;
    }
  }

  generator->addMiddlePoint(insertion_index);
  generator->render();
  synth_.markStateChanged();
  modulation_remap_point_index_ = insertion_index;
  finishModulationRemapEdit("Added " + modulationRemapPointDescription(insertion_index));
}

void SynthEditor::deleteModulationRemapPoint() {
  auto* generator = activeModulationRemapGenerator();
  const int point_index = selectedModulationRemapPointIndex();
  if (generator == nullptr || generator->getNumPoints() <= 2 || point_index <= 0 ||
      point_index >= generator->getNumPoints() - 1) {
    postModulationRemapAnnouncement("Endpoints cannot be deleted");
    return;
  }

  generator->removePoint(point_index);
  generator->render();
  synth_.markStateChanged();
  modulation_remap_point_index_ = jmin(point_index, generator->getNumPoints() - 1);
  finishModulationRemapEdit("Deleted point. " + modulationRemapPointDescription(modulation_remap_point_index_));
}

void SynthEditor::moveModulationRemapPointInput(float delta) {
  auto* generator = activeModulationRemapGenerator();
  const int point_index = selectedModulationRemapPointIndex();
  if (generator == nullptr || point_index <= 0 || point_index >= generator->getNumPoints() - 1) {
    postModulationRemapAnnouncement("Endpoint input is fixed");
    return;
  }

  auto point = generator->getPoint(point_index);
  point.first = jlimit(generator->getPoint(point_index - 1).first,
                       generator->getPoint(point_index + 1).first, point.first + delta);
  generator->setPoint(point_index, point);
  generator->render();
  synth_.markStateChanged();
  finishModulationRemapEdit("Input " + compactFloat(point.first * 100.0f, 1) + "%");
}

void SynthEditor::moveModulationRemapPointOutput(float delta) {
  auto* generator = activeModulationRemapGenerator();
  const int point_index = selectedModulationRemapPointIndex();
  if (generator == nullptr || point_index < 0)
    return;

  auto point = generator->getPoint(point_index);
  const float output = jlimit(0.0f, 1.0f, 1.0f - point.second + delta);
  point.second = 1.0f - output;
  generator->setPoint(point_index, point);
  generator->render();
  synth_.markStateChanged();
  finishModulationRemapEdit("Output " + compactFloat(output * 100.0f, 1) + "%");
}

void SynthEditor::setModulationRemapCurve(int curve) {
  auto* generator = activeModulationRemapGenerator();
  const int point_index = selectedModulationRemapPointIndex();
  if (generator == nullptr || !isPositiveAndBelow(point_index, generator->getNumPoints() - 1))
    return;

  curve = jlimit(0, kNumAccessibleLfoCurves - 1, curve);
  generator->setSmooth(smoothForCurveIndex(curve));
  generator->setPower(point_index, powerForCurveIndex(curve));
  generator->render();
  synth_.markStateChanged();
  finishModulationRemapEdit(curveNameForIndex(curve));
}

void SynthEditor::flipModulationRemap(bool horizontal) {
  auto* generator = activeModulationRemapGenerator();
  if (generator == nullptr)
    return;
  const int previous_index = selectedModulationRemapPointIndex();
  if (horizontal)
    generator->flipHorizontal();
  else
    generator->flipVertical();
  if (horizontal)
    modulation_remap_point_index_ = generator->getNumPoints() - previous_index - 1;
  finishModulationRemapEdit(horizontal ? "Flipped horizontally" : "Flipped vertically");
}

void SynthEditor::resetModulationRemap() {
  auto* generator = activeModulationRemapGenerator();
  if (generator == nullptr)
    return;
  generator->initLinear();
  modulation_remap_point_index_ = 0;
  finishModulationRemapEdit("Reset to linear");
}

bool SynthEditor::handleModulationRemapShortcut(const KeyPress& key) {
  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const bool shift = key.getModifiers().isShiftDown();
  const bool alt = key.getModifiers().isAltDown();
  // KeyPress::operator== only matches a key without modifiers, so arrows that
  // take Shift or Alt are compared by key code.
  const int code = key.getKeyCode();
  const bool no_command = !key.getModifiers().isCommandDown() && !key.getModifiers().isCtrlDown();

  if (shift && no_command && (code == KeyPress::leftKey || code == KeyPress::rightKey)) {
    const float amount = alt ? 0.05f : 0.01f;
    moveModulationRemapPointInput(code == KeyPress::rightKey ? amount : -amount);
    return true;
  }
  if (key == KeyPress::leftKey || character == ',') {
    selectModulationRemapPoint(-1);
    return true;
  }
  if (key == KeyPress::rightKey || character == '.') {
    selectModulationRemapPoint(1);
    return true;
  }
  if ((key == KeyPress::upKey || key == KeyPress::downKey) ||
      (shift && no_command && !alt && (code == KeyPress::upKey || code == KeyPress::downKey))) {
    const float amount = shift ? 0.05f : 0.01f;
    moveModulationRemapPointOutput(code == KeyPress::upKey ? amount : -amount);
    return true;
  }
  if (key.getKeyCode() == KeyPress::backspaceKey) {
    resetModulationRemap();
    return true;
  }
  if (character == 'a') {
    addModulationRemapPoint();
    return true;
  }
  if (character == 'e' || key.getKeyCode() == KeyPress::deleteKey) {
    deleteModulationRemapPoint();
    return true;
  }
  if (character == 'i' || character == 'k') {
    auto* generator = activeModulationRemapGenerator();
    const int point_index = selectedModulationRemapPointIndex();
    if (generator != nullptr && isPositiveAndBelow(point_index, generator->getNumPoints() - 1)) {
      const int current = curveIndexForPower(generator->getPower(point_index), generator->smooth());
      const int next = (current + (character == 'i' ? 1 : -1) + kNumAccessibleLfoCurves) %
          kNumAccessibleLfoCurves;
      setModulationRemapCurve(next);
    }
    return true;
  }
  if (character == 'h') {
    flipModulationRemap(true);
    return true;
  }
  if (character == 'v') {
    flipModulationRemap(false);
    return true;
  }
  return false;
}

void SynthEditor::refreshWavetableNameLabel(int target) {
  if (!isPositiveAndBelow(target, vital::kNumOscillators) || synth_.getWavetableCreator(target) == nullptr)
    return;

  // The oscillator's section header names the loaded wavetable.
  const String section_name = "Oscillator " + String(target + 1);
  for (auto& component : section_headers_) {
    if (component != nullptr && component->getProperties()["SectionName"].toString() == section_name) {
      if (auto* header = dynamic_cast<AccessibleSectionHeader*>(component.get()))
        header->setHeaderTitle(sourceStatusTitleForSection(section_name));
    }
  }
}

void SynthEditor::refreshSampleNameLabel(int target) {
  if (!isPositiveAndBelow(target, vital::kNumOscillators))
    return;

  // The oscillator's section header names the loaded sample.
  const String section_name = "Oscillator " + String(target + 1);
  for (auto& component : section_headers_) {
    if (component != nullptr && component->getProperties()["SectionName"].toString() == section_name) {
      if (auto* header = dynamic_cast<AccessibleSectionHeader*>(component.get()))
        header->setHeaderTitle(sourceStatusTitleForSection(section_name));
    }
  }
}

String SynthEditor::sourceStatusTitleForSection(const String& section_name) const {
  String title = accessibleSectionTitle(section_name);
  const int oscillator = oscillatorIndexForSectionName(section_name);
  if (isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
    if (isAnalogOscillator(oscillator)) {
      const int waveform = jlimit(0, vital::analog::kNumWaveforms - 1, oscillatorModeKey(oscillator) % 64);
      return title + ", analog " + String(strings::kAnalogWaveformNames[waveform]).toLowerCase();
    }
    const auto& type = vital::source_types::describe(sourceSlotType(oscillator));
    if (type.uses_sample) {
      auto* sample = synth_.getSample(oscillator);
      String name(sample != nullptr ? sample->getName() : std::string());
      if (name.trim().isEmpty())
        name = "No sample loaded";
      return title + ", " + String(strings::kOscillatorTypeNames[sourceSlotType(oscillator)]).toLowerCase() +
             ", current sample: " + name;
    }
    if (sourceSlotType(oscillator) == vital::source_types::kMultiSample) {
      String name = "no instrument loaded";
      if (auto* manager = synth_.getMultiSampleManager()) {
        const auto result = manager->result(oscillator);
        if (result != nullptr && result->instrument != nullptr)
          name = String::fromUTF8(result->instrument->name.c_str());
      }
      return title + ", multi-sample, " + name;
    }
    if (!type.uses_wavetable)
      return title + ", " + String(strings::kOscillatorTypeNames[sourceSlotType(oscillator)]).toLowerCase();
    if (auto* creator = synth_.getWavetableCreator(oscillator)) {
      String name(creator->getName());
      if (name.trim().isEmpty())
        name = "Unnamed";
      return title + ", current wavetable: " + name;
    }
  }
  return title;
}

void SynthEditor::chooseWavetableFile() {
  chooseWavetableFile(WavetableCreator::kWavetableSplice);
}

void SynthEditor::chooseWavetableFile(int audioLoadStyle) {
  if (active_oscillator_ < 0)
    return;
  wavetable_chooser_ = std::make_unique<FileChooser>("Load wavetable or audio file",
                                                     File(), vital::kWavetableExtensionsList);
  launchFileChooser(*wavetable_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                         [this, audioLoadStyle](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadWavetableFile(file, audioLoadStyle);
    wavetable_chooser_.reset();
  });
}

void SynthEditor::loadWavetableFile(const File& file) {
  loadWavetableFile(file, WavetableCreator::kWavetableSplice);
}

bool SynthEditor::loadWavetableIntoCreator(WavetableCreator& creator, const File& file, int audioLoadStyle,
                                           String& error) {
  try {
    const auto extension = file.getFileExtension().toLowerCase();
    const bool is_audio_file = extension == ".wav" || extension == ".flac" ||
                               extension == ".aif" || extension == ".aiff";
    String wavetable_string;
    AudioSampleBuffer sample_buffer;
    int sample_rate = 0;
    json data;

    if (is_audio_file) {
      auto input_stream = std::make_unique<FileInputStream>(file);
      if (!input_stream->openedOk())
        throw std::runtime_error("Unable to open audio file");

      wavetable_string = getWavetableDataString(*input_stream);
      sample_rate = loadAudioFile(sample_buffer, input_stream.release());
      if (sample_rate == 0 || sample_buffer.getNumSamples() == 0)
        throw std::runtime_error("Unable to decode audio file");
    }
    else {
      data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
      if (data.is_discarded())
        throw std::runtime_error("Unable to parse wavetable JSON");
    }

    if (is_audio_file) {
      creator.initFromAudioFile(sample_buffer.getReadPointer(0), sample_buffer.getNumSamples(),
                                sample_rate, static_cast<WavetableCreator::AudioFileLoadStyle>(audioLoadStyle),
                                getFadeStyleFromWavetableString(wavetable_string));
      creator.setName(file.getFileNameWithoutExtension().toStdString());
      creator.setAuthor(getAuthorFromWavetableString(wavetable_string).toStdString());
    }
    else {
      // Do not preflight with WavetableCreator::isValidJson() here. Older factory
      // Older tables may omit version/name fields but jsonToState() can still
      // migrate and load them through WavetableCreator::updateJson().
      creator.jsonToState(data);
      if (creator.getName().empty())
        creator.setName(file.getFileNameWithoutExtension().toStdString());
    }
    creator.setFileLoaded(file.getFullPathName().toStdString());
    creator.render();
    synth_.markStateChanged();
    return true;
  }
  catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  catch (...) {
    error = "Unable to load wavetable";
    return false;
  }
}

void SynthEditor::loadWavetableFile(const File& file, int audioLoadStyle) {
  if (active_oscillator_ < 0)
    return;

  const String focus_title = focusedAccessibleTitle();
  try {
    if (auto* creator = synth_.getWavetableCreator(active_oscillator_)) {
      String error;
      if (!loadWavetableIntoCreator(*creator, file, audioLoadStyle, error))
        throw std::runtime_error(error.toStdString());
      creator->setFileLoaded(file.getFullPathName().toStdString());
      // The new table has its own layers and frames; the editor's cursors and
      // controls describe the old one until it is rebuilt.
      wavetable_keyboard_state_[active_oscillator_] = {};
      if (wavetable_editor_visible_[active_oscillator_])
        rebuildWavetableEditor(active_oscillator_, focus_title);
      const auto extension = file.getFileExtension().toLowerCase();
      const bool is_audio_file = extension == ".wav" || extension == ".flac" || extension == ".aif" || extension == ".aiff";
      const String import_suffix = is_audio_file ? " as " + wavetableAudioLoadStyleName(audioLoadStyle) : "";
      postPluginAnnouncement("Loaded " + file.getFileNameWithoutExtension() + import_suffix +
                                               " in oscillator " + String(active_oscillator_ + 1),
                                               AccessibilityHandler::AnnouncementPriority::high);
    }
  }
  catch (const std::exception& error) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                          "Unable to load wavetable", error.what());
    postPluginAnnouncement("Unable to load wavetable",
                                             AccessibilityHandler::AnnouncementPriority::high);
  }
}

void SynthEditor::synthesizePresetToWavetable(int oscillator) {
  static constexpr float kCaptureSeconds = 4.0f;
  static constexpr int kCaptureNote = 16;

  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  if (preset_to_wavetable_rendering_) {
    postPluginAnnouncement("Preset to wavetable capture is already running",
                           AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  preset_to_wavetable_rendering_ = true;
  postPluginAnnouncement("Synthesizing the current preset to oscillator " + String(oscillator + 1) +
                             " wavetable",
                         AccessibilityHandler::AnnouncementPriority::high);

  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this, oscillator] {
    if (safe_this == nullptr)
      return;

    try {
      const int sample_rate = jmax(1, static_cast<SynthBase&>(safe_this->synth_).getSampleRate());
      const int total_samples = roundToInt(sample_rate * kCaptureSeconds);
      auto audio = std::make_unique<float[]>(total_samples);
      safe_this->synth_.renderAudioForResynthesis(audio.get(), total_samples, kCaptureNote);

      float peak = 0.0f;
      for (int sample = 0; sample < total_samples; ++sample)
        peak = jmax(peak, std::abs(audio[sample]));
      if (peak < 0.00001f) {
        safe_this->preset_to_wavetable_rendering_ = false;
        postPluginAnnouncement("Preset to wavetable stopped because the patch produced no audio",
                               AccessibilityHandler::AnnouncementPriority::high);
        return;
      }

      auto* creator = safe_this->synth_.getWavetableCreator(oscillator);
      if (creator == nullptr)
        throw std::runtime_error("The destination oscillator has no wavetable creator");

      creator->initFromAudioFile(audio.get(), total_samples, sample_rate,
                                 WavetableCreator::kPitched, FileSource::kWaveBlend);
      creator->setName("Preset resynthesis");
      creator->setAuthor("");
      creator->setFileLoaded(std::string());
      auto* group = creator->numGroups() > 0 ? creator->getGroup(0) : nullptr;
      auto* file_source = group != nullptr && group->numComponents() > 0 ?
          dynamic_cast<FileSource*>(group->getComponent(0)) : nullptr;
      if (file_source != nullptr) {
        file_source->setWindowSize(sample_rate / vital::utils::midiNoteToFrequency(kCaptureNote));
        creator->render();
        safe_this->synth_.markStateChanged();
      }

      auto& state = safe_this->wavetable_keyboard_state_[oscillator];
      state.frame = 0;
      state.harmonic = 1;
      state.selected_frames.clear();
      safe_this->preset_to_wavetable_rendering_ = false;
      safe_this->refreshWavetableNameLabel(oscillator);
      safe_this->rebuildWavetableEditor(oscillator, "Synthesize preset to wavetable");
      postPluginAnnouncement("Preset synthesized to oscillator " + String(oscillator + 1) +
                                 " wavetable. Use Wave frame to scan the captured sound",
                             AccessibilityHandler::AnnouncementPriority::high);
    }
    catch (const std::exception& error) {
      safe_this->preset_to_wavetable_rendering_ = false;
      postPluginAnnouncement("Unable to synthesize preset to wavetable: " + String(error.what()),
                             AccessibilityHandler::AnnouncementPriority::high);
    }
  });
}

void SynthEditor::saveWavetableFile(int oscillator) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  if (creator == nullptr) {
    postPluginAnnouncement("No wavetable available", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  String name = String(creator->getName()).trim();
  if (name.isEmpty())
    name = "Oscillator " + String(oscillator + 1) + " Wavetable";
  name = name.removeCharacters("\\/:*?\"<>|");
  if (name.isEmpty())
    name = "Wavetable";

  wavetable_chooser_ = std::make_unique<FileChooser>("Export Atlas wavetable",
                                                     LoadSave::getUserWavetableDirectory().getChildFile(name),
                                                     String("*.") + vital::kWavetableExtension);
  launchFileChooser(*wavetable_chooser_, FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                             FileBrowserComponent::warnAboutOverwriting,
                                         [this, oscillator](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file != File()) {
      if (auto* creator = synth_.getWavetableCreator(oscillator)) {
        const File destination = file.withFileExtension(vital::kWavetableExtension);
        const bool saved = destination.replaceWithText(String(creator->stateToJson().dump(2)));
        if (saved)
          postPluginAnnouncement("Exported wavetable " + destination.getFileNameWithoutExtension(),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        else
          NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export wavetable",
                                                "The wavetable file could not be written.");
      }
    }
    wavetable_chooser_.reset();
  });
}

void SynthEditor::exportWavetableWav(int oscillator) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  if (creator == nullptr) {
    postPluginAnnouncement("No wavetable available", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  String name = String(creator->getName()).trim().removeCharacters("\\/:*?\"<>|");
  if (name.isEmpty())
    name = "Wavetable";
  wavetable_chooser_ = std::make_unique<FileChooser>("Export wavetable as WAV",
                                                     LoadSave::getUserWavetableDirectory().getChildFile(name + ".wav"),
                                                     "*.wav");
  launchFileChooser(*wavetable_chooser_, FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                             FileBrowserComponent::warnAboutOverwriting,
                                         [this, oscillator](const FileChooser& chooser) {
    const File file = chooser.getResult().withFileExtension("wav");
    wavetable_chooser_.reset();
    auto* target = synth_.getWavetableCreator(oscillator);
    if (file == File() || target == nullptr)
      return;
    // Vital's layout: 256 frames of 2048 samples, marked so wavetable importers find the cycles.
    constexpr int kFrames = 256;
    constexpr int kSampleRate = 88200;
    const int total = vital::WaveFrame::kWaveformSize * kFrames;
    std::vector<float> buffer(size_t(total), 0.0f);
    target->renderToBuffer(buffer.data(), kFrames, vital::WaveFrame::kWaveformSize);
    file.deleteFile();
    std::unique_ptr<OutputStream> stream = file.createOutputStream();
    WavAudioFormat format;
    StringPairArray metadata;
    metadata.set("clm ", "<!>2048 20000000 wavetable (Atlas)");
    std::unique_ptr<AudioFormatWriter> writer(stream != nullptr ?
        format.createWriterFor(stream.get(), kSampleRate, 1, 16, metadata, 0) : nullptr);
    if (writer == nullptr) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export wavetable",
                                            "The WAV file could not be written.");
      return;
    }
    stream.release();   // the writer owns it now
    const float* channel = buffer.data();
    writer->writeFromFloatArrays(&channel, 1, total);
    writer->flush();
    postPluginAnnouncement("Exported " + file.getFileName(), AccessibilityHandler::AnnouncementPriority::high);
  });
}

int SynthEditor::filterModelForPrefix(const String& filter_prefix) const {
  const auto found = synth_.getControls().find((filter_prefix + "_model").toStdString());
  if (found == synth_.getControls().end() || found->second == nullptr)
    return -1;
  return static_cast<int>(std::round(found->second->value()));
}

void SynthEditor::addFilterTableControlsGroup(const String& filter_prefix, Component& parent, int& y, int x,
                                              int width, int& focus_order, const String& section_name) {
  WavetableCreator* creator = synth_.getFilterTableCreator(filter_prefix.toStdString(), false);
  const String table_name = creator != nullptr ? String(creator->getName()) : String("Default low-pass");
  auto group = makeSectionHeader(
      "Filter wavetable controls", groupRank("Filters"), false, section_name + " filter wavetable controls");
  auto* group_ptr = group.get();
  group_ptr->setDescription("Load, browse, cycle and reset the wavetable this filter reads. Current wavetable: " + table_name);
  group_ptr->setExplicitFocusOrder(focus_order++);
  group_ptr->setBounds(x, y, width, group_ptr->headerHeight());
  parent.addAndMakeVisible(group_ptr);
  if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
    accessible_parent->addAccessibleChild(group_ptr);
  row_focus_order_.push_back(group_ptr);
  section_headers_.push_back(std::move(group));

  int group_y = group_ptr->headerHeight() + 8;
  const int control_width = jmax(200, width - 20);
  auto add_button = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->setWantsKeyboardFocus(true);
    button_ptr->onClick = std::move(action);
    button_ptr->setExplicitFocusOrder(focus_order++);
    button_ptr->setBounds(10, group_y, control_width, 38);
    group_ptr->addAndMakeVisible(button_ptr);
    group_ptr->addAccessibleChild(button_ptr);
    row_focus_order_.push_back(button_ptr);
    section_headers_.push_back(std::move(button));
    group_y += 44;
    return button_ptr;
  };

  auto* current_button = add_button("Current wavetable: " + table_name, "Announce the wavetable this filter reads",
                                    [this, filter_prefix] {
    postPluginAnnouncement("Current wavetable: " + filterTableName(filter_prefix),
                           AccessibilityHandler::AnnouncementPriority::high);
  });
  // Loading a wavetable updates these in place (see refreshFilterTableControls),
  // so the rows are not rebuilt and focus stays where it was.
  filter_table_controls_[filter_prefix] = { group_ptr, current_button };
  add_button("Load wavetable", "Load a wavetable or audio file into " + section_name,
             [this, filter_prefix] { chooseFilterTableFile(filter_prefix); });
  add_button("Browse wavetable folders", "Browse the wavetable folders for " + section_name,
             [this, filter_prefix, group_ptr] { showFilterTableBrowserMenu(filter_prefix, *group_ptr); });
  add_button("Previous wavetable", "Load the previous wavetable from the folders into " + section_name,
             [this, filter_prefix] { loadShiftedFilterTable(filter_prefix, -1); });
  add_button("Next wavetable", "Load the next wavetable from the folders into " + section_name,
             [this, filter_prefix] { loadShiftedFilterTable(filter_prefix, 1); });
  add_button("Reset wavetable", "Restore the default low-pass wavetable in " + section_name,
             [this, filter_prefix, section_name] {
               synth_.resetFilterTable(filter_prefix.toStdString());
               postPluginAnnouncement("Default low-pass wavetable restored in " + section_name,
                                      AccessibilityHandler::AnnouncementPriority::high);
               refreshFilterTableControls(filter_prefix);
             });

  group_ptr->setBounds(x, y, width, group_y);
  y += group_y + 8;
}

void SynthEditor::chooseFilterTableFile(const String& filter_prefix) {
  wavetable_chooser_ = std::make_unique<FileChooser>("Load a wavetable or audio file into the filter",
                                                     File(), vital::kWavetableExtensionsList);
  launchFileChooser(*wavetable_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                         [this, filter_prefix](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadFilterTableFile(filter_prefix, file);
    wavetable_chooser_.reset();
  });
}

String SynthEditor::filterTableName(const String& filter_prefix) const {
  WavetableCreator* creator = synth_.getFilterTableCreator(filter_prefix.toStdString(), false);
  return creator != nullptr ? String(creator->getName()) : String("Default low-pass");
}

void SynthEditor::refreshFilterTableControls(const String& filter_prefix) {
  const auto found = filter_table_controls_.find(filter_prefix);
  if (found == filter_table_controls_.end())
    return;
  auto& controls = found->second;
  if (controls.group == nullptr || controls.current == nullptr) {
    filter_table_controls_.erase(found);
    return;
  }
  const String name = filterTableName(filter_prefix);
  const String title = "Current wavetable: " + name;
  controls.current->setTitle(title);
  if (auto* button = dynamic_cast<Button*>(controls.current.getComponent()))
    button->setButtonText(title);
  if (auto* handler = controls.current->getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  controls.group->setDescription("Load, browse, cycle and reset the wavetable this filter reads. Current wavetable: " + name);
}

void SynthEditor::loadFilterTableFile(const String& filter_prefix, const File& file) {
  WavetableCreator* creator = synth_.getFilterTableCreator(filter_prefix.toStdString(), true);
  if (creator == nullptr)
    return;
  String error;
  if (!loadWavetableIntoCreator(*creator, file, WavetableCreator::kWavetableSplice, error)) {
    postPluginAnnouncement("Unable to load wavetable: " + error, AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  postPluginAnnouncement("Loaded wavetable " + file.getFileNameWithoutExtension(),
                         AccessibilityHandler::AnnouncementPriority::high);
  // Only the name on the "Current wavetable" control changes, so update it in
  // place: no rebuild, and focus (and the VoiceOver cursor) stays on the
  // Previous or Next control that was pressed.
  refreshFilterTableControls(filter_prefix);
}

void SynthEditor::loadShiftedFilterTable(const String& filter_prefix, int direction) {
  WavetableCreator* creator = synth_.getFilterTableCreator(filter_prefix.toStdString(), false);
  const File current_file(creator != nullptr ? creator->getLastFileLoaded() : std::string());
  if (!wavetable_browser_cache_valid_)
    refreshWavetableBrowserCache();
  File wavetable_file = shiftedBrowserFile(cached_wavetable_browser_files_, current_file, direction);
  if (wavetable_file.existsAsFile()) {
    loadFilterTableFile(filter_prefix, wavetable_file);
    return;
  }
  postPluginAnnouncement("No wavetable file found", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showFilterTableBrowserMenu(const String& filter_prefix, Component& target) {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                       LoadSave::kAdditionalWavetableFoldersName);
  if (!wavetable_browser_cache_valid_)
    refreshWavetableBrowserCache();
  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, LoadSave::kWavetableFolderName,
                                         cached_wavetable_browser_files_, choices);
  menu.addSeparator();
  menu.addItem(kBrowserRescanMenuId, "Rescan wavetable folders");
  Component::SafePointer<Component> target_pointer(&target);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                     [this, filter_prefix, choices, target_pointer](int result) {
    if (result == kBrowserRescanMenuId) {
      cached_wavetable_browser_files_.clear();
      wavetable_browser_cache_valid_ = false;
      if (target_pointer != nullptr)
        showFilterTableBrowserMenu(filter_prefix, *target_pointer);
      return;
    }
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;
    loadFilterTableFile(filter_prefix, (*choices)[static_cast<size_t>(result - 1)]);
  });
}

void SynthEditor::addWavetableControlsGroup(int oscillator, Component& parent, int& y, int x,
                                            int width, int& focus_order, const String& section_name,
                                            const GroupRowAppender& append_rows) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  const int outer_y = y;
  auto controls_group = makeSectionHeader(
      "Wavetable controls", groupRank("Oscillators"), false,
      section_name + " wavetable controls");
  auto* controls_group_ptr = controls_group.get();
  controls_group_ptr->setDescription("Wavetable loading, browsing, importing, exporting, capture, and editing");
  controls_group_ptr->setExplicitFocusOrder(focus_order++);
  controls_group_ptr->setBounds(x, outer_y, width, controls_group_ptr->headerHeight());
  parent.addAndMakeVisible(controls_group_ptr);
  if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
    accessible_parent->addAccessibleChild(controls_group_ptr);
  row_focus_order_.push_back(controls_group_ptr);
  section_headers_.push_back(std::move(controls_group));

  int group_y = controls_group_ptr->headerHeight() + 8;
  const int control_width = jmax(200, width - 20);
  auto add_button = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->setWantsKeyboardFocus(true);
    button_ptr->onClick = std::move(action);
    button_ptr->setExplicitFocusOrder(focus_order++);
    button_ptr->setBounds(10, group_y, control_width, 38);
    controls_group_ptr->addAndMakeVisible(button_ptr);
    controls_group_ptr->addAccessibleChild(button_ptr);
    row_focus_order_.push_back(button_ptr);
    section_headers_.push_back(std::move(button));
    group_y += 44;
  };

  add_button("Load wavetable", "Load a wavetable into " + section_name, [this, oscillator] {
    active_oscillator_ = oscillator;
    chooseWavetableFile();
  });
  add_button("Browse wavetable folders", "Browse wavetable folders for " + section_name,
             [this, oscillator, controls_group_ptr] {
               active_oscillator_ = oscillator;
               showWavetableBrowserMenu(oscillator, *controls_group_ptr);
             });
  add_button("Previous wavetable", "Load the previous wavetable into " + section_name,
             [this, oscillator] { loadShiftedWavetable(oscillator, -1); });
  add_button("Next wavetable", "Load the next wavetable into " + section_name,
             [this, oscillator] { loadShiftedWavetable(oscillator, 1); });
  add_button("Wavetable actions", "Open the menu to import audio, export, synthesize the preset, rename or reset the "
             "wavetable in " + section_name,
             [this, oscillator, controls_group_ptr] { showWavetableActionsMenu(oscillator, *controls_group_ptr); });

  if (append_rows)
    append_rows(*controls_group_ptr, group_y, width);

  if (wavetable_editor_visible_[oscillator]) {
    add_button("Hide wavetable editor", "Hide the wavetable editor controls",
               [this, oscillator] { setWavetableEditorVisible(oscillator, false); });
    group_y = addAccessibleWavetableEditorControls(oscillator, *controls_group_ptr, group_y, 10,
                                                    control_width, focus_order);
  }
  else {
    add_button("Show wavetable editor", "Show the wavetable editor controls",
               [this, oscillator] { setWavetableEditorVisible(oscillator, true); });
  }

  const int group_height = group_y + 2;
  controls_group_ptr->setBounds(x, outer_y, width, group_height);
  y = outer_y + group_height + 8;
}

int SynthEditor::addAccessibleWavetableEditorControls(int oscillator, Component& parent, int y, int x,
                                                      int width, int& focus_order) {
  namespace edit = atlas::wavetable_edit;
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto& state = wavetable_keyboard_state_[oscillator];
  const int layers = creator != nullptr ? creator->numGroups() : 0;
  state.layer = jlimit(0, jmax(0, layers - 1), state.layer);

  const int editor_outer_y = y;
  const int editor_outer_x = x;
  const int editor_outer_width = width;
  auto editor_group = makeSectionHeader(
      "Wavetable editor", groupRank("Oscillators"), false,
      "Oscillator " + String(oscillator + 1) + " wavetable editor");
  auto* editor_group_ptr = editor_group.get();
  editor_group_ptr->setDescription("Layers, sources, modifiers and their keyframes");
  editor_group_ptr->setExplicitFocusOrder(focus_order++);
  editor_group_ptr->setBounds(editor_outer_x, editor_outer_y, editor_outer_width,
                              editor_group_ptr->headerHeight());
  parent.addAndMakeVisible(editor_group_ptr);
  if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
    accessible_parent->addAccessibleChild(editor_group_ptr);
  row_focus_order_.push_back(editor_group_ptr);
  section_headers_.push_back(std::move(editor_group));

  Component* controls_parent = editor_group_ptr;
  AccessibleSectionHeader* accessible_controls_parent = editor_group_ptr;
  int control_x = 10;
  int control_y = editor_group_ptr->headerHeight() + 8;
  int control_width = jmax(200, editor_outer_width - 20);

  auto attach = [&](auto control, int height, int gap, bool focusable) {
    auto* control_ptr = control.get();
    control_ptr->setBounds(control_x, control_y, control_width, height);
    controls_parent->addAndMakeVisible(control_ptr);
    if (accessible_controls_parent != nullptr)
      accessible_controls_parent->addAccessibleChild(control_ptr);
    if (focusable) {
      control_ptr->setWantsKeyboardFocus(true);
      control_ptr->setExplicitFocusOrder(focus_order++);
      row_focus_order_.push_back(control_ptr);
    }
    section_headers_.push_back(std::move(control));
    control_y += height + gap;
    return control_ptr;
  };

  auto addCombo = [&](const String& title, const String& description, const StringArray& choices,
                      int selected_id) {
    auto combo = std::make_unique<OffscreenComboBox>();
    auto* combo_ptr = combo.get();
    combo_ptr->setTitle(title);
    combo_ptr->setName(title);
    combo_ptr->setDescription(description);
    combo_ptr->setHelpText("Use the menu or arrow keys to choose an option");
    for (int i = 0; i < choices.size(); ++i)
      combo_ptr->addItem(choices[i], i + 1);
    combo_ptr->setSelectedId(jlimit(1, jmax(1, choices.size()), selected_id), dontSendNotification);
    return attach(std::move(combo), 38, 6, true);
  };

  auto addSlider = [&](const String& title, const String& description, double minimum, double maximum,
                       double value, double interval, const String& suffix, int decimals) {
    auto slider = std::make_unique<OffscreenSlider>();
    auto* slider_ptr = slider.get();
    slider_ptr->setTitle(title);
    slider_ptr->setName(title);
    slider_ptr->setDescription(description);
    slider_ptr->setHelpText("Use arrow keys for small changes, Page Up and Page Down for larger changes");
    slider_ptr->setRange(minimum, jmax(minimum + 1.0e-6, maximum), interval >= 1.0 ? interval : 0.0);
    slider_ptr->setValue(value, dontSendNotification);
    slider_ptr->textFromValueFunction = [suffix, decimals](double slider_value) {
      return String(slider_value, decimals) + suffix;
    };
    slider_ptr->onTextEntryCommand = [this, safe_slider = Component::SafePointer<OffscreenSlider>(slider_ptr),
                                      title](Component& target) {
      if (safe_slider == nullptr)
        return;
      promptForCustomValue(title, safe_slider->getTextFromValue(safe_slider->getValue()), target,
                           [safe_slider](const String& text) {
                             if (safe_slider == nullptr)
                               return;
                             Slider::ScopedDragNotification drag(*safe_slider);
                             safe_slider->setValue(safe_slider->getValueFromText(text), sendNotificationSync);
                           });
    };
    return attach(std::move(slider), 42, 6, true);
  };

  auto addButton = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setName(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->onClick = std::move(action);
    return attach(std::move(button), 38, 6, true);
  };

  auto addText = [&](const String& title, const String& text) {
    auto label = std::make_unique<Label>();
    label->setTitle(title);
    label->setDescription(text);
    label->setText(text, dontSendNotification);
    label->setColour(Label::textColourId, Colours::white);
    return attach(std::move(label), 38, 6, false);
  };

  // Nested groups for the source and each modifier.
  int group_outer_y = 0;
  AccessibleSectionHeader* open_group = nullptr;
  auto beginGroup = [&](const String& title, const String& description) {
    group_outer_y = control_y;
    auto group = makeSectionHeader(title, groupRank("Oscillators"), false);
    auto* group_ptr = group.get();
    group_ptr->setDescription(description);
    group_ptr->setExplicitFocusOrder(focus_order++);
    group_ptr->setBounds(control_x, group_outer_y, control_width, group_ptr->headerHeight());
    editor_group_ptr->addAndMakeVisible(group_ptr);
    editor_group_ptr->addAccessibleChild(group_ptr);
    row_focus_order_.push_back(group_ptr);
    section_headers_.push_back(std::move(group));
    open_group = group_ptr;
    controls_parent = group_ptr;
    accessible_controls_parent = group_ptr;
    control_x = 10;
    control_y = group_ptr->headerHeight() + 8;
    control_width = jmax(180, editor_outer_width - 40);
  };
  auto endGroup = [&] {
    if (open_group == nullptr)
      return;
    const int height = control_y + 2;
    open_group->setBounds(10, group_outer_y, jmax(200, editor_outer_width - 20), height);
    open_group = nullptr;
    controls_parent = editor_group_ptr;
    accessible_controls_parent = editor_group_ptr;
    control_x = 10;
    control_y = group_outer_y + height + 8;
    control_width = jmax(200, editor_outer_width - 20);
  };
  auto finish = [&] {
    const int editor_height = control_y + 2;
    editor_group_ptr->setBounds(editor_outer_x, editor_outer_y, editor_outer_width, editor_height);
    return editor_outer_y + editor_height + 8;
  };

  addText("Wavetable overview", creator != nullptr ? edit::describeWavetable(*creator) : String("No wavetable"));
  if (creator == nullptr || layers == 0)
    return finish();

  // Layers: Vital's groups, each a source with its modifiers, summed together.
  StringArray layer_names;
  for (int i = 0; i < layers; ++i)
    layer_names.add(edit::describeLayer(*creator, i));
  auto* layer_combo = addCombo("Layer", "Choose the layer to edit. Every layer is a source with its own modifiers, "
                               "and the layers are mixed into one wavetable", layer_names, state.layer + 1);
  layer_combo->onChange = [this, oscillator, layer_combo] {
    selectWavetableLayer(oscillator, layer_combo->getSelectedId() - 1);
  };
  auto* new_layer_type = addCombo("New layer source", "The source a new layer starts with: a wave source of frames "
                                  "you shape harmonic by harmonic, a line source of points, or an audio file",
                                  edit::sourceTypeNames(), state.new_layer_type + 1);
  new_layer_type->onChange = [this, oscillator, new_layer_type] {
    wavetable_keyboard_state_[oscillator].new_layer_type = new_layer_type->getSelectedId() - 1;
  };
  addButton("Add layer", "Add a layer with the chosen source", [this, oscillator, new_layer_type] {
    addWavetableLayer(oscillator, new_layer_type->getSelectedId() - 1);
  });
  if (layers > 1) {
    addButton("Remove layer", "Remove the selected layer and its modifiers",
              [this, oscillator] { removeWavetableLayer(oscillator); });
    if (state.layer > 0)
      addButton("Move layer earlier", "Move the selected layer up the list",
                [this, oscillator] { moveWavetableLayer(oscillator, -1); });
    if (state.layer < layers - 1)
      addButton("Move layer later", "Move the selected layer down the list",
                [this, oscillator] { moveWavetableLayer(oscillator, 1); });
  }

  auto* group = creator->getGroup(state.layer);
  auto* source = group != nullptr && group->numComponents() > 0 ? group->getComponent(0) : nullptr;

  // Rows for one source or modifier: its options, then keyframe navigation and
  // the selected keyframe's values. Lambdas look the component up again, since
  // structural edits replace it.
  auto addComponentRows = [&](int component_index, WavetableComponent* component, const String& prefix) {
    const auto list = edit::parameters(component);
    const bool keyed = edit::hasKeyframes(component) && component->numFrames() > 0;
    int& selected = state.keyframes[component_index];
    selected = keyed ? jlimit(0, component->numFrames() - 1, selected) : 0;

    struct Row { Component::SafePointer<OffscreenSlider> slider; std::string id; };
    auto keyframe_rows = std::make_shared<std::vector<Row>>();
    auto line_rows = std::make_shared<std::vector<Row>>();
    auto refresh = std::make_shared<std::function<void()>>();

    auto addParameterRow = [&](const edit::Parameter& parameter) {
      const String title = prefix + " " + parameter.title.toLowerCase();
      const double value = edit::getValue(component, parameter.id, selected);
      if (parameter.isChoice()) {
        auto* combo = addCombo(title, parameter.description, parameter.choices, int(std::round(value)) + 1);
        combo->onChange = [this, oscillator, component_index, id = parameter.id, combo, refresh] {
          setWavetableComponentValue(oscillator, component_index, id, combo->getSelectedId() - 1);
          if (*refresh) (*refresh)();
        };
        return;
      }
      auto* slider = addSlider(title, parameter.description, parameter.minimum, parameter.maximum, value,
                               parameter.interval, parameter.suffix, parameter.decimals);
      slider->onValueChange = [this, oscillator, component_index, id = parameter.id, slider, refresh] {
        setWavetableComponentValue(oscillator, component_index, id, slider->getValue());
        if (*refresh) (*refresh)();
      };
      if (parameter.id == "start_position" || parameter.id == "window_size") {
        const int rate = edit::fileSourceSampleRate(component);
        const int decimals = parameter.decimals;
        slider->textFromValueFunction = [rate, decimals](double samples) {
          String text = String(samples, decimals) + " samples";
          if (rate > 0)
            text << ", " << String(samples / rate, 3) << " seconds";
          return text;
        };
      }
      if (parameter.id == "window_size") {
        // Accept a note name: one period of that note.
        slider->onTextEntryCommand = [this, oscillator, component_index, safe = Component::SafePointer<OffscreenSlider>(slider),
                                      title](Component& target) {
          if (safe == nullptr)
            return;
          promptForCustomValue(title + ", in samples or a note name such as C3", safe->getTextFromValue(safe->getValue()),
                               target, [this, oscillator, component_index, safe](const String& text) {
            if (safe == nullptr)
              return;
            const int rate = atlas::wavetable_edit::fileSourceSampleRate(wavetableComponent(oscillator, component_index));
            Slider::ScopedDragNotification drag(*safe);
            safe->setValue(atlas::wavetable_edit::windowSizeFromText(text, rate), sendNotificationSync);
          });
        };
      }
      if (parameter.per_keyframe)
        keyframe_rows->push_back({ slider, parameter.id });
    };

    for (const auto& parameter : list)
      if (!parameter.per_keyframe)
        addParameterRow(parameter);

    Component::SafePointer<OffscreenSlider> position_slider;
    if (keyed) {
      const int count = component->numFrames();
      auto* keyframe = addSlider(prefix + " keyframe", "Choose which keyframe to edit. Each keyframe holds values at "
                                 "one timeline position, and positions between keyframes are interpolated",
                                 1.0, jmax(2, count), selected + 1, 1.0, "", 0);
      keyframe->setRange(1.0, jmax(1.0001, double(count)), 1.0);
      keyframe->setValue(selected + 1, dontSendNotification);
      keyframe->textFromValueFunction = [this, oscillator, component_index](double value) {
        return atlas::wavetable_edit::describeKeyframe(wavetableComponent(oscillator, component_index),
                                                       roundToInt(value) - 1);
      };
      auto* position = addSlider(prefix + " keyframe position", "Move the selected keyframe along the 256-position "
                                 "timeline; it stops before its neighbours", 1.0, edit::kNumPositions,
                                 component->getFrameAt(selected)->position() + 1, 1.0, "", 0);
      position_slider = position;
      keyframe->onValueChange = [this, oscillator, component_index, keyframe, refresh] {
        wavetable_keyboard_state_[oscillator].keyframes[component_index] = roundToInt(keyframe->getValue()) - 1;
        if (*refresh) (*refresh)();
      };
      position->onValueChange = [this, oscillator, component_index, position, refresh] {
        moveWavetableKeyframe(oscillator, component_index, roundToInt(position->getValue()) - 1);
        if (*refresh) (*refresh)();
      };
      addButton(prefix + " add keyframe", "Add a keyframe after the selected one, midway to the next or at the end "
                "of the timeline, holding the values found there",
                [this, oscillator, component_index, prefix] { addWavetableKeyframe(oscillator, component_index, prefix); });
      if (count > 1) {
        addButton(prefix + " remove keyframe", "Remove the selected keyframe",
                  [this, oscillator, component_index, prefix] { removeWavetableKeyframe(oscillator, component_index, prefix); });
        addButton(prefix + " spread keyframes evenly", "Space the keyframes evenly from the first to the last "
                  "timeline position, so the table morphs across its whole length",
                  [this, oscillator, component_index, prefix] { spreadWavetableKeyframes(oscillator, component_index, prefix); });
      }
      for (const auto& parameter : list)
        if (parameter.per_keyframe)
          addParameterRow(parameter);
    }

    // Line source points, shared in count by every keyframe.
    if (auto* line = dynamic_cast<WaveLineSource*>(component); line != nullptr && keyed) {
      const int points = edit::numLinePoints(line);
      state.line_point = jlimit(0, jmax(0, points - 1), state.line_point);
      auto* point = addSlider(prefix + " line point", "Choose a point of the line; every keyframe has the same points",
                              1.0, jmax(2, points), state.line_point + 1, 1.0, "", 0);
      point->textFromValueFunction = [points](double value) {
        return "Point " + String(roundToInt(value)) + " of " + String(points);
      };
      point->onValueChange = [this, oscillator, point, refresh] {
        wavetable_keyboard_state_[oscillator].line_point = roundToInt(point->getValue()) - 1;
        if (*refresh) (*refresh)();
      };
      struct LineField { const char* id; const char* title; const char* description; double minimum, maximum; const char* suffix; };
      const LineField fields[] = {
        { "phase", "point phase", "Where the point sits across the cycle; the first and last points stay at the ends",
          0.0, 100.0, "%" },
        { "level", "point level", "The wave's level at the point", -100.0, 100.0, "%" },
        { "curve", "point curve", "Bends the line from this point to the next; 0 is straight", -10.0, 10.0, "" },
      };
      for (const auto& field : fields) {
        auto* slider = addSlider(prefix + " " + field.title, field.description, field.minimum, field.maximum,
                                 edit::linePointValue(line, selected, state.line_point, field.id), 0.0, field.suffix, 1);
        slider->onValueChange = [this, oscillator, component_index, id = std::string(field.id), slider, refresh] {
          setWavetableLinePointValue(oscillator, component_index, id, slider->getValue());
          if (*refresh) (*refresh)();
        };
        line_rows->push_back({ slider, field.id });
      }
      addButton(prefix + " add line point", "Add a point after the selected one on every keyframe",
                [this, oscillator, component_index, prefix] { addWavetableLinePoint(oscillator, component_index, prefix); });
      if (points > 2)
        addButton(prefix + " remove line point", "Remove the selected point from every keyframe",
                  [this, oscillator, component_index, prefix] { removeWavetableLinePoint(oscillator, component_index, prefix); });
    }

    // After any change, show the selected keyframe's values (and ranges that
    // depend on other settings, such as the start position's end).
    *refresh = [this, oscillator, component_index, keyframe_rows, line_rows, position_slider] {
      auto* current = wavetableComponent(oscillator, component_index);
      if (current == nullptr)
        return;
      const auto& keyboard = wavetable_keyboard_state_[oscillator];
      const auto found = keyboard.keyframes.find(component_index);
      const int frame = found != keyboard.keyframes.end() ? found->second : 0;
      const auto parameters = atlas::wavetable_edit::parameters(current);
      for (auto& row : *keyframe_rows) {
        if (row.slider == nullptr)
          continue;
        if (const auto* parameter = atlas::wavetable_edit::findParameter(parameters, row.id))
          row.slider->setRange(parameter->minimum, jmax(parameter->minimum + 1.0e-6, parameter->maximum),
                               parameter->interval >= 1.0 ? parameter->interval : 0.0);
        row.slider->setValue(atlas::wavetable_edit::getValue(current, row.id, frame), dontSendNotification);
      }
      if (auto* line = dynamic_cast<WaveLineSource*>(current)) {
        for (auto& row : *line_rows)
          if (row.slider != nullptr)
            row.slider->setValue(atlas::wavetable_edit::linePointValue(line, frame, keyboard.line_point, row.id),
                                 dontSendNotification);
      }
      if (position_slider != nullptr && frame >= 0 && frame < current->numFrames())
        position_slider->setValue(current->getFrameAt(frame)->position() + 1, dontSendNotification);
    };
  };

  // The layer's source.
  const String source_name = edit::componentName(source);
  beginGroup("Layer " + String(state.layer + 1) + " source: " + source_name,
             "The source that generates this layer's frames");
  if (source == nullptr)
    addText("Source", "This layer has no source");
  else if (source->getType() == WavetableComponentFactory::kWaveSource) {
    auto* source = edit::waveSourceOfLayer(creator, state.layer);
    const int frames = jmax(1, source->numFrames());
    auto keyboard = std::make_unique<WavetableKeyboardComponent>();
    auto* keyboard_ptr = keyboard.get();
    keyboard_ptr->setTitle("Wavetable keyboard editor");
    keyboard_ptr->onKeyPressed = [this, oscillator](const KeyPress& key, Component& target) {
      return handleWavetableShortcut(oscillator, key, target);
    };
    keyboard_ptr->getStatusText = [this, oscillator] { return wavetableKeyboardStatusText(oscillator); };
    attach(std::move(keyboard), 62, 6, true);

    auto* scope = addCombo("Edit scope", "Choose whether waveform and harmonic edits affect the selected frame or all frames",
                           { "Current frame", "All frames" }, 1);

    auto& keyboard_state = state;
    keyboard_state.frame = jlimit(0, frames - 1, keyboard_state.frame);
    keyboard_state.harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, keyboard_state.harmonic);
    keyboard_state.starting_waveform = jlimit(0, vital::PredefinedWaveFrames::kNumShapes - 1,
                                               keyboard_state.starting_waveform);
    auto* frame_slider = addSlider("Frame", "Selected wavetable frame", 1.0, frames,
                                   keyboard_state.frame + 1.0, 1.0, "", 0);
    const int frame_position = source != nullptr && source->numFrames() > 0 ?
        source->getFrameAt(keyboard_state.frame)->position() : keyboard_state.frame;
    auto* frame_position_slider = addSlider(
        "Frame timeline position",
        "Move this source frame on the 256-position wavetable timeline without crossing adjacent source frames",
        1.0, vital::kNumOscillatorWaveFrames, frame_position + 1.0, 1.0, "", 0);

    auto selected_frame = [frame_slider] { return jmax(0, roundToInt(frame_slider->getValue()) - 1); };
    auto all_frames = [scope] { return scope->getSelectedId() == 2; };
    auto* starting_waveform = addCombo(
        "Starting waveform", "Choose a waveform to replace the current, selected, or all source frames",
        { "Sine", "Saturated sine", "Triangle", "Square", "Pulse 25 percent", "Saw" },
        keyboard_state.starting_waveform + 1);
    starting_waveform->setHelpText(
        "Choose a waveform, then press Apply starting waveform. On the keyboard editor, keys 1 through 6 apply these waveforms directly");
    starting_waveform->onChange = [this, oscillator, starting_waveform] {
      wavetable_keyboard_state_[oscillator].starting_waveform = starting_waveform->getSelectedId() - 1;
    };
    addButton("Apply starting waveform", "Replace the chosen frame scope with the selected starting waveform",
              [this, oscillator, selected_frame, all_frames, starting_waveform] {
                applyWavetableStartingShape(oscillator, selected_frame(), all_frames(),
                                            starting_waveform->getSelectedId() - 1);
              });

    int interpolation = 0;
    if (source != nullptr && source->getInterpolationStyle() != WavetableComponent::kNone)
      interpolation = 2 * source->getInterpolationStyle() + source->getInterpolationMode() - 1;
    String interpolation_description =
        "How Atlas generates timeline positions between source frames; this does not alter adjacent source frames";
    const int interpolated_positions = edit::interpolatedPositions(source);
    if (source != nullptr && interpolated_positions == 0)
      interpolation_description +=
          ". This wavetable currently has no empty positions between source frames, so the modes sound identical";
    else if (interpolated_positions > 0)
      interpolation_description += ". Currently affects " + String(interpolated_positions) + " generated positions";
    auto* interpolation_combo = addCombo(
        "Frame interpolation", interpolation_description,
        { "None", "Waveform", "Spectral", "Smooth waveform", "Smooth spectral" }, interpolation + 1);
    interpolation_combo->setHelpText(
        "Waveform blends samples, Spectral blends harmonics, and Smooth uses curved rather than linear movement. Move source frames apart to create interpolated positions");
    interpolation_combo->onChange = [this, oscillator, interpolation_combo] {
      setWavetableInterpolation(oscillator, interpolation_combo->getSelectedId() - 1);
    };

    auto* harmonic_slider = addSlider("Harmonic", "Selected harmonic number", 1.0,
                                      vital::WaveFrame::kNumRealComplex - 2,
                                      keyboard_state.harmonic, 1.0, "", 0);
    frame_slider->onValueChange = [this, oscillator, frame_slider, frame_position_slider] {
      wavetable_keyboard_state_[oscillator].frame = jmax(0, roundToInt(frame_slider->getValue()) - 1);
      if (auto* source = currentWaveSource(oscillator);
          source != nullptr && source->numFrames() > 0) {
        const int frame = jlimit(0, source->numFrames() - 1, wavetable_keyboard_state_[oscillator].frame);
        frame_position_slider->setValue(source->getFrameAt(frame)->position() + 1,
                                        dontSendNotification);
      }
      else {
        const int frame = wavetable_keyboard_state_[oscillator].frame;
        frame_position_slider->setValue(frame + 1.0, dontSendNotification);
      }
    };
    frame_position_slider->onValueChange = [this, oscillator, frame_slider, frame_position_slider] {
      const int frame = jmax(0, roundToInt(frame_slider->getValue()) - 1);
      setWavetableFramePosition(oscillator, frame,
                                jmax(0, roundToInt(frame_position_slider->getValue()) - 1));
      if (auto* source = currentWaveSource(oscillator);
          source != nullptr && source->numFrames() > 0) {
        const int current = jlimit(0, source->numFrames() - 1, wavetable_keyboard_state_[oscillator].frame);
        frame_position_slider->setValue(source->getFrameAt(current)->position() + 1,
                                        dontSendNotification);
      }
    };
    harmonic_slider->onValueChange = [this, oscillator, harmonic_slider] {
      wavetable_keyboard_state_[oscillator].harmonic = roundToInt(harmonic_slider->getValue());
    };
    auto* level_slider = addSlider("Harmonic level", "Harmonic level as percent", 0.0, 400.0,
                                   100.0, 1.0, "%", 0);
    auto* phase_slider = addSlider("Harmonic phase", "Harmonic phase in degrees", -180.0, 180.0,
                                   0.0, 1.0, " degrees", 0);

    auto selected_harmonic = [harmonic_slider] { return roundToInt(harmonic_slider->getValue()); };
    addButton("Set harmonic", "Set or add the selected harmonic at the chosen level and phase",
              [this, oscillator, selected_frame, all_frames, selected_harmonic, level_slider, phase_slider] {
                applyWavetableHarmonicEdit(oscillator, selected_frame(), all_frames(), selected_harmonic(),
                                           static_cast<float>(level_slider->getValue()),
                                           static_cast<float>(phase_slider->getValue()));
              });
    addButton("Clear harmonic", "Clear the selected harmonic",
              [this, oscillator, selected_frame, all_frames, selected_harmonic] {
                clearWavetableHarmonic(oscillator, selected_frame(), all_frames(), selected_harmonic());
              });
    addButton("Remove fundamental", "Remove harmonic 1",
              [this, oscillator, selected_frame, all_frames] {
                removeWavetableFundamental(oscillator, selected_frame(), all_frames());
              });
    addButton("Normalize wavetable", "Normalize the current frame or all frames",
              [this, oscillator, selected_frame, all_frames] {
                normalizeWavetableFrame(oscillator, selected_frame(), all_frames());
              });

    addButton("Add frame", "Add a copy of the selected frame after it, midway to the next frame or at the end of "
              "the timeline, so interpolation has positions to fill", [this, oscillator] { addWavetableFrame(oscillator); });
    if (frames > 1) {
      addButton("Remove frame", "Remove the selected frames", [this, oscillator] { deleteWavetableFrames(oscillator); });
      addButton("Spread frames evenly", "Space the frames evenly from the first to the last timeline position, so "
                "interpolation morphs between them across the whole table",
                [this, oscillator] { spreadWavetableKeyframes(oscillator, 0, "Frame"); });
    }
  }
  else {
    addComponentRows(0, source, "Source");
    if (source->getType() == WavetableComponentFactory::kFileSource) {
      const int samples = edit::fileSourceSamples(source);
      const int rate = edit::fileSourceSampleRate(source);
      addText("Audio file", samples > 0 && rate > 0 ?
              "Audio: " + String(double(samples) / rate, 2) + " seconds, " + String(samples) + " samples at " +
              String(rate) + " Hz" : String("No audio loaded; use Replace audio"));
      addButton("Replace audio", "Load another audio file into this layer, keeping its keyframes and settings",
                [this, oscillator] { chooseWavetableLayerAudio(oscillator); });
    }
    addButton("Flatten wavetable to editable frames",
              "Replace every layer, modifier and imported audio with the rendered table as wave source frames, so "
              "each frame can be edited harmonic by harmonic. This cannot be undone",
              [this, oscillator] { flattenWavetableToFrames(oscillator); });
  }
  endGroup();

  // The layer's modifiers.
  for (int component_index = 1; group != nullptr && component_index < group->numComponents(); ++component_index) {
    auto* component = group->getComponent(component_index);
    if (component == nullptr)
      continue;
    const String name = edit::componentName(component);
    const String prefix = name + " " + String(component_index);
    beginGroup("Modifier " + String(component_index) + ": " + name,
               "Wavetable modifier " + String(component_index) + " of layer " + String(state.layer + 1) + ", " + name);
    addComponentRows(component_index, component, prefix);
    if (component_index > 1)
      addButton(prefix + " move up", "Apply this modifier earlier",
                [this, oscillator, component_index] { moveWavetableModifier(oscillator, component_index, -1); });
    if (component_index < group->numComponents() - 1)
      addButton(prefix + " move down", "Apply this modifier later",
                [this, oscillator, component_index] { moveWavetableModifier(oscillator, component_index, 1); });
    addButton(prefix + " reset", "Reset this modifier to one keyframe of default values",
              [this, oscillator, component_index] { resetWavetableModifier(oscillator, component_index); });
    addButton(prefix + " remove", "Remove this modifier",
              [this, oscillator, component_index] { removeWavetableModifier(oscillator, component_index); });
    endGroup();
  }

  auto* add_modifier = addCombo("Add modifier", "Choose a modifier to add to the selected layer",
                                edit::modifierTypeNames(), state.add_modifier_type + 1);
  add_modifier->onChange = [this, oscillator, add_modifier] {
    wavetable_keyboard_state_[oscillator].add_modifier_type = add_modifier->getSelectedId() - 1;
  };
  addButton("Add selected modifier", "Add the chosen modifier to the selected layer",
            [this, oscillator, add_modifier] { addWavetableModifier(oscillator, add_modifier->getSelectedId() - 1); });
  return finish();
}

int SynthEditor::wavetableFrameCount(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return 1;

  if (auto* creator = synth_.getWavetableCreator(oscillator)) {
    if (auto* source = currentWaveSource(oscillator))
      return jmax(1, source->numFrames());
    if (auto* wavetable = creator->getWavetable())
      return jmax(1, wavetable->numFrames());
  }
  return 1;
}

String SynthEditor::wavetableEditorSummary(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return "Wavetable editor";

  String name = "unknown";
  if (auto* creator = synth_.getWavetableCreator(oscillator)) {
    if (!creator->getName().empty())
      name = String(creator->getName());
  }

  return "Wavetable editor, " + name + ", " + String(wavetableFrameCount(oscillator)) + " frames";
}

std::vector<int> SynthEditor::wavetableTargetFrameIndices(int oscillator, int frameCount) const {
  std::vector<int> result;
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators) || frameCount <= 0)
    return result;

  const auto& state = wavetable_keyboard_state_[oscillator];
  for (int frame : state.selected_frames) {
    if (isPositiveAndBelow(frame, frameCount))
      result.push_back(frame);
  }
  if (result.empty())
    result.push_back(jlimit(0, frameCount - 1, state.frame));
  return result;
}

void SynthEditor::pruneWavetableFrameSelection(int oscillator, int frameCount) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto& state = wavetable_keyboard_state_[oscillator];
  state.frame = jlimit(0, jmax(0, frameCount - 1), state.frame);
  for (auto it = state.selected_frames.begin(); it != state.selected_frames.end();) {
    if (!isPositiveAndBelow(*it, frameCount))
      it = state.selected_frames.erase(it);
    else
      ++it;
  }
}

String SynthEditor::wavetableKeyboardStatusText(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return "Wavetable keyboard editor unavailable";

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = currentWaveSource(oscillator);
  if (creator == nullptr)
    return "Wavetable keyboard editor unavailable";
  if (source == nullptr || source->numFrames() == 0)
    return "This layer has no frames to edit; choose a wave source layer, or flatten the wavetable to editable frames";

  return wavetableFrameStatusText(oscillator) + ", " +
         wavetableHarmonicStatusText(oscillator, true, true) + ", " +
         wavetableInterpolationName(oscillator);
}

String SynthEditor::wavetableFrameStatusText(int oscillator) const {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ?
                      synth_.getWavetableCreator(oscillator) : nullptr;
  auto* source = currentWaveSource(oscillator);
  if (source == nullptr || source->numFrames() == 0)
    return "No editable frames";

  const auto& state = wavetable_keyboard_state_[oscillator];
  const int frame = jlimit(0, source->numFrames() - 1, state.frame);
  auto* keyframe = source->getKeyframe(frame);
  int selected = 0;
  for (int selected_frame : state.selected_frames) {
    if (isPositiveAndBelow(selected_frame, source->numFrames()))
      ++selected;
  }

  String selection;
  if (selected == 1)
    selection = ", 1 selected";
  else if (selected > 1)
    selection = ", " + String(selected) + " selected";

  return "Frame " + String(frame + 1) + " of " + String(source->numFrames()) +
         ", position " + String(keyframe->position() + 1) + selection;
}

String SynthEditor::wavetableHarmonicStatusText(int oscillator, bool includeLevel, bool includePhase) const {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ?
                      synth_.getWavetableCreator(oscillator) : nullptr;
  auto* source = currentWaveSource(oscillator);
  if (source == nullptr || source->numFrames() == 0)
    return "No editable harmonics";

  const auto& state = wavetable_keyboard_state_[oscillator];
  const int frame = jlimit(0, source->numFrames() - 1, state.frame);
  const int harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, state.harmonic);
  const auto* wave_frame = source->getKeyframe(frame)->wave_frame();
  const float scale = vital::WaveFrame::kWaveformSize * 0.5f;
  const float level = 100.0f * std::abs(wave_frame->frequency_domain[harmonic]) / scale;
  float phase = std::arg(wave_frame->frequency_domain[harmonic]) * 180.0f / vital::kPi;
  if (std::abs(level) < 0.0001f)
    phase = 0.0f;

  String result = "Harmonic " + String(harmonic);
  if (includeLevel)
    result += ", " + String(level, 1) + "%";
  if (includePhase)
    result += ", phase " + String(phase, 1) + " degrees";
  return result;
}

String SynthEditor::wavetableInterpolationName(int oscillator) const {
  static const char* const interpolation_names[] = {
    "No interpolation", "Waveform blend", "Spectral blend",
    "Smooth waveform blend", "Smooth spectral blend"
  };
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ?
                      synth_.getWavetableCreator(oscillator) : nullptr;
  auto* source = currentWaveSource(oscillator);
  if (source == nullptr)
    return interpolation_names[0];

  int interpolation = 0;
  if (source->getInterpolationStyle() != WavetableComponent::kNone)
    interpolation = 2 * source->getInterpolationStyle() + source->getInterpolationMode() - 1;
  return interpolation_names[jlimit(0, 4, interpolation)];
}

void SynthEditor::moveWavetableFrameCursor(int oscillator, int direction, bool extendSelection) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (source == nullptr || source->numFrames() == 0)
    return;

  auto& state = wavetable_keyboard_state_[oscillator];
  pruneWavetableFrameSelection(oscillator, source->numFrames());
  const int previous = state.frame;
  state.frame = jlimit(0, source->numFrames() - 1, state.frame + direction);
  if (extendSelection) {
    const int first = jmin(previous, state.frame);
    const int last = jmax(previous, state.frame);
    for (int frame = first; frame <= last; ++frame)
      state.selected_frames.insert(frame);
  }
  postWavetableEditorAnnouncement(wavetableFrameStatusText(oscillator),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::moveWavetableHarmonicCursor(int oscillator, int direction) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  state.harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, state.harmonic + direction);
  postWavetableEditorAnnouncement(wavetableHarmonicStatusText(oscillator, true, true),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::toggleWavetableFrameSelection(int oscillator) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (source == nullptr || source->numFrames() == 0)
    return;

  auto& state = wavetable_keyboard_state_[oscillator];
  pruneWavetableFrameSelection(oscillator, source->numFrames());
  const bool selected = state.selected_frames.erase(state.frame) == 0;
  if (selected)
    state.selected_frames.insert(state.frame);
  postWavetableEditorAnnouncement("Frame " + String(state.frame + 1) + (selected ? " selected, " : " unselected, ") +
                                      String(state.selected_frames.size()) + " selected",
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::clearWavetableFrameSelection(int oscillator) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  wavetable_keyboard_state_[oscillator].selected_frames.clear();
  postWavetableEditorAnnouncement("Frame selection cleared",
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::selectAllWavetableFrames(int oscillator) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (source == nullptr || source->numFrames() == 0)
    return;

  auto& selected = wavetable_keyboard_state_[oscillator].selected_frames;
  selected.clear();
  for (int frame = 0; frame < source->numFrames(); ++frame)
    selected.insert(frame);
  postWavetableEditorAnnouncement("Selected all " + String(source->numFrames()) + " wavetable frames",
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::adjustWavetableHarmonicLevel(int oscillator, float deltaPercent) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  pruneWavetableFrameSelection(oscillator, source->numFrames());
  const auto targets = wavetableTargetFrameIndices(oscillator, source->numFrames());
  const int harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2,
                              wavetable_keyboard_state_[oscillator].harmonic);
  const float scale = vital::WaveFrame::kWaveformSize * 0.5f;

  for (int frame : targets) {
    auto* wave_frame = source->getKeyframe(frame)->wave_frame();
    const float current = 100.0f * std::abs(wave_frame->frequency_domain[harmonic]) / scale;
    const float level = jlimit(0.0f, 400.0f, current + deltaPercent);
    const float phase = std::arg(wave_frame->frequency_domain[harmonic]);
    wave_frame->frequency_domain[harmonic] = std::polar(level * 0.01f * scale, phase);
    wave_frame->toTimeDomain();
    wave_frame->normalize();
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement(wavetableHarmonicStatusText(oscillator, true, false) +
                                      (targets.size() > 1 ? ", " + String(targets.size()) + " frames" : ""),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::adjustWavetableHarmonicPhase(int oscillator, float deltaDegrees) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  pruneWavetableFrameSelection(oscillator, source->numFrames());
  const auto targets = wavetableTargetFrameIndices(oscillator, source->numFrames());
  const int harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2,
                              wavetable_keyboard_state_[oscillator].harmonic);
  const float delta = deltaDegrees * vital::kPi / 180.0f;

  for (int frame : targets) {
    auto* wave_frame = source->getKeyframe(frame)->wave_frame();
    const float level = std::abs(wave_frame->frequency_domain[harmonic]);
    const float phase = std::arg(wave_frame->frequency_domain[harmonic]) + delta;
    wave_frame->frequency_domain[harmonic] = std::polar(level, phase);
    wave_frame->toTimeDomain();
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement(wavetableHarmonicStatusText(oscillator, false, true) +
                                      (targets.size() > 1 ? ", " + String(targets.size()) + " frames" : ""),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::copyWavetableFrames(int oscillator) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (source == nullptr || source->numFrames() == 0)
    return;

  pruneWavetableFrameSelection(oscillator, source->numFrames());
  const auto targets = wavetableTargetFrameIndices(oscillator, source->numFrames());
  copied_wavetable_frames_.clear();
  for (int frame : targets) {
    const auto* wave_frame = source->getKeyframe(frame)->wave_frame();
    CopiedWavetableFrame copy;
    copy.samples.assign(wave_frame->time_domain,
                        wave_frame->time_domain + vital::WaveFrame::kWaveformSize);
    copy.frequency_ratio = wave_frame->frequency_ratio;
    copy.sample_rate = wave_frame->sample_rate;
    copied_wavetable_frames_.push_back(std::move(copy));
  }
  postWavetableEditorAnnouncement("Copied " + String(copied_wavetable_frames_.size()) + " wavetable frame" +
                                  (copied_wavetable_frames_.size() == 1 ? String() : String("s")),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::pasteWavetableFrames(int oscillator) {
  if (copied_wavetable_frames_.empty()) {
    postWavetableEditorAnnouncement("No wavetable frames copied", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  pruneWavetableFrameSelection(oscillator, source->numFrames());
  auto targets = wavetableTargetFrameIndices(oscillator, source->numFrames());
  if (wavetable_keyboard_state_[oscillator].selected_frames.empty() && copied_wavetable_frames_.size() > 1) {
    targets.clear();
    const int first = wavetable_keyboard_state_[oscillator].frame;
    for (int i = 0; i < static_cast<int>(copied_wavetable_frames_.size()) && first + i < source->numFrames(); ++i)
      targets.push_back(first + i);
  }

  for (int i = 0; i < static_cast<int>(targets.size()); ++i) {
    const auto& copy = copied_wavetable_frames_[jmin(i, static_cast<int>(copied_wavetable_frames_.size()) - 1)];
    auto* wave_frame = source->getKeyframe(targets[i])->wave_frame();
    if (copy.samples.size() == vital::WaveFrame::kWaveformSize) {
      std::copy(copy.samples.begin(), copy.samples.end(), wave_frame->time_domain);
      wave_frame->setFrequencyRatio(copy.frequency_ratio);
      wave_frame->setSampleRate(copy.sample_rate);
      wave_frame->toFrequencyDomain();
    }
  }
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement("Pasted into " + String(targets.size()) + " wavetable frame" +
                                  (targets.size() == 1 ? String() : String("s")),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::addWavetableFrame(int oscillator) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;
  if (source->numFrames() >= vital::kNumOscillatorWaveFrames) {
    postWavetableEditorAnnouncement("Wavetable already has the maximum number of frames",
                                    AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  auto& state = wavetable_keyboard_state_[oscillator];
  pruneWavetableFrameSelection(oscillator, source->numFrames());
  auto* current = source->getKeyframe(state.frame);
  // Midway to the next frame, or at the end of the timeline after the last one,
  // so interpolation has positions to fill between the frames.
  const int position = atlas::wavetable_edit::suggestKeyframePosition(source, state.frame);
  if (position < 0) {
    postWavetableEditorAnnouncement("No free timeline position is available for another frame",
                                    AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  auto* inserted = source->insertNewKeyframe(position);
  inserted->copy(current);
  auto* inserted_wave = source->getKeyframe(source->indexOf(inserted))->wave_frame();
  const auto* current_wave = source->getKeyframe(source->indexOf(current))->wave_frame();
  inserted_wave->setFrequencyRatio(current_wave->frequency_ratio);
  inserted_wave->setSampleRate(current_wave->sample_rate);
  state.frame = source->indexOf(inserted);
  state.selected_frames.clear();
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement("Frame added, " + wavetableFrameStatusText(oscillator),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::deleteWavetableFrames(int oscillator) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  pruneWavetableFrameSelection(oscillator, source->numFrames());
  auto targets = wavetableTargetFrameIndices(oscillator, source->numFrames());
  if (static_cast<int>(targets.size()) >= source->numFrames()) {
    postWavetableEditorAnnouncement("A wavetable must keep at least one frame",
                                    AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  std::sort(targets.rbegin(), targets.rend());
  const int next_cursor = targets.back();

  for (int frame : targets)
    source->remove(source->getFrameAt(frame));
  auto& state = wavetable_keyboard_state_[oscillator];
  state.frame = jlimit(0, source->numFrames() - 1, next_cursor);
  state.selected_frames.clear();
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement("Deleted " + String(targets.size()) + " wavetable frame" +
                                  (targets.size() == 1 ? String() : String("s")) + ", " +
                                  wavetableFrameStatusText(oscillator),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::setWavetableFramePosition(int oscillator, int frame, int position) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  frame = jlimit(0, source->numFrames() - 1, frame);
  auto* keyframe = source->getFrameAt(frame);
  const int minimum = frame > 0 ? source->getFrameAt(frame - 1)->position() + 1 : 0;
  const int maximum = frame + 1 < source->numFrames() ?
      source->getFrameAt(frame + 1)->position() - 1 : vital::kNumOscillatorWaveFrames - 1;
  const int target = jlimit(minimum, maximum, position);
  if (target == keyframe->position()) {
    if (target != position)
      postWavetableEditorAnnouncement("Frame cannot cross the adjacent source frame",
                                      AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  keyframe->setPosition(target);
  source->reposition(keyframe);
  wavetable_keyboard_state_[oscillator].frame = source->indexOf(keyframe);
  creator->render();
  synth_.markStateChanged();
}

void SynthEditor::clearWavetableDc(int oscillator) {
  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  pruneWavetableFrameSelection(oscillator, source->numFrames());
  const auto targets = wavetableTargetFrameIndices(oscillator, source->numFrames());
  for (int frame : targets) {
    auto* wave_frame = source->getKeyframe(frame)->wave_frame();
    wave_frame->frequency_domain[0] = 0.0f;
    wave_frame->toTimeDomain();
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement("Removed DC offset from " + String(targets.size()) + " wavetable frame" +
                                  (targets.size() == 1 ? String() : String("s")),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::setWavetableInterpolation(int oscillator, int interpolation) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  // Only the selected wave source layer changes: nothing is converted or flattened.
  auto* source = editableWaveSource(oscillator);
  if (source == nullptr)
    return;
  atlas::wavetable_edit::setValue(source, "interpolation", 0, jlimit(0, 4, interpolation));
  synth_.getWavetableCreator(oscillator)->render();
  if (atlas::wavetable_edit::interpolatedPositions(source) == 0 && source->numFrames() > 1)
    postWavetableEditorAnnouncement("The frames sit on neighbouring positions, so there is nothing between them to interpolate. "
                                    "Use Spread frames evenly or move a frame's timeline position",
                                    AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::cycleWavetableInterpolation(int oscillator, int direction) {
  auto* source = editableWaveSource(oscillator);
  if (source == nullptr)
    return;
  const int current = int(atlas::wavetable_edit::getValue(source, "interpolation", 0));
  setWavetableInterpolation(oscillator, (current + direction + 5) % 5);
  postWavetableEditorAnnouncement("Interpolation: " + wavetableInterpolationName(oscillator),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::rebuildWavetableEditor(int oscillator, const String& focus_title) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  const String section = "Oscillator " + String(oscillator + 1);
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this, section, focus_title] {
    if (safe_this == nullptr)
      return;
    safe_this->showAllSections(false);
    safe_this->restoreFocusAfterRebuild({}, nullptr, focus_title, section, true);
  });
}

void SynthEditor::addWavetableModifier(int oscillator, int modifier_type) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  const int layer = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? wavetable_keyboard_state_[oscillator].layer : 0;
  auto* group = creator != nullptr && isPositiveAndBelow(layer, creator->numGroups()) ? creator->getGroup(layer) : nullptr;
  if (group == nullptr) {
    postWavetableEditorAnnouncement("No wavetable layer is available", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  auto* component = atlas::wavetable_edit::addModifier(*group, modifier_type);
  if (component == nullptr)
    return;
  creator->render();
  synth_.markStateChanged();
  const int component_index = group->numComponents() - 1;
  const String name = atlas::wavetable_edit::componentName(component);
  postWavetableEditorAnnouncement(name + " added to layer " + String(layer + 1), AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, name + " " + String(component_index) + " keyframe interpolation");
}

void SynthEditor::removeWavetableModifier(int oscillator, int component_index) {
  auto* component = wavetableComponent(oscillator, component_index);
  if (component == nullptr || component_index < 1)
    return;
  auto* creator = synth_.getWavetableCreator(oscillator);
  const String name = atlas::wavetable_edit::componentName(component);
  const int layer = jlimit(0, creator->numGroups() - 1, wavetable_keyboard_state_[oscillator].layer);
  creator->getGroup(layer)->removeComponent(component_index);
  wavetable_keyboard_state_[oscillator].keyframes.clear();
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement(name + " removed", AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, "Add modifier");
}

void SynthEditor::resetWavetableModifier(int oscillator, int component_index) {
  auto* component = wavetableComponent(oscillator, component_index);
  if (component == nullptr)
    return;
  const String name = atlas::wavetable_edit::componentName(component);
  component->reset();
  wavetable_keyboard_state_[oscillator].keyframes[component_index] = 0;
  synth_.getWavetableCreator(oscillator)->render();
  postWavetableEditorAnnouncement(name + " reset", AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, name + " " + String(component_index) + " reset");
}

void SynthEditor::moveWavetableModifier(int oscillator, int component_index, int direction) {
  auto* component = wavetableComponent(oscillator, component_index);
  if (component == nullptr)
    return;
  auto* creator = synth_.getWavetableCreator(oscillator);
  const int layer = jlimit(0, creator->numGroups() - 1, wavetable_keyboard_state_[oscillator].layer);
  auto* group = creator->getGroup(layer);
  const int destination = component_index + (direction < 0 ? -1 : 1);
  if (destination < 1 || destination >= group->numComponents())
    return;
  const String name = atlas::wavetable_edit::componentName(component);
  if (direction < 0)
    group->moveUp(component_index);
  else
    group->moveDown(component_index);
  auto& keyframes = wavetable_keyboard_state_[oscillator].keyframes;
  std::swap(keyframes[component_index], keyframes[destination]);
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement(name + (direction < 0 ? " moved up" : " moved down"), AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, name + " " + String(destination) + " keyframe interpolation");
}

WaveSource* SynthEditor::currentWaveSource(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return nullptr;
  auto* creator = synth_.getWavetableCreator(oscillator);
  const int layers = creator != nullptr ? creator->numGroups() : 0;
  return atlas::wavetable_edit::waveSourceOfLayer(creator, jlimit(0, jmax(0, layers - 1),
                                                                  wavetable_keyboard_state_[oscillator].layer));
}

WaveSource* SynthEditor::editableWaveSource(int oscillator) {
  if (auto* source = currentWaveSource(oscillator))
    return source;
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  if (creator != nullptr) {
    const int layer = wavetable_keyboard_state_[oscillator].layer;
    auto* component = wavetableComponent(oscillator, 0);
    postWavetableEditorAnnouncement("Layer " + String(layer + 1) + " is " +
                                    (component != nullptr ? "an " + atlas::wavetable_edit::componentName(component).toLowerCase()
                                                          : String("empty")) +
                                    ". Frames and harmonics are edited on a wave source layer; add one, or flatten the wavetable to editable frames",
                                    AccessibilityHandler::AnnouncementPriority::high);
  }
  return nullptr;
}

WavetableComponent* SynthEditor::wavetableComponent(int oscillator, int componentIndex) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return nullptr;
  auto* creator = synth_.getWavetableCreator(oscillator);
  if (creator == nullptr || creator->numGroups() == 0)
    return nullptr;
  const int layer = jlimit(0, creator->numGroups() - 1, wavetable_keyboard_state_[oscillator].layer);
  auto* group = creator->getGroup(layer);
  if (group == nullptr || !isPositiveAndBelow(componentIndex, group->numComponents()))
    return nullptr;
  return group->getComponent(componentIndex);
}

void SynthEditor::selectWavetableLayer(int oscillator, int layer) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  if (creator == nullptr || !isPositiveAndBelow(layer, creator->numGroups()))
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  if (state.layer == layer)
    return;
  state.layer = layer;
  state.frame = 0;
  state.selected_frames.clear();
  state.keyframes.clear();
  state.line_point = 0;
  rebuildWavetableEditor(oscillator, "Layer");
}

void SynthEditor::addWavetableLayer(int oscillator, int sourceType) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  if (creator == nullptr)
    return;
  if (sourceType == WavetableComponentFactory::kFileSource) {
    // An audio layer needs audio: ask for the file first.
    wavetable_chooser_ = std::make_unique<FileChooser>("Add an audio file layer", LoadSave::getUserWavetableDirectory(),
                                                       "*.wav;*.flac;*.aif;*.aiff");
    launchFileChooser(*wavetable_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                           [this, oscillator](const FileChooser& chooser) {
      const File file = chooser.getResult();
      wavetable_chooser_.reset();
      if (file.existsAsFile())
        addWavetableAudioLayer(oscillator, file);
    });
    return;
  }
  if (atlas::wavetable_edit::addLayer(*creator, sourceType) == nullptr)
    return;
  creator->render();
  synth_.markStateChanged();
  auto& state = wavetable_keyboard_state_[oscillator];
  state.layer = creator->numGroups() - 1;
  state.frame = 0;
  state.selected_frames.clear();
  state.keyframes.clear();
  postWavetableEditorAnnouncement("Added layer " + String(state.layer + 1) + ", " +
                                  atlas::wavetable_edit::sourceTypeNames()[sourceType].toLowerCase(),
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, "Layer");
}

namespace {
  bool decodeAudioFile(const File& file, AudioSampleBuffer& buffer, int& sample_rate) {
    auto input_stream = std::make_unique<FileInputStream>(file);
    if (!input_stream->openedOk())
      return false;
    sample_rate = loadAudioFile(buffer, input_stream.release());
    return sample_rate > 0 && buffer.getNumSamples() > 0;
  }
}

void SynthEditor::addWavetableAudioLayer(int oscillator, const File& file) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  AudioSampleBuffer buffer;
  int sample_rate = 0;
  if (creator == nullptr || !decodeAudioFile(file, buffer, sample_rate)) {
    postWavetableEditorAnnouncement("Unable to load " + file.getFileName(), AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  auto* group = atlas::wavetable_edit::addLayer(*creator, WavetableComponentFactory::kFileSource);
  if (group == nullptr)
    return;
  atlas::wavetable_edit::loadAudioLayer(group->getComponent(0), buffer.getReadPointer(0), buffer.getNumSamples(), sample_rate);
  creator->render();
  synth_.markStateChanged();
  auto& state = wavetable_keyboard_state_[oscillator];
  state.layer = creator->numGroups() - 1;
  state.keyframes.clear();
  postWavetableEditorAnnouncement("Added layer " + String(state.layer + 1) + " from " + file.getFileNameWithoutExtension(),
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, "Layer");
}

void SynthEditor::chooseWavetableLayerAudio(int oscillator) {
  wavetable_chooser_ = std::make_unique<FileChooser>("Replace the layer's audio", LoadSave::getUserWavetableDirectory(),
                                                     "*.wav;*.flac;*.aif;*.aiff");
  launchFileChooser(*wavetable_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                         [this, oscillator](const FileChooser& chooser) {
    const File file = chooser.getResult();
    wavetable_chooser_.reset();
    if (file.existsAsFile())
      replaceWavetableLayerAudio(oscillator, file);
  });
}

void SynthEditor::replaceWavetableLayerAudio(int oscillator, const File& file) {
  auto* component = wavetableComponent(oscillator, 0);
  AudioSampleBuffer buffer;
  int sample_rate = 0;
  if (component == nullptr || !decodeAudioFile(file, buffer, sample_rate) ||
      !atlas::wavetable_edit::replaceFileSourceAudio(component, buffer.getReadPointer(0), buffer.getNumSamples(), sample_rate)) {
    postWavetableEditorAnnouncement("Unable to load " + file.getFileName(), AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  synth_.getWavetableCreator(oscillator)->render();
  postWavetableEditorAnnouncement("Layer audio replaced with " + file.getFileNameWithoutExtension(),
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, "Replace audio");
}

void SynthEditor::removeWavetableLayer(int oscillator) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  if (creator == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  const int removed = state.layer;
  if (!atlas::wavetable_edit::removeLayer(*creator, removed)) {
    postWavetableEditorAnnouncement("A wavetable keeps at least one layer", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  creator->render();
  synth_.markStateChanged();
  state.layer = jlimit(0, creator->numGroups() - 1, removed);
  state.frame = 0;
  state.selected_frames.clear();
  state.keyframes.clear();
  postWavetableEditorAnnouncement("Removed layer " + String(removed + 1), AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, "Layer");
}

void SynthEditor::moveWavetableLayer(int oscillator, int direction) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  if (creator == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  if (!atlas::wavetable_edit::moveLayer(*creator, state.layer, direction))
    return;
  state.layer += direction < 0 ? -1 : 1;
  creator->render();
  synth_.markStateChanged();
  postWavetableEditorAnnouncement("Layer moved to position " + String(state.layer + 1), AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, direction < 0 ? "Move layer earlier" : "Move layer later");
}

void SynthEditor::setWavetableComponentValue(int oscillator, int componentIndex, const std::string& id, double value) {
  auto* component = wavetableComponent(oscillator, componentIndex);
  if (component == nullptr)
    return;
  const auto& keyframes = wavetable_keyboard_state_[oscillator].keyframes;
  const auto found = keyframes.find(componentIndex);
  const int keyframe = found != keyframes.end() ? found->second : 0;
  if (atlas::wavetable_edit::setValue(component, id, keyframe, value))
    synth_.getWavetableCreator(oscillator)->render();
}

void SynthEditor::moveWavetableKeyframe(int oscillator, int componentIndex, int position) {
  auto* component = wavetableComponent(oscillator, componentIndex);
  if (component == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  const int keyframe = jlimit(0, jmax(0, component->numFrames() - 1), state.keyframes[componentIndex]);
  const int previous = component->numFrames() > 0 ? component->getFrameAt(keyframe)->position() : -1;
  const int moved = atlas::wavetable_edit::moveKeyframe(component, keyframe, position);
  if (moved < 0)
    return;
  if (moved != position)
    postWavetableEditorAnnouncement("Keyframes cannot cross their neighbours", AccessibilityHandler::AnnouncementPriority::medium);
  if (moved != previous)
    synth_.getWavetableCreator(oscillator)->render();
}

void SynthEditor::addWavetableKeyframe(int oscillator, int componentIndex, const String& focusPrefix) {
  auto* component = wavetableComponent(oscillator, componentIndex);
  if (component == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  const int after = jlimit(0, jmax(0, component->numFrames() - 1), state.keyframes[componentIndex]);
  const int position = atlas::wavetable_edit::suggestKeyframePosition(component, after);
  const int added = position >= 0 ? atlas::wavetable_edit::addKeyframe(component, position) : -1;
  if (added < 0) {
    postWavetableEditorAnnouncement("No free timeline position for another keyframe", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  state.keyframes[componentIndex] = added;
  synth_.getWavetableCreator(oscillator)->render();
  postWavetableEditorAnnouncement("Added " + atlas::wavetable_edit::describeKeyframe(component, added).toLowerCase(),
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, focusPrefix + " keyframe");
}

void SynthEditor::removeWavetableKeyframe(int oscillator, int componentIndex, const String& focusPrefix) {
  auto* component = wavetableComponent(oscillator, componentIndex);
  if (component == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  const int keyframe = jlimit(0, jmax(0, component->numFrames() - 1), state.keyframes[componentIndex]);
  if (!atlas::wavetable_edit::removeKeyframe(component, keyframe)) {
    postWavetableEditorAnnouncement("The last keyframe stays", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  state.keyframes[componentIndex] = jlimit(0, component->numFrames() - 1, keyframe);
  synth_.getWavetableCreator(oscillator)->render();
  postWavetableEditorAnnouncement("Keyframe removed, " + atlas::wavetable_edit::describeKeyframe(component, state.keyframes[componentIndex]),
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, focusPrefix + " keyframe");
}

void SynthEditor::spreadWavetableKeyframes(int oscillator, int componentIndex, const String& focusPrefix) {
  auto* component = wavetableComponent(oscillator, componentIndex);
  if (component == nullptr || !atlas::wavetable_edit::spreadKeyframes(component))
    return;
  synth_.getWavetableCreator(oscillator)->render();
  const bool frames = component->getType() == WavetableComponentFactory::kWaveSource;
  postWavetableEditorAnnouncement(String(component->numFrames()) + (frames ? " frames" : " keyframes") +
                                  " spread across the timeline", AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, frames ? String("Spread frames evenly") : focusPrefix + " spread keyframes evenly");
}

void SynthEditor::setWavetableLinePointValue(int oscillator, int componentIndex, const std::string& field, double value) {
  auto* line = dynamic_cast<WaveLineSource*>(wavetableComponent(oscillator, componentIndex));
  if (line == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  if (atlas::wavetable_edit::setLinePointValue(line, state.keyframes[componentIndex], state.line_point, field, value))
    synth_.getWavetableCreator(oscillator)->render();
}

void SynthEditor::addWavetableLinePoint(int oscillator, int componentIndex, const String& focusPrefix) {
  auto* line = dynamic_cast<WaveLineSource*>(wavetableComponent(oscillator, componentIndex));
  if (line == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  const int added = atlas::wavetable_edit::addLinePoint(line, state.line_point);
  if (added < 0) {
    postWavetableEditorAnnouncement("The line has the most points it can hold", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  state.line_point = added;
  synth_.getWavetableCreator(oscillator)->render();
  postWavetableEditorAnnouncement("Added point " + String(added + 1) + " of " + String(atlas::wavetable_edit::numLinePoints(line)),
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, focusPrefix + " line point");
}

void SynthEditor::removeWavetableLinePoint(int oscillator, int componentIndex, const String& focusPrefix) {
  auto* line = dynamic_cast<WaveLineSource*>(wavetableComponent(oscillator, componentIndex));
  if (line == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  if (!atlas::wavetable_edit::removeLinePoint(line, state.line_point)) {
    postWavetableEditorAnnouncement("A line keeps at least two points", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  state.line_point = jlimit(0, atlas::wavetable_edit::numLinePoints(line) - 1, state.line_point);
  synth_.getWavetableCreator(oscillator)->render();
  postWavetableEditorAnnouncement("Point removed, " + String(atlas::wavetable_edit::numLinePoints(line)) + " points",
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, focusPrefix + " line point");
}

void SynthEditor::flattenWavetableToFrames(int oscillator) {
  auto* creator = isPositiveAndBelow(oscillator, vital::kNumOscillators) ? synth_.getWavetableCreator(oscillator) : nullptr;
  auto* source = atlas::wavetable_edit::flattenToWaveSource(creator);
  if (source == nullptr)
    return;
  auto& state = wavetable_keyboard_state_[oscillator];
  state.layer = 0;
  state.frame = 0;
  state.selected_frames.clear();
  state.keyframes.clear();
  postWavetableEditorAnnouncement("Wavetable flattened to " + String(source->numFrames()) + " editable frames",
                                  AccessibilityHandler::AnnouncementPriority::high);
  rebuildWavetableEditor(oscillator, "Layer");
}

bool SynthEditor::handleWavetableShortcut(int oscillator, const KeyPress& key, Component&) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return false;

  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const bool shift = key.getModifiers().isShiftDown();
  const bool command = key.getModifiers().isCommandDown();
  const int key_code = key.getKeyCode();
  if (key_code == KeyPress::leftKey || key_code == KeyPress::rightKey) {
    moveWavetableFrameCursor(oscillator, key_code == KeyPress::rightKey ? 1 : -1, shift);
    return true;
  }
  if (key_code == KeyPress::homeKey || key_code == KeyPress::endKey) {
    auto* creator = synth_.getWavetableCreator(oscillator);
    auto* source = editableWaveSource(oscillator);
    if (source == nullptr || source->numFrames() == 0)
      return true;
    const int count = source->numFrames();
    auto& state = wavetable_keyboard_state_[oscillator];
    const int previous = state.frame;
    state.frame = key_code == KeyPress::homeKey ? 0 : count - 1;
    if (shift) {
      for (int frame = jmin(previous, state.frame); frame <= jmax(previous, state.frame); ++frame)
        state.selected_frames.insert(frame);
    }
    postWavetableEditorAnnouncement(wavetableFrameStatusText(oscillator),
                                    AccessibilityHandler::AnnouncementPriority::high);
    return true;
  }
  if (key_code == KeyPress::pageUpKey || key_code == KeyPress::pageDownKey) {
    moveWavetableHarmonicCursor(oscillator, key_code == KeyPress::pageUpKey ? 10 : -10);
    return true;
  }
  if (key_code == KeyPress::upKey || key_code == KeyPress::downKey) {
    const float amount = shift ? 5.0f : 1.0f;
    adjustWavetableHarmonicLevel(oscillator, key_code == KeyPress::upKey ? amount : -amount);
    return true;
  }
  if (key_code == KeyPress::backspaceKey) {
    const auto& state = wavetable_keyboard_state_[oscillator];
    clearWavetableHarmonic(oscillator, state.frame, false, state.harmonic);
    return true;
  }
  // Windows sends Ctrl and a letter with no text character, so test the key code too.
  if ((command || shift) && (character == 'a' || key_code == 'A')) {
    selectAllWavetableFrames(oscillator);
    return true;
  }
  if (character == 'a') {
    addWavetableFrame(oscillator);
    return true;
  }
  if (character == 's') {
    toggleWavetableFrameSelection(oscillator);
    return true;
  }
  if (character == 'r') {
    clearWavetableFrameSelection(oscillator);
    return true;
  }
  if (character == ',' || character == '.') {
    moveWavetableHarmonicCursor(oscillator, character == '.' ? 1 : -1);
    return true;
  }
  if (character == '[' || character == '{' || character == ']' || character == '}') {
    const float amount = shift ? 5.0f : 1.0f;
    adjustWavetableHarmonicLevel(oscillator,
                                 (character == ']' || character == '}') ? amount : -amount);
    return true;
  }
  if (character == 'j' || character == 'l') {
    const float amount = shift ? 15.0f : 1.0f;
    adjustWavetableHarmonicPhase(oscillator, character == 'l' ? amount : -amount);
    return true;
  }
  if (character == 'c') {
    copyWavetableFrames(oscillator);
    return true;
  }
  if (character == 'v') {
    pasteWavetableFrames(oscillator);
    return true;
  }
  if (character == 'e' || key_code == KeyPress::deleteKey) {
    deleteWavetableFrames(oscillator);
    return true;
  }
  if (character == 'n') {
    const auto& state = wavetable_keyboard_state_[oscillator];
    normalizeWavetableFrame(oscillator, state.frame, false);
    return true;
  }
  if (character == 'f') {
    const auto& state = wavetable_keyboard_state_[oscillator];
    removeWavetableFundamental(oscillator, state.frame, false);
    return true;
  }
  if (character == '0') {
    clearWavetableDc(oscillator);
    return true;
  }
  if (character >= '1' && character <= '6') {
    auto& state = wavetable_keyboard_state_[oscillator];
    state.starting_waveform = static_cast<int>(character - '1');
    applyWavetableStartingShape(oscillator, state.frame, false, state.starting_waveform);
    return true;
  }
  if (character == 'i') {
    cycleWavetableInterpolation(oscillator, shift ? -1 : 1);
    return true;
  }
  return false;
}

void SynthEditor::setWavetableEditorVisible(int oscillator, bool visible) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  if (wavetable_editor_visible_[oscillator] == visible)
    return;

  const String restore_title = visible ? "Hide wavetable editor" : "Show wavetable editor";

  wavetable_editor_visible_[oscillator] = visible;
  const String section = "Oscillator " + String(oscillator + 1);
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this, restore_title, section, visible] {
    if (safe_this == nullptr)
      return;

    safe_this->showAllSections(false);
    safe_this->restoreFocusAfterRebuild({}, nullptr, restore_title, section, true);

    postWavetableEditorAnnouncement(visible ? "Wavetable editor shown" : "Wavetable editor hidden",
                                    AccessibilityHandler::AnnouncementPriority::medium);
  });
}

void SynthEditor::applyWavetableHarmonicEdit(int oscillator, int frame, bool allFrames,
                                             int harmonic, float levelPercent, float phaseDegrees) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, harmonic);
  pruneWavetableFrameSelection(oscillator, source->numFrames());
  auto& state = wavetable_keyboard_state_[oscillator];
  if (!allFrames && state.selected_frames.empty())
    state.frame = jlimit(0, source->numFrames() - 1, frame);
  std::vector<int> targets;
  if (allFrames) {
    for (int i = 0; i < source->numFrames(); ++i)
      targets.push_back(i);
  }
  else
    targets = wavetableTargetFrameIndices(oscillator, source->numFrames());
  const float amplitude = jlimit(0.0f, 4.0f, levelPercent / 100.0f) *
                          (vital::WaveFrame::kWaveformSize * 0.5f);
  const float phase = phaseDegrees * vital::kPi / 180.0f;

  for (int i : targets) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->frequency_domain[harmonic] = std::polar(amplitude, phase);
    wave_frame->toTimeDomain();
    wave_frame->normalize(true);
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.markStateChanged();

  postWavetableEditorAnnouncement("Set harmonic " + String(harmonic) +
                                  (targets.size() == 1 ? " on frame " + String(targets.front() + 1) :
                                   " on " + String(static_cast<int>(targets.size())) + " frames"),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::applyWavetableStartingShape(int oscillator, int frame, bool allFrames, int shape) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  shape = jlimit(0, vital::PredefinedWaveFrames::kNumShapes - 1, shape);
  pruneWavetableFrameSelection(oscillator, source->numFrames());
  auto& state = wavetable_keyboard_state_[oscillator];
  if (!allFrames && state.selected_frames.empty())
    state.frame = jlimit(0, source->numFrames() - 1, frame);

  std::vector<int> targets;
  if (allFrames) {
    for (int index = 0; index < source->numFrames(); ++index)
      targets.push_back(index);
  }
  else
    targets = wavetableTargetFrameIndices(oscillator, source->numFrames());

  const auto predefined_shape = static_cast<vital::PredefinedWaveFrames::Shape>(shape);
  const vital::WaveFrame* starting_frame = vital::PredefinedWaveFrames::getWaveFrame(predefined_shape);
  for (int index : targets)
    source->getKeyframe(index)->wave_frame()->copy(starting_frame);
  creator->render();
  synth_.markStateChanged();

  static const char* const kShapeNames[] = {
    "Sine", "Saturated sine", "Triangle", "Square", "Pulse 25 percent", "Saw"
  };
  postWavetableEditorAnnouncement(String(kShapeNames[shape]) + " waveform applied to " +
                                      String(static_cast<int>(targets.size())) + " wavetable frame" +
                                      (targets.size() == 1 ? String() : String("s")),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::clearWavetableHarmonic(int oscillator, int frame, bool allFrames, int harmonic) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, harmonic);
  pruneWavetableFrameSelection(oscillator, source->numFrames());
  auto& state = wavetable_keyboard_state_[oscillator];
  if (!allFrames && state.selected_frames.empty())
    state.frame = jlimit(0, source->numFrames() - 1, frame);
  std::vector<int> targets;
  if (allFrames) {
    for (int i = 0; i < source->numFrames(); ++i)
      targets.push_back(i);
  }
  else
    targets = wavetableTargetFrameIndices(oscillator, source->numFrames());

  for (int i : targets) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->frequency_domain[harmonic] = 0.0f;
    wave_frame->toTimeDomain();
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.markStateChanged();

  postWavetableEditorAnnouncement("Cleared harmonic " + String(harmonic) +
                                  (targets.size() == 1 ? " on frame " + String(targets.front() + 1) :
                                   " on " + String(static_cast<int>(targets.size())) + " frames"),
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::normalizeWavetableFrame(int oscillator, int frame, bool allFrames) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = editableWaveSource(oscillator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  pruneWavetableFrameSelection(oscillator, source->numFrames());
  auto& state = wavetable_keyboard_state_[oscillator];
  if (!allFrames && state.selected_frames.empty())
    state.frame = jlimit(0, source->numFrames() - 1, frame);
  std::vector<int> targets;
  if (allFrames) {
    for (int i = 0; i < source->numFrames(); ++i)
      targets.push_back(i);
  }
  else
    targets = wavetableTargetFrameIndices(oscillator, source->numFrames());

  for (int i : targets) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->normalize(true);
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.markStateChanged();

  postWavetableEditorAnnouncement(targets.size() == 1 ? "Normalized wavetable frame " + String(targets.front() + 1) :
                                                        "Normalized " + String(static_cast<int>(targets.size())) +
                                                        " wavetable frames",
                                  AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::removeWavetableFundamental(int oscillator, int frame, bool allFrames) {
  clearWavetableHarmonic(oscillator, frame, allFrames, 1);
}

void SynthEditor::refreshWavetableBrowserCache() {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                       LoadSave::kAdditionalWavetableFoldersName);
  refreshBrowserFileCache(cached_wavetable_browser_files_, roots, vital::kWavetableExtensionsList);
  wavetable_browser_cache_valid_ = true;
}

void SynthEditor::refreshSampleBrowserCache() {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                       LoadSave::kAdditionalSampleFoldersName);
  refreshBrowserFileCache(cached_sample_browser_files_, roots, vital::kSampleExtensionsList);
  sample_browser_cache_valid_ = true;
}

void SynthEditor::primeBrowserFileCaches() {
  if (wavetable_browser_cache_valid_ && sample_browser_cache_valid_)
    return;

  const auto wavetable_roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                                 LoadSave::kAdditionalWavetableFoldersName);
  const auto sample_roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                              LoadSave::kAdditionalSampleFoldersName);
  Thread::launch([safe_this = Component::SafePointer<SynthEditor>(this),
                  wavetable_roots, sample_roots] {
    std::vector<File> wavetable_files;
    std::vector<File> sample_files;
    refreshBrowserFileCache(wavetable_files, wavetable_roots, vital::kWavetableExtensionsList);
    refreshBrowserFileCache(sample_files, sample_roots, vital::kSampleExtensionsList);
    MessageManager::callAsync([safe_this,
                               wavetable_files = std::move(wavetable_files),
                               sample_files = std::move(sample_files)]() mutable {
      if (safe_this == nullptr)
        return;
      if (!safe_this->wavetable_browser_cache_valid_) {
        safe_this->cached_wavetable_browser_files_ = std::move(wavetable_files);
        safe_this->wavetable_browser_cache_valid_ = true;
      }
      if (!safe_this->sample_browser_cache_valid_) {
        safe_this->cached_sample_browser_files_ = std::move(sample_files);
        safe_this->sample_browser_cache_valid_ = true;
      }
    });
  });
}

void SynthEditor::loadShiftedWavetable(int oscillator, int direction) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  const File current_file(creator != nullptr ? creator->getLastFileLoaded() : std::string());
  if (!wavetable_browser_cache_valid_)
    refreshWavetableBrowserCache();

  File wavetable_file = shiftedBrowserFile(cached_wavetable_browser_files_, current_file, direction);
  if (wavetable_file.existsAsFile()) {
    active_oscillator_ = oscillator;
    loadWavetableFile(wavetable_file);
    return;
  }

  postPluginAnnouncement("No wavetable file found",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showWavetableActionsMenu(int oscillator, Component& target) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  enum Item { kImportSplice = 1, kImportVocode, kImportPitched, kExport, kExportWav, kSynthesize, kRename, kReset };
  PopupMenu menu;
  menu.addSectionHeader("Import audio as");
  menu.addItem(kImportSplice, "Import audio as wavetable splice");
  menu.addItem(kImportVocode, "Import audio as vocode");
  menu.addItem(kImportPitched, "Import audio as pitched wavetable");
  menu.addSeparator();
  menu.addItem(kExport, "Export wavetable");
  menu.addItem(kExportWav, "Export wavetable as WAV");
  menu.addItem(kSynthesize, "Synthesize preset to wavetable");
  menu.addSeparator();
  menu.addItem(kRename, "Rename wavetable");
  menu.addItem(kReset, "Reset wavetable");
  Component::SafePointer<Component> target_pointer(&target);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                     [this, oscillator, target_pointer](int result) {
    switch (result) {
      case kImportSplice:
      case kImportVocode:
      case kImportPitched:
        active_oscillator_ = oscillator;
        chooseWavetableFile(result == kImportSplice ? WavetableCreator::kWavetableSplice :
                            result == kImportVocode ? WavetableCreator::kVocoded : WavetableCreator::kPitched);
        break;
      case kExport: saveWavetableFile(oscillator); break;
      case kExportWav: exportWavetableWav(oscillator); break;
      case kSynthesize: synthesizePresetToWavetable(oscillator); break;
      case kRename: {
        auto* creator = synth_.getWavetableCreator(oscillator);
        if (creator == nullptr || target_pointer == nullptr)
          return;
        promptForCustomValue("Wavetable name", String(creator->getName()), *target_pointer,
                             [this, oscillator](const String& text) {
          auto* renamed = synth_.getWavetableCreator(oscillator);
          const String name = text.trim();
          if (renamed == nullptr || name.isEmpty())
            return;
          renamed->setName(name.toStdString());
          refreshWavetableNameLabel(oscillator);
          postPluginAnnouncement("Wavetable renamed " + name, AccessibilityHandler::AnnouncementPriority::high);
        });
        break;
      }
      case kReset:
        if (auto* creator = synth_.getWavetableCreator(oscillator)) {
          creator->loadDefaultCreator();
          creator->render();
          synth_.markStateChanged();
          refreshWavetableNameLabel(oscillator);
          wavetable_keyboard_state_[oscillator] = {};
          if (wavetable_editor_visible_[oscillator])
            rebuildWavetableEditor(oscillator, focusedAccessibleTitle());
          postPluginAnnouncement("Wavetable reset to the default", AccessibilityHandler::AnnouncementPriority::high);
        }
        break;
      default: break;
    }
  });
}

void SynthEditor::showWavetableBrowserMenu(int oscillator, Component& target) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                       LoadSave::kAdditionalWavetableFoldersName);
  if (!wavetable_browser_cache_valid_)
    refreshWavetableBrowserCache();

  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, LoadSave::kWavetableFolderName,
                                         cached_wavetable_browser_files_, choices);
  menu.addSeparator();
  menu.addItem(kBrowserRescanMenuId, "Rescan wavetable folders");
  Component::SafePointer<Component> target_pointer(&target);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                     [this, oscillator, choices, target_pointer](int result) {
    if (result == kBrowserRescanMenuId) {
      cached_wavetable_browser_files_.clear();
      wavetable_browser_cache_valid_ = false;
      if (target_pointer != nullptr)
        showWavetableBrowserMenu(oscillator, *target_pointer);
      return;
    }
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;

    active_oscillator_ = oscillator;
    loadWavetableFile((*choices)[static_cast<size_t>(result - 1)]);
  });
}

void SynthEditor::addSampleControlsGroup(int sample_target, Component& parent, int& y, int x, int width,
                                         int& focus_order, const String& section_name,
                                         const GroupRowAppender& append_rows) {
  auto group = makeSectionHeader(
      "Sample controls", groupRank("Oscillators"), false, section_name + " sample controls");
  auto* group_ptr = group.get();
  group_ptr->setDescription("Load, browse, and cycle samples for " + section_name);
  group_ptr->setExplicitFocusOrder(focus_order++);
  parent.addAndMakeVisible(group_ptr);
  if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
    accessible_parent->addAccessibleChild(group_ptr);
  row_focus_order_.push_back(group_ptr);
  section_headers_.push_back(std::move(group));

  int group_y = group_ptr->headerHeight() + 8;
  auto add_button = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->setWantsKeyboardFocus(true);
    button_ptr->setExplicitFocusOrder(focus_order++);
    button_ptr->onClick = std::move(action);
    button_ptr->setBounds(10, group_y, jmax(200, width - 20), 38);
    group_ptr->addAndMakeVisible(button_ptr);
    group_ptr->addAccessibleChild(button_ptr);
    row_focus_order_.push_back(button_ptr);
    section_headers_.push_back(std::move(button));
    group_y += 44;
  };
  add_button("Load sample", "Load an audio sample for " + section_name,
             [this, sample_target] { chooseSampleFile(sample_target); });
  add_button("Browse sample folders", "Browse sample folders for " + section_name,
             [this, sample_target, group_ptr] { showSampleBrowserMenu(*group_ptr, sample_target); });
  add_button("Previous sample", "Load the previous sample in this folder into " + section_name,
             [this, sample_target] { loadShiftedSample(-1, sample_target); });
  add_button("Next sample", "Load the next sample in this folder into " + section_name,
             [this, sample_target] { loadShiftedSample(1, sample_target); });
  if (append_rows)
    append_rows(*group_ptr, group_y, width);
  group_ptr->setBounds(x, y, width, group_y + 2);
  y += group_y + 10;
}

namespace {
  String instrumentPresetLabel(const vital::multisample::PresetName& preset) {
    String name = String::fromUTF8(preset.name.c_str()).trim();
    if (name.isEmpty())
      name = "Untitled";
    return name + ", bank " + String(preset.bank) + " program " + String(preset.program);
  }

  String instrumentFormatName(const std::string& format) {
    if (format == "sf2")
      return "SF2";
    return format == "dspreset" ? "DecentSampler" : "SFZ";
  }

  // Opens the grouped preset menu instead of JUCE's single scrolling column.
  class InstrumentPresetComboBox : public OffscreenComboBox {
    public:
      std::function<void()> onShowPopup;

      void showPopup() override {
        if (onShowPopup)
          onShowPopup();
        else
          OffscreenComboBox::showPopup();
      }
  };
}

PopupMenu SynthEditor::createInstrumentPresetMenu(const std::vector<vital::multisample::PresetName>& presets,
                                                  int selected_position) {
  static constexpr int kFlatLimit = 24;
  static constexpr int kRunLength = 16;
  PopupMenu menu;
  const int count = static_cast<int>(presets.size());
  if (count <= kFlatLimit) {
    for (int i = 0; i < count; ++i)
      menu.addItem(i + 1, instrumentPresetLabel(presets[static_cast<size_t>(i)]), true, i == selected_position);
    return menu;
  }

  auto programLabel = [](const vital::multisample::PresetName& preset) {
    String name = String::fromUTF8(preset.name.c_str()).trim();
    return (name.isEmpty() ? String("Untitled") : name) + ", program " + String(preset.program);
  };
  // Presets are sorted by bank, then program.
  std::vector<std::pair<int, int>> banks;
  for (int i = 0; i < count; ++i) {
    if (banks.empty() || presets[static_cast<size_t>(banks.back().first)].bank != presets[static_cast<size_t>(i)].bank)
      banks.push_back({ i, i });
    banks.back().second = i + 1;
  }
  for (const auto& bank : banks) {
    PopupMenu bank_menu;
    const int bank_count = bank.second - bank.first;
    if (bank_count <= kFlatLimit) {
      for (int i = bank.first; i < bank.second; ++i)
        bank_menu.addItem(i + 1, programLabel(presets[static_cast<size_t>(i)]), true, i == selected_position);
    }
    else {
      for (int start = bank.first; start < bank.second; start += kRunLength) {
        const int end = std::min(bank.second, start + kRunLength);
        PopupMenu run;
        for (int i = start; i < end; ++i)
          run.addItem(i + 1, programLabel(presets[static_cast<size_t>(i)]), true, i == selected_position);
        const bool holds_selection = selected_position >= start && selected_position < end;
        bank_menu.addSubMenu("Programs " + String(presets[static_cast<size_t>(start)].program) + " to " +
                                 String(presets[static_cast<size_t>(end - 1)].program),
                             run, true, nullptr, holds_selection);
      }
    }
    if (banks.size() == 1)
      return bank_menu;
    const bool holds_selection = selected_position >= bank.first && selected_position < bank.second;
    menu.addSubMenu("Bank " + String(presets[static_cast<size_t>(bank.first)].bank), bank_menu, true, nullptr,
                    holds_selection);
  }
  return menu;
}

void SynthEditor::stepInstrumentPreset(int oscillator, int direction) {
  auto* manager = synth_.getMultiSampleManager();
  const auto result = manager != nullptr ? manager->result(oscillator) : nullptr;
  if (result == nullptr || result->instrument == nullptr || result->instrument->preset_names.size() < 2) {
    postPluginAnnouncement("No other instrument presets", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  const auto& presets = result->instrument->preset_names;
  const int count = static_cast<int>(presets.size());
  int position = 0;
  for (int i = 0; i < count; ++i) {
    if (presets[static_cast<size_t>(i)].index == result->instrument->preset_index)
      position = i;
  }
  position = (position + direction + count) % count;
  const auto& preset = presets[static_cast<size_t>(position)];
  manager->selectSf2Preset(oscillator, preset.index);
  syncInstrumentPresetMenu(oscillator);
  postPluginAnnouncement("Instrument preset " + instrumentPresetLabel(preset),
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::syncInstrumentPresetMenu(int oscillator) {
  auto* manager = synth_.getMultiSampleManager();
  const auto result = manager != nullptr ? manager->result(oscillator) : nullptr;
  for (auto& component : section_headers_) {
    auto* combo = dynamic_cast<ComboBox*>(component.get());
    const var& property = combo != nullptr ? combo->getProperties()["InstrumentPresetMenu"] : var();
    if (property.isVoid() || static_cast<int>(property) != oscillator)
      continue;
    // Another SoundFont (a new load, a preset or host state naming one) has
    // other presets: the menu must be built again, not kept with stale items.
    // Loading the same file again only moves the selection.
    if (result == nullptr || result->instrument == nullptr ||
        combo->getProperties()["InstrumentPresetList"].toString() != String(result->hash)) {
      instrument_preset_menu_stale_ = true;
      return;
    }
    const auto& presets = result->instrument->preset_names;
    for (size_t i = 0; i < presets.size(); ++i) {
      if (presets[i].index == result->instrument->preset_index && combo->getSelectedId() != static_cast<int>(i) + 1)
        combo->setSelectedId(static_cast<int>(i) + 1, dontSendNotification);
    }
  }
}

int SynthEditor::multiSampleUiState(int oscillator) const {
  auto* manager = synth_.getMultiSampleManager();
  if (manager == nullptr)
    return 0;
  const auto result = manager->result(oscillator);
  if (result == nullptr || result->instrument == nullptr)
    return 0;
  if (result->instrument->format == "sf2")
    return result->instrument->preset_names.size() > 1 ? 3 : 2;
  return result->instrument->controls.empty() ? 1 : 4;
}

String SynthEditor::instrumentStatusTitle(int oscillator) const {
  auto* manager = synth_.getMultiSampleManager();
  if (manager == nullptr)
    return "No instrument loaded";
  const auto status = manager->status(oscillator);
  const auto result = manager->result(oscillator);
  if (status.state == vital::multisample::SlotStatus::kLoading) {
    const String phase = String(status.phase).isEmpty() ? String("Loading") : String(status.phase);
    // Tens of percent: the title changes rarely enough for a screen reader.
    return "Loading instrument, " + phase.toLowerCase() + ", " + String(10 * (roundToInt(status.progress * 100.0f) / 10)) +
           " percent";
  }
  if (result != nullptr && result->instrument != nullptr) {
    const auto& instrument = *result->instrument;
    String title = "Instrument: " + String::fromUTF8(instrument.name.c_str()) + ", " +
                   instrumentFormatName(instrument.format);
    if (instrument.format == "sf2") {
      for (const auto& preset : instrument.preset_names) {
        if (preset.index == instrument.preset_index)
          title += ", preset " + String::fromUTF8(preset.name.c_str()).trim();
      }
    }
    title += ", " + String(static_cast<int>(instrument.regions.size())) + " regions";
    if (status.state == vital::multisample::SlotStatus::kFailed)
      title += ", the last load failed";
    return title;
  }
  if (status.state == vital::multisample::SlotStatus::kFailed)
    return "Instrument not loaded: " + String::fromUTF8(status.error.c_str());
  return "No instrument loaded";
}

String SynthEditor::instrumentDetails(int oscillator) const {
  auto* manager = synth_.getMultiSampleManager();
  String details = instrumentStatusTitle(oscillator) + ".";
  if (manager == nullptr)
    return details;
  const auto status = manager->status(oscillator);
  const auto result = manager->result(oscillator);
  if (status.state == vital::multisample::SlotStatus::kFailed && result != nullptr)
    details += " " + String::fromUTF8(status.error.c_str());
  if (result != nullptr && result->instrument != nullptr) {
    const auto& instrument = *result->instrument;
    details += " Uses " + File::descriptionOfSizeInBytes(static_cast<int64>(instrument.memory_bytes)) +
               " of memory.";
    if (!instrument.missing_files.empty())
      details += " " + String(static_cast<int>(instrument.missing_files.size())) + " samples could not be loaded, first " +
                 String::fromUTF8(instrument.missing_files.front().c_str()) + ".";
    if (!instrument.warnings.empty())
      details += " " + String(static_cast<int>(instrument.warnings.size())) +
                 " " + instrumentFormatName(instrument.format) + " features are not supported and were ignored.";
    if (!status.notice.empty())
      details += " " + String::fromUTF8(status.notice.c_str());
    const auto spec = manager->spec(oscillator);
    if (!spec.path.empty())
      details += " File: " + String::fromUTF8(spec.path.c_str());
  }
  return details;
}

void SynthEditor::addMultiSampleControlsGroup(int oscillator, Component& parent, int& y, int x, int width,
                                              int& focus_order, const String& section_name,
                                              const GroupRowAppender& append_rows) {
  auto group = makeSectionHeader(
      "Instrument", groupRank("Oscillators"), false, section_name + " instrument");
  auto* group_ptr = group.get();
  group_ptr->setDescription("Load an SFZ, DecentSampler or SF2 instrument and choose how it plays in " + section_name);
  group_ptr->setExplicitFocusOrder(focus_order++);
  parent.addAndMakeVisible(group_ptr);
  if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
    accessible_parent->addAccessibleChild(group_ptr);
  row_focus_order_.push_back(group_ptr);
  section_headers_.push_back(std::move(group));

  int group_y = group_ptr->headerHeight() + 8;
  auto place = [&](std::unique_ptr<Component> component, int height) {
    auto* pointer = component.get();
    pointer->setWantsKeyboardFocus(true);
    pointer->setExplicitFocusOrder(focus_order++);
    pointer->setBounds(10, group_y, jmax(200, width - 20), height);
    group_ptr->addAndMakeVisible(pointer);
    group_ptr->addAccessibleChild(pointer);
    row_focus_order_.push_back(pointer);
    section_headers_.push_back(std::move(component));
    group_y += height + 6;
    return pointer;
  };
  auto add_button = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    button->setTitle(title);
    button->setDescription(description);
    button->setHelpText("Press Enter to " + description.toLowerCase());
    button->onClick = std::move(action);
    return static_cast<OffscreenTextButton*>(place(std::move(button), 38));
  };

  add_button("Load instrument", "Choose an SFZ, DecentSampler or SF2 file for " + section_name,
             [this, oscillator] { chooseInstrumentFile(oscillator); });

  // The status reads what is loaded; Enter speaks the details.
  const String status_title = instrumentStatusTitle(oscillator);
  auto* status = add_button(status_title, "Hear the details of the instrument in " + section_name,
                            [this, oscillator] {
                              postPluginAnnouncement(instrumentDetails(oscillator),
                                                     AccessibilityHandler::AnnouncementPriority::high);
                            });
  status->getProperties().set("InstrumentStatus", oscillator);
  add_button("Clear instrument", "Remove the instrument from " + section_name,
             [this, oscillator] { clearInstrument(oscillator); });

  auto* manager = synth_.getMultiSampleManager();
  const auto result = manager != nullptr ? manager->result(oscillator) : nullptr;
  const int ui_state = multiSampleUiState(oscillator);
  if (ui_state == 3 && result != nullptr && result->instrument != nullptr) {
    auto presets = std::make_shared<std::vector<vital::multisample::PresetName>>(result->instrument->preset_names);
    auto combo = std::make_unique<InstrumentPresetComboBox>();
    auto* combo_ptr = combo.get();
    combo_ptr->getProperties().set("InstrumentPresetMenu", oscillator);
    // Which SoundFont the items list; syncInstrumentPresetMenu compares it.
    const std::string preset_list = result->hash;
    combo_ptr->getProperties().set("InstrumentPresetList", String(preset_list));
    const String title = String(oscillator + 1) + " Instrument preset";
    combo_ptr->setTitle(title);
    combo_ptr->setDescription("Choose which preset of the SoundFont " + section_name + " plays");
    combo_ptr->setHelpText("Use Up and Down Arrow to step through presets, or open the menu to choose one by bank");
    int selected = 0;
    for (size_t i = 0; i < presets->size(); ++i) {
      combo_ptr->addItem(instrumentPresetLabel((*presets)[i]), static_cast<int>(i) + 1);
      if ((*presets)[i].index == result->instrument->preset_index)
        selected = static_cast<int>(i) + 1;
    }
    combo_ptr->setSelectedId(selected, dontSendNotification);
    combo_ptr->onShowPopup = [presets, safe_combo = Component::SafePointer<InstrumentPresetComboBox>(combo_ptr)] {
      if (safe_combo == nullptr)
        return;
      PopupMenu menu = createInstrumentPresetMenu(*presets, safe_combo->getSelectedId() - 1);
      showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(safe_combo.getComponent()), [safe_combo](int result) {
        if (safe_combo != nullptr && result > 0)
          safe_combo->setSelectedId(result, sendNotificationSync);
      });
    };
    combo_ptr->onChange = [this, oscillator, presets, preset_list, combo_ptr] {
      const int item = combo_ptr->getSelectedId() - 1;
      auto* multisample = synth_.getMultiSampleManager();
      if (multisample == nullptr || !isPositiveAndBelow(item, static_cast<int>(presets->size())))
        return;
      // Another SoundFont loaded since this menu was built; it is about to be rebuilt.
      const auto current = multisample->result(oscillator);
      if (current == nullptr || current->hash != preset_list)
        return;
      multisample->selectSf2Preset(oscillator, (*presets)[static_cast<size_t>(item)].index);
      postPluginAnnouncement("Instrument preset " + instrumentPresetLabel((*presets)[static_cast<size_t>(item)]),
                             AccessibilityHandler::AnnouncementPriority::high);
    };
    place(std::move(combo), 38);
    add_button("Previous preset", "Play the previous preset of the SoundFont in " + section_name,
               [this, oscillator] { stepInstrumentPreset(oscillator, -1); });
    add_button("Next preset", "Play the next preset of the SoundFont in " + section_name,
               [this, oscillator] { stepInstrumentPreset(oscillator, 1); });
  }
  if (ui_state == 1 || ui_state == 4) {
    const String format = instrumentFormatName(result != nullptr && result->instrument != nullptr
                                                   ? result->instrument->format : std::string("sfz"));
    add_button("Open in text editor", "Open the " + format + " file of " + section_name + " in a text editor",
               [this, oscillator] { openInstrumentInTextEditor(oscillator); });
    add_button("Reload " + format, "Read the " + format + " file of " + section_name + " again after editing it",
               [this, oscillator] { reloadInstrument(oscillator); });
  }
  if (ui_state == 4)
    addInstrumentControlRows(oscillator, *result, place, section_name);

  if (append_rows)
    append_rows(*group_ptr, group_y, width);
  group_ptr->setBounds(x, y, width, group_y + 2);
  y += group_y + 10;
}

void SynthEditor::addSpeechControlsGroup(int oscillator, Component& parent, int& y, int x, int width,
                                         int& focus_order, const String& section_name,
                                         const GroupRowAppender& append_rows) {
  auto group = makeSectionHeader("Speech controls", groupRank("Oscillators"), false, section_name + " speech controls");
  auto* group_ptr = group.get();
  group_ptr->setDescription("The words " + section_name + " sings and the espeak-ng voice that sings them");
  group_ptr->setExplicitFocusOrder(focus_order++);
  parent.addAndMakeVisible(group_ptr);
  if (auto* accessible_parent = dynamic_cast<AccessibleSectionHeader*>(&parent))
    accessible_parent->addAccessibleChild(group_ptr);
  row_focus_order_.push_back(group_ptr);
  section_headers_.push_back(std::move(group));

  int group_y = group_ptr->headerHeight() + 8;
  auto* manager = synth_.getSpeechManager();
  auto editor = std::make_unique<AccessibleTextEditor>();
  auto* editor_ptr = editor.get();
  editor_ptr->setTitle(String(oscillator + 1) + " Text to sing");
  editor_ptr->setDescription(speechStatusDescription(oscillator));
  editor_ptr->setHelpText("Type the words, then press Enter. Each note sings the next syllable, and the notes of a "
                          "chord sing one syllable together. Escape restores the words.");
  editor_ptr->setTextToShowWhenEmpty("Type the words to sing", Colours::grey);
  editor_ptr->setMultiLine(false);
  editor_ptr->setReturnKeyStartsNewLine(false);
  editor_ptr->setInputRestrictions(vital::speech::Manager::kMaxTextLength);
  editor_ptr->setWantsKeyboardFocus(true);
  if (manager != nullptr)
    editor_ptr->setText(String::fromUTF8(manager->text(oscillator).c_str()), dontSendNotification);
  editor_ptr->getProperties().set("SpeechText", oscillator);
  Component::SafePointer<TextEditor> safe_editor(editor_ptr);
  editor_ptr->onReturnKey = [this, oscillator, safe_editor] {
    if (safe_editor != nullptr)
      commitSpeechText(oscillator, safe_editor->getText());
  };
  editor_ptr->onFocusLost = editor_ptr->onReturnKey;
  editor_ptr->onEscapeKey = [this, oscillator, safe_editor] {
    auto* speech = synth_.getSpeechManager();
    if (safe_editor == nullptr || speech == nullptr)
      return;
    safe_editor->setText(String::fromUTF8(speech->text(oscillator).c_str()), dontSendNotification);
    postPluginAnnouncement("Words restored", AccessibilityHandler::AnnouncementPriority::medium);
  };
  editor_ptr->setExplicitFocusOrder(focus_order++);
  editor_ptr->setBounds(10, group_y, jmax(200, width - 20), 38);
  group_ptr->addAndMakeVisible(editor_ptr);
  group_ptr->addAccessibleChild(editor_ptr);
  row_focus_order_.push_back(editor_ptr);
  section_headers_.push_back(std::move(editor));
  group_y += 44;

  if (append_rows)
    append_rows(*group_ptr, group_y, width);
  group_ptr->setBounds(x, y, width, group_y + 2);
  y += group_y + 10;
}

void SynthEditor::commitSpeechText(int oscillator, const String& text) {
  auto* manager = synth_.getSpeechManager();
  if (manager == nullptr || !isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  const std::string words = text.trim().toStdString();
  if (words == manager->text(oscillator))
    return;
  manager->setText(oscillator, words);
  synth_.markStateChanged();
  if (words.empty()) {
    speech_announcements_[static_cast<size_t>(oscillator)].announce = false;
    postPluginAnnouncement("Oscillator " + String(oscillator + 1) + " words cleared",
                           AccessibilityHandler::AnnouncementPriority::medium);
  }
  else {
    // The render finishes in a moment; refreshSpeechStatus reports it.
    speech_announcements_[static_cast<size_t>(oscillator)].announce = true;
  }
}

String SynthEditor::speechStatusDescription(int oscillator) {
  auto* manager = synth_.getSpeechManager();
  const String base = "Words Oscillator " + String(oscillator + 1) + " sings";
  if (manager == nullptr)
    return base;
  const auto status = manager->status(oscillator);
  switch (status.state) {
    case vital::speech::SlotStatus::kRendering:
      return base + ", preparing the voice";
    case vital::speech::SlotStatus::kReady:
      return base + ", " + String(status.syllables) + (status.syllables == 1 ? " syllable" : " syllables") +
             " in " + String(status.words) + (status.words == 1 ? " word" : " words");
    case vital::speech::SlotStatus::kFailed:
      return base + ", " + String::fromUTF8(status.error.c_str());
    case vital::speech::SlotStatus::kEmpty:
    default:
      return base;
  }
}

void SynthEditor::refreshSpeechStatus() {
  auto* manager = synth_.getSpeechManager();
  if (manager == nullptr)
    return;
  for (int oscillator = 0; oscillator < vital::kNumOscillators; ++oscillator) {
    const auto status = manager->status(oscillator);
    auto& announced = speech_announcements_[static_cast<size_t>(oscillator)];
    if (status.revision == announced.revision)
      continue;
    announced.revision = status.revision;

    const String description = speechStatusDescription(oscillator);
    const String text = String::fromUTF8(manager->text(oscillator).c_str());
    for (auto& component : section_headers_) {
      auto* editor = dynamic_cast<TextEditor*>(component.get());
      const var& property = component != nullptr ? component->getProperties()["SpeechText"] : var();
      if (editor == nullptr || property.isVoid() || static_cast<int>(property) != oscillator)
        continue;
      editor->setDescription(description);
      // A preset or the host changed the words: show them, unless the user is typing.
      if (!editor->hasKeyboardFocus(true) && editor->getText() != text)
        editor->setText(text, dontSendNotification);
    }

    if (!announced.announce || status.state == vital::speech::SlotStatus::kRendering)
      continue;
    announced.announce = false;
    const String prefix = "Oscillator " + String(oscillator + 1);
    if (status.state == vital::speech::SlotStatus::kReady) {
      postPluginAnnouncement(prefix + " sings " + String(status.syllables) +
                                 (status.syllables == 1 ? " syllable" : " syllables"),
                             AccessibilityHandler::AnnouncementPriority::medium);
    }
    else if (status.state == vital::speech::SlotStatus::kFailed) {
      postPluginAnnouncement(prefix + " cannot sing the words: " + String::fromUTF8(status.error.c_str()),
                             AccessibilityHandler::AnnouncementPriority::high);
    }
  }
}

namespace {
  String instrumentControlLabel(const vital::multisample::Control& control) {
    const String label = String::fromUTF8(control.label.c_str()).trim();
    if (label.isNotEmpty())
      return label;
    using Kind = vital::multisample::Control::Kind;
    const String kind = control.kind == Kind::kKnob ? "Knob" : control.kind == Kind::kMenu ? "Menu" : "Button";
    return kind + " " + String(control.index + 1);
  }

  // A 0 to 1 knob reads as a percentage; any other range as its numbers.
  bool instrumentControlIsPercent(const vital::multisample::Control& control) {
    return !control.integer && control.minimum == 0.0 && control.maximum == 1.0;
  }

  String instrumentControlText(const vital::multisample::Control& control, double value) {
    if (instrumentControlIsPercent(control))
      return String(roundToInt(value * 100.0)) + "%";
    if (control.integer)
      return String(roundToInt(value));
    String text = String(value, 2);
    if (text.containsChar('.'))
      text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text == "-0" ? String("0") : text;
  }

  // The value the slot plays: one the user set, else the file's.
  double instrumentControlValue(const vital::multisample::InstrumentSpec& spec,
                                const vital::multisample::Control& control) {
    const auto found = spec.control_values.find(control.index);
    return found != spec.control_values.end() ? found->second : control.value;
  }
}

void SynthEditor::addInstrumentControlRows(int oscillator, const vital::multisample::LoadResult& result,
                                           const std::function<Component*(std::unique_ptr<Component>, int)>& place,
                                           const String& section_name) {
  auto* manager = synth_.getMultiSampleManager();
  if (manager == nullptr || result.instrument == nullptr)
    return;
  using Kind = vital::multisample::Control::Kind;
  const auto spec = manager->spec(oscillator);
  const String format = instrumentFormatName(result.instrument->format);
  for (const auto& control : result.instrument->controls) {
    const String label = instrumentControlLabel(control);
    // "Instrument" keeps a knob called Level apart from the oscillator's own.
    const String title = String(oscillator + 1) + " Instrument " + label;
    const int index = control.index;
    const double value = instrumentControlValue(spec, control);
    auto mark = [&](Component& component) {
      component.getProperties().set("InstrumentControl", oscillator);
      component.getProperties().set("InstrumentControlIndex", index);
      // Which instrument the rows were built for; another one rebuilds them.
      component.getProperties().set("InstrumentControlsHash", String(result.hash));
    };

    if (control.kind == Kind::kKnob) {
      auto slider = std::make_unique<OffscreenSlider>();
      auto* slider_ptr = slider.get();
      mark(*slider_ptr);
      slider_ptr->setTitle(title);
      slider_ptr->setName(title);
      slider_ptr->setDescription(label + ", a control of the " + format + " instrument in " + section_name);
      slider_ptr->setHelpText("Use arrow keys for small changes, Page Up and Page Down for larger changes, "
                              "Enter to type a value and Backspace to return to the instrument's setting");
      slider_ptr->setRange(control.minimum, jmax(control.minimum + 1.0e-6, control.maximum),
                           control.integer ? 1.0 : 0.0);
      slider_ptr->setValue(value, dontSendNotification);
      const vital::multisample::Control described = control;
      slider_ptr->textFromValueFunction = [described](double slider_value) {
        return instrumentControlText(described, slider_value);
      };
      slider_ptr->valueFromTextFunction = [described](const String& text) {
        const double number = text.retainCharacters("-0123456789.").getDoubleValue();
        return instrumentControlIsPercent(described) ? number / 100.0 : number;
      };
      slider_ptr->onValueChange = [this, oscillator, index, slider_ptr] {
        if (auto* multisample = synth_.getMultiSampleManager())
          multisample->setControlValue(oscillator, index, slider_ptr->getValue());
      };
      const double default_value = control.default_value;
      slider_ptr->onDefaultCommand = [slider_ptr, default_value] {
        Slider::ScopedDragNotification drag(*slider_ptr);
        slider_ptr->setValue(default_value, sendNotificationSync);
      };
      slider_ptr->onTextEntryCommand = [this, safe_slider = Component::SafePointer<OffscreenSlider>(slider_ptr),
                                        title](Component& target) {
        if (safe_slider == nullptr)
          return;
        promptForCustomValue(title, safe_slider->getTextFromValue(safe_slider->getValue()), target,
                             [safe_slider](const String& text) {
                               if (safe_slider == nullptr)
                                 return;
                               Slider::ScopedDragNotification drag(*safe_slider);
                               safe_slider->setValue(safe_slider->getValueFromText(text), sendNotificationSync);
                               if (auto* handler = safe_slider->getAccessibilityHandler())
                                 handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
                             });
      };
      place(std::move(slider), 42);
      continue;
    }

    // Menus and buttons choose one of their options.
    auto combo = std::make_unique<ParameterComboBox>();
    auto* combo_ptr = combo.get();
    mark(*combo_ptr);
    combo_ptr->setTitle(title);
    combo_ptr->setName(title);
    combo_ptr->setDescription(label + ", a " + (control.kind == Kind::kMenu ? "menu" : "button") + " of the " +
                              format + " instrument in " + section_name);
    combo_ptr->setHelpText("Use the arrow keys or open the menu to choose, and Backspace to return to the "
                           "instrument's setting");
    for (size_t i = 0; i < control.choices.size(); ++i)
      combo_ptr->addItem(String::fromUTF8(control.choices[i].c_str()), static_cast<int>(i) + 1);
    const int first = roundToInt(control.minimum);
    combo_ptr->setSelectedId(roundToInt(value) - first + 1, dontSendNotification);
    combo_ptr->onChange = [this, oscillator, index, first, combo_ptr] {
      if (combo_ptr->getSelectedId() <= 0)
        return;
      if (auto* multisample = synth_.getMultiSampleManager())
        multisample->setControlValue(oscillator, index, first + combo_ptr->getSelectedId() - 1);
    };
    const int default_id = roundToInt(control.default_value) - first + 1;
    combo_ptr->onDefaultCommand = [combo_ptr, default_id] {
      combo_ptr->setSelectedId(default_id, sendNotificationSync);
    };
    place(std::move(combo), 38);
  }
}

void SynthEditor::syncInstrumentControls(int oscillator) {
  auto* manager = synth_.getMultiSampleManager();
  const auto result = manager != nullptr ? manager->result(oscillator) : nullptr;
  const auto spec = manager != nullptr ? manager->spec(oscillator) : vital::multisample::InstrumentSpec();
  for (auto& component : section_headers_) {
    if (component == nullptr)
      continue;
    const var& property = component->getProperties()["InstrumentControl"];
    if (property.isVoid() || static_cast<int>(property) != oscillator)
      continue;
    if (result == nullptr || result->instrument == nullptr ||
        component->getProperties()["InstrumentControlsHash"].toString() != String(result->hash)) {
      instrument_preset_menu_stale_ = true;
      return;
    }
    const int index = component->getProperties()["InstrumentControlIndex"];
    for (const auto& control : result->instrument->controls) {
      if (control.index != index)
        continue;
      // A preset or the host can set the value as well as these rows.
      const double value = instrumentControlValue(spec, control);
      if (auto* slider = dynamic_cast<Slider*>(component.get())) {
        if (slider->getValue() != value)
          slider->setValue(value, dontSendNotification);
      }
      else if (auto* combo = dynamic_cast<ComboBox*>(component.get())) {
        const int id = roundToInt(value - control.minimum) + 1;
        if (combo->getSelectedId() != id)
          combo->setSelectedId(id, dontSendNotification);
      }
    }
  }
}

void SynthEditor::chooseInstrumentFile(int oscillator) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  File start = last_instrument_folder_;
  if (auto* manager = synth_.getMultiSampleManager()) {
    const auto spec = manager->spec(oscillator);
    const File current(String::fromUTF8(spec.path.c_str()));
    if (current.existsAsFile())
      start = current;
  }
  instrument_chooser_ = std::make_unique<FileChooser>("Load instrument for Oscillator " + String(oscillator + 1),
                                                      start, "*.sfz;*.dspreset;*.sf2;*.SFZ;*.DSPRESET;*.SF2");
  launchFileChooser(*instrument_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                          [this, oscillator](const FileChooser& chooser) {
    const File file = chooser.getResult();
    instrument_chooser_.reset();
    if (!file.existsAsFile())
      return;
    auto* manager = synth_.getMultiSampleManager();
    if (manager == nullptr)
      return;
    last_instrument_folder_ = file.getParentDirectory();
    setParameterEngineValue(vital::source_types::slotPrefix(oscillator) + "on", 1.0f);
    manager->loadFromUser(oscillator, file);
    synth_.markStateChanged();
    postPluginAnnouncement("Loading instrument " + file.getFileNameWithoutExtension() + " into Oscillator " +
                               String(oscillator + 1),
                           AccessibilityHandler::AnnouncementPriority::high);
  });
}

void SynthEditor::openInstrumentInTextEditor(int oscillator) {
  auto* manager = synth_.getMultiSampleManager();
  const File file = manager != nullptr ? manager->editableFile(oscillator) : File();
  if (!file.existsAsFile()) {
    postPluginAnnouncement("No instrument text file to open", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  // Open as text, not with whatever application claims .sfz or .dspreset files.
  const String format = file.hasFileExtension("dspreset") ? "DecentSampler" : "SFZ";
  StringArray command;
#if JUCE_MAC
  command = { "/usr/bin/open", "-t", file.getFullPathName() };
#elif JUCE_WINDOWS
  command = { "notepad.exe", file.getFullPathName() };
#else
  command = { "xdg-open", file.getFullPathName() };
#endif
  text_editor_process_ = std::make_unique<ChildProcess>();
  if (!text_editor_process_->start(command, 0)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to open the " + format + " file",
                                          "No text editor could be started for " + file.getFullPathName());
    postPluginAnnouncement("Unable to open the " + format + " instrument file", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  postPluginAnnouncement("Opened " + file.getFileName() + " in the text editor. After saving changes, choose Reload " + format,
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::reloadInstrument(int oscillator) {
  auto* manager = synth_.getMultiSampleManager();
  if (manager == nullptr || manager->result(oscillator) == nullptr) {
    postPluginAnnouncement("No instrument to reload", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  const auto loaded = manager->result(oscillator);
  const String format = loaded->instrument != nullptr ? instrumentFormatName(loaded->instrument->format) : String("SFZ");
  manager->reload(oscillator);
  postPluginAnnouncement("Reloading " + format + " instrument for Oscillator " + String(oscillator + 1),
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::clearInstrument(int oscillator) {
  auto* manager = synth_.getMultiSampleManager();
  if (manager == nullptr)
    return;
  const String section_name = "Oscillator " + String(oscillator + 1);
  const auto status = manager->status(oscillator);
  if (manager->result(oscillator) == nullptr && manager->spec(oscillator).empty() &&
      status.state == vital::multisample::SlotStatus::kEmpty) {
    postPluginAnnouncement("No instrument to clear in " + section_name,
                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  // A first load in progress has nothing playing yet: clearing only cancels it.
  const bool only_loading = status.state == vital::multisample::SlotStatus::kLoading &&
                            manager->result(oscillator) == nullptr;
  // Rows follow what is loaded; a load in progress or a missing file shows none.
  const bool rows_change = multiSampleUiState(oscillator) != 0;
  manager->clear(oscillator);
  const String announcement = section_name + (only_loading ? " instrument load cancelled" : " instrument cleared");
  if (!rows_change) {
    postPluginAnnouncement(announcement, AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  // The preset menu or text-file buttons go away, so rebuild once the click has
  // returned (before the timer does) and stay on Clear instrument here.
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this, section_name, announcement] {
    if (safe_this == nullptr)
      return;
    safe_this->showAllSections(false);
    safe_this->restoreFocusAfterRebuild({}, nullptr, "Clear instrument", section_name, true);
    postPluginAnnouncement(announcement, AccessibilityHandler::AnnouncementPriority::high);
  });
}

void SynthEditor::refreshMultiSampleStatus() {
  auto* manager = synth_.getMultiSampleManager();
  if (manager == nullptr)
    return;
  for (int oscillator = 0; oscillator < vital::kNumOscillators; ++oscillator) {
    const auto status = manager->status(oscillator);
    auto& announced = instrument_announcements_[static_cast<size_t>(oscillator)];
    if (status.revision == announced.revision)
      continue;
    announced.revision = status.revision;
    const int previous_state = announced.state;
    announced.state = status.state;

    const String title = instrumentStatusTitle(oscillator);
    const String section_name = "Oscillator " + String(oscillator + 1);
    for (auto& component : section_headers_) {
      if (component == nullptr)
        continue;
      const var& property = component->getProperties()["InstrumentStatus"];
      if (!property.isVoid() && static_cast<int>(property) == oscillator && component->getTitle() != title) {
        component->setTitle(title);
        if (auto* button = dynamic_cast<Button*>(component.get()))
          button->setButtonText(title);
        if (auto* handler = component->getAccessibilityHandler())
          handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
      }
      if (component->getProperties()["SectionName"].toString() == section_name) {
        if (auto* header = dynamic_cast<AccessibleSectionHeader*>(component.get()))
          header->setHeaderTitle(sourceStatusTitleForSection(section_name));
      }
    }
    syncInstrumentPresetMenu(oscillator);
    syncInstrumentControls(oscillator);

    const String prefix = "Oscillator " + String(oscillator + 1) + " instrument";
    if (status.state == vital::multisample::SlotStatus::kLoading) {
      // Progress at most every quarter or every three seconds.
      const int percent = roundToInt(status.progress * 100.0f);
      const uint32 now = Time::getMillisecondCounter();
      if (previous_state != vital::multisample::SlotStatus::kLoading) {
        announced.announced_percent = 0;
        announced.announced_at = now;
      }
      else if (percent >= announced.announced_percent + 25 || now - announced.announced_at > 3000) {
        announced.announced_percent = percent;
        announced.announced_at = now;
        postPluginAnnouncement(prefix + " loading, " + String(percent) + " percent",
                               AccessibilityHandler::AnnouncementPriority::medium);
      }
    }
    else if (status.state == vital::multisample::SlotStatus::kLoaded &&
             previous_state == vital::multisample::SlotStatus::kLoading) {
      const auto result = manager->result(oscillator);
      String message = prefix + " loaded";
      if (result != nullptr && result->instrument != nullptr) {
        const auto& instrument = *result->instrument;
        message = "Oscillator " + String(oscillator + 1) + ": loaded " + String::fromUTF8(instrument.name.c_str()) +
                  ", " + String(static_cast<int>(instrument.regions.size())) + " regions, " +
                  File::descriptionOfSizeInBytes(static_cast<int64>(instrument.memory_bytes));
        if (!instrument.missing_files.empty())
          message += ", " + String(static_cast<int>(instrument.missing_files.size())) + " samples missing";
      }
      if (!status.notice.empty())
        message += ". " + String::fromUTF8(status.notice.c_str());
      postPluginAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
    }
    else if (status.state == vital::multisample::SlotStatus::kFailed &&
             previous_state == vital::multisample::SlotStatus::kLoading) {
      const String error = String::fromUTF8(status.error.c_str());
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to load instrument", error);
      postPluginAnnouncement(prefix + " failed to load. " + error, AccessibilityHandler::AnnouncementPriority::high);
    }
  }
}

void SynthEditor::chooseSampleFile(int sample_target) {
  const String title = "Load sample for Oscillator " + String(sample_target + 1);
  auto* sample = isPositiveAndBelow(sample_target, vital::kNumOscillators) ? synth_.getSample(sample_target) : nullptr;
  const File current_file(sample != nullptr ? sample->getLastBrowsedFile() : std::string());
  sample_chooser_ = std::make_unique<FileChooser>(title,
                                                  current_file, vital::kSampleExtensionsList);
  launchFileChooser(*sample_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                      [this, sample_target](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadSampleFile(file, sample_target);
    sample_chooser_.reset();
  });
}

void SynthEditor::loadSampleFile(const File& file, int sample_target) {
  static constexpr int kMaxFileSamples = 17640000;

  try {
    AudioFormatManager format_manager;
    format_manager.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> format_reader(format_manager.createReaderFor(file));
    if (format_reader == nullptr)
      throw std::runtime_error("Unable to decode audio file");

    const int num_samples = static_cast<int>(std::min<long long>(format_reader->lengthInSamples, kMaxFileSamples));
    if (num_samples <= 0)
      throw std::runtime_error("Audio file is empty");

    AudioSampleBuffer sample_buffer;
    sample_buffer.setSize(static_cast<int>(format_reader->numChannels), num_samples);
    format_reader->read(&sample_buffer, 0, num_samples, 0, true, true);

    auto* sample = isPositiveAndBelow(sample_target, vital::kNumOscillators) ? synth_.getSample(sample_target) : nullptr;
    if (sample == nullptr)
      throw std::runtime_error("Sample engine is unavailable");

    synth_.pauseProcessing(true);
    if (sample_buffer.getNumChannels() > 1)
      sample->loadSample(sample_buffer.getReadPointer(0), sample_buffer.getReadPointer(1),
                         num_samples, static_cast<int>(format_reader->sampleRate));
    else
      sample->loadSample(sample_buffer.getReadPointer(0), num_samples, static_cast<int>(format_reader->sampleRate));
    sample->setName(file.getFileNameWithoutExtension().toStdString());
    sample->setLastBrowsedFile(file.getFullPathName().toStdString());
    synth_.pauseProcessing(false);
    synth_.markStateChanged();

    // Granular and spectral oscillators switch on when given material, as the fixed sources did.
    const int type = sourceSlotType(sample_target);
    if (type == vital::source_types::kGranular || type == vital::source_types::kSpectral)
      setParameterEngineValue(vital::source_types::slotPrefix(sample_target) + "on", 1.0f);

    refreshSampleNameLabel(sample_target);

    postPluginAnnouncement("Loaded sample " + file.getFileNameWithoutExtension(),
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
  catch (const std::exception& error) {
    synth_.pauseProcessing(false);
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                          "Unable to load sample", error.what());
    postPluginAnnouncement("Unable to load sample",
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
}

void SynthEditor::loadShiftedSample(int direction, int sample_target) {
  auto* sample = isPositiveAndBelow(sample_target, vital::kNumOscillators) ? synth_.getSample(sample_target) : nullptr;
  const File current_file(sample != nullptr ? sample->getLastBrowsedFile() : std::string());
  File sample_file = LoadSave::getShiftedFile(LoadSave::kSampleFolderName, vital::kSampleExtensionsList,
                                              LoadSave::kAdditionalSampleFoldersName,
                                              current_file, direction);
  if (sample_file.existsAsFile()) {
    loadSampleFile(sample_file, sample_target);
    return;
  }

  postPluginAnnouncement("No sample file found",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showSampleBrowserMenu(Component& target, int sample_target) {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                       LoadSave::kAdditionalSampleFoldersName);
  if (!sample_browser_cache_valid_)
    refreshSampleBrowserCache();

  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, LoadSave::kSampleFolderName,
                                         cached_sample_browser_files_, choices);
  menu.addSeparator();
  menu.addItem(kBrowserRescanMenuId, "Rescan sample folders");
  Component::SafePointer<Component> target_pointer(&target);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                     [this, choices, sample_target, target_pointer](int result) {
    if (result == kBrowserRescanMenuId) {
      cached_sample_browser_files_.clear();
      sample_browser_cache_valid_ = false;
      if (target_pointer != nullptr)
        showSampleBrowserMenu(*target_pointer, sample_target);
      return;
    }
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;

    loadSampleFile((*choices)[static_cast<size_t>(result - 1)], sample_target);
  });
}

vital::ConvolutionReverbModule* SynthEditor::convolutionModuleForPrefix(const String& modulePrefix) {
  for (auto& prefix_module : synth_.getConvolutionReverbModules()) {
    if (String(prefix_module.first) == modulePrefix)
      return prefix_module.second;
  }
  return nullptr;
}

void SynthEditor::refreshIrBrowserCache() {
  if (ir_browser_scan_) return;
  const auto roots = LoadSave::getIrDirectories();
  ir_browser_scan_ = std::make_shared<IrBrowserScan>();
  ir_browser_scan_thread_ = std::thread([scan = ir_browser_scan_, roots] {
    refreshBrowserFileCache(scan->files, roots, vital::kIrExtensionsList, &scan->cancelled);
    scan->ready.store(true, std::memory_order_release);
  });
}

void SynthEditor::chooseConvolutionIrFile(const String& modulePrefix) {
  ir_chooser_ = std::make_unique<FileChooser>("Load impulse response",
                                              LoadSave::getUserIrDirectory(), vital::kIrExtensionsList);
  launchFileChooser(*ir_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [this, modulePrefix](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file.existsAsFile())
      loadConvolutionIrFile(modulePrefix, file);
    ir_chooser_.reset();
  });
}

void SynthEditor::loadConvolutionIrFile(const String& modulePrefix, const File& file) {
  auto* module = convolutionModuleForPrefix(modulePrefix);
  if (module == nullptr)
    return;
  pending_ir_navigation_.erase(modulePrefix);
  pending_ir_browser_target_ = nullptr;

  // Advance the cursor immediately so rapid arrow presses select successive
  // files, even while the preceding file is still being decoded.
  module->setLastBrowsedFile(file.getFullPathName().toStdString());
  pending_ir_loads_[modulePrefix] = module->requestIrFile(file);
}

void SynthEditor::refreshConvolutionIrLoads() {
  using Result = vital::ConvolutionReverbModule::IrLoadResult;
  if (ir_browser_scan_ && ir_browser_scan_->ready.load(std::memory_order_acquire)) {
    ir_browser_scan_thread_.join();
    cached_ir_browser_files_ = std::move(ir_browser_scan_->files);
    ir_browser_scan_.reset();
    ir_browser_cache_valid_ = true;
    auto navigation = std::move(pending_ir_navigation_);
    pending_ir_navigation_.clear();
    for (const auto& entry : navigation) {
      auto* module = convolutionModuleForPrefix(entry.first);
      const auto& pending = entry.second;
      if (!module || module->irLoadStatus().request != pending.request) continue;
      File start = pending.start;
      int64_t steps = pending.steps;
      if (!cached_ir_browser_files_.empty() &&
          std::find(cached_ir_browser_files_.begin(), cached_ir_browser_files_.end(), start) == cached_ir_browser_files_.end()) {
        start = cached_ir_browser_files_.front();
        steps -= pending.first_step;
      }
      const auto file = shiftedBrowserFile(cached_ir_browser_files_, start, steps);
      if (file != File()) loadConvolutionIrFile(entry.first, file);
      else postPluginAnnouncement("No impulse response file found. Add files to the Impulse Responses folders.",
                                  AccessibilityHandler::AnnouncementPriority::high);
    }
    if (pending_ir_browser_target_) {
      auto target = pending_ir_browser_target_;
      pending_ir_browser_target_ = nullptr;
      showConvolutionIrBrowserMenu(pending_ir_browser_prefix_, *target);
    }
  }
  for (auto it = pending_ir_loads_.begin(); it != pending_ir_loads_.end();) {
    auto* module = convolutionModuleForPrefix(it->first);
    const auto status = module ? module->irLoadStatus() : vital::ConvolutionReverbModule::IrLoadStatus();
    if (status.request == it->second && status.result == Result::loading) { ++it; continue; }
    if (status.request == it->second && status.result == Result::loaded)
      postPluginAnnouncement("Loaded impulse response " + String(module->irName()),
                             AccessibilityHandler::AnnouncementPriority::high);
    else if (status.request == it->second && status.result == Result::failed) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to load impulse response",
                                            "The selected file is not a supported impulse response. The previous impulse response is unchanged.");
      postPluginAnnouncement("Unable to load impulse response. The previous impulse response is unchanged.",
                             AccessibilityHandler::AnnouncementPriority::high);
    }
    it = pending_ir_loads_.erase(it);
  }
  for (auto it = ir_controls_.begin(); it != ir_controls_.end();) {
    auto& controls = it->second;
    if (!controls.group || !controls.browse) { it = ir_controls_.erase(it); continue; }
    auto* module = convolutionModuleForPrefix(it->first);
    if (module && controls.revision != module->irRevision()) {
      controls.revision = module->irRevision();
      const String name(module->irName());
      const String title = "Browse impulse response folders, current: " + name;
      controls.browse->setTitle(title);
      if (auto* button = dynamic_cast<Button*>(controls.browse.getComponent())) button->setButtonText(title);
      controls.group->setDescription("Impulse response loading, browsing, navigation, editing, and saving. Current: " + name);
    }
    ++it;
  }
}

void SynthEditor::showConvolutionIrBrowserMenu(const String& modulePrefix, Component& target) {
  const auto roots = LoadSave::getIrDirectories();
  if (!ir_browser_cache_valid_) {
    refreshIrBrowserCache();
    pending_ir_browser_target_ = &target;
    pending_ir_browser_prefix_ = modulePrefix;
    postPluginAnnouncement("Scanning impulse response folders", AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, String(LoadSave::kIrFolderName),
                                         cached_ir_browser_files_, choices);
  menu.addSeparator();
  menu.addItem(kBrowserRescanMenuId, "Rescan impulse response folders");
  Component::SafePointer<Component> target_pointer(&target);
  showAccessibleMenuAsync(menu, PopupMenu::Options().withTargetComponent(&target),
                     [safe_this = Component::SafePointer<SynthEditor>(this), choices, modulePrefix, target_pointer](int result) {
    if (!safe_this) return;
    if (result == kBrowserRescanMenuId) {
      safe_this->cached_ir_browser_files_.clear();
      safe_this->ir_browser_cache_valid_ = false;
      if (target_pointer != nullptr)
        safe_this->showConvolutionIrBrowserMenu(modulePrefix, *target_pointer);
      return;
    }
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;

    safe_this->loadConvolutionIrFile(modulePrefix, (*choices)[static_cast<size_t>(result - 1)]);
  });
}

void SynthEditor::loadShiftedConvolutionIr(const String& modulePrefix, int direction) {
  auto* module = convolutionModuleForPrefix(modulePrefix);
  if (module == nullptr)
    return;

  pending_ir_browser_target_ = nullptr;
  if (!ir_browser_cache_valid_) {
    refreshIrBrowserCache();
    auto inserted = pending_ir_navigation_.try_emplace(modulePrefix);
    auto& pending = inserted.first->second;
    if (inserted.second) {
      pending.start = File(module->getLastBrowsedFile());
      pending.request = module->irLoadStatus().request;
      pending.first_step = direction;
    }
    pending.steps += direction;
    return;
  }

  const File current_file(module->getLastBrowsedFile());
  File ir_file = shiftedBrowserFile(cached_ir_browser_files_, current_file, direction);
  if (ir_file != File()) {
    loadConvolutionIrFile(modulePrefix, ir_file);
    return;
  }

  postPluginAnnouncement("No impulse response file found. Add files to the Impulse Responses folders.",
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::saveConvolutionIrFile(const String& modulePrefix) {
  auto* module = convolutionModuleForPrefix(modulePrefix);
  if (module == nullptr)
    return;

  const String name = module->irName().empty() ? "Impulse Response" : String(module->irName());
  File start_file = LoadSave::getUserIrDirectory().getChildFile(name + "." + String(vital::kIrExtension));
  ir_chooser_ = std::make_unique<FileChooser>("Save impulse response", start_file,
                                              String("*.") + String(vital::kIrExtension));
  launchFileChooser(*ir_chooser_, FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                  FileBrowserComponent::warnAboutOverwriting,
                                  [this, modulePrefix](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file != File()) {
      auto* module = convolutionModuleForPrefix(modulePrefix);
      if (module != nullptr && module->saveIrToFile(file)) {
        cached_ir_browser_files_.clear();
        ir_browser_cache_valid_ = false;
        postPluginAnnouncement("Saved impulse response " + file.getFileNameWithoutExtension(),
                               AccessibilityHandler::AnnouncementPriority::high);
      }
      else {
        postPluginAnnouncement("Unable to save impulse response",
                               AccessibilityHandler::AnnouncementPriority::high);
      }
    }
    ir_chooser_.reset();
  });
}

void SynthEditor::chooseLfoPresetFile(int lfoIndex) {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos))
    return;

  lfo_chooser_ = std::make_unique<FileChooser>("Import Atlas LFO preset",
                                               LoadSave::getUserLfoDirectory(), vital::kLfoExtensionsList);
  launchFileChooser(*lfo_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                   [this, lfoIndex](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file.existsAsFile())
      loadLfoPresetFile(lfoIndex, file);
    lfo_chooser_.reset();
  });
}

void SynthEditor::loadLfoPresetFile(int lfoIndex, const File& file) {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos) || !file.existsAsFile())
    return;

  json data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
  if (data.is_discarded() || !LineGenerator::isValidJson(data)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import LFO preset",
                                          "The selected file is not a valid Atlas LFO preset.");
    postPluginAnnouncement("Unable to import LFO preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  if (auto* generator = lfoGeneratorForIndex(lfoIndex)) {
    try {
      generator->jsonToState(data);
    }
    catch (const json::exception&) {
      NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import LFO preset",
                                            "The selected file is not a valid Atlas LFO preset.");
      postPluginAnnouncement("Unable to import LFO preset", AccessibilityHandler::AnnouncementPriority::high);
      return;
    }
    generator->setLastBrowsedFile(file.getFullPathName().toStdString());
    const String prefix = "lfo_" + String(lfoIndex + 1) + "_";
    if (data.count("deform") && data["deform"].is_number())
      setParameterEngineValue(prefix + "deform", jlimit(0.0f, 1.0f, data["deform"].get<float>()));
    if (data.count("swing") && data["swing"].is_number())
      setParameterEngineValue(prefix + "swing", jlimit(0.0f, 1.0f, data["swing"].get<float>()));
    if (active_lfo_index_ == lfoIndex)
      syncLfoEditorState();
    postPluginAnnouncement("Loaded LFO preset " + file.getFileNameWithoutExtension() +
                                           " into LFO " + String(lfoIndex + 1),
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
}

void SynthEditor::saveLfoPresetFile(int lfoIndex) {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos))
    return;

  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr)
    return;

  String name = String(generator->getName()).trim();
  if (name.isEmpty())
    name = "LFO " + String(lfoIndex + 1);
  name = name.removeCharacters("\\/:*?\"<>|");
  if (name.isEmpty())
    name = "LFO";

  lfo_chooser_ = std::make_unique<FileChooser>("Save Atlas LFO preset",
                                               LoadSave::getUserLfoDirectory().getChildFile(name),
                                               String("*.") + vital::kLfoExtension);
  launchFileChooser(*lfo_chooser_, FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                       FileBrowserComponent::warnAboutOverwriting,
                                   [this, lfoIndex](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file != File()) {
      if (auto* generator = lfoGeneratorForIndex(lfoIndex)) {
        const File destination = file.withFileExtension(vital::kLfoExtension);
        const bool saved = destination.replaceWithText(String(generator->stateToJson().dump(2)));
        if (saved)
          postPluginAnnouncement("Saved LFO preset " + destination.getFileNameWithoutExtension(),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        else
          NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save LFO preset",
                                                "The LFO preset could not be written.");
      }
    }
    lfo_chooser_.reset();
  });
}

void SynthEditor::chooseFxBusPresetFile(const String& sectionName, bool save) {
  if (!isEffectChainSection(sectionName) || fx_bus_chooser_ != nullptr)
    return;

  const int bus = fxBusForSection(sectionName);
  const String bus_name = fxBusNameForSection(sectionName);
  const json preset = save ? synth_.fxBusPreset(bus) : json();
  const auto directory = LoadSave::getUserFxDirectory();
  fx_bus_chooser_ = std::make_unique<FileChooser>(save ? "Save Atlas FX bus preset" : "Load Atlas FX bus preset",
      save ? directory.getChildFile(bus_name).withFileExtension(vital::kFxBusExtension) : directory,
      String("*.") + vital::kFxBusExtension);
  Component::SafePointer<SynthEditor> safe_this(this);
  const int flags = FileBrowserComponent::canSelectFiles | (save ? FileBrowserComponent::saveMode |
      FileBrowserComponent::warnAboutOverwriting : FileBrowserComponent::openMode);
  launchFileChooser(*fx_bus_chooser_, flags, [safe_this, sectionName, bus, bus_name, preset, save](const FileChooser& chooser) {
    if (safe_this == nullptr)
      return;
    const File file = chooser.getResult();
    if (file != File()) {
      String error;
      bool success = false;
      if (save) {
        const auto destination = file.withFileExtension(vital::kFxBusExtension);
        success = destination.replaceWithText(String(preset.dump(2)));
        if (!success)
          error = "The FX bus preset could not be written.";
      }
      else {
        const json data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
        success = safe_this->synth_.loadFxBusPreset(bus, data, error);
        if (success)
          safe_this->scheduleEffectChainRebuild(sectionName, "Load bus FX preset");
      }
      if (success)
        postPluginAnnouncement((save ? "Saved " : "Loaded ") + bus_name + " preset " +
            file.getFileNameWithoutExtension(), AccessibilityHandler::AnnouncementPriority::high);
      else {
        const String title = save ? "Unable to save FX bus preset" : "Unable to load FX bus preset";
        NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, title, error);
        postPluginAnnouncement(title, AccessibilityHandler::AnnouncementPriority::high);
      }
    }
    safe_this->fx_bus_chooser_.reset();
  });
}

void SynthEditor::chooseEffectPresetFile(const String& sectionName) {
  if (effectIdForSection(sectionName).isEmpty())
    return;

  fx_chooser_ = std::make_unique<FileChooser>("Import Atlas FX preset",
                                              LoadSave::getUserFxDirectory(),
                                              String("*.") + vital::kFxExtension);
  launchFileChooser(*fx_chooser_, FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [this, sectionName](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file.existsAsFile())
      loadEffectPresetFile(sectionName, file);
    fx_chooser_.reset();
  });
}

void SynthEditor::loadEffectPresetFile(const String& sectionName, const File& file) {
  const String effect_id = effectIdForSection(sectionName);
  if (effect_id.isEmpty() || !file.existsAsFile())
    return;

  json data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
  if (data.is_discarded() || !data.count("parameters") || !data["parameters"].is_object()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import FX preset",
                                          "The selected file is not a valid Atlas FX preset.");
    postPluginAnnouncement("Unable to import FX preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  if (data.count("effect") && data["effect"].is_string() &&
      String(data["effect"].get<std::string>()) != effect_id) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Wrong FX preset type",
                                          "This preset was saved for " +
                                          effectPresetDisplayName(String(data["effect"].get<std::string>())) +
                                          ", not " + accessibleSectionTitle(sectionName) + ".");
    return;
  }

  const String chain_prefix = effectChainPrefixForSection(sectionName);
  int changed = 0;
  for (auto it = data["parameters"].begin(); it != data["parameters"].end(); ++it) {
    if (!it.value().is_number())
      continue;
    const String suffix = it.key();
    if (!suffix.startsWith(effect_id + "_"))
      continue;
    const String target_id = chain_prefix + suffix;
    if (parameterBridge(target_id) == nullptr)
      continue;
    setParameterEngineValue(target_id, it.value().get<float>());
    ++changed;
  }

  if (changed == 0) {
    postPluginAnnouncement("No matching FX parameters found", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  timerCallback();
  postPluginAnnouncement("Loaded " + accessibleSectionTitle(sectionName) + " FX preset " +
                                         file.getFileNameWithoutExtension(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::saveEffectPresetFile(const String& sectionName) {
  const String effect_id = effectIdForSection(sectionName);
  if (effect_id.isEmpty())
    return;

  const String chain_prefix = effectChainPrefixForSection(sectionName);
  json parameters = json::object();
  for (const auto& parameter : parameters_by_id_) {
    const String id = parameter.first;
    if (!id.startsWith(chain_prefix + effect_id + "_"))
      continue;
    if (parameter.second == nullptr)
      continue;
    const String suffix = id.fromFirstOccurrenceOf(chain_prefix, false, false);
    parameters[suffix.toStdString()] = parameter.second->convertToEngineValue(parameter.second->getValue());
  }

  if (parameters.empty()) {
    postPluginAnnouncement("No FX parameters found", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  String name = accessibleSectionTitle(sectionName).removeCharacters("\\/:*?\"<>|");
  if (name.isEmpty())
    name = "FX Preset";
  fx_chooser_ = std::make_unique<FileChooser>("Save Atlas FX preset",
                                              LoadSave::getUserFxDirectory().getChildFile(name),
                                              String("*.") + vital::kFxExtension);
  launchFileChooser(*fx_chooser_, FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                      FileBrowserComponent::warnAboutOverwriting,
                                  [this, effect_id, sectionName, parameters](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file != File()) {
      json data;
      data["type"] = "Atlas FX Preset";
      data["version"] = 1;
      data["effect"] = effect_id.toStdString();
      data["name"] = accessibleSectionTitle(sectionName).toStdString();
      data["parameters"] = parameters;
      const File destination = file.withFileExtension(vital::kFxExtension);
      const bool saved = destination.replaceWithText(String(data.dump(2)));
      if (saved)
        postPluginAnnouncement("Saved FX preset " + destination.getFileNameWithoutExtension(),
                                               AccessibilityHandler::AnnouncementPriority::high);
      else
        NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save FX preset",
                                              "The FX preset could not be written.");
    }
    fx_chooser_.reset();
  });
}

void SynthEditor::moveToSection(int direction) {
  const int count = section_names_.size();
  if (count == 0)
    return;
  if (kSectionKeysStepThroughEffectSlots && moveThroughEffectSlots(direction))
    return;
  int current = section_names_.indexOf(last_section_name_);
  if (current < 0)
    current = 0;
  const String next = section_names_[(current + direction + count) % count];
  // Back onto an effects chain, the step before the element after it is the
  // chain's last slot.
  if (kSectionKeysStepThroughEffectSlots && direction < 0 && isEffectChainSection(next)) {
    const int slots = static_cast<int>(activeEffectSlots(next).size());
    if (slots > 0) {
      selectSectionByName(next, false);
      if (focusEffectSlotHeader(next, slots - 1))
        return;
    }
  }
  selectSectionByName(next, true);
}

bool SynthEditor::moveThroughEffectSlots(int direction) {
  // The chain focus is on: a slot (its header or a control inside it), the
  // chain's other controls, or the group the chain is laid out in (Effects,
  // Bus 1 to 3).
  String section, section_key, group;
  int slot = -1;
  for (auto* component = Component::getCurrentlyFocusedComponent();
       component != nullptr && component != &rows_container_; component = component->getParentComponent()) {
    const auto& properties = component->getProperties();
    if (properties.contains("FxSlot")) {
      section = properties["SectionName"].toString();
      slot = static_cast<int>(properties["FxSlot"]);
      break;
    }
    if (section_key.isEmpty())
      section_key = properties["SectionName"].toString();
    if (component->getParentComponent() == &rows_container_)
      group = component->getTitle();
  }
  if (section.isEmpty()) {
    if (isEffectChainSection(section_key))
      section = section_key;
    else if (section_key.isEmpty()) {
      const auto found = group_sections_.find(group);
      if (found != group_sections_.end() && found->second.size() == 1 && isEffectChainSection(found->second[0]))
        section = found->second[0];
    }
  }
  if (!isEffectChainSection(section) || !section_names_.contains(section))
    return false;

  // The chain is the current element now; leaving it steps on from there.
  last_section_name_ = section;
  const auto slots = activeEffectSlots(section);
  const auto found = std::find(slots.begin(), slots.end(), slot);
  // Elsewhere on the chain, period goes into its first slot.
  if (found == slots.end())
    return direction > 0 && focusEffectSlotHeader(section, 0);

  const int next = static_cast<int>(std::distance(slots.begin(), found)) + direction;
  if (next >= static_cast<int>(slots.size()))
    return false;
  if (next >= 0)
    return focusEffectSlotHeader(section, next);
  // Back from the first slot to the chain itself, as period from the element
  // before it arrives.
  selectSectionByName(section, true);
  return true;
}

bool SynthEditor::focusEffectSlotHeader(const String& sectionName, int position) {
  const auto slots = activeEffectSlots(sectionName);
  if (!isPositiveAndBelow(position, static_cast<int>(slots.size())))
    return false;

  const int slot = slots[static_cast<size_t>(position)];
  for (auto* component : row_focus_order_) {
    if (component == nullptr || !component->isShowing())
      continue;
    const auto& properties = component->getProperties();
    if (properties.contains("FxSlot") && static_cast<int>(properties["FxSlot"]) == slot &&
        properties["SectionName"].toString() == sectionName) {
      ensureComponentVisible(component);
      component->grabKeyboardFocus();
      return true;
    }
  }
  return false;
}

bool SynthEditor::focusSectionShortcut(const String& section) {
  if (!section_names_.contains(section))
    return false;

  selectSectionByName(section, true);
  return true;
}

bool SynthEditor::focusGroupShortcut(const String& group, const String& fallbackSection) {
  if (!group_names_.contains(group)) {
    if (fallbackSection.isNotEmpty())
      return focusSectionShortcut(fallbackSection);
    return false;
  }

  if (row_focus_order_.empty())
    showAllSections(false);
  for (auto* component : row_focus_order_) {
    if (component != nullptr && component->getTitle() == group) {
      ensureComponentVisible(component);
      component->grabKeyboardFocus();
      current_group_ = group;
      postPluginAnnouncement(group, AccessibilityHandler::AnnouncementPriority::high);
      return true;
    }
  }

  const auto found = group_sections_.find(group);
  if (found == group_sections_.end() || found->second.isEmpty())
    return false;

  selectSectionByName(found->second[0], true);
  return true;
}

bool SynthEditor::focusShortcutTarget(const KeyPress& key) {
  if (dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent()) != nullptr)
    return false;

  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const ModifierKeys modifiers = key.getModifiers();

  if (isControlChord(modifiers)) {
    if (key.getKeyCode() == '1') return focusGroupShortcut("Bus 1");
    if (key.getKeyCode() == '2') return focusGroupShortcut("Bus 2");
    if (key.getKeyCode() == '3') return focusGroupShortcut("Bus 3");
  }

  // if (modifiers.isAltDown() && !modifiers.isCommandDown() && !modifiers.isCtrlDown() && character == 'm')
  //   return focusGroupShortcut("Modulation", "Modulation routing");

  if (modifiers.isAnyModifierKeyDown())
    return false;

  switch (character) {
    case 'o': return focusGroupShortcut("Oscillators");
    case 'x': return focusGroupShortcut("Mixer and routing", kSignalRoutingSection);
    case 'f': return focusGroupShortcut("Filters");
    case 'e': return focusGroupShortcut("Envelopes");
    case 'l': return focusGroupShortcut("LFOs");
    case 'm': return focusSectionShortcut("Modulation routing");
    case 'p': return focusGroupShortcut("Effects", kEffectsChainSection);
    case 'r': return focusSectionShortcut(kSignalRoutingSection);
    case 'z': return focusGroupShortcut("Zones");
    case 'g': return focusSectionShortcut("Master and global");
    default: break;
  }

  if (key.getKeyCode() >= '1' && key.getKeyCode() <= '9') {
    const int index = key.getKeyCode() - '1';
    const auto found = group_sections_.find(current_group_);
    if (found != group_sections_.end() && isPositiveAndBelow(index, found->second.size())) {
      selectSectionByName(found->second[index], true);
      return true;
    }
  }

  return false;
}

namespace {
  // Surge-style single-octave QWERTY layout: keyboard key code -> semitone offset from the
  // base note. Letters use their uppercase ASCII code (matching KeyPress::isKeyCurrentlyDown);
  // the four punctuation keys extend the top of the range.
  struct QwertyKeyMapping {
    int key_code;
    int offset;
  };

  const QwertyKeyMapping kQwertyKeys[] = {
    { 'A', 0 },  { 'W', 1 },  { 'S', 2 },  { 'E', 3 },  { 'D', 4 },  { 'F', 5 },
    { 'T', 6 },  { 'G', 7 },  { 'Y', 8 },  { 'H', 9 },  { 'U', 10 }, { 'J', 11 },
    { 'K', 12 }, { 'O', 13 }, { 'L', 14 }, { 'P', 15 },
    { ';', 16 }, { '[', 17 }, { '\'', 18 }, { ']', 19 },
  };

  bool isQwertyNoteKeyCode(int key_code) {
    for (const QwertyKeyMapping& mapping : kQwertyKeys) {
      if (mapping.key_code == key_code)
        return true;
    }
    return false;
  }

  // Keys the virtual keyboard owns while it is on. Command and Control chords stay with the
  // focused control so shortcuts such as Command S keep working.
  bool isQwertyReservedKey(const KeyPress& key) {
    const ModifierKeys modifiers = key.getModifiers();
    if (modifiers.isCommandDown() || modifiers.isCtrlDown())
      return false;
    const int code = key.getKeyCode();
    return isQwertyNoteKeyCode(code) || code == 'X' || code == 'C' || code == 'V' || code == 'B';
  }
} // namespace

bool SynthEditor::QwertyKeyInterceptor::keyPressed(const KeyPress& key, Component*) {
  if (!editor_.isQwertyKeyboardActive() || !isQwertyReservedKey(key))
    return false;
  return editor_.keyPressed(key);
}

void SynthEditor::attachQwertyKeyInterceptor() {
  Component* focused = Component::getCurrentlyFocusedComponent();
  if (focused == qwerty_key_interceptor_target_.getComponent())
    return;
  detachQwertyKeyInterceptor();
  if (focused != nullptr && focused != this && isParentOf(focused)) {
    focused->addKeyListener(&qwerty_key_interceptor_);
    qwerty_key_interceptor_target_ = focused;
  }
}

void SynthEditor::detachQwertyKeyInterceptor() {
  if (auto* target = qwerty_key_interceptor_target_.getComponent())
    target->removeKeyListener(&qwerty_key_interceptor_);
  qwerty_key_interceptor_target_ = nullptr;
}

bool SynthEditor::isQwertyKeyboardActive() const {
  return qwerty_keyboard_on_ &&
         dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent()) == nullptr;
}

void SynthEditor::toggleQwertyKeyboard() {
  qwerty_keyboard_on_ = !qwerty_keyboard_on_;
  if (qwerty_keyboard_on_) {
    attachQwertyKeyInterceptor();
  }
  else {
    allQwertyNotesOff();
    qwerty_control_keys_down_.clear();
    detachQwertyKeyInterceptor();
  }
  postPluginAnnouncement(qwerty_keyboard_on_ ? "Virtual keyboard on" : "Virtual keyboard off",
                         AccessibilityHandler::AnnouncementPriority::high);
}

bool SynthEditor::updateQwertyNotes() {
  MidiKeyboardState* state = getSynth()->getKeyboardState();
  if (state == nullptr)
    return false;

  bool changed = false;
  const float velocity = qwerty_velocity_ / 127.0f;
  for (const QwertyKeyMapping& mapping : kQwertyKeys) {
    const int note = 12 * qwerty_octave_ + mapping.offset;
    if (note < 0 || note > 127)
      continue;

    const bool key_down = KeyPress::isKeyCurrentlyDown(mapping.key_code);
    const bool tracked = qwerty_notes_down_.count(note) != 0;
    if (key_down && !tracked) {
      state->noteOn(1, note, velocity);
      qwerty_notes_down_.insert(note);
      changed = true;
    }
    else if (!key_down && tracked) {
      state->noteOff(1, note, 0.0f);
      qwerty_notes_down_.erase(note);
      changed = true;
    }
  }
  return changed;
}

void SynthEditor::allQwertyNotesOff() {
  MidiKeyboardState* state = getSynth()->getKeyboardState();
  if (state != nullptr) {
    for (int note : qwerty_notes_down_)
      state->noteOff(1, note, 0.0f);
  }
  qwerty_notes_down_.clear();
}

bool SynthEditor::keyStateChanged(bool isKeyDown) {
  if (!isQwertyKeyboardActive())
    return false;

  // Key-down state changes arrive before the matching keyPressed, so follow focus here to
  // have the interceptor on whichever control will receive it.
  attachQwertyKeyInterceptor();

  // Release octave/velocity control keys that are no longer held so they can fire again on
  // the next physical press (see keyPressed).
  for (auto it = qwerty_control_keys_down_.begin(); it != qwerty_control_keys_down_.end();) {
    if (!KeyPress::isKeyCurrentlyDown(*it))
      it = qwerty_control_keys_down_.erase(it);
    else
      ++it;
  }

  // Only consume the key-state change when it actually drives a qwerty note; otherwise let it
  // fall through to the host so keys like Escape and Alt+F4 still close the interface.
  return updateQwertyNotes();
}

bool SynthEditor::keyPressed(const KeyPress& key) {
  if ((modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) &&
      key.getKeyCode() == KeyPress::escapeKey) {
    cancelInlineTextPrompt();
    return true;
  }
  // Escape on the save form's Save or Cancel button closes it, as in its text boxes.
  if (save_patch_dialog_visible_ && key.getKeyCode() == KeyPress::escapeKey &&
      (save_patch_ok_.hasKeyboardFocus(false) || save_patch_cancel_.hasKeyboardFocus(false))) {
    hideSavePresetDialog();
    return true;
  }
  {
    const ModifierKeys modifiers = key.getModifiers();
    if (modifiers.isAltDown() && !modifiers.isCommandDown() && !modifiers.isCtrlDown() &&
        key.getKeyCode() == 'K') {
      toggleQwertyKeyboard();
      return true;
    }
  }
  if (isQwertyKeyboardActive()) {
    const ModifierKeys modifiers = key.getModifiers();
    const int code = key.getKeyCode();
    if (code == 'X' || code == 'C') {
      // Fire once per physical press; ignore OS auto-repeat until the key is released
      // (keyStateChanged prunes qwerty_control_keys_down_ on release).
      if (qwerty_control_keys_down_.insert(code).second) {
        allQwertyNotesOff();
        qwerty_octave_ = jlimit(0, 9, qwerty_octave_ + (code == 'C' ? 1 : -1));
        postPluginAnnouncement("Octave " + String(qwerty_octave_),
                               AccessibilityHandler::AnnouncementPriority::high);
      }
      return true;
    }
    if (code == 'V' || code == 'B') {
      if (qwerty_control_keys_down_.insert(code).second) {
        int step = modifiers.isShiftDown() ? 1 : 10;
        if (code == 'V')
          step = -step;
        qwerty_velocity_ = jlimit(1, 127, qwerty_velocity_ + step);
        postPluginAnnouncement("Velocity " + String(qwerty_velocity_),
                               AccessibilityHandler::AnnouncementPriority::high);
      }
      return true;
    }
    // Consume mapped note keys so single-letter navigation is suppressed; the note on/off
    // itself is generated in keyStateChanged.
    if (isQwertyNoteKeyCode(code))
      return true;
  }
  {
    const ModifierKeys modifiers = key.getModifiers();
    if (isCommandChord(modifiers) &&
        CharacterFunctions::toLowerCase(static_cast<juce_wchar>(key.getKeyCode())) == 's') {
      if (modifiers.isShiftDown())
        savePresetAs();
      else
        savePatch();
      return true;
    }
  }
  if ((key.getKeyCode() == KeyPress::returnKey || key.getKeyCode() == KeyPress::spaceKey) &&
      preset_table_.hasKeyboardFocus(true)) {
    loadSelectedPreset();
    return true;
  }
  if (!isQwertyKeyboardActive() && focusShortcutTarget(key))
    return true;
  if (!isQwertyKeyboardActive()) {
    if (key.getTextCharacter() == ',') {
      moveToSection(-1);
      return true;
    }
    if (key.getTextCharacter() == '.') {
      moveToSection(1);
      return true;
    }
  }
  return AudioProcessorEditor::keyPressed(key);
}

void SynthEditor::timerCallback() {
  // A rebuild below may move the screen reader back onto its control after a
  // delay; what the status refreshes announce first waits for that move.
  const ScopedAnnouncementHold hold;
  // keyStateChanged ignores releases while a text field has focus; release
  // virtual-keyboard notes then instead of leaving them stuck.
  if (!qwerty_notes_down_.empty() && !isQwertyKeyboardActive())
    allQwertyNotesOff();
  refreshConvolutionIrLoads();
  refreshMultiSampleStatus();
  refreshSpeechStatus();
  if (cpu_meter_ != nullptr && cpu_meter_->isShowing())
    cpu_meter_->refresh();
  synth_.dispatchPendingGuiUpdates(*this);
  // Slot rows depend on these choices (Dimension Expander mode, Resonator model).
  // A queued rebuild re-reads every slot, and a swap in progress makes these
  // keys (physical slot ids) look changed, so wait for it rather than queue a
  // rebuild that would focus a control the swap is about to destroy.
  if (!effect_chain_rebuild_queued_) {
    for (auto& mode : effect_slot_modes_) {
      const auto found = synth_.getControls().find(mode.first.toStdString());
      const int current = effectSlotRowVariant(mode.first, found != synth_.getControls().end() ? found->second->value() : 0.0f);
      if (current != mode.second) {
        mode.second = current;
        screen_reader_refocus_requested_ = true;
        keep_switching_row_on_rebuild_ = true;
        scheduleEffectChainRebuild(last_section_name_, focusedAccessibleTitle(), focusedParameterId());
        return;
      }
    }
  }
  if (refreshFilterRowsIfNeeded())
    return;
  if (refreshRandomAmplitudeRowsIfNeeded())
    return;
  if (refreshOscillatorModeRowsIfNeeded())
    return;
  if (refreshZoneCrossfadeRowIfNeeded())
    return;
  if (refreshEnvelopeAnalogRowsIfNeeded())
    return;
  for (auto& row : rows_)
    row->refresh();
  refreshFxBusToggles();
  refreshEffectChainSummaries();
  for (int oscillator = 0; oscillator < vital::kNumOscillators; ++oscillator)
    refreshWavetableNameLabel(oscillator);
  for (int slot = 0; slot < vital::kNumOscillators; ++slot) {
    if (vital::source_types::describe(sourceSlotType(slot)).uses_sample)
      refreshSampleNameLabel(slot);
  }
}

void SynthEditor::updateFullGui() {
  // Preset and host-state loads can replace the modulation topology, the
  // effects in every slot and the oscillator types. Rebuild the section
  // parameter lists too, not just the view: otherwise an effect slot whose type
  // changed shows up as an empty group until the editor is reopened.
  rebuildSectionsAfterEffectOrderChange(last_section_name_);
  timerCallback();
  synth_.updateHostDisplay();
}

void SynthEditor::notifyMidiLearned(const std::string& name, int midi_id) {
  postPluginAnnouncement(modulationDestinationLabelForId(String(name)) + " MIDI learn set to " +
                             midiControlName(midi_id),
                         AccessibilityHandler::AnnouncementPriority::high);
}
