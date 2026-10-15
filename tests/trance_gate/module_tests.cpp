#include "JuceHeader.h"
#include "../slot_test_ids.h"
#include "trance_gate_module.h"
#include "reorderable_effect_chain.h"
#include "synth_plugin.h"
#include "sound_engine.h"
#include "sample_source.h"
#include "synth_editor.h"
#include "../editor_navigation.h"
#include "fx_automation_parameter.h"
#include "load_save.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { std::atomic<bool> tracking {false}; std::atomic<int> allocations {0}; }
void* operator new(std::size_t size) {
  if (tracking) ++allocations;
  if (void* p = std::malloc(size)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
int failures = 0;
void expect(bool ok, const std::string& text) { if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; } }
struct Fixture {
  vital::Value bps { 2.0f };
  vital::TranceGateClock clock;
  vital::TranceGateModule gate { bps.output(), &clock };
  vital::control_map controls;
  int rate, factor;
  double seconds = 0;
  Fixture(int sr = 48000, int os = 1) : rate(sr), factor(os) {
    gate.init(); gate.setSampleRate(sr); gate.setOversampleAmount(os); controls = gate.getControls();
  }
  void set(const std::string& suffix, float value) { controls.at("trance_gate_" + suffix)->set(value); }
  std::vector<float> render(int count, int block = 113) {
    std::vector<float> out(count * factor);
    std::vector<vital::poly_float> input(block * factor);
    for (auto& sample : input) for (int lane = 0; lane < vital::poly_float::kSize; ++lane) sample.set(lane, lane % 2 ? -0.5f : 1.0f);
    allocations = 0; tracking = true;
    bool stereo = true;
    for (int cursor = 0; cursor < count; cursor += block) {
      int samples = std::min(block, count - cursor);
      clock.prepare(samples, rate, bps.value());
      gate.correctToTime(seconds + double(cursor) / rate);
      gate.processWithInput(input.data(), samples * factor);
      for (int i = 0; i < samples * factor; ++i) {
        auto audio = gate.output()->buffer[i];
        stereo &= std::abs(audio[0] * -0.5f - audio[1]) < 1.0e-6;
        out[cursor * factor + i] = audio[0];
      }
    }
    tracking = false;
    expect(allocations == 0, "render and clock must not allocate");
    expect(stereo, "same volume gain must preserve stereo polarity and balance");
    seconds += double(count) / rate;
    return out;
  }
};
bool dry(const std::vector<float>& out) { return std::all_of(out.begin(), out.end(), [](float v) { return v == 1.0f; }); }
double error(const std::vector<float>& a, const std::vector<float>& b) {
  double maximum = 0;
  for (size_t i = 0; i < a.size(); ++i) maximum = std::max(maximum, double(std::abs(a[i] - b[i])));
  return maximum;
}
Component* titled(Component& parent, const String& title) {
  if (parent.getTitle() == title) return &parent;
  for (auto* child : parent.getChildren()) if (auto* found = titled(*child, title)) return found;
  return nullptr;
}
bool select(Component& editor, const String& title, const String& value) {
  return editor_navigation::focusHeading(editor, title, value);
}
void gather(Component& parent, std::vector<TranceGateEditor*>& gates, int& sliders) {
  if (auto* gate = dynamic_cast<TranceGateEditor*>(&parent)) gates.push_back(gate);
  if (auto* slider = dynamic_cast<Slider*>(&parent)) {
    ++sliders;
    expect(!slider->getTitle().contains("Step "), "step data must never become sliders");
  }
  for (auto* child : parent.getChildren()) gather(*child, gates, sliders);
}
struct PlayHead : AudioPlayHead {
  bool playing = false;
  double ppq = 0.0;
  Optional<PositionInfo> getPosition() const override {
    PositionInfo info; info.setIsPlaying(playing); info.setBpm(120.0); info.setPpqPosition(ppq); return info;
  }
};
KeyPress key(char c, bool shift = false) { return KeyPress(c, shift ? ModifierKeys::shiftModifier : ModifierKeys::noModifiers, c); }
}
int main() {
  ScopedJuceInitialiser_GUI gui;
  for (int sr : { 44100, 48000, 96000 }) for (int os : { 1, 2, 4, 8 }) {
    Fixture a(sr, os), b(sr, os);
    for (auto* f : { &a, &b }) {
      f->set("grid", 6); f->set("stop", 4); f->set("attack", 0);
      for (int i = 0; i < 4; ++i) f->set("step_" + std::to_string(i + 1) + "_value", i / 3.0f - 1.0f);
    }
    expect(dry(a.render(131)), "stopped playback must be bit transparent");
    a.seconds = b.seconds = 0.137;
    a.clock.setPlaying(true); b.clock.setPlaying(true);
    expect(error(a.render(sr / 3, 127), b.render(sr / 3, 31)) < 3.0e-6, "playback phase independent of block size");
    a.clock.setPlaying(false);
    expect(dry(a.render(29)), "transport stop must immediately pass dry audio");
    a.gate.hardReset(); b.gate.hardReset(); a.seconds = b.seconds = -0.111;
    a.clock.setPlaying(true);
    expect(error(a.render(191), b.render(191)) < 3.0e-6, "negative PPQ count-ins and seek must remain deterministic");

    Fixture wet(sr, os), mixed(sr, os), unmixed(sr, os);
    for (auto* f : { &wet, &mixed, &unmixed }) {
      f->clock.setPlaying(true); f->set("attack", 0); f->set("stop", 1);
      f->set("step_1_value", -1); f->set("step_1_mode", 4);
    }
    expect(wet.controls.count("trance_gate_release") == 0, "Release is removed from the DSP controls");
    mixed.set("mix", 0.37f); unmixed.set("mix", 0);
    const auto wet_audio = wet.render(4096), mixed_audio = mixed.render(4096);
    bool linear_mix = true;
    for (size_t i = 0; i < wet_audio.size(); ++i)
      linear_mix &= std::abs(mixed_audio[i] - (0.63f + 0.37f * wet_audio[i])) < 2.0e-6;
    expect(linear_mix, "Mix linearly blends dry and sequenced audio at every sample");
    expect(dry(unmixed.render(4096)), "zero Mix passes dry audio through during playback");
    mixed.clock.setPlaying(false);
    expect(dry(mixed.render(31)), "stopped playback stays dry at partial Mix");

    Fixture mix_a(sr, os), mix_b(sr, os);
    for (auto* f : { &mix_a, &mix_b }) {
      f->clock.setPlaying(true); f->set("attack", 0); f->set("stop", 1); f->set("step_1_value", -1);
      f->render(128); f->set("mix", 0);
    }
    auto fade = mix_a.render(sr / 10, 127);
    expect(fade.front() > 0 && fade.front() < 0.01f && fade.back() > 0.9999f,
           "Mix automation is smoothed to avoid a gain jump");
    expect(error(fade, mix_b.render(sr / 10, 31)) < 2.0e-6,
           "Mix automation smoothing is independent of host block size");

    Fixture midi(sr, os), continuous(sr, os);
    for (auto* f : { &midi, &continuous }) {
      f->set("trigger", 1); f->set("attack", 0); f->set("grid", 5);
      f->set("start", 1); f->set("stop", 1); f->set("step_1_value", -1);
      f->set("step_1_mode", 4);
      f->clock.noteOn(60, 0, 17);
    }
    auto first = midi.render(137);
    expect(first[16 * os] == 1 && std::abs(first[17 * os]) < 1.0e-7, "MIDI trigger at exact sample offset, including oversampling");
    expect(error(first, continuous.render(137)) < 1.0e-6, "first-note baseline");
    midi.clock.noteOn(64, 0, 0); midi.clock.noteOn(67, 1, 0); midi.clock.noteOn(72, 2, 23);
    expect(error(midi.render(251), continuous.render(251)) < 1.0e-6, "held base note must suppress added chords across channels");
    midi.clock.noteOff(60, 0); midi.clock.noteOff(64, 0); midi.clock.noteOn(76, 0, 0);
    expect(error(midi.render(127), continuous.render(127)) < 1.0e-6, "remaining held notes keep legato phrase active");
    midi.clock.allNotesOff();
    expect(error(midi.render(73), continuous.render(73)) < 1.0e-6, "release tails continue without retrigger or abrupt bypass");
    midi.clock.noteOn(60, 0, 5);
    auto restart = midi.render(19);
    expect(std::abs(restart[5 * os] - continuous.render(19)[5 * os]) > 1.0e-5, "new phrase retriggers after all keys released");
    midi.clock.reset();
    expect(dry(midi.render(43)), "panic resets held notes and MIDI start state");
  }
  {
    vital::TranceGateClock clock;
    clock.noteOn(60, 0, 9); clock.noteOn(60, 0, 12); clock.noteOff(60, 0);
    clock.noteOff(99, 0); clock.noteOn(64, 0, 18); clock.prepare(31, 48000, 2);
    expect(clock.midiBeat(8) < 0 && clock.midiBeat(9) == 0 && clock.midiBeat(12) > 0 && clock.midiBeat(18) == 0,
           "duplicate notes, unmatched note offs, and multiple phrase starts within one block");
    clock.noteOn(70, 1, 0); clock.allNotesOff(0, 0); clock.noteOn(75, 0, 0);
    clock.prepare(16, 48000, 2);
    expect(clock.midiBeat(0) > 0, "channel all-notes-off must not clear held keys on another channel");
    clock.reset(); clock.noteOn(64, 23, 11); clock.prepare(16, 48000, 2);
    expect(clock.midiBeat(10) < 0 && clock.midiBeat(11) == 0, "internal arpeggiator destination notes trigger the gate");
    clock.noteOn(67, 16, 0); clock.noteOn(72, 0, 3); clock.prepare(16, 48000, 2);
    expect(clock.midiBeat(0) > 0 && clock.midiBeat(3) > clock.midiBeat(0), "held arpeggiator and keyboard notes share legato tracking");
    clock.allNotesOff(); clock.noteOn(60, 16, 0); clock.prepare(16, 48000, 2);
    expect(clock.midiBeat(0) == 0, "all-notes-off clears internal destination notes too");
  }
  {
    Fixture f;
    f.set("attack", 0); f.set("start", 4); f.set("stop", 2); f.clock.setPlaying(true);
    f.set("step_4_value", 1); f.set("step_3_value", 0); f.set("step_2_value", -1);
    expect(f.render(2048).back() > 1.999f, "reverse range begins at Pattern Start");
    f.seconds = 0.5;
    expect(std::abs(f.render(2048).back() - 1) < 1.0e-5, "reverse range decrements the step");
    f.seconds = 1;
    expect(f.render(2048).back() < 1.0e-5, "reverse range reaches Pattern Stop");
    f.seconds = 1.5;
    expect(f.render(2048).back() > 1.999f, "reverse range wraps");
    f.bps.set(4); f.seconds = 0.25;
    expect(std::abs(f.render(2048).back() - 1) < 1.0e-5, "live tempo sets the exact grid duration");
  }
  for (const std::string prefix : { std::string(), std::string("bus_1_"), std::string("bus_2_"), std::string("bus_3_") }) {
    vital::Value bps {2.0f}, note {60.0f}, order {0.0f};
    vital::TranceGateClock clock; clock.setPlaying(true);
    vital::ReorderableEffectChain chain(bps.output(), note.output(), prefix, nullptr, nullptr, &clock);
    chain.init(); chain.setSampleRate(48000); chain.setOversampleAmount(1);
    chain.plug(order.output(), vital::ReorderableEffectChain::kOrder);
    auto controls = chain.getControls();
    for (int slot = 1; slot <= 2; ++slot) {
      const auto id = prefix + "fx_slot_" + std::to_string(slot) + "_";
      controls.at(id + "type")->set(vital::constants::kTranceGate + 1);
      controls.at(id + "trance_gate_attack")->set(0);
      controls.at(id + "trance_gate_start")->set(1); controls.at(id + "trance_gate_stop")->set(1);
      controls.at(id + "trance_gate_step_1_value")->set(slot == 1 ? -0.5f : 0.5f);
    }
    std::array<vital::poly_float, 128> input; input.fill(vital::poly_float(1.0f));
    for (int block = 0; block < 32; ++block) {
      clock.prepare(128, 48000, 2); chain.correctToTime(double(block * 128) / 48000);
      chain.processWithInput(input.data(), 128);
    }
    expect(std::abs(chain.output()->buffer[127][0] - 0.75f) < 1.0e-6, "independent serial Trance gates on " + prefix);
    expect(chain.getLatencySamples() == 0, "Trance gate has no added latency");
    controls.at(prefix + "fx_slot_1_enabled")->set(0); controls.at(prefix + "fx_slot_2_type")->set(0);
    chain.processWithInput(input.data(), 128);
    expect(chain.output()->buffer[0][0] == 1, "slot bypass and removal return dry audio immediately");
  }
  // Real plugin parameter registration, state and accessible UI, with two
  // independent Trance gates on every chain and no additional host parameters.
  {
    SynthPlugin synth;
    const int host_parameters = synth.getParameters().size();
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) for (int slot = 1; slot <= 2; ++slot) {
      const auto prefix = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") + "fx_slot_" + std::to_string(slot) + "_";
      synth.valueChanged(prefix + "type", vital::constants::kTranceGate + 1);
      synth.valueChanged(prefix + "trance_gate_step_32_value", -0.17f * slot);
      synth.valueChanged(prefix + "trance_gate_step_32_mode", slot + 2);
      synth.valueChanged(prefix + "trance_gate_step_32_curve", 0.23f * slot);
      synth.valueChanged(prefix + "trance_gate_mix", 0.2f * slot);
      expect(synth.assignFxParameterToAutomationSlot(prefix + "trance_gate_mix") >= 0, "Mix supports FX automation");
      expect(synth.assignFxParameterToAutomationSlot(prefix + "trance_gate_attack") >= 0, "global control supports FX automation");
    }
    MemoryBlock saved;
    synth.getStateInformation(saved);
    synth.valueChanged("fx_slot_1_trance_gate_step_32_value", 1);
    synth.valueChanged("fx_slot_1_trance_gate_mix", 0);
    synth.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    expect(synth.getParameters().size() == host_parameters, "Trance gate keeps the fixed host automation layout");
    {
      MemoryInputStream stream(saved.getData(), saved.getSize(), false);
      auto legacy = json::parse(stream.readEntireStreamAsString().toStdString());
      for (int chain = 0; chain <= vital::kNumBuses; ++chain) for (int slot = 1; slot <= 2; ++slot) {
        const auto prefix = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") +
            "fx_slot_" + std::to_string(slot) + "_trance_gate_";
        legacy["settings"].erase(prefix + "mix");
        legacy["settings"][prefix + "step_32_mode"] = 5;
        legacy["settings"][prefix + "release"] = 500;
      }
      const auto data = legacy.dump();
      synth.setStateInformation(data.data(), static_cast<int>(data.size()));
      for (int chain = 0; chain <= vital::kNumBuses; ++chain) for (int slot = 1; slot <= 2; ++slot) {
        const auto prefix = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") +
            "fx_slot_" + std::to_string(slot) + "_trance_gate_";
        expect(synth.getControls().at(prefix + "mix")->value() == 1 &&
               synth.getControls().at(prefix + "step_32_mode")->value() == 4 &&
               synth.getControls().count(prefix + "release") == 0,
               "old patches default to full Mix, map removed shape to Ramp Up, and ignore Release");
      }
      synth.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    }
    {
      synth.setPlayConfigDetails(0, 2, 48000, 128);
      synth.prepareToPlay(48000, 128);
      synth.getControls().at("volume")->set(6400);
      for (int osc = 1; osc <= 4; ++osc) synth.getControls().at("osc_" + std::to_string(osc) + "_on")->set(0);
      // The test binary has no installed factory wavetables. Use explicit PCM
      // to prove that the stopped/muted comparisons contain actual input audio.
      std::vector<float> pcm(48000);
      for (int i = 0; i < 48000; ++i) pcm[i] = 0.4f * std::sin(i * 0.031f);
      synth.getEngine()->getSample(slot_ids::kSampleSlot)->loadSample(pcm.data(), 48000, 48000);
      synth.getControls().at(slot_ids::id("sample_on"))->set(1); synth.getControls().at(slot_ids::id("sample_level"))->set(0.3f);
      synth.getControls().at(slot_ids::id("sample_destination"))->set(vital::constants::kEffects);
      synth.getControls().at(slot_ids::id("sample_random_phase"))->set(0); synth.getControls().at(slot_ids::id("sample_loop"))->set(1);
      synth.getControls().at("env_1_attack")->set(0.001f); synth.getControls().at("env_1_decay")->set(0.001f);
      synth.getControls().at("env_1_sustain")->set(1);
      synth.getControls().at("fx_slot_2_enabled")->set(0);
      for (int chain = 1; chain <= vital::kNumBuses; ++chain)
        for (int slot = 1; slot <= 2; ++slot)
          synth.getControls().at("bus_" + std::to_string(chain) + "_fx_slot_" + std::to_string(slot) + "_enabled")->set(0);
      for (int step = 1; step <= 32; ++step)
        synth.getControls().at("fx_slot_1_trance_gate_step_" + std::to_string(step) + "_value")->set(-1);
      synth.getControls().at("fx_slot_1_trance_gate_step_32_mode")->set(0);
      synth.getControls().at("fx_slot_1_trance_gate_trigger")->set(0);
      synth.getControls().at("fx_slot_1_trance_gate_mix")->set(1);
      PlayHead playhead; synth.setPlayHead(&playhead);
      AudioBuffer<float> audio(2, 128); MidiBuffer midi;
      auto render = [&](bool playing) {
        playhead.playing = playing;
        synth.getEngine()->allSoundsOff();
        midi.addEvent(MidiMessage::noteOn(1, 60, uint8_t(100)), 17);
        double peak = 0;
        for (int block = 0; block < 12; ++block) {
          audio.clear(); synth.processBlock(audio, midi); midi.clear();
          if (block >= 2) peak = std::max(peak, double(audio.getMagnitude(0, 128)));
          if (playing) playhead.ppq += 128.0 * 2.0 / 48000;
        }
        return peak;
      };
      expect(render(false) > 0.001, "real plugin passes stopped playback through");
      expect(render(true) < 1.0e-7, "real plugin applies the sequence during playback");
      synth.getControls().at("fx_slot_1_trance_gate_trigger")->set(1);
      expect(render(false) < 1.0e-7, "real plugin MIDI trigger works while the host is stopped");
      synth.setPlayHead(nullptr); synth.releaseResources();
      synth.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    }
    SynthEditor editor(synth);
    for (int chain = 0; chain <= vital::kNumBuses; ++chain) {
      expect(select(editor, "Editor group", chain == 0 ? String("Effects") : "Bus " + String(chain)) &&
             select(editor, "Editor element", "Chain and routing"), "chain is accessible");
      std::vector<TranceGateEditor*> gates; int sliders = 0;
      gather(editor, gates, sliders);
      const String section = chain == 0 ? String("Chain and routing") : "Bus " + String(chain) + " - Chain and routing";
      gates.erase(std::remove_if(gates.begin(), gates.end(), [&](auto* gate) {
        return gate->getProperties()["SectionName"].toString() != section;
      }), gates.end());
      sliders = 0;
      for (auto* gate : gates) {
        std::vector<TranceGateEditor*> ignored;
        gather(*gate->getParentComponent(), ignored, sliders);
      }
      expect(gates.size() == 2, "one keyboard editor per Trance gate instance");
      expect(sliders < 30, "only global sliders, no sliders per step");
      const auto prefix = (chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_") + "fx_slot_1_trance_gate_";
      expect(std::abs(synth.getControls().at(prefix + "step_32_value")->value() + 0.17f) < 1.0e-6 &&
             synth.getControls().at(prefix + "step_32_mode")->value() == 3 &&
             std::abs(synth.getControls().at(prefix + "step_32_curve")->value() - 0.23f) < 1.0e-6,
             "all step fields survive preset recall on each bus");
      expect(synth.fxAutomationSlotNameForParameter(prefix + "attack").isNotEmpty(), "automation survives state restore");
      expect(synth.fxAutomationSlotNameForParameter(prefix + "mix").isNotEmpty() &&
             std::abs(synth.getControls().at(prefix + "mix")->value() - 0.2f) < 1.0e-6,
             "Mix value and automation survive preset recall on each chain");
      if (gates.size() != 2) continue;
      auto* gate = gates[0];
      for (auto* instance : gates) {
        std::vector<Slider*> controls;
        std::function<void(Component&)> collect = [&](Component& parent) {
          if (auto* slider = dynamic_cast<Slider*>(&parent)) controls.push_back(slider);
          for (auto* child : parent.getChildren()) collect(*child);
        };
        collect(*instance->getParentComponent());
        auto mix = std::find_if(controls.begin(), controls.end(), [](auto* c) { return c->getTitle().endsWith("Mix"); });
        auto trigger = std::find_if(controls.begin(), controls.end(), [](auto* c) { return c->getTitle().endsWith("Trigger"); });
        expect(mix != controls.end() && trigger != controls.end() && std::next(mix) == trigger,
               "Mix slider is immediately before Trigger in each instance");
        expect(std::none_of(controls.begin(), controls.end(), [](auto* c) { return c->getTitle().endsWith("Release"); }),
               "Release is absent from the editor");
        if (mix != controls.end()) {
          const float initial = instance->read("mix");
          (*mix)->keyPressed(KeyPress(KeyPress::upKey));
          expect(std::abs(instance->read("mix") - initial - 0.01f) < 1.0e-6,
                 "Mix slider uses Atlas one-percent arrow increments");
          (*mix)->keyPressed(KeyPress(KeyPress::downKey));
        }
      }
      auto handler = gate->createAccessibilityHandler();
      expect(handler && handler->getRole() == AccessibilityRole::group &&
             handler->getDescription().startsWith("Bar 1 beat 1 0%, Step 1"),
             "accessible group announces position before step number");
      auto announcements = std::make_shared<std::vector<String>>();
      gate->announce = [announcements](const String& message) { announcements->push_back(message); };
      auto press = [&](const KeyPress& shortcut, const String& expected) {
        announcements->clear();
        expect(gate->keyPressed(shortcut), "editor handles " + shortcut.getTextDescription().toStdString());
        expect(announcements->size() == 1 && announcements->front() == expected,
               "one concise announcement: " + expected.toStdString());
      };
      press(KeyPress(KeyPress::upKey), "0.01");
      expect(std::abs(gate->read("step_1_value") - 0.01f) < 1.0e-6, "arrow changes the current step");
      press(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0), "0.06");
      press(KeyPress(KeyPress::downKey, ModifierKeys::shiftModifier, 0), "0.01");
      press(KeyPress(KeyPress::downKey), "0.00");
      expect(!gate->statusText().contains("-0.00"), "zero is never announced as negative zero");
      press(key('.'), "1%");
      press(key(','), "0%");
      press(key(','), "-1%");
      press(key('.', true), "4%");
      press(key('<', true), "-1%");
      press(key('>', true), "4%");
      press(key('i'), "5%");
      press(key('k', true), "0%");
      expect(std::abs(gate->read("step_1_curve")) < 1.0e-6 &&
             gate->statusText().contains("Step 1") && gate->read("step_2_curve") == 0,
             "comma and period edit the current curve without navigating or changing another step");
      for (auto* shape : { "Medium", "Short", "Ramp Down (-inf)", "Ramp Up (0 dB)", "Long" }) press(key('m'), shape);
      for (auto* shape : { "Ramp Up (0 dB)", "Ramp Down (-inf)", "Short", "Medium", "Long" }) press(key('m', true), shape);
      // Preset browsing must wrap through the same five shapes.
      press(key('g'), "Ramp Up (0 dB)");
      press(key('h'), "Long");
      press(key('m'), "Medium");
      press(key('h'), "Medium");
      press(key('h'), "Short");
      expect(gate->read("step_1_mode") == 1, "choosing a shape waits for Apply");
      press(key('a'), "Short");
      expect(gate->read("step_1_mode") == 2 && gate->read("step_2_mode") == 0,
             "shape edits apply only to the current step");
      gate->write({ { "grid", 4 } });
      press(KeyPress(KeyPress::rightKey), "Bar 1 beat 1 25%, Step 2, value 0.00, Long, curve 0%");
      expect(!gate->statusText().contains("grid") && !gate->statusText().contains("range") &&
             !gate->statusText().contains("gain"), "navigation omits redundant global and gain information");
      press(KeyPress(KeyPress::upKey), "0.01");
      expect(std::abs(gate->read("step_1_value")) < 1.0e-6 &&
             std::abs(gate->read("step_2_value") - 0.01f) < 1.0e-6,
             "navigation alone chooses the step to edit, without point selection");
      gate->write({ { "step_2_value", 0.99f }, { "step_2_curve", 0.99f } });
      press(KeyPress(KeyPress::upKey, ModifierKeys::shiftModifier, 0), "1.00");
      press(key('.', true), "100%");
      expect(gate->read("step_2_value") == 1 && gate->read("step_2_curve") == 1,
             "announcements report the actual clamped upper limits");
      gate->write({ { "step_2_value", -0.99f }, { "step_2_curve", -0.99f } });
      press(KeyPress(KeyPress::downKey, ModifierKeys::shiftModifier, 0), "-1.00");
      press(key(',', true), "-100%");
      expect(gate->read("step_2_value") == -1 && gate->read("step_2_curve") == -1,
             "announcements report the actual clamped lower limits");
      gate->write({ { "grid", 2 } });
      press(key('='), "1/8");
      press(key(';'), "1 to 31");
      expect(gate->read("grid") == 3 && gate->read("stop") == 31,
             "grid and range are announced only when those globals change");
      press(KeyPress(KeyPress::deleteKey), "Cleared step");
      expect(gate->read("step_2_value") == -1 && gate->read("step_2_curve") == 0 &&
             gate->read("step_1_mode") == 2 && gates[1]->read("step_2_value") == 0,
             "Delete clears only the current step in this instance");

      auto snapshot = [&] {
        std::vector<float> result;
        for (auto* instance : gates) {
          for (int i = 1; i <= 32; ++i)
            for (auto* field : { "value", "mode", "curve" })
              result.push_back(instance->read("step_" + String(i) + "_" + field));
          for (auto* field : { "mix", "trigger", "grid", "start", "stop", "attack", "decay", "sustain" })
            result.push_back(instance->read(field));
        }
        return result;
      };
      const auto before = snapshot();
      const auto before_status = gate->statusText();
      std::vector<KeyPress> removed;
      for (char c : std::string("serjlcvd1234567890")) {
        removed.push_back(key(c)); removed.push_back(key(c, true));
      }
      for (char c : std::string("!@#$%^&*()")) removed.push_back(key(c, true));
      removed.emplace_back(KeyPress::leftKey, ModifierKeys::shiftModifier, 0);
      removed.emplace_back(KeyPress::rightKey, ModifierKeys::shiftModifier, 0);
      for (int modifier : { ModifierKeys::commandModifier, ModifierKeys::ctrlModifier, ModifierKeys::altModifier })
        for (int arrow : { KeyPress::upKey, KeyPress::downKey }) removed.emplace_back(arrow, modifier, 0);
      for (const auto& shortcut : removed) {
        announcements->clear();
        expect(!gate->keyPressed(shortcut), "removed/host shortcut is not handled: " + shortcut.getTextDescription().toStdString());
        expect(announcements->empty() && snapshot() == before && gate->statusText() == before_status,
               "removed/host shortcut leaves steps, globals and editor feedback unchanged");
      }
      press(KeyPress(KeyPress::backspaceKey), "Cleared Trance gate");
      expect(gate->read("step_32_value") == -1 && gate->read("step_32_mode") == 0 &&
             gate->statusText().startsWith("Bar 1 beat 1 0%, Step 1"),
             "Backspace clears the whole pattern and returns to the first step");
      expect(std::abs(gates[1]->read("step_32_value") + 0.34f) < 1.0e-6,
             "editing never changes another Trance gate instance");
    }
  }
  std::cout << "Trance gate: " << failures << " failures\n";
  return failures ? 1 : 0;
}
