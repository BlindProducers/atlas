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

namespace vital {

  class Value : public Processor {
    public:
      enum {
        kSet,
        kNumInputs
      };

      Value(poly_float value = 0.0f, bool control_rate = false);

      virtual Processor* clone() const override { return new Value(*this); }
      virtual void process(int num_samples) override;
      virtual void setOversampleAmount(int oversample) override;

      force_inline mono_float value() const { return value_[0]; }
      virtual void set(poly_float value);

    protected:
      Value(poly_float value, DetachedTag tag) : Processor(tag), value_(value) { }

      poly_float value_;

      JUCE_LEAK_DETECTOR(Value)
  };

  // A parameter's value and nothing else: no inputs, outputs or processing,
  // and no allocation. Effect shells keep their parameters as these; the
  // processors of a live effect read them (see PreparedEffectModule). Only
  // value() and set() apply: it never joins a graph.
  class StoredValue : public Value {
    public:
      explicit StoredValue(poly_float value = 0.0f) : Value(value, DetachedTag()) { }

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }
      void process(int num_samples) override { }
      void setSampleRate(int sample_rate) override { }
      void setOversampleAmount(int oversample) override { }
      void enable(bool enable) override { }
      void set(poly_float value) override { value_ = value; }
  };

  namespace cr {
    class Value : public ::vital::Value {
      public:
        Value(poly_float value = 0.0f) : ::vital::Value(value, true) { }
        virtual Processor* clone() const override { return new Value(*this); }

        void process(int num_samples) override;
    };
  } // namespace cr
} // namespace vital

