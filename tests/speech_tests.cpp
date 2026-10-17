// Speech oscillator tests: espeak-ng's phonemes split into syllables, the
// rendered pitch layers with their marks and steady vowel grains, the pitch
// a note sings at, the two envelope modes, which syllable each note takes,
// host state, and the editor's text box and menus.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "sound_engine.h"
#include "source_types.h"
#include "source_slot_module.h"
#include "espeak_speech.h"
#include "speech_manager.h"
#include "speech_slot.h"
#include "speech_syllables.h"
#include "speech_voices.h"
#include "synth_strings.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
  constexpr int kRate = 48000;
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void close(double actual, double expected, const std::string& message, double tolerance) {
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
           message + " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
  }

  int finish(const char* name) {
    if (failures == 0)
      std::cout << name << ": all tests passed\n";
    return failures == 0 ? 0 : 1;
  }

  struct MidiEvent {
    int sample;
    juce::MidiMessage message;
  };

  void setControl(SynthPlugin& plugin, const std::string& name, float value) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    expect(found != controls.end(), "control exists: " + name);
    if (found != controls.end())
      found->second->set(value);
  }

  float control(SynthPlugin& plugin, const std::string& name) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    return found == controls.end() ? -1.0f : found->second->value();
  }

  std::shared_ptr<vital::speech::Phrase> speak(const std::string& text, const std::string& voice) {
    vital::speech::Recording recording;
    std::string error;
    const bool spoken = vital::speech::espeak::speak(text, voice, vital::speech::espeak::kVoicePitch, recording, error);
    expect(spoken, "espeak-ng speaks \"" + text + "\": " + error);
    if (!spoken)
      return nullptr;
    return vital::speech::buildPhrase(recording);
  }

  // Every pitch layer, as the Speech slots render them.
  std::shared_ptr<vital::speech::Phrase> speakLayers(const std::string& text, const std::string& voice) {
    std::vector<vital::speech::Recording> recordings;
    for (int pitch : vital::speech::Manager::kLayerPitches) {
      recordings.emplace_back();
      std::string error;
      const bool spoken = vital::speech::espeak::speak(text, voice, pitch, recordings.back(), error);
      expect(spoken, "espeak-ng speaks \"" + text + "\" at pitch " + std::to_string(pitch) + ": " + error);
      if (!spoken)
        return nullptr;
    }
    return vital::speech::buildPhrase(recordings, vital::speech::Manager::kReferenceLayer);
  }

  const std::vector<vital::speech::Syllable>& syllablesOf(const vital::speech::Phrase& phrase) {
    static const std::vector<vital::speech::Syllable> none;
    const auto* layer = phrase.referenceLayer();
    return layer != nullptr ? layer->syllables : none;
  }

  std::string syllableList(const vital::speech::Phrase& phrase) {
    std::string list;
    const auto& syllables = syllablesOf(phrase);
    for (size_t i = 0; i < syllables.size(); ++i) {
      if (i > 0)
        list += syllables[i].index_in_word == 0 ? " " : ".";
      list += syllables[i].phonemes;
    }
    return list;
  }

  // How much the level of signal[start, end) wavers, frame to frame (40 ms):
  // the standard deviation of the frames' RMS over their mean.
  double levelWaver(const std::vector<float>& signal, int start, int end, int rate) {
    const int frame = rate / 25;
    std::vector<double> levels;
    for (int i = start; i + frame <= end; i += frame) {
      double sum = 0.0;
      for (int k = i; k < i + frame; ++k)
        sum += double(signal[static_cast<size_t>(k)]) * signal[static_cast<size_t>(k)];
      levels.push_back(std::sqrt(sum / frame));
    }
    if (levels.empty())
      return 0.0;
    double mean = 0.0;
    for (double level : levels)
      mean += level;
    mean /= levels.size();
    double variance = 0.0;
    for (double level : levels)
      variance += (level - mean) * (level - mean);
    return mean > 0.0 ? std::sqrt(variance / levels.size()) / mean : 0.0;
  }

  // Waits for the slots' renders and hands them to the engine.
  void settle(SynthPlugin& plugin) {
    auto* speech = plugin.getSpeechManager();
    speech->refresh();
    expect(speech->waitForPendingWork(20000), "renders finish");
    speech->processPendingResults();
  }

  // Slot 1 sings `text` straight to the effects, every effect and other
  // oscillator off, Env 1 open with a near-zero release.
  void preparePlayback(SynthPlugin& plugin, const std::string& text) {
    plugin.setPlayConfigDetails(0, 2, kRate, 128);
    plugin.prepareToPlay(kRate, 128);
    setControl(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= 20; ++i)
        setControl(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    for (int i = 1; i <= 8; ++i)
      setControl(plugin, "osc_" + std::to_string(i) + "_on", 0.0f);
    plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kSpeech));
    setControl(plugin, "osc_1_on", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(vital::constants::kEffects));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_sustain", 1.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
    expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kSpeech,
           "slot 1 runs the Speech type");
    plugin.getSpeechManager()->setText(0, text);
    settle(plugin);
    expect(plugin.getEngine()->getSpeechSlot(0)->phrase() != nullptr, "slot 1 has its phrase");
  }

  std::vector<float> render(SynthPlugin& plugin, int samples, const std::vector<MidiEvent>& events) {
    std::vector<float> result(static_cast<size_t>(samples));
    for (int offset = 0; offset < samples;) {
      const int count = std::min(128, samples - offset);
      juce::AudioBuffer<float> audio(2, count);
      audio.clear();
      juce::MidiBuffer midi;
      for (const MidiEvent& event : events) {
        if (event.sample >= offset && event.sample < offset + count)
          midi.addEvent(event.message, event.sample - offset);
      }
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i)
        result[static_cast<size_t>(offset + i)] = audio.getSample(0, i);
      offset += count;
    }
    return result;
  }

  bool finite(const std::vector<float>& signal) {
    for (float sample : signal) {
      if (!std::isfinite(sample))
        return false;
    }
    return true;
  }

  double rms(const std::vector<float>& signal, int start, int end) {
    double sum = 0.0;
    end = std::min<int>(end, static_cast<int>(signal.size()));
    for (int i = start; i < end; ++i)
      sum += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
    return end > start ? std::sqrt(sum / (end - start)) : 0.0;
  }

  // The fundamental of signal[start, end) by autocorrelation, in Hz.
  double pitchOf(const std::vector<float>& signal, int start, int end, int rate) {
    const int min_lag = rate / 1500;
    const int max_lag = rate / 50;
    double best = 0.0;
    int best_lag = 0;
    std::vector<double> values(static_cast<size_t>(max_lag + 2), 0.0);
    for (int lag = min_lag; lag <= max_lag + 1; ++lag) {
      double cross = 0.0;
      double first = 0.0;
      double second = 0.0;
      for (int i = start; i + lag < end; ++i) {
        cross += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i + lag)];
        first += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
        second += double(signal[static_cast<size_t>(i + lag)]) * signal[static_cast<size_t>(i + lag)];
      }
      values[static_cast<size_t>(lag)] = first > 0.0 && second > 0.0 ? cross / std::sqrt(first * second) : 0.0;
      best = std::max(best, values[static_cast<size_t>(lag)]);
    }
    // The shortest lag near the best: the period, not a multiple of it.
    for (int lag = min_lag + 1; lag <= max_lag; ++lag) {
      const double value = values[static_cast<size_t>(lag)];
      if (value >= 0.85 * best && value >= values[static_cast<size_t>(lag - 1)] &&
          value >= values[static_cast<size_t>(lag + 1)]) {
        best_lag = lag;
        break;
      }
    }
    return best_lag > 0 ? static_cast<double>(rate) / best_lag : 0.0;
  }

  // ---- Syllables ------------------------------------------------------------------

  int syllables() {
    using vital::speech::isNucleus;
    expect(isNucleus("a") && isNucleus("ˈæ") && isNucleus("ˌɔː") && isNucleus("eɪ") && isNucleus("ɚ"),
           "vowels, stressed vowels and diphthongs are nuclei");
    expect(isNucleus("n̩") && isNucleus("ɹ̩"), "syllabic consonants are nuclei");
    expect(!isNucleus("t") && !isNucleus("ʃ") && !isNucleus("dʒ") && !isNucleus("j") && !isNucleus(""),
           "consonants and pauses are not");

    // Consonants between vowels: none, one and three.
    std::vector<vital::speech::PhonemeEvent> events = {
      { "m", 0, 0 }, { "ˈɔ", 100, 0 }, { "n", 200, 0 }, { "s", 300, 0 }, { "t", 400, 0 }, { "ɚ", 500, 0 },
      { "", 600, 0 },
      { "h", 700, 1 }, { "ə", 800, 1 }, { "l", 900, 1 }, { "ˈoʊ", 1000, 1 },
      { "ʃ", 1200, 2 }, { "h", 1300, 2 },
    };
    const auto spans = vital::speech::syllabify(events, 1400);
    expect(spans.size() == 5, "monster, hello and a vowelless word make five syllables (" +
                                  std::to_string(spans.size()) + ")");
    if (spans.size() == 5) {
      expect(spans[0].phonemes == "mˈɔn" && spans[1].phonemes == "stɚ", "mon-ster");
      expect(spans[0].start == 0 && spans[0].end == 300 && spans[1].start == 300 && spans[1].end == 600,
             "syllable spans end at the next phoneme or pause");
      expect(spans[0].nucleus_start == 100 && spans[0].nucleus_end == 200, "the nucleus is the vowel");
      expect(spans[2].phonemes == "hə" && spans[3].phonemes == "lˈoʊ", "a single consonant starts the next syllable");
      expect(spans[3].index_in_word == 1 && spans[3].word == 1, "syllables know their word");
      expect(spans[4].phonemes == "ʃh" && spans[4].nucleus_start < 0, "a word without a vowel is one syllable");
    }

    // espeak-ng itself.
    struct Case {
      const char* text;
      const char* voice;
      size_t count;
    };
    for (const Case& test : { Case { "Happy birthday to you", "gmw/en-US", 6 },
                              Case { "monster", "gmw/en-US", 2 },
                              Case { "strengths", "gmw/en-US", 1 },
                              Case { "Guten Morgen", "gmw/de", 4 },
                              Case { "Buenos días", "roa/es", 4 } }) {
      auto phrase = speak(test.text, test.voice);
      if (phrase == nullptr)
        continue;
      const std::string list = syllableList(*phrase);
      expect(syllablesOf(*phrase).size() == test.count, std::string(test.text) + " has " + std::to_string(test.count) +
                                                         " syllables (" + list + ")");
      std::cout << test.text << ": " << list << '\n';
    }
    return finish("syllables");
  }

  // ---- The rendered phrase --------------------------------------------------------

  int phrase() {
    for (const char* voice : { "gmw/en-US", "gmw/en-US+f3", "roa/fr+klatt" }) {
      auto phrase = speakLayers("Singing all day long", voice);
      if (phrase == nullptr)
        continue;
      const std::string name = voice;
      expect(phrase->layers.size() >= 4, name + ": the phrase is spoken at several pitches (" +
                                              std::to_string(phrase->layers.size()) + " layers)");
      const auto* reference = phrase->referenceLayer();
      expect(reference != nullptr && reference->frequency > 60.0f && reference->frequency < 400.0f,
             name + ": the voice's own pitch is a voice pitch");
      std::string frequencies;
      for (size_t l = 0; l < phrase->layers.size(); ++l) {
        frequencies += " " + std::to_string(static_cast<int>(phrase->layers[l].frequency));
        if (l > 0)
          expect(phrase->layers[l].frequency > phrase->layers[l - 1].frequency * 1.1f,
                 name + ": layers rise in pitch");
      }
      std::cout << name << " layers (Hz):" << frequencies << '\n';
      if (phrase->layers.size() >= 2) {
        const float span = phrase->layers.back().frequency / phrase->layers.front().frequency;
        expect(span > 2.0f, name + ": the layers span more than an octave (" + std::to_string(span) + ")");
        expect(phrase->layerFor(phrase->layers.front().frequency * 0.5f) == 0 &&
               phrase->layerFor(phrase->layers.back().frequency * 2.0f) == static_cast<int>(phrase->layers.size()) - 1,
               name + ": notes beyond the layers sing from the nearest one");
      }
      for (size_t l = 0; l < phrase->layers.size(); ++l) {
        const auto& layer = phrase->layers[l];
        expect(layer.syllables.size() == 5, name + ": five syllables in every layer (" + syllableList(*phrase) + ")");
        for (size_t i = 0; i < layer.syllables.size(); ++i) {
          const auto& syllable = layer.syllables[i];
          const std::string label = name + " layer " + std::to_string(l) + " syllable " + std::to_string(i + 1);
          float peak = 0.0f;
          for (float sample : syllable.audio)
            peak = std::max(peak, std::abs(sample));
          expect(peak > 0.05f && peak <= 0.9001f, label + " is normalised audio");
          expect(!syllable.marks.empty(), label + " has pitch marks");
          bool ordered = true;
          int voiced = 0;
          for (size_t m = 1; m < syllable.marks.size(); ++m)
            ordered = ordered && syllable.marks[m].position > syllable.marks[m - 1].position;
          for (const auto& mark : syllable.marks) {
            voiced += mark.voiced ? 1 : 0;
            ordered = ordered && mark.half_width > 0;
          }
          expect(ordered, label + " marks rise");
          expect(voiced >= 3, label + " has voiced marks (" + std::to_string(voiced) + " of " +
                                  std::to_string(syllable.marks.size()) + ", " + syllable.phonemes + ")");
          expect(syllable.hold_point >= 0 && syllable.hold_point < syllable.length(), label + " holds inside its audio");
          bool on_mark = false;
          for (const auto& mark : syllable.marks)
            on_mark = on_mark || (mark.voiced && mark.position == syllable.hold_point);
          expect(on_mark, label + " holds on a voiced mark");
          const size_t grain = 2 * static_cast<size_t>(std::lround(layer.period));
          expect(syllable.steady_grain.size() == grain, label + " has a steady grain two periods long");
          if (syllable.steady_grain.size() == grain) {
            // As loud as the recording's grain at the hold point, and silent at its ends.
            double steady = 0.0;
            double recorded = 0.0;
            for (size_t n = 0; n < grain; ++n) {
              const int index = syllable.hold_point - static_cast<int>(grain / 2) + static_cast<int>(n);
              const double window = 0.5 - 0.5 * std::cos(6.283185307 * n / grain);
              const double value = index >= 0 && index < syllable.length() ? syllable.audio[static_cast<size_t>(index)] * window : 0.0;
              recorded += value * value;
              steady += double(syllable.steady_grain[n]) * syllable.steady_grain[n];
            }
            close(std::sqrt(steady / std::max(recorded, 1.0e-20)), 1.0, label + " steady grain level", 0.01);
            expect(std::abs(syllable.steady_grain.front()) < 1.0e-3f && std::abs(syllable.steady_grain.back()) < 0.05f,
                   label + " steady grain is windowed");
          }
          // Voiced marks sit about a period apart.
          int close_spacing = 0;
          int pairs = 0;
          for (size_t m = 1; m < syllable.marks.size(); ++m) {
            if (!syllable.marks[m].voiced || !syllable.marks[m - 1].voiced)
              continue;
            ++pairs;
            const float spacing = static_cast<float>(syllable.marks[m].position - syllable.marks[m - 1].position);
            close_spacing += std::abs(spacing - layer.period) < 0.25f * layer.period ? 1 : 0;
          }
          expect(pairs == 0 || close_spacing >= pairs * 8 / 10, label + " voiced marks are a period apart");
        }
      }
    }

    // Text that says nothing.
    auto empty = speak("...", "gmw/en-US");
    expect(empty == nullptr || empty->numSyllables() == 0, "punctuation alone sings nothing");
    return finish("phrase");
  }

  // ---- Pitch and sustain through the plugin ---------------------------------------

  int pitch() {
    for (int note : { 45, 57, 69, 76 }) {
      SynthPlugin plugin;
      preparePlayback(plugin, "la");
      const int held = kRate * 3 / 2;
      const auto audio = render(plugin, held + kRate / 2, { { 0, juce::MidiMessage::noteOn(1, note, 1.0f) },
                                                           { held, juce::MidiMessage::noteOff(1, note) } });
      const std::string label = "note " + std::to_string(note);
      expect(finite(audio), label + " is finite");
      const double expected = 440.0 * std::pow(2.0, (note - 69) / 12.0);
      // Well into the held vowel, and late in it.
      for (int start : { kRate / 2, kRate }) {
        const double measured = pitchOf(audio, start, start + kRate / 10, kRate);
        close(measured, expected, label + " sings its pitch at " + std::to_string(start) + " samples",
              expected * 0.02);
      }
      expect(rms(audio, kRate * 5 / 4, held) > 1.0e-3, label + " holds the vowel while the key is down");
      // The held vowel is the steady grain: its level barely moves.
      const double waver = levelWaver(audio, kRate / 2, held, kRate);
      expect(waver < 0.05, label + " holds a steady vowel (level wavers " + std::to_string(waver) + ")");
      expect(rms(audio, held + kRate / 4, held + kRate / 2) < 1.0e-4, label + " ends after release");
    }
    return finish("pitch");
  }

  // ---- Envelope modes --------------------------------------------------------------

  int envelope() {
    // "Moon" ends on a held n after the vowel. Env 1 has no release, so only
    // the Voice mode sings the n once the key is up.
    const int held = kRate / 2;
    double voice_tail = 0.0;
    double envelope_tail = 0.0;
    for (int mode : { 0, 1 }) {
      SynthPlugin plugin;
      preparePlayback(plugin, "moon");
      setControl(plugin, "osc_1_speech_amp_env", static_cast<float>(mode));
      expect(plugin.getEngine()->getSourceSlot(0)->holdsVoiceOpen() == (mode == 0),
             mode == 0 ? "Voice holds the voice open" : "Envelope 1 does not hold the voice open");
      const auto audio = render(plugin, held + kRate, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                       { held, juce::MidiMessage::noteOff(1, 60) } });
      expect(finite(audio), "mode " + std::to_string(mode) + " is finite");
      expect(rms(audio, held / 2, held) > 1.0e-3, "mode " + std::to_string(mode) + " sings while held");
      const double tail = rms(audio, held + kRate / 200, held + kRate / 20);
      (mode == 0 ? voice_tail : envelope_tail) = tail;
      expect(rms(audio, held + kRate / 2, held + kRate) < 1.0e-5, "mode " + std::to_string(mode) + " ends");
      expect(plugin.getEngine()->getNumActiveVoices() == 0, "mode " + std::to_string(mode) + " frees the voice");
    }
    expect(voice_tail > 1.0e-3, "Voice sings the final consonant after release (" + std::to_string(voice_tail) + ")");
    expect(envelope_tail < voice_tail * 0.1, "Envelope 1 with no release cuts it (" + std::to_string(envelope_tail) + ")");
    return finish("envelope");
  }

  // ---- Mono legato ----------------------------------------------------------------

  int legato() {
    // One voice: 60, then 64 pressed while 60 is held, 60 released, 64 released.
    const int second = kRate / 2;
    const std::vector<MidiEvent> events = {
      { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
      { second, juce::MidiMessage::noteOn(1, 64, 1.0f) },
      { second + kRate / 4, juce::MidiMessage::noteOff(1, 60) },
      { second + kRate * 3 / 4, juce::MidiMessage::noteOff(1, 64) },
    };
    for (bool legato : { true, false }) {
      SynthPlugin plugin;
      preparePlayback(plugin, "one two three");
      setControl(plugin, "polyphony", 1.0f);
      setControl(plugin, "legato", legato ? 1.0f : 0.0f);
      const std::string label = legato ? "legato" : "mono";
      const auto audio = render(plugin, second + kRate, events);
      expect(finite(audio), label + " is finite");
      expect(rms(audio, second, second + kRate / 10) > 1.0e-3, label + " sings through the note change");
      const double expected = 440.0 * std::pow(2.0, (64 - 69) / 12.0);
      close(pitchOf(audio, second + kRate * 3 / 8, second + kRate * 3 / 8 + kRate / 10, kRate), expected,
            label + " moves to the new pitch", expected * 0.02);
      // The next syllable a note would take: legato used one, mono two.
      const int next = plugin.getEngine()->getSpeechSlot(0)->takeSyllable(10.0);
      expect(next == (legato ? 1 : 2), label + (legato ? " keeps the first syllable across the legato note"
                                                       : " sings a new syllable on each note") +
                                           " (next is " + std::to_string(next) + ")");
    }
    return finish("legato");
  }

  // ---- Which syllable each note sings ---------------------------------------------

  int sequence() {
    auto phrase = std::make_shared<vital::speech::Phrase>();
    phrase->layers.emplace_back();
    for (int i = 0; i < 3; ++i) {
      vital::speech::Syllable syllable;
      syllable.audio.assign(1000, 0.1f);
      syllable.marks.push_back({ 500, 100, true });
      phrase->layers.back().syllables.push_back(syllable);
    }
    vital::SpeechSlot slot;
    expect(slot.takeSyllable(0.0) == -1, "a slot without words sings nothing");
    slot.setPhrase(phrase);
    const uint32_t generation = slot.generation();
    expect(slot.takeSyllable(0.0) == 0, "the first note sings the first syllable");
    expect(slot.takeSyllable(0.01) == 0, "a note of the same chord sings it too");
    slot.advanceClock(4800, kRate);
    expect(slot.takeSyllable(0.0) == 1, "the next note sings the next syllable");
    slot.advanceClock(4800, kRate);
    expect(slot.takeSyllable(0.0) == 2, "and the next");
    slot.advanceClock(4800, kRate);
    expect(slot.takeSyllable(0.0) == 0, "after the last syllable the words start again");
    slot.advanceClock(4800, kRate);
    slot.rewind();
    expect(slot.takeSyllable(0.0) == 0, "rewinding starts the words again");

    auto other = std::make_shared<vital::speech::Phrase>(*phrase);
    slot.setPhrase(other);
    expect(slot.phraseFor(generation) == phrase.get(), "notes of the previous words finish with them");
    expect(slot.phraseFor(slot.generation()) == other.get(), "new notes sing the new words");
    slot.setPhrase(phrase);
    expect(slot.phraseFor(generation) == nullptr, "words two changes old are let go");

    // Through the plugin: three notes in a row and a chord all sound.
    SynthPlugin plugin;
    preparePlayback(plugin, "one two three four");
    auto* speech = plugin.getEngine()->getSpeechSlot(0);
    std::vector<MidiEvent> events;
    for (int i = 0; i < 3; ++i) {
      events.push_back({ i * kRate / 2, juce::MidiMessage::noteOn(1, 60, 1.0f) });
      events.push_back({ i * kRate / 2 + kRate / 3, juce::MidiMessage::noteOff(1, 60) });
    }
    events.push_back({ 3 * kRate / 2, juce::MidiMessage::noteOn(1, 60, 1.0f) });
    events.push_back({ 3 * kRate / 2, juce::MidiMessage::noteOn(1, 64, 1.0f) });
    events.push_back({ 3 * kRate / 2, juce::MidiMessage::noteOn(1, 67, 1.0f) });
    const auto audio = render(plugin, 2 * kRate, events);
    expect(finite(audio), "the sequence is finite");
    for (int i = 0; i < 4; ++i)
      expect(rms(audio, i * kRate / 2, i * kRate / 2 + kRate / 4) > 1.0e-3,
             "note " + std::to_string(i + 1) + " sings");
    // Three notes took "one", "two", "three"; the chord sang "four" together.
    expect(speech->takeSyllable(10.0) == 0, "the chord took one syllable, so the words start again");
    return finish("sequence");
  }

  // ---- Host state and parameters --------------------------------------------------

  int parameters() {
    juce::MemoryBlock state;
    const int french = vital::speech::languageIndex("roa/fr");
    {
      SynthPlugin plugin;
      for (int slot = 1; slot <= vital::kNumOscillators; ++slot) {
        const std::string prefix = "osc_" + std::to_string(slot) + "_speech_";
        expect(plugin.getControls().count(prefix + "language") == 1 &&
               plugin.getControls().count(prefix + "variant") == 1, "slot " + std::to_string(slot) + " has its voice");
      }
      expect(strings::kSpeechLanguageNames[vital::speech::kDefaultLanguage] == "English (America)",
             "the default language is English (America)");
      expect(control(plugin, "osc_2_speech_language") == vital::speech::kDefaultLanguage &&
             control(plugin, "osc_2_speech_variant") == 0.0f, "slots default to English with its own voice");
      expect(control(plugin, "osc_2_speech_amp_env") == 0.0f && strings::kSpeechAmpEnvNames[0] == "Voice",
             "slots default to the Voice envelope");
      expect(strings::kSpeechVariantNames[0] == "Default", "variant 0 is the language's own voice");
      expect(vital::speech::voiceName(french, 0) == "roa/fr", "voice names without a variant");
      preparePlayback(plugin, "Bonjour");
      setControl(plugin, "osc_1_speech_language", static_cast<float>(french));
      setControl(plugin, "osc_1_speech_variant", 3.0f);
      setControl(plugin, "osc_1_speech_amp_env", 1.0f);
      settle(plugin);
      auto phrase = plugin.getEngine()->getSpeechSlot(0)->phrase();
      expect(phrase != nullptr && phrase->language == french && phrase->variant == 3,
             "a new language renders the words again");
      expect(vital::speech::voiceName(french, 3) == std::string("roa/fr+") + vital::speech::kVariantIds[2],
             "voice names add the variant");
      plugin.getStateInformation(state);

      // Another type frees the phrase; Speech again renders it.
      plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kBraids));
      settle(plugin);
      expect(plugin.getEngine()->getSpeechSlot(0)->phrase() == nullptr, "another type lets the words go");
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin, "something else");
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      plugin.checkOversampling();
      settle(plugin);
      expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kSpeech,
             "the restored slot runs Speech");
      expect(plugin.getSpeechManager()->text(0) == "Bonjour", "the words are restored");
      expect(control(plugin, "osc_1_speech_language") == french && control(plugin, "osc_1_speech_variant") == 3.0f,
             "the language and variant are restored");
      expect(control(plugin, "osc_1_speech_amp_env") == 1.0f, "the envelope mode is restored");
      auto phrase = plugin.getEngine()->getSpeechSlot(0)->phrase();
      expect(phrase != nullptr && phrase->text == "Bonjour" && phrase->numSyllables() == 2,
             "the restored words are rendered");
      const auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
      expect(finite(audio) && rms(audio, 0, kRate / 2) > 1.0e-3, "the restored words sing");
      plugin.getSpeechManager()->setText(0, "");
      settle(plugin);
      expect(plugin.getEngine()->getSpeechSlot(0)->phrase() == nullptr, "no words, no phrase");
      // The note still held from before finishes with the words it began.
      const auto silent = render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOff(1, 60) },
                                                     { 0, juce::MidiMessage::noteOn(1, 62, 1.0f) } });
      expect(rms(silent, kRate / 8, kRate / 4) < 1.0e-5, "a slot without words is silent");
    }
    return finish("parameters");
  }

  // ---- Editor ---------------------------------------------------------------------

  Component* findTitled(Component& parent, const std::function<bool(const String&)>& match) {
    if (match(parent.getTitle()))
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = findTitled(*child, match))
        return found;
    }
    return nullptr;
  }

  Component* titledExactly(Component& parent, const String& title) {
    return findTitled(parent, [&title](const String& text) { return text == title; });
  }

  Component* oscillatorSection(Component& parent) {
    if (parent.getProperties()["SectionName"].toString() == "Oscillator 1")
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = oscillatorSection(*child))
        return found;
    }
    return nullptr;
  }

  template <typename Predicate>
  void pumpUntil(Predicate done, int timeout_ms = 5000) {
    const uint32 start = juce::Time::getMillisecondCounter();
    while (!done() && juce::Time::getMillisecondCounter() - start < static_cast<uint32>(timeout_ms))
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  int editorTest() {
    SynthPlugin synth;
    synth.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kSpeech));
    synth.getSpeechManager()->setText(0, "Hello world");
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpUntil([&] { return synth.getSpeechManager()->status(0).state == vital::speech::SlotStatus::kReady; });

    auto* section = oscillatorSection(editor);
    expect(section != nullptr, "Oscillator 1 section exists");
    if (section == nullptr)
      return finish("editor");

    expect(titledExactly(*section, "Speech controls") != nullptr, "a Speech controls group");
    auto* text = dynamic_cast<TextEditor*>(titledExactly(*section, "1 Text to sing"));
    expect(text != nullptr, "a text box for the words");
    if (text != nullptr) {
      expect(text->getText() == "Hello world", "the text box shows the words");
      expect(text->getWantsKeyboardFocus() && text->getHelpText().isNotEmpty(), "the text box is accessible");
      expect(text->getDescription().contains("3 syllables"), "the text box says how many syllables (" +
                                                                 text->getDescription().toStdString() + ")");
      text->setText("Row row row your boat", dontSendNotification);
      if (text->onReturnKey)
        text->onReturnKey();
      expect(synth.getSpeechManager()->text(0) == "Row row row your boat", "Enter hands the words to the slot");
      pumpUntil([&] { return synth.getSpeechManager()->status(0).syllables == 5; });
      expect(synth.getSpeechManager()->status(0).syllables == 5, "the new words are rendered");
    }

    auto* language = dynamic_cast<ComboBox*>(titledExactly(*section, "1 Speech Language"));
    expect(language != nullptr, "the language is a menu");
    if (language != nullptr) {
      expect(language->getNumItems() == vital::speech::kNumLanguages, "the menu lists every language");
      expect(language->getText() == "English (America)", "English (America) is selected");
      expect(language->getWantsKeyboardFocus() && language->getDescription().isNotEmpty(), "the menu is accessible");
    }
    auto* variant = dynamic_cast<ComboBox*>(titledExactly(*section, "1 Speech Variant"));
    expect(variant != nullptr, "the variant is a menu");
    if (variant != nullptr) {
      expect(variant->getNumItems() == vital::speech::kNumVariants, "the menu lists every variant");
      expect(variant->getText() == "Default", "the language's own voice is selected");
    }
    auto* envelope = titledExactly(*section, "1 Speech Amplitude Envelope");
    expect(envelope != nullptr && envelope->getWantsKeyboardFocus(), "an envelope mode row");
    if (envelope != nullptr)
      expect(envelope->getDescription().contains("last consonant"), "the envelope mode is described");
    if (text != nullptr && language != nullptr && variant != nullptr)
      expect(text->getScreenY() < language->getScreenY() && language->getScreenY() < variant->getScreenY(),
             "the text box comes first, then the language and the variant");
    expect(findTitled(*section, [](const String& t) {
             return t.contains("Transpose") || t.contains("Warp") || t.contains("Braids") || t.contains("Wave Frame");
           }) == nullptr,
           "Speech shows no other controls");
    return finish("editor");
  }

  // Not a ctest: the start of "Happy Birthday", sung, for listening.
  int renderSong(const juce::File& file) {
    SynthPlugin plugin;
    preparePlayback(plugin, "Happy birthday to you");
    setControl(plugin, "env_1_release", 0.15f);
    struct Note { int key; double beats; };
    const Note notes[] = { { 67, 0.75 }, { 67, 0.25 }, { 69, 1.0 }, { 67, 1.0 }, { 72, 1.0 }, { 71, 2.0 } };
    const double beat = 0.5 * kRate;
    std::vector<MidiEvent> events;
    double time = 0.1 * kRate;
    for (const Note& note : notes) {
      const int start = static_cast<int>(time);
      const int end = static_cast<int>(time + note.beats * beat - 0.04 * kRate);
      events.push_back({ start, juce::MidiMessage::noteOn(1, note.key, 0.9f) });
      events.push_back({ end, juce::MidiMessage::noteOff(1, note.key) });
      time += note.beats * beat;
    }
    const int length = static_cast<int>(time + kRate);
    std::vector<float> left(static_cast<size_t>(length));
    juce::AudioBuffer<float> song(2, length);
    for (int offset = 0; offset < length;) {
      const int count = std::min(128, length - offset);
      juce::AudioBuffer<float> audio(2, count);
      audio.clear();
      juce::MidiBuffer midi;
      for (const MidiEvent& event : events) {
        if (event.sample >= offset && event.sample < offset + count)
          midi.addEvent(event.message, event.sample - offset);
      }
      plugin.processBlock(audio, midi);
      for (int channel = 0; channel < 2; ++channel)
        song.copyFrom(channel, offset, audio, channel, 0, count);
      offset += count;
    }
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr) {
      std::cerr << "cannot write " << file.getFullPathName() << '\n';
      return 1;
    }
    const auto options = juce::AudioFormatWriterOptions().withSampleRate(kRate).withNumChannels(2).withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer = format.createWriterFor(stream, options);
    if (writer == nullptr)
      return 1;
    writer->writeFromAudioSampleBuffer(song, 0, length);
    std::cout << "wrote " << file.getFullPathName() << " (" << syllableList(*plugin.getEngine()->getSpeechSlot(0)->phrase())
              << ")\n";
    return 0;
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  vital::speech::espeak::setDataDirectory(juce::File(ATLAS_TEST_ESPEAK_DATA));
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  if (flag == "--syllables")
    return syllables();
  if (flag == "--phrase")
    return phrase();
  if (flag == "--pitch")
    return pitch();
  if (flag == "--sequence")
    return sequence();
  if (flag == "--envelope")
    return envelope();
  if (flag == "--legato")
    return legato();
  if (flag == "--parameters")
    return parameters();
  if (flag == "--editor")
    return editorTest();
  if (flag == "--render" && argc > 2)
    return renderSong(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
  std::cerr << "usage: AtlasSpeechTests --syllables | --phrase | --pitch | --envelope | --legato | --sequence | --parameters | --editor | "
               "--render <file.wav>\n";
  return 2;
}
