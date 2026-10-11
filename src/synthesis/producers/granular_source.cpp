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

#include "granular_source.h"

#include "futils.h"
#include "one_pole_filter.h"
#include "synth_constants.h"

namespace vital {
  namespace {
    constexpr int kGranularTransposeQuantizeCustomScale = 14;
    constexpr int kGranularTransposeQuantizeScaleMasks[] = {
      0,
      (1 << 12) - 1,
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),
      -1,
    };

    int rotateGranularTransposeQuantizeMask(int mask, int key) {
      int result = 0;
      key = std::max(0, std::min(kNotesPerOctave - 1, key));
      for (int i = 0; i < kNotesPerOctave; ++i) {
        if ((mask >> i) & 1)
          result |= 1 << ((i + key) % kNotesPerOctave);
      }
      return result;
    }

    // Steps in beats for 1/1 .. 1/128.
    constexpr mono_float kGridStepBeats[GranularSource::kNumGridRates] = {
      4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f
    };

    float windowShapeValue(int shape, float t) {
      t = std::max(0.0f, std::min(1.0f, t));
      switch (shape) {
        case GranularSource::kSoftPlateau: {
          // Tukey window: cosine edges over the outer 25% each side, flat
          // middle. Longer grains keep more of the source clearly audible.
          constexpr float kEdge = 0.25f;
          if (t < kEdge)
            return 0.5f - 0.5f * std::cos(kPi * t / kEdge);
          if (t > 1.0f - kEdge)
            return 0.5f - 0.5f * std::cos(kPi * (1.0f - t) / kEdge);
          return 1.0f;
        }
        case GranularSource::kTriangle:
          return 1.0f - std::abs(2.0f * t - 1.0f);
        case GranularSource::kGaussian: {
          // Scaled so both ends reach exactly zero.
          constexpr float kWidth = 0.16f;
          const float edge = std::exp(-0.5f * (0.5f / kWidth) * (0.5f / kWidth));
          const float x = (t - 0.5f) / kWidth;
          return std::max(0.0f, (std::exp(-0.5f * x * x) - edge) / (1.0f - edge));
        }
        case GranularSource::kPercussive:
        case GranularSource::kReversePercussive: {
          if (shape == GranularSource::kReversePercussive)
            t = 1.0f - t;
          constexpr float kAttack = 0.04f;
          if (t < kAttack)
            return 0.5f - 0.5f * std::cos(kPi * t / kAttack);
          const float decay = 1.0f - (t - kAttack) / (1.0f - kAttack);
          return decay * decay * decay;
        }
        case GranularSource::kHann:
        default:
          return 0.5f - 0.5f * std::cos(2.0f * kPi * t);
      }
    }
  }

  const mono_float* GranularSource::windowTable(int shape) {
    using Table = std::array<mono_float, kWindowResolution + 2>;
    static const std::array<Table, kNumWindowShapes> tables = [] {
      std::array<Table, kNumWindowShapes> result {};
      for (int shape = 0; shape < kNumWindowShapes; ++shape) {
        for (int i = 0; i <= kWindowResolution; ++i)
          result[shape][i] = windowShapeValue(shape, static_cast<float>(i) / kWindowResolution);
        result[shape][kWindowResolution + 1] = result[shape][kWindowResolution];
      }
      return result;
    }();
    return tables[utils::iclamp(shape, 0, kNumWindowShapes - 1)].data();
  }

  mono_float GranularSource::gridStepBeats(int rate_index, int timing_mode) {
    mono_float beats = kGridStepBeats[utils::iclamp(rate_index, 0, kNumGridRates - 1)];
    if (timing_mode == kGridDottedTiming)
      beats *= 1.5f;
    else if (timing_mode == kGridTripletTiming)
      beats *= 2.0f / 3.0f;
    return beats;
  }

  void GranularSource::correctToTime(double seconds) {
    *sync_seconds_ = futils::isFinite(seconds) ? seconds : 0.0;
  }

  void GranularSource::LaneState::reset() {
    playhead = 0.0;
    grain_phase = 1.0;
    modulation_phase = 0.0;
    active_count = 0;
    pending = false;
    pending_delay = 0;
    normalization = 1.0f;
    grid_started = false;
    last_grid_step = 0.0;
    for (auto& grain : grains)
      grain.active = false;
  }

  GranularSource::GranularSource() : GranularSource(nullptr) { }

  GranularSource::GranularSource(std::shared_ptr<Sample> sample) :
      Processor(kNumInputs, kNumOutputs), pan_amplitude_(0.0f), transpose_quantize_(0),
      last_quantized_transpose_(0.0f), low_cut_state_(0.0f), high_cut_state_(0.0f), phase_(0.0f),
      phase_output_(std::make_shared<cr::Output>()), random_generator_(0.0f, 1.0f),
      sample_(sample ? std::move(sample) : std::make_shared<Sample>()),
      sync_seconds_(std::make_shared<double>(0.0)) { }

  poly_float GranularSource::onePoleCoefficient(poly_float midi_cutoff, int sample_rate) {
    poly_float frequency = utils::midiNoteToFrequency(midi_cutoff);
    frequency = utils::clamp(frequency, 5.0f, sample_rate * 0.49f);
    return OnePoleFilter<>::computeCoefficient(frequency, sample_rate);
  }

  double GranularSource::wrapPosition(double value, double length) {
    if (length <= 0.0f)
      return 0.0f;

    if (value >= 0.0 && value < length)
      return value;
    value = std::fmod(value, length);
    return value < 0.0f ? value + length : value;
  }

  mono_float GranularSource::inputValueAt(int input_index, int frame, int lane) const {
    const auto* source = input(input_index)->source;
    const int index = source->buffer_size > frame ? frame : 0;
    return source->buffer[index][lane];
  }

  void GranularSource::resetLane(int lane, mono_float start) {
    lanes_[lane].reset();
    mono_float manual_position = input(kPosition)->at(0)[lane];
    bool manual = input(kMode)->at(0)[0] >= 0.5f;
    lanes_[lane].playhead = manual ? manual_position : 0.0;
    phase_.set(lane, lanes_[lane].playhead);
  }

  void GranularSource::prepareGrainReader(int lane, Grain& grain) const {
    int active_length = sample_->activeLength();
    grain.read_buffer = nullptr;
    if (active_length <= Sample::kMinSize)
      return;

    const int active_index = sample_->getActiveIndex(std::max<mono_float>(std::abs(grain.source_increment), 1.0f));
    grain.read_length = std::max(Sample::kMinSize, active_length >> active_index);
    grain.read_scale = 1 << active_index;
    grain.read_buffer = ((lane % 2) ? sample_->getActiveRightLoopBuffer(active_index) :
                                     sample_->getActiveLeftLoopBuffer(active_index)) + Sample::kBufferSamples;
  }

  mono_float GranularSource::readSampleAt(const Grain& grain) const {
    if (grain.read_buffer == nullptr)
      return 0.0f;
    const int buffer_length = grain.read_length;
    double scaled_position = wrapPosition(grain.source_position / grain.read_scale, buffer_length);
    int first = static_cast<int>(scaled_position);
    int previous = first == 0 ? buffer_length - 1 : first - 1;
    int second = first + 1 == buffer_length ? 0 : first + 1;
    int third = second + 1 == buffer_length ? 0 : second + 1;
    mono_float blend = static_cast<mono_float>(scaled_position - first);

    const mono_float* buffer = grain.read_buffer;
    const mono_float y0 = buffer[previous];
    const mono_float y1 = buffer[first];
    const mono_float y2 = buffer[second];
    const mono_float y3 = buffer[third];
    const mono_float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const mono_float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const mono_float c = -0.5f * y0 + 0.5f * y2;
    return ((a * blend + b) * blend + c) * blend + y1;
  }

  GranularSource::GrainRandomValues GranularSource::nextGrainRandomValues() {
    GrainRandomValues values;
    values.position = random_generator_.next() * 2.0f - 1.0f;
    values.pitch = random_generator_.next() * 2.0f - 1.0f;
    values.volume = random_generator_.next();
    values.pan = random_generator_.next() * 2.0f - 1.0f;
    values.interval_roll = random_generator_.next();
    values.direction_roll = random_generator_.next();
    values.timing = random_generator_.next();
    return values;
  }

  void GranularSource::spawnGrain(int lane, int frame, mono_float start, mono_float end,
                                  mono_float normalized_position, mono_float pitch_ratio,
                                  const GrainRandomValues& random) {
    LaneState& state = lanes_[lane];
    int maximum_active = utils::iclamp(static_cast<int>(input(kGrainCount)->at(0)[0] + 0.5f), 1, kMaxGrains);
    if (state.active_count >= maximum_active)
      return;

    int slot = 0;
    while (slot < state.active_count && state.active_grains[slot] == slot)
      ++slot;
    for (int i = state.active_count; i > slot; --i)
      state.active_grains[i] = state.active_grains[i - 1];
    state.active_grains[slot] = slot;
    ++state.active_count;
    Grain* target = &state.grains[slot];

    const mono_float region = std::max<mono_float>(Sample::kMinSize, end - start);
    mono_float normalized = normalized_position;
    const mono_float position_depth = utils::clamp(inputValueAt(kRandomPosition, frame, lane), 0.0f, 1.0f);
    normalized += random.position * position_depth * 0.5f;
    normalized = wrapPosition(normalized, 1.0f);
    const int position_grid = utils::iclamp(static_cast<int>(input(kPositionGrid)->at(0)[0] + 0.5f),
                                            0, kNumPositionGrids - 1);
    if (position_grid > 0) {
      // Start grains on equal slices of the region (2 .. 128 slices).
      const mono_float slices = static_cast<mono_float>(1 << position_grid);
      normalized = std::min(std::floor(normalized * slices + 1.0e-4f), slices - 1.0f) / slices;
    }

    mono_float semitones = random.pitch * inputValueAt(kRandomPitch, frame, lane);
    const mono_float interval_chance = utils::clamp(inputValueAt(kIntervalChance, frame, lane), 0.0f, 1.0f);
    if (random.interval_roll < interval_chance)
      semitones += inputValueAt(kInterval, frame, lane);

    int direction = utils::iclamp(static_cast<int>(input(kDirection)->at(0)[0] + 0.5f), 0, 2);
    bool reverse = direction == 1 || (direction == 2 && random.direction_roll >= 0.5f);
    mono_float grain_pitch = pitch_ratio * utils::centsToRatio(semitones * kCentsPerNote);
    mono_float source_increment = grain_pitch * sample_->activeSampleRate() / getSampleRate() *
                                  (1 << Sample::kUpsampleTimes) * (reverse ? -1.0 : 1.0);
    mono_float grain_size_ms = utils::clamp(inputValueAt(kGrainSize, frame, lane), 1.0f, 2000.0f);
    if (static_cast<int>(input(kSizeMode)->at(0)[0] + 0.5f) == kGridStepSize) {
      // Grain length as a multiple of the grid step at the current tempo.
      const int timing_mode = utils::iclamp(static_cast<int>(input(kTimingMode)->at(0)[0] + 0.5f),
                                            0, kNumTimingModes - 1);
      const int rate = static_cast<int>(input(kGridRate)->at(0)[0] + 0.5f);
      const mono_float bps = std::max(0.1f, input(kBeatsPerSecond)->at(0)[0]);
      const mono_float step_ms = 1000.0f * gridStepBeats(rate, timing_mode) / bps;
      const mono_float multiple = utils::clamp(inputValueAt(kGridLength, frame, lane), 0.05f, 4.0f);
      grain_size_ms = utils::clamp(step_ms * multiple, 1.0f, 4000.0f);
    }
    int grain_length = std::max(1, static_cast<int>(grain_size_ms * 0.001f * getSampleRate()));

    const mono_float volume_depth = utils::clamp(inputValueAt(kRandomVolume, frame, lane), 0.0f, 1.0f);
    const mono_float volume_gain = std::exp2(-4.0f * random.volume * volume_depth);
    const mono_float pan_depth = utils::clamp(inputValueAt(kRandomPan, frame, lane), 0.0f, 1.0f);
    const mono_float random_pan = utils::clamp(random.pan * pan_depth, -1.0f, 1.0f);
    const mono_float pan_gain = (lane % 2) ? std::sqrt(std::max(0.0f, 1.0f + random_pan)) :
                                            std::sqrt(std::max(0.0f, 1.0f - random_pan));

    // Keep the whole grain inside the selected region when it fits, so it
    // doesn't run past the region end (or wrap around the sample) mid-grain.
    // Longer grains still continue through the sample as before.
    double position = start + normalized * region;
    const double span = std::abs(static_cast<double>(source_increment)) * grain_length;
    if (span < region) {
      if (source_increment >= 0.0f)
        position = std::min(position, static_cast<double>(end) - span);
      else
        position = std::max(position, static_cast<double>(start) + span);
    }

    target->active = true;
    target->source_position = position;
    target->source_increment = source_increment;
    target->age = 0;
    target->length = grain_length;
    target->gain = volume_gain * pan_gain;
    target->window_position = 0.0f;
    target->window_step = static_cast<mono_float>(kWindowResolution) / std::max(1, grain_length - 1);
    target->window_shape = utils::iclamp(static_cast<int>(input(kWindowShape)->at(0)[0] + 0.5f),
                                         0, kNumWindowShapes - 1);
    prepareGrainReader(lane, *target);
  }

  void GranularSource::process(int num_samples) {
    if (num_samples <= 0)
      return;

    sample_->markUsed();
    // Pin/reload the current sample before caching its pointers. Samples can
    // be replaced between blocks while existing grains are still playing.
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      auto& state = lanes_[lane];
      for (int i = 0; i < state.active_count; ++i)
        prepareGrainReader(lane, state.grains[state.active_grains[i]]);
    }

    poly_float current_pan_amplitude = pan_amplitude_;
    poly_float input_pan = utils::clamp(input(kPan)->at(0), -1.0f, 1.0f);
    pan_amplitude_ = futils::panAmplitude(input_pan);

    poly_float input_midi = 0.0f;
    if (input(kKeytrack)->at(0)[0])
      input_midi = input(kMidi)->at(0) - input(kRootKey)->at(0);

    int transpose_quantize = static_cast<int>(input(kTransposeQuantize)->at(0)[0]);
    int quantize_scale = static_cast<int>(input(kTransposeQuantizeScale)->at(0)[0]);
    if (quantize_scale != kGranularTransposeQuantizeCustomScale) {
      quantize_scale = std::max(0, std::min(kGranularTransposeQuantizeCustomScale - 1, quantize_scale));
      int key = static_cast<int>(input(kTransposeQuantizeKey)->at(0)[0]);
      int mode = static_cast<int>(input(kTransposeQuantizeMode)->at(0)[0]);
      transpose_quantize = rotateGranularTransposeQuantizeMask(kGranularTransposeQuantizeScaleMasks[quantize_scale],
                                                               key);
      if (mode >= 1)
        transpose_quantize |= 1 << kNotesPerOctave;
    }

    poly_float transpose = snapTranspose(input_midi, input(kTranspose)->at(0), transpose_quantize);
    transpose = utils::clamp(transpose + input(kTune)->at(0), kMinTranspose, kMaxTranspose);
    poly_float pitch_ratio = utils::centsToRatio(transpose * kCentsPerNote);

    int audio_length = sample_->activeLength();
    mono_float audio_end = audio_length;
    mono_float minimum_region = Sample::kMinSize * (1 << Sample::kUpsampleTimes);
    poly_float sample_start = utils::clamp(input(kStart)->at(0), 0.0f, 1.0f) * audio_end;
    poly_float sample_end = utils::clamp(input(kEnd)->at(0), 0.0f, 1.0f) * audio_end;
    sample_start = utils::floor(utils::clamp(sample_start, 0.0f, audio_end - minimum_region));
    sample_end = utils::floor(utils::clamp(sample_end, sample_start + minimum_region, audio_end));

    poly_mask reset_mask = getResetMask(kReset);
    current_pan_amplitude = utils::maskLoad(current_pan_amplitude, pan_amplitude_, reset_mask);
    low_cut_state_ = utils::maskLoad(low_cut_state_, 0.0f, reset_mask);
    high_cut_state_ = utils::maskLoad(high_cut_state_, 0.0f, reset_mask);
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (reset_mask[lane])
        resetLane(lane, sample_start[lane]);
    }

    mono_float sample_inc = 1.0f / num_samples;
    poly_float delta_pan_amplitude = (pan_amplitude_ - current_pan_amplitude) * sample_inc;

    poly_float* raw_output = output(kRaw)->buffer;
    utils::zeroBuffer(raw_output, num_samples);

    const bool manual = input(kMode)->at(0)[0] >= 0.5f;
    const bool midi_density = input(kMidiDensity)->at(0)[0] >= 0.5f;
    std::array<mono_float, poly_float::kSize> note_densities {};
    if (midi_density)
      for (int lane = 0; lane < poly_float::kSize; ++lane)
        note_densities[lane] = utils::midiNoteToFrequency(input(kMidi)->at(0)[lane]);

    const mono_float sample_rate = static_cast<mono_float>(getSampleRate());
    const int timing_mode = utils::iclamp(static_cast<int>(input(kTimingMode)->at(0)[0] + 0.5f),
                                          0, kNumTimingModes - 1);
    const bool grid_timing = timing_mode != kFreeTiming;
    const mono_float beats_per_second = std::max(0.1f, input(kBeatsPerSecond)->at(0)[0]);
    const double grid_step_beats = gridStepBeats(static_cast<int>(input(kGridRate)->at(0)[0] + 0.5f), timing_mode);
    const double grid_step_samples = grid_step_beats / beats_per_second * sample_rate;
    const double block_start_beats = *sync_seconds_ * beats_per_second;
    const double beats_per_frame = beats_per_second / static_cast<double>(sample_rate);
    // Normalization follows overlap quickly when it rises and slowly when it
    // falls, so it never adds audible amplitude modulation inside a grain.
    const mono_float normalization_attack = 1.0f - std::exp(-1.0f / (0.002f * sample_rate));
    const mono_float normalization_release = 1.0f - std::exp(-1.0f / (0.030f * sample_rate));
    const SlotModulation::Block modulation = modulation_.resolve(*this, kDistortionType, kDistortionAmount,
                                                                 kSecondaryDistortionType,
                                                                 kSecondaryDistortionAmount, true);
    const bool fm_active = modulation.anyFm();

    for (int frame = 0; frame < num_samples; ++frame) {
      poly_float frame_output = 0.0f;
      const poly_float rate_scale = fm_active ? modulation.rateScale(frame) : poly_float(1.0f);
      std::array<mono_float, poly_float::kSize> target_positions { };
      std::array<bool, poly_float::kSize> triggers { };
      std::array<mono_float, poly_float::kSize> interval_samples { };
      // The small offset keeps exact grid lines (e.g. triplets) from rounding
      // down into the previous step.
      const double grid_step = grid_timing ? std::floor((block_start_beats + frame * beats_per_frame) /
                                                        grid_step_beats + 1.0e-9) : 0.0;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        LaneState& state = lanes_[lane];
        mono_float density = midi_density ? note_densities[lane] :
                             inputValueAt(kDensity, frame, lane);
        density = utils::clamp(density, 1.0f, sample_rate);
        mono_float lfo = std::sin(state.modulation_phase * kPi * 2.0);
        mono_float target_position = manual ? inputValueAt(kPosition, frame, lane) :
                                             static_cast<mono_float>(state.playhead);
        const mono_float position_mod = utils::clamp(inputValueAt(kPositionMod, frame, lane), 0.0f, 1.0f);
        target_position += lfo * position_mod * 0.5f;
        target_position = wrapPosition(target_position, 1.0f);
        target_positions[static_cast<size_t>(lane)] = target_position;

        if (grid_timing) {
          // A note starts with a grain, then grains follow the host's grid.
          interval_samples[static_cast<size_t>(lane)] = static_cast<mono_float>(grid_step_samples);
          if (state.grain_phase >= 1.0) {
            triggers[static_cast<size_t>(lane)] = true;
            state.grain_phase = 0.0;
          }
          else if (state.grid_started && grid_step != state.last_grid_step)
            triggers[static_cast<size_t>(lane)] = true;
          state.grid_started = true;
          state.last_grid_step = grid_step;
        }
        else {
          interval_samples[static_cast<size_t>(lane)] = sample_rate / density;
          if (state.grain_phase >= 1.0) {
            triggers[static_cast<size_t>(lane)] = true;
            state.grain_phase -= std::floor(state.grain_phase);
          }
          state.grain_phase += density / sample_rate;
        }

        state.modulation_phase += inputValueAt(kPositionModRate, frame, lane) / sample_rate;
        state.modulation_phase -= std::floor(state.modulation_phase);

        if (!manual) {
          const mono_float region = std::max<mono_float>(Sample::kMinSize,
                                                        sample_end[lane] - sample_start[lane]);
          state.playhead += inputValueAt(kSpeed, frame, lane) * sample_->activeSampleRate() /
                            sample_rate * (1 << Sample::kUpsampleTimes) / region;
          state.playhead -= std::floor(state.playhead);
        }
      }

      const auto spawn = [&](int lane, const GrainRandomValues& random) {
        spawnGrain(lane, frame, sample_start[lane], sample_end[lane],
                   target_positions[static_cast<size_t>(lane)], pitch_ratio[lane], random);
      };
      // Timing jitter delays each triggered grain by up to half its interval.
      // Stereo pairs share random values so left and right stay together.
      const auto schedule = [&](int lane, const GrainRandomValues& random) {
        LaneState& state = lanes_[lane];
        if (state.pending) {
          state.pending = false;
          spawn(lane, state.pending_random);
        }
        const mono_float jitter = utils::clamp(inputValueAt(kTimingJitter, frame, lane), 0.0f, 1.0f);
        const int delay = static_cast<int>(random.timing * jitter * 0.5f *
                                           interval_samples[static_cast<size_t>(lane)]);
        if (delay <= 0)
          spawn(lane, random);
        else {
          state.pending = true;
          state.pending_delay = delay;
          state.pending_random = random;
        }
      };

      for (int lane = 0; lane < poly_float::kSize; lane += 2) {
        const bool trigger_left = triggers[static_cast<size_t>(lane)];
        const bool trigger_right = triggers[static_cast<size_t>(lane + 1)];
        if (trigger_left && trigger_right) {
          const GrainRandomValues random = nextGrainRandomValues();
          schedule(lane, random);
          schedule(lane + 1, random);
        }
        else {
          if (trigger_left)
            schedule(lane, nextGrainRandomValues());
          if (trigger_right)
            schedule(lane + 1, nextGrainRandomValues());
        }
      }

      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        LaneState& state = lanes_[lane];
        if (state.pending && --state.pending_delay <= 0) {
          state.pending = false;
          spawn(lane, state.pending_random);
        }

        mono_float lane_output = 0.0f;
        mono_float window_power = 0.0f;
        int surviving_grains = 0;
        for (int active = 0; active < state.active_count; ++active) {
          const int slot = state.active_grains[active];
          Grain& grain = state.grains[slot];

          const mono_float window = windowValue(grain);
          mono_float sample = readSampleAt(grain);
          lane_output += sample * window * grain.gain;
          window_power += window * window;

          grain.window_position += grain.window_step;
          grain.source_position = wrapPosition(grain.source_position + grain.source_increment * rate_scale[lane],
                                               audio_length);
          if (++grain.age >= grain.length)
            grain.active = false;
          else
            state.active_grains[surviving_grains++] = slot;
        }
        state.active_count = surviving_grains;

        const mono_float target_normalization = window_power > 1.0f ? std::sqrt(window_power) : 1.0f;
        const mono_float coefficient = target_normalization > state.normalization ?
                                       normalization_attack : normalization_release;
        state.normalization += (target_normalization - state.normalization) * coefficient;
        lane_output /= std::max(1.0f, state.normalization);

        frame_output.set(lane, lane_output);
        phase_.set(lane, utils::clamp(target_positions[static_cast<size_t>(lane)], 0.0f, 1.0f));
      }
      raw_output[frame] = frame_output;
      VITAL_ASSERT(utils::isContained(raw_output[frame]));
    }

    poly_float low_cutoff = utils::clamp(input(kLowCutoff)->at(0), 8.0f, 136.0f);
    poly_float high_cutoff = utils::clamp(input(kHighCutoff)->at(0), 8.0f, 136.0f);
    poly_mask use_low_cut = poly_float::greaterThan(low_cutoff, 8.5f);
    poly_mask use_high_cut = poly_float::lessThan(high_cutoff, 135.5f);
    poly_float low_coefficient = onePoleCoefficient(low_cutoff, getSampleRate());
    poly_float high_coefficient = onePoleCoefficient(high_cutoff, getSampleRate());

    for (int i = 0; i < num_samples; ++i) {
      poly_float value = raw_output[i];
      poly_float low = processLowPass(value, low_cut_state_, low_coefficient);
      value = utils::maskLoad(value, value - low, use_low_cut);
      poly_float high = processLowPass(value, high_cut_state_, high_coefficient);
      raw_output[i] = utils::maskLoad(value, high, use_high_cut);
    }

    const poly_float* level_input = input(kLevel)->source->buffer;
    modulation.applyRingMod(raw_output, num_samples);

    poly_float* levelled_output = output(kLevelled)->buffer;
    for (int i = 0; i < num_samples; ++i) {
      current_pan_amplitude += delta_pan_amplitude;
      poly_float level = utils::clamp(level_input[i], poly_float(0.0f), poly_float(kMaxAmplitude));
      levelled_output[i] = current_pan_amplitude * level * level * raw_output[i];
    }

    phase_output_->buffer[0] = utils::encodePhaseAndVoice(phase_, input(kNoteCount)->at(0));
    sample_->markUnused();
  }

  poly_float GranularSource::snapTranspose(poly_float input_midi, poly_float transpose, int quantize) {
    if (quantize == 0)
      return input_midi + transpose;

    bool global_transpose = utils::isTransposeQuantizeGlobal(quantize);
    poly_float pre_add = 0.0f;
    poly_float post_add = input_midi;
    if (global_transpose) {
      pre_add = input_midi;
      post_add = 0.0f;
    }

    poly_float snapped = utils::snapTranspose(pre_add + transpose, quantize);
    last_quantized_transpose_ = snapped;
    transpose_quantize_ = quantize;
    return post_add + snapped;
  }
} // namespace vital
