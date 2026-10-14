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

#include "producers_module.h"
#include "filter_routing.h"
#include "modulation_connection_processor.h"
#include "voice_handler.h"

#include <algorithm>

namespace vital {
  namespace {
    // Internal arpeggiator notes arrive on MIDI channels 16.. with one channel per slot.
    constexpr int kFirstArpDestinationChannel = 16;
    constexpr int kNumArpDestinations = kNumOscillators;
  }

  ProducersModule::ProducersModule(const Output* beats_per_second) :
      SynthModule(kNumInputs, kNumOutputs),
      oscillator_routes_(std::make_shared<std::vector<OscillatorRoute>>()),
      portamento_glide_zones_(nullptr), zone_crossfade_(nullptr),
      filter1_on_(nullptr), filter2_on_(nullptr) {
    // Routes change on the audio thread; never allocate there.
    oscillator_routes_->reserve(kMaxModulationConnections);
    for (int i = 0; i < kNumOscillators; ++i) {
      slots_[i] = new SourceSlotModule(i, beats_per_second);
      addSubmodule(slots_[i]);
      addProcessor(slots_[i]);
      // Slots are processed explicitly, in modulation order, not by the router loop.
      slots_[i]->enable(false);
      slot_destinations_[i] = nullptr;
      slot_solo_[i] = nullptr;
      slot_key_zone_start_[i] = nullptr;
      slot_key_zone_end_[i] = nullptr;
      slot_velocity_zone_start_[i] = nullptr;
      slot_velocity_zone_end_[i] = nullptr;
      slot_pitch_clamp_[i] = new cr::ZonePitchClamp();
      addProcessor(slot_pitch_clamp_[i]);
      slot_zone_gain_[i] = 0.0f;
    }
    for (int arp = 0; arp < 2; ++arp) {
      arp_on_[arp] = nullptr;
      for (int destination = 0; destination < kNumArpDestinations; ++destination)
        arp_destinations_[arp][destination] = nullptr;
    }
  }

  void ProducersModule::init() {
    portamento_glide_zones_ = createBaseControl("portamento_glide_zones");
    zone_crossfade_ = createBaseControl("zone_crossfade");
    for (int arp = 0; arp < 2; ++arp) {
      const std::string prefix = "arp_" + std::to_string(arp + 1) + "_";
      arp_on_[arp] = createBaseControl(prefix + "on");
      createBaseControl(prefix + "time_mode");
      createBaseControl(prefix + "sync_rate");
      createBaseControl(prefix + "free_rate");
      createBaseControl(prefix + "style");
      createBaseControl(prefix + "play_order");
      createBaseControl(prefix + "gate");
      createBaseControl(prefix + "random_gate");
      createBaseControl(prefix + "repeats");
      createBaseControl(prefix + "loop");
      createBaseControl(prefix + "offset");
      createBaseControl(prefix + "distance");
      createBaseControl(prefix + "steps");
      createBaseControl(prefix + "velocity");
      createBaseControl(prefix + "velocity_decay");
      createBaseControl(prefix + "random_velocity");
      createBaseControl(prefix + "hold");
      createBaseControl(prefix + "retrigger");
      createBaseControl(prefix + "retrigger_division");
      createBaseControl(prefix + "channel");
      for (int destination = 0; destination < kNumArpDestinations; ++destination) {
        arp_destinations_[arp][destination] =
            createBaseControl(prefix + "destination_osc_" + std::to_string(destination + 1));
      }
    }

    for (int i = 0; i < kNumOscillators; ++i) {
      const std::string prefix = source_types::slotPrefix(i);
      slot_destinations_[i] = createBaseControl(prefix + "destination");
      slot_solo_[i] = createBaseControl(prefix + "solo");
      slot_key_zone_start_[i] = createBaseControl(prefix + "key_zone_start");
      slot_key_zone_end_[i] = createBaseControl(prefix + "key_zone_end");
      slot_velocity_zone_start_[i] = createBaseControl(prefix + "velocity_zone_start");
      slot_velocity_zone_end_[i] = createBaseControl(prefix + "velocity_zone_end");

      plugPitchClamp(slot_pitch_clamp_[i], slot_key_zone_start_[i], slot_key_zone_end_[i]);

      slots_[i]->useInput(input(kReset), SourceSlotModule::kReset);
      slots_[i]->useInput(input(kRetrigger), SourceSlotModule::kRetrigger);
      slots_[i]->useInput(input(kVoiceEvent), SourceSlotModule::kVoiceEvent);
      slots_[i]->plug(slot_pitch_clamp_[i]->output(), SourceSlotModule::kMidi);
      slots_[i]->useInput(input(kActiveVoices), SourceSlotModule::kActiveVoices);
      slots_[i]->useInput(input(kNoteCount), SourceSlotModule::kNoteCount);
      slots_[i]->useInput(input(kNotePressed), SourceSlotModule::kNotePressed);
      slots_[i]->useInput(input(kVelocity), SourceSlotModule::kVelocity);
      slots_[i]->useInput(input(kZoneMidi), SourceSlotModule::kNote);
      slots_[i]->useInput(input(kMidiKey), SourceSlotModule::kMidiKey);
    }

    SynthModule::init();

    // Every slot can modulate every other slot: position k reads the k-th other slot.
    for (int i = 0; i < kNumOscillators; ++i) {
      for (int position = 0; position < SourceSlotModule::kNumModulatorPositions; ++position) {
        const int source = getModulationIndex(i, position);
        slots_[i]->setModulatorOutput(position, slots_[source]->output(SourceSlotModule::kRaw));
      }
    }
  }

