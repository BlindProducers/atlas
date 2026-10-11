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

#include "synth_module.h"

namespace vital {

  // Spline-graph waveshaper. The transfer curve math is ported from
  // Wolf Shaper by Patrick Desaulniers (GPL v3+):
  // https://github.com/pdesaulniers/wolf-shaper
  class WaveShaperModule : public SynthModule {
    public:
      enum Shape {
        kShapeCurve,
        kShapeSCurve,
        kShapeStairs,
        kShapeSineWave,
        kShapeHardClip,
        kShapeFold,
        kShapeDoubleFold,
        kNumShapes
      };

      enum WarpType {
        kWarpNone,
        kWarpBendUp,
        kWarpBendDown,
        kWarpBendUpDown,
        kWarpSkewUp,
        kWarpSkewDown,
        kWarpSkewUpDown,
        kNumWarpTypes
      };

      enum CurveType {
        kCurveSingle,
        kCurveDouble,
        kCurveStairs,
        kCurveWave
      };

      WaveShaperModule(std::string prefix = "");

      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void hardReset() override;
      void enable(bool enable) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      static constexpr int kMaxVertices = 8;
      static constexpr int kCrossfadeSamples = 1024;

      struct Vertex {
        float x;
        float y;
        int curve_type;
      };

      void rebuildGraph(int shape);
      static float graphValueAt(const Vertex* vertices, int num_vertices, float x, float tension);
      static float shapeSample(const Vertex* vertices, int num_vertices, float value, float tension,
                               float warp_amount, int warp_type, bool bipolar);

      Value* shape_;
      Value* warp_type_;
      Value* mode_;
      Value* remove_dc_;
      Output* drive_;
      Output* tension_;
      Output* warp_amount_;
      Output* mix_;
      Output* output_gain_;
      std::string prefix_;

      Vertex vertices_[kMaxVertices];
      int num_vertices_;
      int current_shape_;
      int current_warp_type_;
      bool current_bipolar_;
      float current_warp_amount_;

      Vertex old_vertices_[kMaxVertices];
      int old_num_vertices_;
      int old_warp_type_;
      bool old_bipolar_;
      int crossfade_samples_left_;

      poly_float mix_amount_;
      float current_tension_;
      poly_float current_output_gain_;
      poly_float dc_input_previous_;
      poly_float dc_output_previous_;

      JUCE_LEAK_DETECTOR(WaveShaperModule)
  };
} // namespace vital
