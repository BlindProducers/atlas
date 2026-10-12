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

#include "common.h"
#include "poly_utils.h"

#include <cstring>
#include <new>
#include <vector>

namespace vital {

  class Processor;
  class ProcessorRouter;

  struct Output {
    // Output storage is left uninitialised: clearBuffer zeroes the samples in
    // use, so capacity reserved for higher quality settings costs no memory
    // until it is used.
    struct BufferDeleter {
      void operator()(poly_float* buffer) const {
        ::operator delete[](buffer, std::align_val_t(alignof(poly_float)));
      }
    };
    using BufferPointer = std::unique_ptr<poly_float[], BufferDeleter>;
    static BufferPointer allocateBuffer(int size) {
      return BufferPointer(static_cast<poly_float*>(
          ::operator new[](size_t(size) * sizeof(poly_float), std::align_val_t(alignof(poly_float)))));
    }

    Output(int size = kMaxBufferSize, int max_oversample = 1) {
      VITAL_ASSERT(size > 0);

      owner = nullptr;
      buffer_size = size * max_oversample;
      buffer_capacity = buffer_size;
      owned_buffer = allocateBuffer(buffer_size);
      buffer = owned_buffer.get();
      clearBuffer();
      clearTrigger();
    }

    virtual ~Output() { }

    force_inline void trigger(poly_mask mask, poly_float value, poly_int offset) {
      trigger_mask |= mask;
      trigger_value = utils::maskLoad(trigger_value, value, mask);
      trigger_offset = utils::maskLoad(trigger_offset, offset, mask);
    }

    force_inline void clearTrigger() {
      trigger_mask = 0;
      trigger_value = 0.0f;
      trigger_offset = 0;
    }

    void clearBuffer() {
      utils::zeroBuffer(owned_buffer.get(), buffer_size);
    }

    force_inline bool isControlRate() const { return buffer_size == 1; }

    void ensureBufferSize(int new_max_buffer_size) {
      if (buffer_size >= new_max_buffer_size || buffer_size == 1)
        return;

      buffer_size = new_max_buffer_size;
      if (buffer_capacity < buffer_size) {
        // Reserve every supported quality level on the first expansion during
        // graph preparation. Later quality changes reuse this storage without
        // increasing the logical length processed by control values.
        buffer_capacity = std::max(buffer_size, kMaxBufferSize * 8);
        bool buffer_is_original = buffer == owned_buffer.get();
        owned_buffer = allocateBuffer(buffer_capacity);
        if (buffer_is_original)
          buffer = owned_buffer.get();
      }
      clearBuffer();
    }

    poly_float* buffer;
    BufferPointer owned_buffer;
    Processor* owner;

