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

#include "filters_module.h"
#include "filter_module.h"
#include "synth_constants.h"

namespace vital {

  FiltersModule::FiltersModule() : SynthModule(kNumInputs, kNumOutputs), filter_1_(nullptr), filter_2_(nullptr),
                                   filter_1_filter_input_(nullptr), filter_2_filter_input_(nullptr),
                                   filter_1_destination_(nullptr), filter_2_destination_(nullptr) {
    filter_1_input_ = std::make_shared<Output>();
    filter_2_input_ = std::make_shared<Output>();
    filter_1_amplitude_ = std::make_shared<Output>();
    filter_2_amplitude_ = std::make_shared<Output>();
    plug(&constants::kValueOne, kAmplitude);
  }

  void FiltersModule::init() {
    filter_1_filter_input_ = createBaseControl("filter_1_filter_input");
    filter_1_destination_ = createBaseControl("filter_1_destination");
    filter_1_ = new FilterModule("filter_1");
    addSubmodule(filter_1_);
    addProcessor(filter_1_);

    filter_1_->plug(filter_1_input_.get(), FilterModule::kAudio);
    filter_1_->plug(filter_1_amplitude_.get(), FilterModule::kAmplitude);
    filter_1_->useInput(input(kReset), FilterModule::kReset);
    filter_1_->useInput(input(kKeytrack), FilterModule::kKeytrack);
    filter_1_->useInput(input(kMidi), FilterModule::kMidi);

    filter_2_filter_input_ = createBaseControl("filter_2_filter_input");
    filter_2_destination_ = createBaseControl("filter_2_destination");
    filter_2_ = new FilterModule("filter_2");
    addSubmodule(filter_2_);
    addProcessor(filter_2_);

    filter_2_->plug(filter_2_input_.get(), FilterModule::kAudio);
    filter_2_->plug(filter_2_amplitude_.get(), FilterModule::kAmplitude);
    filter_2_->useInput(input(kReset), FilterModule::kReset);
    filter_2_->useInput(input(kKeytrack), FilterModule::kKeytrack);
    filter_2_->useInput(input(kMidi), FilterModule::kMidi);

    SynthModule::init();
  }

  void FiltersModule::prepareFilterInput(int index, int num_samples) {
    auto* destination = index ? filter_2_input_.get() : filter_1_input_.get();
    const auto* source = input(index ? kFilter2Input : kFilter1Input)->source->buffer;
    destination->buffer = destination->owned_buffer.get();
    for (int i = 0; i < num_samples; ++i)
      destination->buffer[i] = source[i] * (envelope_before_[index] ? voiceAmplitude(i) : poly_float(1));
  }

  void FiltersModule::prepareFilterAmplitude(int index, int num_samples) {
    // The Distortion model shapes each note at its enveloped level, as the
    // effect would. Where the envelope already comes before the filter, the
    // input carries it and the model shapes it as it is.
    FilterModule* filter = index ? filter_2_ : filter_1_;
    if (filter->getOnValue()->value() <= .5f ||
        filter->getModelValue()->value() != constants::kDistortionFilter)
      return;
    poly_float* amplitude = (index ? filter_2_amplitude_ : filter_1_amplitude_)->buffer;
    for (int i = 0; i < num_samples; ++i)
      amplitude[i] = envelope_before_[index] ? poly_float(1.0f) : voiceAmplitude(i);
  }

  void FiltersModule::processParallel(int num_samples) {
    prepareFilterInput(0, num_samples);
    prepareFilterInput(1, num_samples);

    getLocalProcessor(filter_1_)->process(num_samples);
    getLocalProcessor(filter_2_)->process(num_samples);

    routeFilterOutputs(num_samples, FilterRouting::kParallel);
  }