  bool ProducersModule::prepareSelectedSources() {
    bool changed = false;
    for (int i = 0; i < kNumOscillators; ++i)
      changed = slots_[i]->prepareActiveType() || changed;
    return changed;
  }

  void ProducersModule::plugPitchClamp(cr::ZonePitchClamp* clamp, Value* key_start, Value* key_end) {
    clamp->useInput(input(kMidi), cr::ZonePitchClamp::kMidi);
    clamp->useInput(input(kZoneGlideMidi), cr::ZonePitchClamp::kGlideMidi);
    clamp->plug(key_start, cr::ZonePitchClamp::kZoneStart);
    clamp->plug(key_end, cr::ZonePitchClamp::kZoneEnd);
    clamp->plug(portamento_glide_zones_, cr::ZonePitchClamp::kEnable);
  }

  bool ProducersModule::arpDestinationOn(int arp, int destination) const {
    return isPositiveAndBelow(arp, 2) && isPositiveAndBelow(destination, kNumArpDestinations) &&
           arp_on_[arp] != nullptr && arp_on_[arp]->value() != 0.0f &&
           arp_destinations_[arp][destination] != nullptr && arp_destinations_[arp][destination]->value() != 0.0f;
  }

  bool ProducersModule::sourceHasActiveArp(int destination) const {
    return arpDestinationOn(0, destination) || arpDestinationOn(1, destination);
  }

  bool ProducersModule::anyArpDestinationOn() const {
    for (int arp = 0; arp < 2; ++arp) {
      for (int destination = 0; destination < kNumArpDestinations; ++destination) {
        if (arpDestinationOn(arp, destination))
          return true;
      }
    }
    return false;
  }

  poly_float ProducersModule::getSourcePlaybackMask(int destination, poly_float zone_mask) const {
    poly_float channel = input(kChannel)->at(0);
    if (!anyArpDestinationOn())
      return zone_mask;

    poly_mask internal_arp_mask = poly_float::equal(channel, kFirstArpDestinationChannel);
    for (int i = 1; i < kNumArpDestinations; ++i)
      internal_arp_mask |= poly_float::equal(channel, kFirstArpDestinationChannel + i);

    if (!sourceHasActiveArp(destination))
      return zone_mask & ~internal_arp_mask;

    poly_mask destination_mask = poly_float::equal(channel, kFirstArpDestinationChannel + destination);
    return poly_float(1.0f) & destination_mask;
  }