    int buffer_size;
    int buffer_capacity;
    poly_mask trigger_mask;
    poly_float trigger_value;
    poly_int trigger_offset;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Output)
  };

  struct Input {
    Input() : source(nullptr) { }

    const Output* source;

    force_inline poly_float at(int i) const { return source->buffer[i]; }
    force_inline const poly_float& operator[](std::size_t i) {
      return source->buffer[i];
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Input)
  };

  struct ProcessorState {
    ProcessorState() {
      sample_rate = kDefaultSampleRate;
      oversample_amount = 1;
      control_rate = false;
      enabled = true;
      initialized = false;
    }

    int sample_rate;
    int oversample_amount;
    bool control_rate;
    bool enabled;
    bool initialized;
  };

  namespace cr {
    struct Output : public ::vital::Output {
      Output() : ::vital::Output(1) {
        buffer = &trigger_value;
        clearBuffer();
        clearTrigger();
      }
    };
  } // namespace cr

  class Processor {
    public:
      Processor(int num_inputs, int num_outputs, bool control_rate = false, int max_oversample = 1);

      virtual ~Processor() { }

      // Clone for polyphony.
      virtual Processor* clone() const = 0;

      // Does the processor require any data per voice.
      virtual bool hasState() const { return true; }

      // Override this for main processing code.
      virtual void process(int num_samples) = 0;
      virtual void processWithInput(const poly_float* audio_in, int num_samples) { VITAL_ASSERT(false); }

      // Override this for plugging in inputs and outputs.
      virtual void init() {
        VITAL_ASSERT(!initialized());
        state_->initialized = true;
      }

      // Override this to handle state resetting when a new note is played.
      virtual void reset(poly_mask reset_mask) { }

      // Override this to handle state resetting when the Processor is turned off/on.
      virtual void hardReset() { reset(poly_mask(-1)); }

      bool initialized() { return state_->initialized; }

      // Subclasses should override this if they need to adjust for change in
      // sample rate.
      virtual void setSampleRate(int sample_rate) {
        state_->sample_rate = sample_rate * state_->oversample_amount;
      }

      virtual void setOversampleAmount(int oversample) {
        state_->sample_rate /= state_->oversample_amount;
        state_->oversample_amount = oversample;
        state_->sample_rate *= state_->oversample_amount;

        for (int i = 0; i < numOwnedOutputs(); ++i)
          ownedOutput(i)->ensureBufferSize(kMaxBufferSize * oversample);
        for (int i = 0; i < numOutputs(); ++i)
          output(i)->ensureBufferSize(kMaxBufferSize * oversample);
      }

      force_inline bool enabled() const {
        return state_->enabled;
      }

      // Clones share their original's state, so this tells a live clone apart
      // from a stale one whose original was freed and its address reused.
      force_inline bool sharesStateWith(const Processor* other) const {
        return other != nullptr && state_ == other->state_;
      }

      virtual void enable(bool enable) {
        state_->enabled = enable;
      }

      force_inline int getSampleRate() const {
        return state_->sample_rate;
      }

      force_inline int getOversampleAmount() const {
        return state_->oversample_amount;
      }

      force_inline bool isControlRate() const {
        return state_->control_rate;
      }

      virtual void setControlRate(bool control_rate) {
        state_->control_rate = control_rate;
      }

      force_inline poly_mask getResetMask(int input_index) const {
        poly_float trigger_value = inputs_->at(input_index)->source->trigger_value;
        return poly_float::equal(trigger_value, kVoiceOn);
      }

      force_inline void clearOutputBufferForReset(poly_mask reset_mask, int input_index, int output_index) const {
        poly_float* audio_out = output(output_index)->buffer;
        poly_int trigger_offset = input(input_index)->source->trigger_offset & reset_mask;
        int num_samples_first = trigger_offset[0];
        poly_int mask(0, 0, -1, -1);
        for (int i = 0; i < num_samples_first; ++i)
          audio_out[i] = audio_out[i] & mask;

        mask = poly_int(-1, -1, 0, 0);
        int num_samples_second = trigger_offset[2];
        for (int i = 0; i < num_samples_second; ++i)
          audio_out[i] = audio_out[i] & mask;
      }

      bool inputMatchesBufferSize(int input = 0);

      // Returns true if non control-rate inputs and outputs are big enough for sample block.
      bool checkInputAndOutputSize(int num_samples);

      virtual bool isPolyphonic() const;

      // Attaches an output to an input in this processor.
      void plug(const Output* source);
      void plug(const Output* source, unsigned int input_index);
      void plug(const Processor* source);
      void plug(const Processor* source, unsigned int input_index);

      // Attaches an output to the first available input in this processor.
      void plugNext(const Output* source);
      void plugNext(const Processor* source);

      // Use an existing input as our input.
      void useInput(Input* input);
      void useInput(Input* input, int index);

      // Use an existing output as our output.
      void useOutput(Output* output);
      void useOutput(Output* output, int index);

      // Count how many inputs are connected to processors
      int connectedInputs();

      // Remove a connection between two processors.
      virtual void unplugIndex(unsigned int input_index);
      virtual void unplug(const Output* source);
      virtual void unplug(const Processor* source);

      virtual void numInputsChanged() { }

      // Sets the ProcessorRouter that will own this Processor.
      force_inline void router(ProcessorRouter* router) { router_ = router; VITAL_ASSERT((Processor*)router != this); }

      // Returns the ProcessorRouter that owns this Processor.
      force_inline ProcessorRouter* router() const { return router_; }

      // Returns the ProcessorRouter that owns this Processor.
      ProcessorRouter* getTopLevelRouter() const;

      virtual void registerInput(Input* input, int index);
      virtual Output* registerOutput(Output* output, int index);
      virtual void registerInput(Input* input);
      virtual Output* registerOutput(Output* output);

      force_inline int numInputs() const { return static_cast<int>(inputs_->size()); }
      force_inline int numOutputs() const { return static_cast<int>(outputs_->size()); }
      force_inline int numOwnedInputs() const { return static_cast<int>(owned_inputs_.size()); }
      force_inline int numOwnedOutputs() const { return static_cast<int>(owned_outputs_.size()); }

      force_inline Input* input(unsigned int index = 0) const {
        VITAL_ASSERT(index < inputs_->size());

        return inputs_->operator[](index);
      }

      force_inline bool isInputSourcePolyphonic(int index = 0) {
        return input(index)->source->owner && input(index)->source->owner->isPolyphonic();
      }

      force_inline Input* ownedInput(unsigned int index = 0) const {
        VITAL_ASSERT(index < owned_inputs_.size());

        return owned_inputs_[index].get();
      }

      force_inline Output* output(unsigned int index = 0) const {
        VITAL_ASSERT(index < outputs_->size());

        return outputs_->operator[](index);
      }

      force_inline Output* ownedOutput(unsigned int index = 0) const {
        VITAL_ASSERT(index < owned_outputs_.size());

        return owned_outputs_[index].get();
      }

      void setPluggingStart(int start) { plugging_start_ = start; }

    protected:
      Output* addOutput(int oversample = 1);
      Input* addInput();

      std::shared_ptr<ProcessorState> state_;

      int plugging_start_;
      std::vector<std::shared_ptr<Input>> owned_inputs_;
      std::vector<std::shared_ptr<Output>> owned_outputs_;

      std::shared_ptr<std::vector<Input*>> inputs_;
      std::shared_ptr<std::vector<Output*>> outputs_;

      ProcessorRouter* router_;

      static const Output null_source_;

      JUCE_LEAK_DETECTOR(Processor)
  };
} // namespace vital
