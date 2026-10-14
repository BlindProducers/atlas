// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../src/plugin/synth_editor.h"
#include "slot_test_ids.h"
#include "../src/common/envelope_time_utils.h"

namespace envelope_regressions {
  int failures = 0;
  void check(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "Envelope regression: " << message << '\n'; }
  }

  struct EnvelopeFixture {
    vital::Envelope env;
    std::array<vital::Value, vital::Envelope::kNumInputs> controls;
    vital::Output trigger, reset;
    EnvelopeFixture(int rate = 48000) {
      using E = vital::Envelope;
      for (int i = 0; i < E::kNumInputs; ++i) env.plug(&controls[i], i);
      env.plug(&trigger, E::kTrigger); env.plug(&reset, E::kReset);
      env.setSampleRate(rate); env.setControlRate(false);
      controls[E::kAttack].set(.02f); controls[E::kDecay].set(.01f);
      controls[E::kSustain].set(1); controls[E::kRelease].set(1);
    }
    void event(float value, int offset = 0, bool fresh = false) {
      trigger.trigger(vital::poly_mask(-1), value, offset);
      if (fresh) reset.trigger(vital::constants::kFirstMask, vital::kVoiceOn, offset);
    }
    void process(int count) { env.process(count); trigger.clearTrigger(); reset.clearTrigger(); }
  };

  void testFreshVoiceAndAudibleRetrigger() {
    for (int rate : {44100, 48000, 96000}) for (bool control : {false, true}) {
      EnvelopeFixture fresh(rate), audible(rate);
      for (auto* f : {&fresh, &audible}) {
        f->env.setControlRate(control);
        f->event(vital::kVoiceOn);
        for (int i = 0; i < rate / 10; ++i) f->process(1);
        f->event(vital::kVoiceOff);
        for (int i = 0; i < rate / 10; ++i) f->process(1);
      }
      // One frame at a time keeps the control-rate reference observable too.
      for (int i = 0; i < 64; ++i) {
        if (i == 17) {
          fresh.event(vital::kVoiceOn, 0, true);
          audible.event(vital::kVoiceOn);
        }
        fresh.process(1); audible.process(1);
        const auto a = fresh.env.output()->buffer[0], b = audible.env.output()->buffer[0];
        if (i < 17) check(a[0] == b[0], "fresh reset must leave earlier audio untouched");
        if (i == 17) {
          check(a[0] == 0 && a[1] == 0, "fresh voice must start from zero");
          check(b[0] > .8f, "audible retrigger must retain its release value");
        }
        check(a[2] == b[2] && a[3] == b[3], "reset must not alter the other voice pair");
      }
    }
    // Exercise an actual in-block reset; samples before offset 17 must be identical.
    EnvelopeFixture fresh, reference;
    for (auto* f : {&fresh, &reference}) {
      f->event(vital::kVoiceOn); for (int b = 0; b < 100; ++b) f->process(128);
      f->event(vital::kVoiceOff); for (int b = 0; b < 20; ++b) f->process(128);
    }
    fresh.event(vital::kVoiceOn, 17, true); fresh.process(128); reference.process(128);
    for (int i = 0; i < 17; ++i)
      check(fresh.env.output()->buffer[i][0] == reference.env.output()->buffer[i][0], "in-block reset must preserve prefix");
    check(fresh.env.output()->buffer[17][0] == 0, "fresh reset must happen at its sample offset");
    for (bool control : {false, true}) {
      EnvelopeFixture delayed;
      delayed.env.setControlRate(control);
      delayed.event(vital::kVoiceOn);
      for (int b = 0; b < 100; ++b) delayed.process(128);
      delayed.event(vital::kVoiceOff); delayed.process(128);
      delayed.controls[vital::Envelope::kDelay].set(.005f);
      delayed.event(vital::kVoiceOn, 0, true);
      for (int i = 0; i < 240; ++i) {
        delayed.process(1);
        check(delayed.env.output()->buffer[0][0] == 0, "a fresh voice's Delay must not inherit an old release");
      }
    }
  }

  void testDelayBehaviorPreserved() {
    for (bool control : {false, true}) {
      EnvelopeFixture f;
      f.env.setControlRate(control); f.controls[vital::Envelope::kDelay].set(.005f);
      float before = 0, onset = 0, restored = 0;
      for (int i = 0; i <= 9841; ++i) {
        if (i == 0 || i == 9600) f.event(vital::kVoiceOn);
        if (i == 4800) f.event(vital::kVoiceOff);
        f.process(1);
        if (i == 9599) before = f.env.output()->buffer[0][0];
        if (i == 9600) onset = f.env.output()->buffer[0][0];
        if (i == 9841) restored = f.env.output()->buffer[0][0];
      }
      check(std::abs(before - .900025f) < 1e-5f, "Delay reference release value must be unchanged");
      check(std::abs(onset - (control ? 0.f : before)) < 1e-6f, "requested Delay retrigger behavior must remain intact");
      if (!control) check(std::abs(restored - before) < 1e-6f, "Delay-to-Attack restoration must remain intact");
    }
  }

  std::vector<float> handoff(const std::vector<int>& blocks) {
    vital::ModulationConnectionProcessor route(0);
    route.setControlRate(false); route.setSampleRate(48000); route.setCollapsePolyphonicSource(true);
    int previous_voice, next_voice;
    route.output()->buffer[0] = 0;
    route.collapseToVoice(vital::constants::kFirstMask, 1, -1, &previous_voice);
    std::vector<float> result;
    int block = 0;
    while (result.size() < 1200) {
      int count = std::min(blocks[block++ % blocks.size()], 1200 - static_cast<int>(result.size()));
      for (int i = 0; i < count; ++i) route.output()->buffer[i] = 1;
      // The same lane in a different voice group must still hand off.
      route.collapseToVoice(vital::constants::kFirstMask, count, -1, &next_voice);
      for (int i = 0; i < count; ++i) result.push_back(route.output()->buffer[i][0]);
    }
    return result;
  }
  void testVoiceOwnershipHandoff() {
    const auto a = handoff({1}), b = handoff({128, 17, 63});
    check(a == b, "release handoff must be independent of callback partitioning");
    check(a[0] == 0 && a.back() == 1, "handoff must begin at previous value and settle on new value");
    for (size_t i = 1; i < a.size(); ++i)
      check(a[i] >= a[i-1] && a[i] - a[i-1] < .002f, "handoff must not contain a parameter jump");

    vital::ModulationConnectionProcessor control(0);
    control.setControlRate(true); control.setSampleRate(48000); control.setCollapsePolyphonicSource(true);
    int previous_voice, next_voice;
    control.output()->buffer[0] = 0;
    control.collapseToVoice(vital::constants::kFirstMask, 128, -1, &previous_voice);
    const float duration = std::round(vital::kModulationHandoffTime * 48000);
    for (int block = 1; block <= 8; ++block) {
      control.output()->buffer[0] = 1;
      control.collapseToVoice(vital::constants::kFirstMask, 128, -1, &next_voice);
      check(std::abs(control.output()->buffer[0][0] - std::min(1.f, block * 128 / duration)) < 1e-6f,
            "control-rate ownership must advance to the correct block endpoint");
    }
  }

  struct Rig {
    SynthPlugin plugin;
    vital::ModulationConnectionProcessor* route = nullptr;
    Rig(int model, int env, bool fx, float amp_release = .1f, float mod_release = .01f,
        int oversampling = 0, int rate = 48000, int bus = 0) {
      plugin.setPlayConfigDetails(0, 2, rate, 128); plugin.prepareToPlay(rate, 128);
      auto set = [&](const std::string& id, float v) { plugin.getControls().at(id)->set(v); };
      set("oversampling", oversampling); plugin.checkOversampling();
      set("volume", 6400); set("polyphony", 8); set("legato", 0); set("velocity_track", 0);
      for (int i = 1; i <= 4; ++i) set("osc_" + std::to_string(i) + "_on", 0);
      set(slot_ids::id("sample_on"), 1); set(slot_ids::id("sample_level"), .5f); set(slot_ids::id("sample_loop"), 1);
      set(slot_ids::id("sample_random_phase"), 0); set(slot_ids::id("sample_keytrack"), 0);
      const int destinations[] = {vital::constants::kEffects, vital::constants::kBus1, vital::constants::kBus2, vital::constants::kBus3};
      set(slot_ids::id("sample_destination"), fx ? destinations[bus] : vital::constants::kFilter1);
      std::vector<float> pcm(48000);
      for (int i = 0; i < 48000; ++i)
        pcm[i] = .4f * std::sin(2.0 * juce::MathConstants<double>::pi * 110 * i / 48000) +
                 .1f * std::sin(2.0 * juce::MathConstants<double>::pi * 770 * i / 48000);
      plugin.getEngine()->getSample(slot_ids::kSampleSlot)->loadSample(pcm.data(), pcm.size(), 48000);
      for (int en = 1; en <= 2; ++en) {
        std::string p = "env_" + std::to_string(en) + "_";
        set(p+"attack", std::pow(.01f, .25f)); set(p+"decay", std::pow(.02f, .25f)); set(p+"sustain", 1);
        set(p+"release", std::pow(en == 1 ? amp_release : mod_release, .25f));
        set(p+"attack_power", 0); set(p+"decay_power", 0); set(p+"release_power", 0);
      }
      const std::string chain = bus ? "bus_" + std::to_string(bus) + "_" : "";
      const auto prefix = fx ? chain + "fx_slot_1_filter_fx" : "filter_1";
      if (fx) { set(chain+"fx_slot_1_type", vital::constants::kFilterFx + 1); set(chain+"fx_slot_1_enabled", 1); }
      else set("filter_1_on", 1);
      set(prefix+"_model", model); set(prefix+"_style", 0); set(prefix+"_cutoff", 36);
      set(prefix+"_resonance", .5f); set(prefix+"_mix", 1); set(prefix+"_keytrack", 0);
      check(plugin.connectModulation("env_"+std::to_string(env), prefix+"_cutoff"), "envelope route must connect");
      set("modulation_1_amount", .5f);
      route = plugin.getEngine()->getModulationBank().atIndex(0)->modulation_processor.get();
      for (int b = 0; b < 64; ++b) process();
    }
    double process(int note = -1, bool on = true) {
      juce::AudioBuffer<float> audio(2, 128); audio.clear(); juce::MidiBuffer midi;
      if (note >= 0) midi.addEvent(on ? juce::MidiMessage::noteOn(1, note, .8f) : juce::MidiMessage::noteOff(1, note), 0);
      plugin.processBlock(audio, midi);
      double energy = 0;
      for (int i = 0; i < 128; ++i) {
        const float x = audio.getSample(0, i);
        check(std::isfinite(x), "rendered audio must stay finite"); energy += double(x) * x;
      }
      return energy;
    }
  };

  void testRenderedFallback(int model, int env, int oversampling = 0, int rate = 48000, int bus = 0) {
    Rig r(model, env, true, .1f, .01f, oversampling, rate, bus);
    float previous = 0, max_rise = 0, final_value = 0;
    double energy = 0;
    for (int b = 0; b < 240; ++b) {
      energy += b == 0 ? r.process(60) : b == 40 ? r.process(67) : b == 80 ? r.process(67, false) : r.process();
      const int samples = 128 * r.route->getOversampleAmount();
      for (int i = 0; i < samples; ++i) {
        const float value = r.route->output()->buffer[i][0];
        // The release moves downward; the ownership fallback moves upward.
        // A deliberately fast Env 2 release can be steeper than the handoff.
        if (b > 81) max_rise = std::max(max_rise, value - previous);
        previous = value; final_value = value;
      }
    }
    const float limit = 64.f / std::round(vital::kModulationHandoffTime * r.route->getSampleRate()) + .0001f;
    check(energy > .01, "fallback fixture must render audible audio");
    check(max_rise > 0 && max_rise <= limit, "FX fallback must replace the old 64-semitone jump with a bounded transition");
    check(std::abs(final_value - 64) < .001f, "FX modulation must return to the held note");
    std::cout << "FX fallback model="<<model<<" env="<<env<<" rate="<<rate<<" oversampling="<<oversampling
              <<" bus="<<bus<<" largest_rise="<<max_rise<<" limit="<<limit<<'\n';
  }

  void testRenderedVoiceReuse() {
    for (int next_note : {67, 69}) {
      Rig r(3, 2, false, .01f, 1);
      for (int b = 0; b < 40; ++b) { if (b == 0) r.process(60); else r.process(); }
      r.process(67); const float first = r.route->output()->buffer[0][0];
      for (int b = 0; b < 39; ++b) r.process();
      r.process(67, false); for (int b = 0; b < 39; ++b) r.process();
      check(r.plugin.getEngine()->getNumActiveVoices() == 1, "Env 1 must retire the second voice before reuse");
      r.process(next_note); const float reused = r.route->output()->buffer[0][0];
      check(first == 0 && reused == 0, "a recycled voice must not inherit the old 57-semitone filter offset");
      std::cout << "Voice reuse note="<<next_note<<" first="<<first<<" reused="<<reused<<'\n';
    }
  }

  Component* withTitle(Component& c, const String& title) {
    if (c.getTitle() == title) return &c;
    for (auto* child : c.getChildren()) if (auto* found = withTitle(*child, title)) return found;
    return nullptr;
  }
  bool select(Component& c, const String& title, const String& choice) {
    auto* combo = dynamic_cast<ComboBox*>(withTitle(c, title));
    if (combo == nullptr) return false;
    for (int i = 0; i < combo->getNumItems(); ++i) if (combo->getItemText(i) == choice) {
      combo->setSelectedItemIndex(i, sendNotificationSync); return true;
    }
    return false;
  }
  ValueBridge* bridgeFor(SynthPlugin& p, const std::string& name) {
    for (auto* parameter : p.getParameters()) if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
      if (bridge->getParameterId() == String(name)) return bridge;
    return nullptr;
  }

  void testSyncedEnvelopeControls() {
    SynthPlugin synth;
    synth.getControls().at("beats_per_minute")->set(2);
    SynthEditor editor(synth);
    check(select(editor, "Editor group", "Envelopes"), "envelope group must be available");
    for (int env = 1; env <= vital::kNumEnvelopes; ++env) {
      const auto prefix = "env_"+std::to_string(env)+"_";
      synth.getControls().at(prefix+"tempo_sync")->set(1);
      check(select(editor, "Editor element", "Envelope " + String(env)), "envelope page must be available");
      editor.updateFullGui();
      for (const auto* stage : {"delay", "attack", "hold", "decay", "release"}) {
        auto* bridge = bridgeFor(synth, prefix+stage);
        check(bridge != nullptr, "envelope time must have a host parameter"); if (!bridge) continue;
        auto* slider = dynamic_cast<ParameterSlider*>(withTitle(editor, bridge->getName(128)));
        check(slider && slider->getInterval() == 1, "synced envelope must expose discrete duration steps"); if (!slider) continue;
        auto handler = slider->createAccessibilityHandler();
        auto* ax = handler ? handler->getValueInterface() : nullptr;
        check(ax != nullptr, "envelope duration must expose accessibility values"); if (!ax) continue;
        for (const auto* text : {"1/4", "1/4d", "1/4t", "1/8", "1/8d", "1/8t"}) {
          ax->setValueAsString(text);
          check(ax->getCurrentValueAsString() == text && bridge->getText(bridge->getValue(),128) == text,
                "editor, text entry, accessibility, and host must agree on musical duration");
        }
        ax->setValueAsString("1/4");
        const double initial = slider->getValue();
        slider->keyPressed(KeyPress(KeyPress::upKey));
        check(slider->getValue() == initial + 1, "arrow must select one duration");
        slider->keyPressed(KeyPress(KeyPress::downKey, ModifierKeys::shiftModifier, 0));
        check(slider->getValue() == initial, "Shift arrow must still select a complete musical duration");
        slider->keyPressed(KeyPress(KeyPress::pageDownKey));
        check(slider->getValue() == std::max(0.0, initial - 10), "Page Down must move ten durations");
        ax->setValue(ax->getCurrentValue() + ax->getRange().getInterval());
        check(ax->getRange().getInterval() == 1, "VoiceOver must increment a complete duration");
        slider->keyPressed(KeyPress(KeyPress::homeKey)); check(slider->getValue() == slider->getMaximum(), "Home must select longest duration");
        slider->keyPressed(KeyPress(KeyPress::endKey)); check(slider->getValue() == 0, "End must select shortest duration");
        for (int index = 0; index <= bridge->maximumEnvelopeTimeDivision(); ++index) {
          const float value = bridge->valueForEnvelopeTimeDivision(index);
          check(bridge->envelopeTimeDivision(value) == index, "every displayed duration must be reachable by the DSP");
        }
      }
      // Refresh the existing rows via their real timer when sync changes.
      synth.getControls().at(prefix+"tempo_sync")->set(0);
      auto* bridge = bridgeFor(synth, prefix+"attack");
      auto* slider = dynamic_cast<ParameterSlider*>(withTitle(editor, bridge->getName(128)));
      // Wait for the observable refresh: a single 60 ms sleep can race JUCE's
      // timer thread even though the editor requests a 50 ms timer interval.
      const auto start = juce::Time::getMillisecondCounter();
      while (slider && slider->getInterval() != 0 && juce::Time::getMillisecondCounter() - start < 1000) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
        slider = dynamic_cast<ParameterSlider*>(withTitle(editor, bridge->getName(128)));
      }
      check(slider && slider->getInterval() == 0 && slider->getMaximum() == 1,
            "disabling sync must restore the continuous time slider for " + prefix);
      check(bridge->getText(bridge->getValue(),128).contains("ms"), "free time must restore milliseconds");
    }
    // Verify musical parsing at different host tempos using known beat lengths.
    auto* bridge = bridgeFor(synth, "env_1_release");
    synth.getControls().at("env_1_tempo_sync")->set(1);
    for (float bps : {1.f, 2.f, 3.f}) {
      synth.getControls().at("beats_per_minute")->set(bps);
      for (const auto& division : vital::envelope_time::divisions) {
        if (division.beats / bps > 32) continue;
        const float normalized = bridge->getValueForText(division.name);
        const float raw = bridge->convertToEngineValue(normalized);
        check(std::abs(raw*raw*raw*raw - division.beats/bps) < .00003f, "musical text must map to the played duration at host tempo");
      }
    }
  }

  void testSyncedDspRelease() {
    for (int rate : {44100, 48000, 96000}) for (float bps : {1.f, 2.f, 3.f})
      for (float beats : {1.f, 1.5f, 2.f/3}) {
        EnvelopeFixture f(rate);
        f.controls[vital::Envelope::kTempoSync].set(1);
        f.controls[vital::Envelope::kBeatsPerSecond].set(bps);
        f.controls[vital::Envelope::kRelease].set(beats/bps);
        f.event(vital::kVoiceOn);
        for (int b = 0; b < rate/128; ++b) f.process(128);
        check(f.env.output()->buffer[127][0] > .99f, "synced release fixture must reach sustain");
        f.event(vital::kVoiceOff);
        int silent = -1;
        for (int offset = 0; offset < rate * 2 && silent < 0; offset += 128) {
          f.process(128);
          for (int i = 0; i < 128; ++i) if (f.env.output()->buffer[i][0] <= 1e-6f) { silent = offset+i; break; }
        }
        check(silent >= 0 && std::abs(silent - rate*beats/bps) < 32,
              "DSP must play quarter, dotted quarter, and triplet quarter release lengths");
      }
  }

  // Envelope 1's analog mode: RC curves, carrying on from the current level,
  // and Age for worn timing parts.
  void testAnalogMode() {
    using E = vital::Envelope;
    constexpr int rate = 48000;
    auto make = [rate](float age) {
      auto f = std::make_unique<EnvelopeFixture>(rate);
      f->env.setVoiceCardSalt("env_1");
      f->controls[E::kAnalogMode].set(1);
      f->controls[E::kAnalogAge].set(age);
      f->controls[E::kAttack].set(.1f);
      f->controls[E::kDecay].set(1);
      f->controls[E::kSustain].set(.5f);
      f->controls[E::kRelease].set(1);
      return f;
    };
    auto value = [](EnvelopeFixture& f) { return f.env.output()->buffer[0][0]; };
    // Samples until the attack peaks, and the level halfway through the Attack time.
    auto attack = [&](EnvelopeFixture& f, float& halfway, float& peak) {
      f.event(vital::kVoiceOn, 0, true);
      int samples = 0;
      float last = -1.0f;
      for (; samples < rate * 2; ++samples) {
        f.process(1);
        if (samples == rate / 20)
          halfway = value(f);
        if (value(f) <= last)
          break;
        last = value(f);
      }
      peak = last;
      return samples;
    };

    auto plain = make(0.0f);
    float halfway = 0.0f, peak = 0.0f;
    const int attack_samples = attack(*plain, halfway, peak);
    std::cout << "Analog envelope: attack " << 1000.0 * attack_samples / rate << " ms, halfway " << halfway << '\n';
    check(std::abs(attack_samples - rate / 10) < rate / 500, "analog attack takes the Attack time");
    check(halfway > .6f, "analog attack rises fast and bends over at the top");
    check(peak == 1.0f, "unworn analog attack peaks at full level");

    for (int i = 0; i < rate / 2; ++i) plain->process(1);
    check(std::abs(value(*plain) - .55f) < .01f, "analog decay falls exponentially");
    for (int i = 0; i < rate / 2; ++i) plain->process(1);
    check(std::abs(value(*plain) - .5f) < .006f, "analog decay reaches sustain in the Decay time");

    plain->event(vital::kVoiceOff);
    for (int i = 0; i < rate / 2; ++i) plain->process(1);
    const float releasing = value(*plain);
    check(std::abs(releasing - .05f) < .005f, "analog release falls exponentially");
    plain->event(vital::kVoiceOn);
    plain->process(1);
    check(value(*plain) >= releasing, "a new note carries on from the current level");
    plain->event(vital::kVoiceOff);
    for (int i = 0; i < rate * 3; ++i) plain->process(1);
    check(value(*plain) == 0.0f, "analog release ends in silence so the voice can stop");

    // Age: worn timing that is the same each load, a lower peak and a leaking sustain.
    auto worn = make(1.0f), again = make(1.0f);
    float worn_halfway = 0.0f, worn_peak = 0.0f, again_halfway = 0.0f, again_peak = 0.0f;
    const int worn_samples = attack(*worn, worn_halfway, worn_peak);
    const int again_samples = attack(*again, again_halfway, again_peak);
    for (int i = 0; i < rate * 5; ++i) worn->process(1);
    std::cout << "Worn envelope: attack " << 1000.0 * worn_samples / rate << " ms, peak " << worn_peak
              << ", sustain after 5 s " << value(*worn) << '\n';
    check(worn_samples != attack_samples && worn_samples > rate / 25 && worn_samples < rate / 4,
          "Age changes the attack time within an octave or so");
    check(worn_samples == again_samples && worn_peak == again_peak, "worn timing is the same every load");
    check(worn_peak <= 1.0f && worn_peak >= .64f, "Age lowers the attack peak a little");
    check(value(*worn) < .5f * .85f, "Age makes sustain leak away");

    auto sluggish = make(1.0f);
    sluggish->controls[E::kAttack].set(.0005f);
    float sluggish_halfway = 0.0f, sluggish_peak = 0.0f;
    check(attack(*sluggish, sluggish_halfway, sluggish_peak) > rate / 40, "Age makes the shortest attack sluggish");
  }

  int run() {
    juce::ScopedJuceInitialiser_GUI gui;
    testAnalogMode();
    testFreshVoiceAndAudibleRetrigger(); testDelayBehaviorPreserved(); testVoiceOwnershipHandoff();
    for (int env : {1,2}) for (int model : {0,1,2,3,4,7}) testRenderedFallback(model,env);
    for (int bus : {1,2,3}) testRenderedFallback(3,2,0,48000,bus);
    for (int oversampling : {1,2,3}) testRenderedFallback(3,1,oversampling,oversampling == 2 ? 96000 : 44100);
    testRenderedVoiceReuse(); testSyncedEnvelopeControls(); testSyncedDspRelease();
    std::cout << "Envelope regressions: " << failures << " failures\n";
    return failures ? 1 : 0;
  }
}
