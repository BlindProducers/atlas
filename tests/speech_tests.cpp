// Speech oscillator tests: OpenEVV's renders (the frames a voice replays
// against the engine's own samples, the syllable splitter, every language,
// the voice controls) and the Speech type in the plugin (a piece per key, its
// pitch, chords, looping, host state and the editor).

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "parameter_slider.h"
#include "sound_engine.h"
#include "source_types.h"
#include "source_slot_module.h"
#include "speech_manager.h"
#include "speech_slot.h"
#include "speech_source.h"
#include "openevv_engine.h"
#include "synth_strings.h"

#if ATLAS_HAS_OPENEVV
#include "atlas_openevv.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {
  namespace evv = atlas::openevv;

  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  int finish(const char* name) {
    if (failures == 0)
      std::cout << name << ": all tests passed\n";
    return failures == 0 ? 0 : 1;
  }

  std::shared_ptr<const evv::Utterance> speak(const std::string& text, int language = 0,
                                              evv::VoiceSettings voice = {}) {
    evv::RenderRequest request;
    request.text = text;
    request.language = language;
    request.voice = voice;
    std::string error;
    auto utterance = evv::render(request, error);
    expect(utterance != nullptr, "renders \"" + text + "\": " + error);
    return utterance;
  }

  std::string unitsText(const evv::Utterance& utterance, int split) {
    std::string out;
    for (const auto& unit : utterance.unitsFor(split)) {
      out += "[" + std::to_string(unit.start) + "-" + std::to_string(unit.end) + " w" + std::to_string(unit.word) +
             " " + std::to_string(unit.part + 1) + "/" + std::to_string(unit.parts) + "]";
    }
    return out;
  }

  // ---- The engine ------------------------------------------------------------

  // A render's frames played through a voice's synthesiser give the engine's
  // own samples, sample for sample: the voices hear what Eloquence says.
  int engineTest() {
    expect(evv::available(), "the engine is built in");
    expect(evv::languageAvailable(0), "US English is built in");
    auto utterance = speak("Hello there, wonderful Atlas.");
    if (utterance == nullptr)
      return finish("engine");
    expect(utterance->frames.size() > 300 && utterance->samples.size() > 20000,
           "the render has frames and samples (" + std::to_string(utterance->frames.size()) + ")");
    expect(utterance->volumes.size() == utterance->frames.size() &&
           utterance->voice_of_frame.size() == utterance->frames.size() && !utterance->voices.empty(),
           "every frame has its volume and voice");
    long frame_samples = 0;
    for (const auto& frame : utterance->frames)
      frame_samples += static_cast<long>(frame[evv::kStep]) * evv::kEngineRate / 1000;
    expect(frame_samples == static_cast<long>(utterance->samples.size()),
           "the frames last as long as the samples (" + std::to_string(frame_samples) + " and " +
           std::to_string(utterance->samples.size()) + ")");

#if ATLAS_HAS_OPENEVV
    struct Sink {
      std::vector<int16_t> samples;
    } sink;
    alignas(16) unsigned char state[ATLAS_KLATT_STATE_BYTES];
    const auto collect = [](void* user, const int32_t* samples, int32_t count) {
      auto* target = static_cast<Sink*>(user);
      for (int i = 0; i < count; ++i)
        target->samples.push_back(static_cast<int16_t>(std::clamp<int32_t>(samples[i], -32768, 32767)));
    };
    const evv::KlattVoice& source = utterance->voices.front();
    const AtlasKlattVoice voice { source.add_20, source.add_24, source.add_28, source.add_2c, source.n_formants };
    expect(atlas_klatt_open(state, evv::kEngineRate, &voice, utterance->volumes.front(), collect, &sink) != 0,
           "a voice's synthesiser opens");
    for (size_t i = 0; i < utterance->frames.size(); ++i) {
      atlas_klatt_set_volume(state, utterance->volumes[i]);
      atlas_klatt_synth(state, utterance->frames[i].data());
    }
    size_t differ = 0;
    for (size_t i = 0; i < std::min(sink.samples.size(), utterance->samples.size()); ++i)
      differ += sink.samples[i] != utterance->samples[i] ? 1 : 0;
    expect(sink.samples.size() == utterance->samples.size() && differ == 0,
           "the frames replay the engine's samples exactly (" + std::to_string(differ) + " of " +
           std::to_string(sink.samples.size()) + " differ)");
#endif

    // The same text renders the same every time (a fresh instance each render).
    auto again = speak("Hello there, wonderful Atlas.");
    expect(again != nullptr && again->samples == utterance->samples, "a render is deterministic");
    // Nothing to say is no render at all.
    auto empty = speak("   ");
    expect(empty != nullptr && empty->frames.empty() && empty->units[evv::kSyllables].empty(), "blank text is silent");
    return finish("engine");
  }

  // Syllables, words and the whole text of a few phrases with known counts.
  int splitTest() {
    struct Case {
      const char* text;
      int words;
      int syllables;
    };
    const Case cases[] = {
      { "Hello wonderful Atlas.", 3, 7 },
      { "One two three four five.", 5, 5 },
      { "Banana.", 1, 3 },
      { "Synthesizer.", 1, 4 },
      { "Sing to me.", 3, 3 },
    };
    for (const Case& test : cases) {
      auto utterance = speak(test.text);
      if (utterance == nullptr)
        continue;
      const auto& syllables = utterance->units[evv::kSyllables];
      const auto& words = utterance->units[evv::kWords];
      expect(static_cast<int>(words.size()) == test.words,
             std::string(test.text) + ": " + std::to_string(test.words) + " words (" + unitsText(*utterance, evv::kWords) + ")");
      expect(static_cast<int>(syllables.size()) == test.syllables,
             std::string(test.text) + ": " + std::to_string(test.syllables) + " syllables (" +
             unitsText(*utterance, evv::kSyllables) + ")");
      expect(utterance->units[evv::kWholeText].size() == 1, std::string(test.text) + ": one whole text");
      int previous_end = 0;
      bool ordered = true;
      bool sounding = true;
      for (const auto& unit : syllables) {
        ordered = ordered && unit.start >= previous_end && unit.start <= unit.nucleus && unit.nucleus < unit.end;
        sounding = sounding && !evv::isSilent(utterance->frames[static_cast<size_t>(unit.start)]);
        previous_end = unit.end;
      }
      expect(ordered, std::string(test.text) + ": syllables in order, each around its nucleus");
      expect(sounding, std::string(test.text) + ": no syllable starts in silence");
      for (const auto& unit : syllables)
        expect(unit.f0 > 0, std::string(test.text) + ": every syllable has a pitch");
    }

    // Each word gets its own syllables, at the usual speed and faster: no
    // word runs into the next ("out of" is two keys) and none is lost.
    struct WordCase {
      const char* text;
      std::vector<int> syllables;
    };
    const WordCase word_cases[] = {
      { "Get out of here.", { 1, 1, 1, 1 } },
      { "We ran out of milk.", { 1, 1, 1, 1, 1 } },
      { "Figure it out.", { 2, 1, 1 } },
      { "A lazy man ran over the hill.", { 1, 2, 1, 1, 2, 1, 1 } },
      { "Let it be.", { 1, 1, 1 } },
      { "Fly me to the moon.", { 1, 1, 1, 1, 1 } },
    };
    for (int speed : { 50, 80, 120 }) {
      evv::VoiceSettings voice;
      voice.speed = speed;
      for (const WordCase& test : word_cases) {
        auto utterance = speak(test.text, 0, voice);
        if (utterance == nullptr)
          continue;
        std::vector<int> counts(utterance->words.size(), 0);
        for (const auto& unit : utterance->units[evv::kSyllables]) {
          if (unit.word >= 0 && unit.word < static_cast<int>(counts.size()))
            ++counts[static_cast<size_t>(unit.word)];
        }
        expect(counts == test.syllables, std::string(test.text) + " at speed " + std::to_string(speed) +
                                             ": syllables per word (" + unitsText(*utterance, evv::kSyllables) + ")");
      }
    }
    return finish("split");
  }

  // Every language module speaks, and accented text arrives as letters.
  int languagesTest() {
    const char* const texts[evv::kNumLanguages] = {
      "Hello, this is a test.", "Hello, this is a test.", "Guten Tag, das ist ein Test.",
      "Hola, esto es una prueba.", "Hola, esto es una prueba.", "Bonjour, ceci est un essai.",
      "Bonjour, ceci est un essai.", "Ciao, questa è una prova.", "Hyvää päivää, tämä on testi.",
      "Olá, isto é um teste."
    };
    for (int language = 0; language < evv::kNumLanguages; ++language) {
      expect(evv::languageAvailable(language), std::string(evv::kLanguages[language].name) + " is built in");
      auto utterance = speak(texts[language], language);
      expect(utterance != nullptr && utterance->units[evv::kSyllables].size() >= 4 &&
             utterance->samples.size() < 4 * evv::kEngineRate,
             std::string(evv::kLanguages[language].name) + " speaks its sentence (" +
             std::to_string(utterance ? utterance->samples.size() : 0) + " samples)");
    }
    // UTF-8 becomes Windows-1252: "ä" one byte, not two spelled out.
    expect(evv::engineText("p\xC3\xA4iv\xC3\xA4\xC3\xA4", false) == "p\xE4iv\xE4\xE4", "UTF-8 reaches the engine as Latin-1");
    expect(evv::engineText("a`vb80 b", false) == "a vb80 b" && evv::engineText("a`vb80 b", true) == "a`vb80 b",
           "backquotes are spaces unless annotations are on");
    return finish("languages");
  }

  // The eight voice controls change the render.
  int voiceTest() {
    const std::string text = "Testing the voice controls.";
    auto base = speak(text);
    evv::VoiceSettings fast;
    fast.speed = 150;
    auto quick = speak(text, 0, fast);
    expect(base && quick && quick->samples.size() < base->samples.size() * 3 / 4,
           "a higher speed speaks faster (" + std::to_string(quick ? quick->samples.size() : 0) + " against " +
           std::to_string(base ? base->samples.size() : 0) + ")");

    const auto average_f1 = [](const evv::Utterance& utterance) {
      double sum = 0.0;
      int count = 0;
      for (const auto& frame : utterance.frames) {
        if (frame[evv::kAv] > 40) {
          sum += frame[evv::kF1];
          ++count;
        }
      }
      return count > 0 ? sum / count : 0.0;
    };
    evv::VoiceSettings small;
    small.head_size = 0;
    evv::VoiceSettings large;
    large.head_size = 100;
    auto small_head = speak(text, 0, small);
    auto large_head = speak(text, 0, large);
    expect(small_head && large_head && average_f1(*small_head) > average_f1(*large_head) * 1.05,
           "a smaller head raises the formants");

    const auto average_f0 = [](const evv::Utterance& utterance) {
      double sum = 0.0;
      int count = 0;
      for (const auto& frame : utterance.frames) {
        if (frame[evv::kAv] > 0) {
          sum += frame[evv::kF0];
          ++count;
        }
      }
      return count > 0 ? sum / count : 0.0;
    };
    evv::VoiceSettings high;
    high.pitch_baseline = 100;
    auto high_voice = speak(text, 0, high);
    expect(high_voice && base && average_f0(*high_voice) > average_f0(*base) * 1.2, "a higher baseline raises the pitch");

    evv::VoiceSettings breathy;
    breathy.breathiness = 100;
    auto breathy_voice = speak(text, 0, breathy);
    expect(breathy_voice && base && breathy_voice->samples != base->samples, "breathiness changes the voice");
    evv::VoiceSettings rough;
    rough.roughness = 100;
    auto rough_voice = speak(text, 0, rough);
    expect(rough_voice && base && rough_voice->samples != base->samples, "roughness changes the voice");
    evv::VoiceSettings female = evv::kPresetVoices[1];
    auto female_voice = speak(text, 0, female);
    expect(female_voice && base && !(female_voice->voices.front() == base->voices.front()),
           "a female voice gives the synthesiser another voice");
    return finish("voice");
  }

  // ---- The plugin --------------------------------------------------------------

  constexpr int kRate = 48000;
  constexpr int kBlock = 128;

  struct MidiEvent {
    int sample;
    juce::MidiMessage message;
  };

  void setControl(SynthPlugin& plugin, const std::string& name, float value) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    expect(found != controls.end(), "control exists: " + name);
    if (found != controls.end())
      plugin.valueChanged(name, value);
  }

  float control(SynthPlugin& plugin, const std::string& name) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    return found == controls.end() ? -1.0f : found->second->value();
  }

  void preparePlayback(SynthPlugin& plugin, const std::string& text) {
    plugin.setNonRealtime(true);
    plugin.setPlayConfigDetails(0, 2, kRate, kBlock);
    plugin.prepareToPlay(kRate, kBlock);
    setControl(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= vital::constants::kMaxEffectSlots; ++i)
        setControl(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    for (int i = 1; i <= vital::kNumOscillators; ++i)
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
    plugin.getSpeechManager()->renderPendingNow();
  }

  std::vector<float> render(SynthPlugin& plugin, int samples, const std::vector<MidiEvent>& events) {
    std::vector<float> result(static_cast<size_t>(samples));
    for (int offset = 0; offset < samples;) {
      const int count = std::min(kBlock, samples - offset);
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

  double rms(const std::vector<float>& signal, int start, int end) {
    double sum = 0.0;
    end = std::min<int>(end, static_cast<int>(signal.size()));
    for (int i = start; i < end; ++i)
      sum += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
    return end > start ? std::sqrt(sum / (end - start)) : 0.0;
  }

  bool finite(const std::vector<float>& signal) {
    for (float value : signal) {
      if (!std::isfinite(value))
        return false;
    }
    return true;
  }

  // The strongest normalised autocorrelation period, as a frequency.
  double pitch(const std::vector<float>& signal, int start, int end, double min_hz, double max_hz) {
    const int min_lag = static_cast<int>(kRate / max_hz);
    const int max_lag = static_cast<int>(kRate / min_hz) + 1;
    std::vector<double> scores(static_cast<size_t>(max_lag + 2), 0.0);
    for (int lag = min_lag - 1; lag <= max_lag + 1; ++lag) {
      double sum = 0.0, energy_a = 0.0, energy_b = 0.0;
      for (int i = start; i + lag < end; ++i) {
        const double a = signal[static_cast<size_t>(i)];
        const double b = signal[static_cast<size_t>(i + lag)];
        sum += a * b;
        energy_a += a * a;
        energy_b += b * b;
      }
      scores[static_cast<size_t>(lag)] = sum / std::sqrt(std::max(1.0e-30, energy_a * energy_b));
    }
    int best = min_lag;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
      if (scores[static_cast<size_t>(lag)] > scores[static_cast<size_t>(best)])
        best = lag;
    }
    const double left = scores[static_cast<size_t>(best - 1)];
    const double middle = scores[static_cast<size_t>(best)];
    const double right = scores[static_cast<size_t>(best + 1)];
    const double denominator = left - 2.0 * middle + right;
    const double shift = denominator != 0.0 ? 0.5 * (left - right) / denominator : 0.0;
    return kRate / (best + shift);
  }

  // A key sings its syllable at the key's pitch, holding the vowel while it is down.
  int oscillatorTest() {
    for (const auto& [note, quality] : std::vector<std::pair<int, int>> { { 60, 0 }, { 57, 0 }, { 67, 1 }, { 48, 0 } }) {
      SynthPlugin plugin;
      preparePlayback(plugin, "Ah");
      setControl(plugin, "osc_1_speech_quality", static_cast<float>(quality));
      const int length = kRate;
      const auto audio = render(plugin, length, { { 0, juce::MidiMessage::noteOn(1, note, 1.0f) } });
      if (const char* folder = std::getenv("ATLAS_SPEECH_WAV")) {
        juce::WavAudioFormat format;
        juce::File file = juce::File(folder).getChildFile("note_" + juce::String(note) + "_q" + juce::String(quality) + ".wav");
        file.deleteFile();
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(new juce::FileOutputStream(file), kRate, 1, 24, {}, 0));
        if (writer != nullptr) {
          const float* channel = audio.data();
          writer->writeFromFloatArrays(&channel, 1, static_cast<int>(audio.size()));
        }
      }
      expect(finite(audio), "the voice is finite");
      const double level = rms(audio, kRate / 4, kRate * 3 / 4);
      expect(level > 0.01, "the held vowel sounds (" + std::to_string(level) + ")");
      const double expected = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
      const double measured = pitch(audio, kRate / 4, kRate / 4 + 8192, expected / 1.5, expected * 1.5);
      expect(std::abs(measured / expected - 1.0) < 0.01,
             "note " + std::to_string(note) + " at quality " + std::to_string(quality) + " sings at " +
             std::to_string(expected) + " Hz (" + std::to_string(measured) + ")");
    }

    // Released, the piece finishes and the voice goes quiet.
    {
      SynthPlugin plugin;
      preparePlayback(plugin, "Ah");
      const auto audio = render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                 { kRate / 4, juce::MidiMessage::noteOff(1, 60) } });
      expect(rms(audio, kRate / 8, kRate / 4) > 0.01 && rms(audio, kRate * 3 / 4, kRate) < 1e-4,
             "the piece ends after the key is released");
    }

    // Spoken keeps the voice's melody; Rate acts on each note.
    {
      SynthPlugin plugin;
      preparePlayback(plugin, "Hello there, how are you today?");
      setControl(plugin, "osc_1_speech_split", static_cast<float>(evv::kWholeText));
      setControl(plugin, "osc_1_speech_sustain", 0.0f);
      setControl(plugin, "osc_1_speech_pitch_mode", 1.0f);
      setControl(plugin, "osc_1_speech_rate", 2.0f);
      const auto audio = render(plugin, kRate * 3, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
      expect(finite(audio) && rms(audio, 0, kRate / 2) > 0.01, "the whole text plays");
      expect(rms(audio, kRate * 2, kRate * 3) < 1e-4, "at double rate it is over within two seconds");
    }
    return finish("oscillator");
  }

  // Each key takes the next piece; a chord shares one; the text loops or stops.
  int steppingTest() {
    SynthPlugin plugin;
    preparePlayback(plugin, "Hello wonderful Atlas.");
    vital::SpeechSlot* slot = plugin.getSpeechSlot(0);
    expect(slot != nullptr && slot->get() != nullptr, "the slot has the render");
    const int syllables = slot && slot->get() ? static_cast<int>(slot->get()->units[evv::kSyllables].size()) : 0;
    expect(syllables >= 6, "the text has its syllables (" + std::to_string(syllables) + ")");

    const int gap = kRate / 4;
    std::vector<MidiEvent> events;
    for (int i = 0; i < 3; ++i) {
      events.push_back({ i * gap, juce::MidiMessage::noteOn(1, 60, 1.0f) });
      events.push_back({ i * gap + gap / 2, juce::MidiMessage::noteOff(1, 60) });
    }
    render(plugin, 3 * gap, events);
    expect(slot->lastPlayed() == 2 && slot->lastPlayedOf() == syllables, "three keys played three syllables");

    // A chord of three keys sings one syllable.
    render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { 0, juce::MidiMessage::noteOn(1, 64, 1.0f) },
                          { 2, juce::MidiMessage::noteOn(1, 67, 1.0f) }, { gap / 2, juce::MidiMessage::noteOff(1, 60) },
                          { gap / 2, juce::MidiMessage::noteOff(1, 64) }, { gap / 2, juce::MidiMessage::noteOff(1, 67) } });
    expect(slot->lastPlayed() == 3, "a chord played one syllable (" + std::to_string(slot->lastPlayed()) + ")");

    // Restart starts the text again.
    slot->requestRestart();
    render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 62, 1.0f) }, { gap / 2, juce::MidiMessage::noteOff(1, 62) } });
    expect(slot->lastPlayed() == 0, "Restart plays the first syllable next");

    // Past the end the text loops; with Loop off the keys fall silent.
    events.clear();
    for (int i = 0; i < syllables; ++i) {
      events.push_back({ i * gap, juce::MidiMessage::noteOn(1, 60, 1.0f) });
      events.push_back({ i * gap + gap / 2, juce::MidiMessage::noteOff(1, 60) });
    }
    render(plugin, syllables * gap, events);
    expect(slot->lastPlayed() == 0, "the text loops to its first syllable");
    setControl(plugin, "osc_1_speech_loop", 0.0f);
    slot->requestRestart();
    events.clear();
    for (int i = 0; i < syllables; ++i) {
      events.push_back({ i * gap, juce::MidiMessage::noteOn(1, 60, 1.0f) });
      events.push_back({ i * gap + gap / 2, juce::MidiMessage::noteOff(1, 60) });
    }
    render(plugin, syllables * gap, events);
    const auto quiet = render(plugin, kRate, {});
    expect(rms(quiet, kRate / 2, kRate) < 1e-4, "the last syllable ends (" + std::to_string(rms(quiet, kRate / 2, kRate)) + ")");
    const auto after = render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
    expect(rms(after, 0, gap) < 1e-4,
           "with Loop off a key after the end is silent (" + std::to_string(rms(after, 0, gap)) + ")");
    render(plugin, gap, { { 0, juce::MidiMessage::noteOff(1, 60) } });

    // Restart on release: each phrase starts the text.
    setControl(plugin, "osc_1_speech_loop", 1.0f);
    setControl(plugin, "osc_1_speech_restart_on_release", 1.0f);
    render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { gap / 4, juce::MidiMessage::noteOn(1, 62, 1.0f) },
                          { gap / 2, juce::MidiMessage::noteOff(1, 60) }, { gap / 2 + 10, juce::MidiMessage::noteOff(1, 62) } });
    expect(slot->lastPlayed() == 1, "a legato phrase steps through the text");
    render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { gap / 2, juce::MidiMessage::noteOff(1, 60) } });
    expect(slot->lastPlayed() == 0, "the next phrase starts the text again");

    // Legato mode: a key pressed while another is held keeps the syllable
    // (only the pitch moves) until every key is up.
    setControl(plugin, "osc_1_speech_restart_on_release", 0.0f);
    setControl(plugin, "polyphony", 1.0f);
    setControl(plugin, "legato", 1.0f);
    slot->requestRestart();
    const auto legato = render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                              { gap / 4, juce::MidiMessage::noteOn(1, 64, 1.0f) },
                                              { gap / 2, juce::MidiMessage::noteOff(1, 60) },
                                              { gap / 2 + gap / 8, juce::MidiMessage::noteOn(1, 67, 1.0f) },
                                              { gap * 3 / 4, juce::MidiMessage::noteOff(1, 64) },
                                              { gap * 7 / 8, juce::MidiMessage::noteOff(1, 67) } });
    expect(slot->lastPlayed() == 0,
           "legato keys stay on the first syllable (" + std::to_string(slot->lastPlayed()) + ")");
    expect(rms(legato, gap / 4, gap / 2) > 0.005, "the held syllable sounds through the legato key");
    render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { gap / 2, juce::MidiMessage::noteOff(1, 60) } });
    expect(slot->lastPlayed() == 1, "a key after every key was up plays the next syllable");
    setControl(plugin, "legato", 0.0f);
    setControl(plugin, "polyphony", 8.0f);

    // Words and the whole text.
    setControl(plugin, "osc_1_speech_restart_on_release", 0.0f);
    setControl(plugin, "osc_1_speech_split", static_cast<float>(evv::kWords));
    render(plugin, gap * 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { gap / 2, juce::MidiMessage::noteOff(1, 60) },
                              { gap, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { gap + gap / 2, juce::MidiMessage::noteOff(1, 60) } });
    expect(slot->lastPlayedSplit() == evv::kWords && slot->lastPlayed() == 1 && slot->lastPlayedOf() == 3,
           "Word plays the second word on the second key");

    // A voice control renders again and keeps the place in the text.
    setControl(plugin, "osc_1_speech_head_size", 0.2f);
    plugin.getSpeechManager()->renderPendingNow();
    render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { gap / 2, juce::MidiMessage::noteOff(1, 60) } });
    expect(slot->lastPlayed() == 2 && slot->get()->request.voice.head_size == 20,
           "a new voice carries on with the next word (" + std::to_string(slot->lastPlayed()) + ")");
    // New text starts from its first piece.
    plugin.getSpeechManager()->setText(0, "Goodbye now.");
    plugin.getSpeechManager()->renderPendingNow();
    render(plugin, gap, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) }, { gap / 2, juce::MidiMessage::noteOff(1, 60) } });
    expect(slot->lastPlayed() == 0 && slot->lastPlayedOf() == 2, "new text starts at its first word");
    return finish("stepping");
  }

  // The text and the controls survive the host's state, rendered before the first note.
  int stateTest() {
    juce::MemoryBlock state;
    {
      SynthPlugin plugin;
      preparePlayback(plugin, "Caf\xC3\xA9 au lait, s'il vous pla\xC3\xAEt.");
      setControl(plugin, "osc_1_speech_language", 5.0f);
      setControl(plugin, "osc_1_speech_gender", 1.0f);
      setControl(plugin, "osc_1_speech_breathiness", 0.6f);
      setControl(plugin, "osc_1_speech_speed", 80.0f);
      setControl(plugin, "osc_1_speech_split", 1.0f);
      plugin.getStateInformation(state);
    }
    {
      SynthPlugin plugin;
      plugin.setNonRealtime(true);
      plugin.setPlayConfigDetails(0, 2, kRate, kBlock);
      plugin.prepareToPlay(kRate, kBlock);
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      expect(plugin.getSpeechManager()->text(0) == "Caf\xC3\xA9 au lait, s'il vous pla\xC3\xAEt.", "the text is restored");
      expect(control(plugin, "osc_1_speech_language") == 5.0f && control(plugin, "osc_1_speech_gender") == 1.0f &&
             control(plugin, "osc_1_speech_speed") == 80.0f && control(plugin, "osc_1_speech_split") == 1.0f,
             "the controls are restored");
      const auto status = plugin.getSpeechManager()->status(0);
      expect(status.state == evv::SpeechManager::State::kReady && status.utterance != nullptr &&
             status.utterance->request.language == 5 && status.utterance->request.voice.breathiness == 60,
             "the text was rendered with the restored voice before any note");
      const auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) } });
      expect(rms(audio, 0, kRate / 2) > 0.005, "the first key speaks");
    }
    {
      // Another type's slot keeps its text, and the init preset clears it.
      SynthPlugin plugin;
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      plugin.loadInitPreset();
      expect(plugin.getSpeechManager()->text(0).empty(), "the init preset has no text");
    }
    return finish("state");
  }

  // ---- Editor ------------------------------------------------------------------

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

  Component* sectionNamed(Component& parent, const String& name) {
    if (parent.getProperties()["SectionName"].toString() == name)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = sectionNamed(*child, name))
        return found;
    }
    return nullptr;
  }

  ParameterSlider* sliderTitled(Component& parent, const String& title) {
    if (auto* slider = dynamic_cast<ParameterSlider*>(&parent); slider != nullptr && slider->getTitle() == title)
      return slider;
    for (auto* child : parent.getChildren()) {
      if (auto* found = sliderTitled(*child, title))
        return found;
    }
    return nullptr;
  }

  String valueText(ParameterSlider* slider) {
    if (slider == nullptr)
      return {};
    auto handler = slider->createAccessibilityHandler();
    auto* value = handler != nullptr ? handler->getValueInterface() : nullptr;
    return value != nullptr ? value->getCurrentValueAsString() : String();
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
    synth.getSpeechManager()->setText(0, "Hello wonderful Atlas.");
    synth.getSpeechManager()->renderPendingNow();
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpUntil([] { return false; }, 300);

    auto* section = sectionNamed(editor, "Oscillator 1");
    expect(section != nullptr, "Oscillator 1 section exists");
    if (section == nullptr)
      return finish("editor");

    auto* text = dynamic_cast<TextEditor*>(titledExactly(*section, "Text to speak"));
    expect(text != nullptr && text->getText() == "Hello wonderful Atlas." && text->getWantsKeyboardFocus() &&
           text->getDescription().isNotEmpty(), "the text box holds the text");
    auto* status = findTitled(*section, [](const String& t) { return t.contains("syllables") && t.contains("words"); });
    expect(status != nullptr && status->getTitle() == "7 syllables, 3 words",
           "the status reads the syllables and words (" + (status ? status->getTitle().toStdString() : "") + ")");
    expect(titledExactly(*section, "Restart text") != nullptr && titledExactly(*section, "Voice preset") != nullptr,
           "Restart and Voice preset are there");

    for (const auto& [title, value] : std::vector<std::pair<const char*, const char*>> {
             { "1 Speech Language", "English (US)" }, { "1 Speech Gender", "Male" }, { "1 Speech Head Size", "50%" },
             { "1 Speech Breathiness", "0%" }, { "1 Speech Speed", "50" }, { "1 Speech Split", "Syllable" },
             { "1 Speech Amplitude Envelope", "Speech" }, { "1 Speech Pitch Mode", "Key" }, { "1 Speech Rate", "100%" },
             { "1 Speech Quality", "11 kHz" } }) {
      auto* row = sliderTitled(*section, title);
      expect(row != nullptr && valueText(row) == value && row->getDescription().isNotEmpty() && row->getWantsKeyboardFocus(),
             std::string("a described row reading its value: ") + title + " (" + valueText(row).toStdString() + ")");
    }
    auto* language = sliderTitled(*section, "1 Speech Language");
    auto* split = sliderTitled(*section, "1 Speech Split");
    expect(language != nullptr && split != nullptr && language->getScreenY() < split->getScreenY(),
           "the voice rows come before the playing rows");
    expect(findTitled(*section, [](const String& t) { return t.contains("OpenWurli") || t.contains("Wave Frame"); }) == nullptr,
           "other types' rows are hidden");

    // Typing new text and pressing Enter renders it.
    if (text != nullptr) {
      text->setText("Sing to me.", juce::sendNotificationSync);
      text->onReturnKey();
      pumpUntil([&synth] {
        const auto status = synth.getSpeechManager()->status(0);
        return status.state == evv::SpeechManager::State::kReady && status.utterance != nullptr &&
               status.utterance->request.text == "Sing to me.";
      });
      expect(synth.getSpeechManager()->text(0) == "Sing to me.", "Enter uses the typed text");
      pumpUntil([&] {
        auto* found = findTitled(*section, [](const String& t) { return t == "3 syllables, 3 words"; });
        return found != nullptr;
      }, 2000);
      expect(findTitled(*section, [](const String& t) { return t == "3 syllables, 3 words"; }) != nullptr,
             "the status follows the new text");
    }
    return finish("editor");
  }
  // Prints each frame's voicing, first formant and noise, with the pieces, to tune the splitter.
  int dump(const std::string& text, int speed) {
    evv::VoiceSettings voice;
    voice.speed = speed;
    auto utterance = speak(text, 0, voice);
    if (utterance == nullptr)
      return 1;
    for (size_t i = 0; i < utterance->frames.size(); ++i) {
      const auto& frame = utterance->frames[i];
      std::string marks;
      for (const auto& unit : utterance->units[evv::kSyllables]) {
        if (unit.start == static_cast<int>(i)) marks += " <start";
        if (unit.nucleus == static_cast<int>(i)) marks += " *nucleus";
        if (unit.end == static_cast<int>(i)) marks += " end>";
      }
      for (size_t w = 0; w < utterance->word_starts.size(); ++w) {
        if (utterance->word_starts[w] == static_cast<int>(i)) marks += " [word " + utterance->words[w] + "]";
      }
      std::printf("%4zu ui %2d av %2d ah %2d af %2d tl %2d f0 %4d f1 %4d b1 %4d f2 %4d f3 %4d fnz %4d anv %2d ab %2d di %2d%s\n",
                  i, frame[evv::kStep], frame[evv::kAv], frame[evv::kAh], frame[evv::kAf], frame[4], frame[evv::kF0], frame[evv::kF1],
                  frame[10], frame[evv::kF2], frame[evv::kF3], frame[evv::kFnz], frame[evv::kAnv], frame[evv::kAb],
                  frame[6], marks.c_str());
    }
    for (const auto& unit : utterance->units[evv::kWords])
      std::printf("word %d-%d: %s\n", unit.start, unit.end,
                  unit.word < static_cast<int>(utterance->words.size()) ? utterance->words[unit.word].c_str() : "?");
    for (const auto& unit : utterance->units[evv::kSyllables]) {
      const std::string word = unit.word < static_cast<int>(utterance->words.size()) ? utterance->words[unit.word] : "?";
      std::printf("syllable %d-%d nucleus %d: %s %d/%d\n", unit.start, unit.end, unit.nucleus, word.c_str(),
                  unit.part + 1, unit.parts);
    }
    return 0;
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI gui;
  const std::string flag = argc > 1 ? argv[1] : "";
  if (flag == "--engine") return engineTest();
  if (flag == "--split") return splitTest();
  if (flag == "--languages") return languagesTest();
  if (flag == "--voice") return voiceTest();
  if (flag == "--oscillator") return oscillatorTest();
  if (flag == "--stepping") return steppingTest();
  if (flag == "--state") return stateTest();
  if (flag == "--editor") return editorTest();
  if (flag == "--dump" && argc > 2) return dump(argv[2], argc > 3 ? std::atoi(argv[3]) : evv::VoiceSettings {}.speed);
  std::cerr << "usage: AtlasSpeechTests --engine | --split | --languages | --voice | --oscillator | --stepping | "
               "--state | --editor\n";
  return 2;
}
