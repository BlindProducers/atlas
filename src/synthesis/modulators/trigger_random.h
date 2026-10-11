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

#include "processor.h"
#include "synth_constants.h"
#include "utils.h"

#include <array>
#include <memory>

namespace vital {

  class TriggerRandom : public Processor {
    public:
      enum {
        kReset,
        kNumInputs
      };

      TriggerRandom();
      virtual ~TriggerRandom() { }

      virtual Processor* clone() const override { return new TriggerRandom(*this); }
      virtual void process(int num_samples) override;

      Output* independentOutput(int connection_index) {
        VITAL_ASSERT(connection_index >= 0 && connection_index < kMaxModulationConnections);
        return output(connection_index + 1);
      }
      void setIndependentOutputActive(int connection_index, bool active) {
        VITAL_ASSERT(connection_index >= 0 && connection_index < kMaxModulationConnections);
        independent_output_active_->at(connection_index) = active;
      }

    private:
      poly_float value_;
      std::array<poly_float, kMaxModulationConnections> independent_values_ {};
      std::shared_ptr<std::array<bool, kMaxModulationConnections>> independent_output_active_;
      utils::RandomGenerator random_generator_;
      utils::RandomGenerator independent_random_generator_;

      JUCE_LEAK_DETECTOR(TriggerRandom)
  };
} // namespace vital