  poly_float ProducersModule::getZoneMask(Value* key_start, Value* key_end,
                                          Value* velocity_start, Value* velocity_end) {
    poly_float note = (portamento_glide_zones_->value() != 0.0f) ? input(kZoneGlideMidi)->at(0)
                                                                  : input(kZoneMidi)->at(0);
    poly_float velocity = input(kVelocity)->at(0);
    poly_float key_min = std::min(key_start->value(), key_end->value());
    poly_float key_max = std::max(key_start->value(), key_end->value());
    poly_float velocity_min = std::min(velocity_start->value(), velocity_end->value());
    poly_float velocity_max = std::max(velocity_start->value(), velocity_end->value());
    poly_mask key_mask = poly_float::greaterThanOrEqual(note, key_min) &
                         poly_float::lessThanOrEqual(note, key_max);
    poly_mask velocity_mask = poly_float::greaterThanOrEqual(velocity, velocity_min) &
                              poly_float::lessThanOrEqual(velocity, velocity_max);
    return poly_float(1.0f) & key_mask & velocity_mask;
  }

  ProducersModule::ZoneGain ProducersModule::advanceZoneGain(poly_float& gain_state, poly_float target,
                                                             poly_mask reset_mask, bool crossfade_on,
                                                             mono_float max_move) {
    ZoneGain gain;
    if (crossfade_on) {
      // Snap to the target on note-on so fresh notes don't crossfade in; glides ramp.
      gain.start = utils::maskLoad(gain_state, target, reset_mask);
      poly_float diff = target - gain.start;
      gain.end = gain.start + utils::clamp(diff, -max_move, max_move);
    }
    else {
      gain.start = target;
      gain.end = target;
    }
    gain_state = gain.end;
    return gain;
  }

  void ProducersModule::addZonedBuffer(poly_float* dest, const poly_float* source,
                                       const ZoneGain& gain, int num_samples, const poly_float* ratio) const {
    poly_float current = gain.start;
    poly_float delta = (gain.end - gain.start) * (1.0f / num_samples);
    if (ratio != nullptr) {
      for (int i = 0; i < num_samples; ++i) {
        dest[i] += source[i] * current * ratio[i];
        current += delta;
      }
      return;
    }
    for (int i = 0; i < num_samples; ++i) {
      dest[i] += source[i] * current;
      current += delta;
    }
  }

  void ProducersModule::addOscillatorRoute(ModulationConnectionProcessor* processor, int source, int destination) {
    removeOscillatorRoute(processor);
    if (oscillator_routes_->size() < oscillator_routes_->capacity())
      oscillator_routes_->push_back({ processor, source, destination });
  }

  void ProducersModule::removeOscillatorRoute(ModulationConnectionProcessor* processor) {
    auto& routes = *oscillator_routes_;
    routes.erase(std::remove_if(routes.begin(), routes.end(),
                                [processor](const OscillatorRoute& route) { return route.processor == processor; }),
                 routes.end());
  }

  void ProducersModule::runOscillatorRoutes(int slot, bool into_slot, int num_samples) {
    ProcessorRouter* voice = voice_handler_ != nullptr ? voice_handler_->processingVoiceRouter() : nullptr;
    if (voice == nullptr)
      return;

    for (const OscillatorRoute& route : *oscillator_routes_) {
      const bool runs_now = into_slot ? route.destination == slot
                                      : route.source == slot && route.destination < 0;
      if (!runs_now)
        continue;
      if (Processor* local = voice->findLocalProcessor(route.processor))
        static_cast<ModulationConnectionProcessor*>(local)->processRoute(num_samples);
    }
  }