  void FiltersModule::processSerialForward(int num_samples) {
    prepareFilterInput(0, num_samples);
    filter_2_input_->buffer = filter_2_input_->owned_buffer.get();

    getLocalProcessor(filter_1_)->process(num_samples);

    poly_float* filter_2_input_buffer = filter_2_input_->buffer;
    const poly_float* filter_1_output_buffer = filter_1_->output()->buffer;
    const poly_float* filter_2_straight_input = input(kFilter2Input)->source->buffer;

    for (int i = 0; i < num_samples; ++i)
      filter_2_input_buffer[i] = filter_1_output_buffer[i] + filter_2_straight_input[i] *
          (envelope_before_[1] ? voiceAmplitude(i) : poly_float(1));

    getLocalProcessor(filter_2_)->process(num_samples);
    routeFilterOutputs(num_samples, FilterRouting::kSerialForward);
  }

  void FiltersModule::processSerialBackward(int num_samples) {
    filter_1_input_->buffer = filter_1_input_->owned_buffer.get();
    prepareFilterInput(1, num_samples);

    getLocalProcessor(filter_2_)->process(num_samples);

    poly_float* filter_1_input_buffer = filter_1_input_->buffer;
    const poly_float* filter_2_output_buffer = filter_2_->output()->buffer;
    const poly_float* filter_1_straight_input = input(kFilter1Input)->source->buffer;

    for (int i = 0; i < num_samples; ++i)
      filter_1_input_buffer[i] = filter_2_output_buffer[i] + filter_1_straight_input[i] *
          (envelope_before_[0] ? voiceAmplitude(i) : poly_float(1));

    getLocalProcessor(filter_1_)->process(num_samples);
    routeFilterOutputs(num_samples, FilterRouting::kSerialBackward);
  }

  void FiltersModule::routeFilterOutputs(int num_samples, FilterRouting routing) {
    poly_float* main_output = output(kMainOut)->buffer;
    poly_float* bus1_output = output(kBus1Out)->buffer;
    poly_float* bus2_output = output(kBus2Out)->buffer;
    poly_float* bus3_output = output(kBus3Out)->buffer;
    poly_float* direct_output = output(kDirectOut)->buffer;
    auto outputFor = [](int destination) {
      if (destination == constants::kBus1) return kBus1Out;
      if (destination == constants::kBus2) return kBus2Out;
      if (destination == constants::kBus3) return kBus3Out;
      if (destination == constants::kDirectOut) return kDirectOut;
      return kMainOut;
    };
    // Clear only the outputs routed to this block, and the ones routed to last
    // block (whole, as a later, longer block reads past this one's samples).
    unsigned written = 0;
    if (routing != FilterRouting::kSerialForward)
      written |= 1u << outputFor(filter_1_destination_->value());
    if (routing != FilterRouting::kSerialBackward)
      written |= 1u << outputFor(filter_2_destination_->value());
    for (int index : { kMainOut, kBus1Out, kBus2Out, kBus3Out, kDirectOut }) {
      const unsigned bit = 1u << index;
      if (written & bit)
        utils::zeroBuffer(output(index)->buffer, num_samples);
      else if (written_outputs_ & bit)
        utils::zeroBuffer(output(index)->buffer, output(index)->buffer_size);
    }
    written_outputs_ = written;

    auto route = [this, num_samples, main_output, bus1_output, bus2_output, bus3_output, direct_output](
                     const poly_float* source, int destination, bool envelope_before) {
      poly_float* target = main_output;
      if (destination == constants::kBus1)
        target = bus1_output;
      else if (destination == constants::kBus2)
        target = bus2_output;
      else if (destination == constants::kBus3)
        target = bus3_output;
      else if (destination == constants::kDirectOut)
        target = direct_output;
      for (int i = 0; i < num_samples; ++i)
        target[i] += source[i] * (envelope_before ? poly_float(1) : voiceAmplitude(i));
    };

    // In serial mode only the last filter reaches the output buses. Sending
    // the first stage there as well creates a parallel path around the second.
    if (routing != FilterRouting::kSerialForward)
      route(filter_1_->output()->buffer, filter_1_destination_->value(), envelope_before_[0]);
    if (routing != FilterRouting::kSerialBackward)
      route(filter_2_->output()->buffer, filter_2_destination_->value(), envelope_before_[1]);
  }

