#pragma once

#include "operators.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace vital {
  // Align parallel paths before summing them. Storage is reserved at graph
  // construction; only the currently required delay is read/written. Invalid
  // history is represented by a count, avoiding large clears on the audio thread.
  class LatencyCompensatedSum : public VariableAdd {
    public:
      LatencyCompensatedSum(int inputs, int maximum_delay) : VariableAdd(inputs), lines_(inputs) {
        for (auto& line : lines_) {
          line.capacity = maximum_delay;
          if (maximum_delay > 0)
            line.history.reset(new float[static_cast<size_t>(maximum_delay) * poly_float::kSize]);
        }
      }

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

      // Negative latency denotes a disconnected input. Returns the latency at
      // the sum output, in samples at this processor's effective sample rate.
      int setInputLatencies(const int* latencies) {
        int maximum = 0;
        for (size_t i = 0; i < lines_.size(); ++i)
          maximum = std::max(maximum, latencies[i]);
        can_use_plain_sum_ = true;
        for (size_t i = 0; i < lines_.size(); ++i) {
          auto& line = lines_[i];
          const bool connected = latencies[i] >= 0;
          int delay = connected ? maximum - latencies[i] : 0;
          VITAL_ASSERT(delay <= line.capacity);
          // Release builds: never index past the reserved history.
          delay = std::min(delay, line.capacity);
          if (line.delay != delay || line.connected != connected) {
            line.position = line.valid = 0;
            line.delay = delay;
            line.connected = connected;
          }
          can_use_plain_sum_ &= delay == 0 && connected;
        }
        return maximum;
      }

      void hardReset() override {
        for (auto& line : lines_)
          line.position = line.valid = 0;
      }

      void process(int samples) override {
        if (can_use_plain_sum_) {
          VariableAdd::process(samples);
          return;
        }
        auto* destination = output()->buffer;
        utils::zeroBuffer(destination, samples);
        for (size_t i = 0; i < lines_.size(); ++i) {
          auto& line = lines_[i];
          if (!line.connected)
            continue;
          const auto* source = input(static_cast<int>(i))->source->buffer;
          if (line.delay == 0) {
            for (int s = 0; s < samples; ++s)
              destination[s] += source[s];
            continue;
          }
          for (int s = 0; s < samples; ++s) {
            float* history = line.history.get() + static_cast<size_t>(line.position) * poly_float::kSize;
            poly_float delayed = 0.0f;
            if (line.valid == line.delay)
              std::memcpy(&delayed.value, history, sizeof(delayed.value));
            else
              ++line.valid;
            std::memcpy(history, &source[s].value, sizeof(source[s].value));
            destination[s] += delayed;
            if (++line.position == line.delay)
              line.position = 0;
          }
        }
      }

    private:
      struct DelayLine {
        std::unique_ptr<float[]> history;
        int capacity = 0;
        int delay = 0;
        int position = 0;
        int valid = 0;
        bool connected = true;
      };
      std::vector<DelayLine> lines_;
      bool can_use_plain_sum_ = true;
  };
}