  void ProducersModule::process(int num_samples) {
    SynthModule::process(num_samples);

    // A slot reading another slot through FM, RM or sync, or through a
    // modulation route from that slot's audio, renders after it.
    unsigned dependencies[kNumOscillators];
    for (int i = 0; i < kNumOscillators; ++i) {
      dependencies[i] = 0;
      for (int which = 0; which < 2; ++which) {
        const int position = slots_[i]->modulatorPosition(which == 1);
        if (position >= 0)
          dependencies[i] |= 1u << getModulationIndex(i, position);
      }
    }
    for (const OscillatorRoute& route : *oscillator_routes_) {
      if (route.destination >= 0)
        dependencies[route.destination] |= 1u << route.source;
    }

    unsigned processed = 0;
    auto render = [&](int slot) {
      runOscillatorRoutes(slot, true, num_samples);
      getLocalProcessor(slots_[slot])->process(num_samples);
      processed |= 1u << slot;
      runOscillatorRoutes(slot, false, num_samples);
    };

    int num_processed = 0;
    int index = 0;
    for (int i = 0; i < kNumOscillators * kNumOscillators && num_processed < kNumOscillators; ++i) {
      const unsigned slot_bit = 1u << index;
      if ((processed & slot_bit) == 0 && (dependencies[index] & ~processed) == 0) {
        num_processed++;
        render(index);
      }
      index = (index + 1) % kNumOscillators;
    }
    // FM, RM, sync and audio routes can form a cycle. Break it in slot order
    // using the preceding block for sources not yet rendered, so every slot
    // advances instead of replaying a stale output.
    if (num_processed < kNumOscillators) {
      for (int i = 0; i < kNumOscillators; ++i) {
        if ((processed & (1u << i)) == 0)
          render(i);
      }
    }

    poly_float* filter1_output = output(kToFilter1)->buffer;
    poly_float* filter2_output = output(kToFilter2)->buffer;
    poly_float* raw_output = output(kRawOut)->buffer;
    poly_float* bus1_output = output(kBus1Out)->buffer;
    poly_float* bus2_output = output(kBus2Out)->buffer;
    poly_float* bus3_output = output(kBus3Out)->buffer;
    poly_float* direct_output = output(kDirectOut)->buffer;
    utils::zeroBuffer(filter1_output, num_samples);
    utils::zeroBuffer(filter2_output, num_samples);
    utils::zeroBuffer(raw_output, num_samples);
    utils::zeroBuffer(bus1_output, num_samples);
    utils::zeroBuffer(bus2_output, num_samples);
    utils::zeroBuffer(bus3_output, num_samples);
    utils::zeroBuffer(direct_output, num_samples);

    bool filter1_on = isFilter1On();
    bool filter2_on = isFilter2On();
    FilterRouting routing = filterRouting(filter1_on, filter2_on,
                                          filter1_input_ && filter1_input_->value() > 0.5f,
                                          filter2_input_ && filter2_input_->value() > 0.5f);
    auto sourceDestination = [routing, filter1_on, filter2_on](const Value* destination) {
      return filterSourceDestination(destination->value(), routing, filter1_on, filter2_on);
    };

    bool any_source_solo = false;
    for (int i = 0; i < kNumOscillators; ++i)
      any_source_solo = any_source_solo || slot_solo_[i]->value() >= 0.5f;

    // Zone crossfade smooths the gain when a producer enters or leaves its zone during a glide.
    poly_mask zone_reset_mask = getResetMask(kReset);
    mono_float crossfade_seconds = zone_crossfade_->value();
    bool crossfade_on = portamento_glide_zones_->value() != 0.0f && crossfade_seconds > 0.0f;
    mono_float zone_max_move = crossfade_on ? (num_samples / (crossfade_seconds * getSampleRate())) : 0.0f;

    // First the zone gains, and whether a Multi-sample instrument holds a voice open.
    ZoneGain zone_gains[kNumOscillators];
    bool audible[kNumOscillators];
    poly_float instrument_hold = 0.0f;
    for (int i = 0; i < kNumOscillators; ++i) {
      audible[i] = !(any_source_solo && slot_solo_[i]->value() < 0.5f);
      if (!audible[i])
        continue;
      poly_float source_mask = getSourcePlaybackMask(i,
          getZoneMask(slot_key_zone_start_[i], slot_key_zone_end_[i],
                      slot_velocity_zone_start_[i], slot_velocity_zone_end_[i]));
      zone_gains[i] = advanceZoneGain(slot_zone_gain_[i], source_mask,
                                      zone_reset_mask, crossfade_on, zone_max_move);
      instrument_hold = utils::max(instrument_hold,
                                   slots_[i]->output(SourceSlotModule::kHold)->buffer[0] * source_mask);
    }

    // The voice amplitude follows Env 1, raised to full while an instrument in
    // Instrument envelope mode still sounds (closing over 5 ms after). Only that
    // instrument's slot plays at the raised level: every other slot is scaled by
    // (Env 1 / raised)^2, the voice amplitude being the square of the envelope,
    // so it still follows Env 1. With no instrument holding nothing changes.
    const poly_float* envelope = input(kAmplitudeEnvelope)->source->buffer;
    poly_float* gated_envelope = output(kGatedAmplitudeEnvelope)->buffer;
    envelope_gate_ = envelope_gate_ & ~zone_reset_mask;
    const bool gating = poly_float::greaterThan(envelope_gate_, 0.0f).anyMask() ||
                        poly_float::greaterThan(instrument_hold, 0.0f).anyMask();
    poly_float envelope_ratio[kMaxBufferSize * kMaxOversample];
    if (gating) {
      const mono_float step = 1.0f / std::max(1.0f, kInstrumentGateSeconds * getSampleRate());
      const int count = std::min(num_samples, kMaxBufferSize * kMaxOversample);
      for (int i = 0; i < count; ++i) {
        envelope_gate_ = utils::max(instrument_hold, utils::max(envelope_gate_ - step, 0.0f));
        const poly_float gated = utils::max(envelope[i], envelope_gate_);
        gated_envelope[i] = gated;
        const poly_float ratio = envelope[i] / utils::max(gated, 1.0e-30f);
        envelope_ratio[i] = ratio * ratio;
      }
    }
    else {
      for (int i = 0; i < num_samples; ++i)
        gated_envelope[i] = envelope[i];
    }

    for (int i = 0; i < kNumOscillators; ++i) {
      if (!audible[i])
        continue;

      const poly_float* buffer = slots_[i]->output(SourceSlotModule::kLevelled)->buffer;
      const ZoneGain& zone_gain = zone_gains[i];
      const poly_float* ratio = gating && !slots_[i]->holdsVoiceOpen() ? envelope_ratio : nullptr;
      int destination = sourceDestination(slot_destinations_[i]);
      bool raw = destination == constants::kEffects;
      bool filter1 = destination == constants::kFilter1 || destination == constants::kDualFilters;
      bool filter2 = destination == constants::kFilter2 || destination == constants::kDualFilters;
      bool bus1 = destination == constants::kBus1;
      bool bus2 = destination == constants::kBus2;
      bool bus3 = destination == constants::kBus3;
      bool direct_out = destination == constants::kDirectOut;

      if (raw || (!filter2 && filter1 && !filter1_on) ||
                 (!filter1 && filter2 && !filter2_on) ||
                 (filter1 && filter2 && !filter1_on && !filter2_on)) {
        addZonedBuffer(raw_output, buffer, zone_gain, num_samples, ratio);
      }
      if (filter1)
        addZonedBuffer(filter1_output, buffer, zone_gain, num_samples, ratio);
      if (filter2)
        addZonedBuffer(filter2_output, buffer, zone_gain, num_samples, ratio);
      if (bus1)
        addZonedBuffer(bus1_output, buffer, zone_gain, num_samples, ratio);
      if (bus2)
        addZonedBuffer(bus2_output, buffer, zone_gain, num_samples, ratio);
      if (bus3)
        addZonedBuffer(bus3_output, buffer, zone_gain, num_samples, ratio);
      if (direct_out)
        addZonedBuffer(direct_output, buffer, zone_gain, num_samples, ratio);
    }
  }

  void ProducersModule::correctToTime(double seconds) {
    for (int i = 0; i < kNumOscillators; ++i) {
      if (SynthModule* module = slots_[i]->activeModule())
        module->correctToTime(seconds);
    }
  }
} // namespace vital