  void FiltersModule::process(int num_samples) {
    FilterRouting routing = filterRouting(filter_1_->getOnValue()->value() > 0.5f,
                                          filter_2_->getOnValue()->value() > 0.5f,
                                          filter_1_filter_input_->value() > 0.5f,
                                          filter_2_filter_input_->value() > 0.5f);
    // Excite Reverb with the enveloped source, then let its stored energy decay
    // naturally. Serial chains apply the envelope once before the whole chain;
    // unrelated parallel branches retain their usual output envelope.
    envelope_before_[0] = filter_1_->getOnValue()->value() > .5f &&
        filter_1_->getModelValue()->value() == constants::kReverbFilter;
    envelope_before_[1] = filter_2_->getOnValue()->value() > .5f &&
        filter_2_->getModelValue()->value() == constants::kReverbFilter;
    bool tail_enabled = envelope_before_[0] || envelope_before_[1];
    const bool scream_in_chain =
        (filter_1_->getOnValue()->value() > .5f && filter_1_->getModelValue()->value() == constants::kScreamFilter) ||
        (filter_2_->getOnValue()->value() > .5f && filter_2_->getModelValue()->value() == constants::kScreamFilter);
    // A serial Scream/Reverb chain still follows Env 1 at its output. Otherwise
    // Scream's self-oscillation could keep exciting Reverb after the note ends.
    if (routing != FilterRouting::kParallel && scream_in_chain) tail_enabled = false;
    if (routing != FilterRouting::kParallel)
      envelope_before_[0] = envelope_before_[1] = tail_enabled;
    prepareFilterAmplitude(0, num_samples);
    prepareFilterAmplitude(1, num_samples);
    if (routing == FilterRouting::kSerialBackward)
      processSerialBackward(num_samples);
    else if (routing == FilterRouting::kSerialForward)
      processSerialForward(num_samples);
    else
      processParallel(num_samples);

    const auto* events = input(kVoiceEvent)->source;
    const int kill_samples = std::max(1, int(getSampleRate() * kVoiceKillTime));
    const int hold_samples = getSampleRate() / 2;
    for (int i = 0; i < num_samples; ++i) {
      poly_float alive = voiceAmplitude(i);
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        if (events->trigger_mask[lane] && events->trigger_offset[lane] == i) {
          if (events->trigger_value[lane] == kVoiceOn) {
            killed_[lane] = false;
            tail_hold_[lane] = 0;
          }
          else if (events->trigger_value[lane] == kVoiceKill) {
            killed_[lane] = true;
            kill_remaining_[lane] = kill_samples;
          }
        }
        const float kill_gain = killed_[lane] ? float(kill_remaining_[lane]) / kill_samples : 1.f;
        if (killed_[lane] && kill_remaining_[lane] > 0) --kill_remaining_[lane];
        float peak = 0;
        for (int out = kMainOut; out <= kDirectOut; ++out) {
          const float sample = output(out)->buffer[i][lane];
          peak = std::max(peak, std::abs(sample));
          if (tail_enabled) output(out)->buffer[i].set(lane, sample * kill_gain);
        }
        // Cover silent gaps between long low-cutoff reflections, but retire
        // inaudible tails. Forced voice steals still use the short kill fade.
        if (!tail_enabled || (killed_[lane] && kill_remaining_[lane] == 0)) tail_hold_[lane] = 0;
        else if (peak > 1e-5f || std::abs(alive[lane]) > 1e-5f) tail_hold_[lane] = hold_samples;
        else if (tail_hold_[lane] > 0) --tail_hold_[lane];
        if (tail_hold_[lane] > 0) alive.set(lane, 1);
      }
      output(kVoiceAlive)->buffer[i] = alive;
    }
  }
} // namespace vital
