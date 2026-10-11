// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the user's Opus processDrive/processTape/processNoise algorithms.
#include "tape_module.h"
#include "analogue/tape_saturation.h"
#include <algorithm>
#include <cmath>

namespace vital {
namespace {
constexpr double tapePi = 3.14159265358979323846;
double tapeGain(double db) { return std::pow(10.0, db / 20.0); }
int loopFade(const TapeNoiseLibrary::Clip& clip) {
  return std::clamp(static_cast<int>(std::round(clip.sample_rate * .008)), 1, std::max(1, clip.audio.getNumSamples() / 8));
}
}
TapeModule::TapeModule(std::string prefix, std::shared_ptr<TapeNoiseLibrary> library) :
    SynthModule(0, 1), prefix_(std::move(prefix)), library_(library ? std::move(library) : TapeNoiseLibrary::shared()) {}
void TapeModule::init() {
  for (int i = 0; i < tape::kNumControls; ++i) {
    const auto& spec = tape::controls[i];
    const auto name = prefix_ + "tape_" + spec.suffix;
    controls_[i] = spec.indexed ? createBaseControl(name)->output() : createMonoModControl(name);
  }
  prepareDelay();
  SynthModule::init();
}
void TapeModule::prepareDelay() {
  const int rate = std::max(1, getSampleRate());
  if (rate == prepared_rate_) return;
  // At 100% wow and 0.1 Hz the Opus delay excursion is under 90 ms.
  delay_.assign(std::max(64, static_cast<int>(std::ceil(rate * .1)) + 32), poly_float(0));
  prepared_rate_ = rate;
  for (int i = 0; i < tape::kNumControls; ++i)
    smoothing_[i] = static_cast<float>(-std::expm1(-1.0 / (rate * tape::controls[i].smoothing)));
  envelope_step_ = -std::expm1(-1 / (.12 * rate));
  auto_step_ = -std::expm1(-1 / (.04 * rate));
  delay_step_ = -std::expm1(-1 / (.00035 * rate));
  attack_ = -std::expm1(-1 / (.01 * rate));
  release_ = -std::expm1(-1 / (.18 * rate));
  hardReset();
}
void TapeModule::setSampleRate(int rate) { SynthModule::setSampleRate(rate); prepareDelay(); }
void TapeModule::setOversampleAmount(int amount) { SynthModule::setOversampleAmount(amount); prepareDelay(); }
void TapeModule::hardReset() {
  lanes_ = {};
  for (int i = 0; i < poly_float::kSize; ++i) lanes_[i].random += (i / 2) * 0x14628b7u;
  std::fill(delay_.begin(), delay_.end(), poly_float(0));
  for (auto* filter : { &noise_filters_, &global_filters_ }) {
    filter->hp = CutFilter(true); filter->lp = CutFilter(false);
  }
  write_ = tone_counter_ = fade_remaining_ = 0;
  ready_ = noise_was_enabled_ = false;
  clip_ = fading_clip_ = nullptr;
}
void TapeModule::enable(bool on) {
  if (on != enabled()) hardReset();
  SynthModule::enable(on);
}
void TapeModule::configureFilters(Filters& filters, int hp, int lp) {
  for (const auto entry : { std::make_pair(&filters.hp, hp), std::make_pair(&filters.lp, lp) }) {
    poly_float midi;
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      const double hz = std::min<double>(target_[entry.second + 1][lane], getSampleRate() * .45);
      midi.set(lane, static_cast<float>(69 + 12 * std::log2(hz / 440)));
    }
    entry.first->configure(midi, 2 * static_cast<int>(target_[entry.second + 2][0]) + 1, getSampleRate());
  }
}
poly_float TapeModule::filterAndWidth(poly_float audio, Filters& filters, int width, int hp, int lp) {
  const auto swap = utils::swapStereo(audio);
  audio += (audio - swap) * .5f * (current_[width] - 1.0f);
  auto filtered = filters.hp.tick(audio);
  audio += (filtered - audio) * current_[hp];
  filtered = filters.lp.tick(audio);
  return audio + (filtered - audio) * current_[lp];
}
float TapeModule::delaySample(int lane, double delay) const {
  const int size = static_cast<int>(delay_.size());
  double position = write_ - std::clamp(delay, 4.0, double(size - 4));
  if (position < 0) position += size;
  const int i = static_cast<int>(position);
  const double fraction = position - i;
  const auto at = [&](int index) { return double(delay_[(index + size) % size][lane]); };
  const double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
  return static_cast<float>(((-.5*y0 + 1.5*y1 - 1.5*y2 + .5*y3) * fraction +
                            (y0 - 2.5*y1 + 2*y2 - .5*y3)) * fraction * fraction +
                           (-.5*y0 + .5*y2) * fraction + y1);
}
float TapeModule::noiseSample(const TapeNoiseLibrary::Clip& clip, int channel, double position) {
  const int length = clip.audio.getNumSamples(), fade = loopFade(clip);
  const float* data = clip.audio.getReadPointer(std::min(channel, clip.audio.getNumChannels() - 1));
  const auto read = [&](double p) {
    const int i = std::clamp(static_cast<int>(p), 0, length - 1);
    return data[i] + static_cast<float>(p - i) * (data[(i + 1) % length] - data[i]);
  };
  const float value = read(position);
  if (position < length - fade) return value;
  const double start = position - (length - fade);
  const float blend = std::clamp(static_cast<float>(start / fade), 0.0f, 1.0f);
  return value * std::sqrt(1 - blend) + read(start) * std::sqrt(blend);
}
void TapeModule::advanceNoise(const TapeNoiseLibrary::Clip& clip, double& position, double increment) {
  position += increment;
  const int length = clip.audio.getNumSamples(), period = std::max(1, length - loopFade(clip));
  if (position >= length) position = length - period + std::fmod(position - length, period);
}
void TapeModule::processWithInput(const poly_float* input, int count) {
  using namespace tape;
  if (count <= 0) return;
  SynthModule::process(count);
  for (int i = 0; i < kNumControls; ++i) {
    target_[i] = utils::clamp(controls_[i]->buffer[0], controls[i].minimum, controls[i].maximum);
    if (!ready_) current_[i] = target_[i];
  }
  const bool drive_active = poly_float::greaterThan(current_[kDriveEnabled] + target_[kDriveEnabled], poly_float(0)).anyMask();
  const bool motion_active = poly_float::greaterThan(current_[kMotionEnabled] + target_[kMotionEnabled], poly_float(0)).anyMask();
  // The motion delay line is only written while motion is active; clear it on
  // re-activation so the first reads don't replay audio from the last use.
  if (motion_active && !motion_was_active_) {
    std::fill(delay_.begin(), delay_.end(), poly_float(0.0f));
    write_ = 0;
  }
  motion_was_active_ = motion_active;
  const double rate = getSampleRate();
  const bool noise_on = target_[kNoiseEnabled][0] > .5f;
  const auto* next = noise_on ? library_->request(static_cast<int>(target_[kNoiseSource][0])) : nullptr;
  if (next && next != clip_ && fade_remaining_ == 0) {
    if (!clip_) current_[kNoiseEnabled] = 0.0f;
    fading_clip_ = clip_;
    clip_ = next;
    fade_length_ = std::max(1, static_cast<int>(rate * .025));
    fade_remaining_ = fading_clip_ ? fade_length_ : 0;
    for (auto& lane : lanes_) { lane.fading_position = lane.noise_position; lane.noise_position = 0; }
  }
  if (noise_on && !noise_was_enabled_) {
    for (auto& lane : lanes_) lane.noise_position = 0;
    fading_clip_ = nullptr; fade_remaining_ = 0;
  }
  noise_was_enabled_ = noise_on;
  if (!clip_) current_[kNoiseEnabled] = 0.0f;
  const bool global_active =
      poly_float::notEqual(current_[kGlobalWidth], poly_float(1)).anyMask() ||
      poly_float::notEqual(target_[kGlobalWidth], poly_float(1)).anyMask() ||
      poly_float::greaterThan(current_[kGlobalHpEnabled] + target_[kGlobalHpEnabled] +
                             current_[kGlobalLpEnabled] + target_[kGlobalLpEnabled], poly_float(0)).anyMask();
  if (!drive_active && !motion_active && !noise_on && current_[kNoiseEnabled][0] == 0 && !global_active) {
    utils::copyBuffer(output()->buffer, input, count);
    ready_ = true;
    return;
  }
  configureFilters(global_filters_, kGlobalHpEnabled, kGlobalLpEnabled);
  configureFilters(noise_filters_, kNoiseHpEnabled, kNoiseLpEnabled);
  std::array<int, kNumControls> moving_controls;
  int moving_count = 0;
  for (int i = 0; i < kNumControls; ++i)
    if (poly_float::notEqual(current_[i], target_[i]).anyMask())
      moving_controls[moving_count++] = i;
  for (int sample = 0; sample < count; ++sample) {
    for (int moving = 0; moving < moving_count;) {
      const int i = moving_controls[moving];
      current_[i] += (target_[i] - current_[i]) * smoothing_[i];
      const auto close = poly_float::lessThan(poly_float::abs(target_[i] - current_[i]), poly_float(1.0e-6f));
      current_[i] = utils::maskLoad(current_[i], target_[i], close);
      if (!(~close).anyMask())
        moving_controls[moving] = moving_controls[--moving_count];
      else
        ++moving;
    }
    poly_float driven = input[sample];
    if (drive_active) for (int lane = 0; lane < poly_float::kSize; ++lane) {
      auto& state = lanes_[lane];
      const auto value = [&](int i) { return double(current_[i][lane]); };
      if (tone_counter_ == 0 && state.drive_tone_value != value(kDriveTone)) {
        state.drive_tone.c = juce::dsp::IIR::ArrayCoefficients<double>::makeLowPass(rate, std::min(rate * .45, 2500 * std::pow(5.6, value(kDriveTone))), .7071);
        state.drive_tone_value = value(kDriveTone);
      }
      const double dry = input[sample][lane];
      const double drive_gain = state.drive_gain.get(value(kDriveAmount), [](double amount) {
        return atlas::analogue::tape_saturation::driveGain(amount / 10);
      });
      double wet = atlas::analogue::tape_saturation::shape(dry * drive_gain);
      wet = state.drive_tone.tick(wet);
      state.dry_envelope += (dry * dry - state.dry_envelope) * envelope_step_;
      state.wet_envelope += (wet * wet - state.wet_envelope) * envelope_step_;
      if (state.dry_envelope > 1e-8 && state.wet_envelope > 1e-8)
        state.auto_gain += (std::clamp(std::sqrt(state.dry_envelope / state.wet_envelope), .1, 2.0) - state.auto_gain) * auto_step_;
      driven.set(lane, static_cast<float>((dry + value(kDriveMix) * (wet * state.auto_gain - dry)) * state.output_gain.get(value(kDriveOutput), tapeGain)));
    }
    const poly_float signal = input[sample] + (driven - input[sample]) * current_[kDriveEnabled];
    if (motion_active) delay_[write_] = signal;
    poly_float motion = signal;
    if (motion_active) for (int lane = 0; lane < poly_float::kSize; ++lane) {
      auto& s = lanes_[lane];
      const auto v = [&](int i) { return double(current_[i][lane]); };
      const double wow_rate = v(kWowRate), flutter_rate = v(kFlutterRate);
      const double wow = s.wow_depth.get(v(kWow), [](double depth) { return std::exp2(depth * 45 / 1200) - 1; }) * rate / (2 * tapePi * wow_rate);
      const double flutter = s.flutter_depth.get(v(kFlutter), [](double depth) { return std::exp2(depth * 32 / 1200) - 1; }) * rate / (2 * tapePi * flutter_rate);
      const double wow_lfo = .76 * std::sin(s.wow_phase) + .17 * std::sin(2 * s.wow_phase + .8) + .07 * std::sin(3 * s.wow_phase + 2.1);
      const double flutter_lfo = (std::sin(s.flutter_phase) + .22 * std::sin(2 * s.flutter_phase + .4)) / 1.22;
      const double delay = 12 + 1.08 * (wow + flutter) + wow * wow_lfo + flutter * flutter_lfo;
      if (!ready_ && sample == 0) s.delay = delay;
      s.delay += (delay - s.delay) * delay_step_;
      const auto random = [&] {
        s.random ^= s.random << 13; s.random ^= s.random >> 17; s.random ^= s.random << 5;
        return double(s.random & 0xffffffu) / 16777215;
      };
      if (v(kMotionEnabled) < 1e-6 || v(kDropoutDensity) < .001 || v(kDropouts) < 1e-6) {
        s.dropout_remaining = 0; s.dropout_wait = 1;
      }
      else if (--s.dropout_wait <= 0) {
        s.dropout_wait = std::max(1, static_cast<int>((.15 + (1 - v(kDropoutDensity)) * 5 + random() * ((1 - v(kDropoutDensity)) * 7 + .5)) * rate));
        s.dropout_length = std::max(16, static_cast<int>((.012 + random() * .11) * rate));
        s.dropout_remaining = s.dropout_length;
      }
      double gain = 1;
      if (s.dropout_remaining > 0) {
        const double progress = 1 - double(s.dropout_remaining--) / s.dropout_length;
        // A zero depth is truly off (Opus otherwise retained a 15% dip).
        gain -= (.5 - .5 * std::cos(2 * tapePi * progress)) * .95 * v(kDropouts);
      }
      motion.set(lane, static_cast<float>(delaySample(lane, s.delay) * gain));
      s.wow_phase = std::fmod(s.wow_phase + 2 * tapePi * wow_rate / rate, 2 * tapePi);
      s.flutter_phase = std::fmod(s.flutter_phase + 2 * tapePi * flutter_rate / rate, 2 * tapePi);
    }
    if (motion_active) write_ = (write_ + 1) % static_cast<int>(delay_.size());
    const poly_float processed = signal + (motion - signal) * current_[kMotionEnabled];
    poly_float noise = 0, noise_gain = 0;
    if (clip_ && (noise_on || current_[kNoiseEnabled][0] > 0)) {
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        auto& s = lanes_[lane];
        if (tone_counter_ == 0 && s.noise_tone_value != current_[kNoiseTone][lane]) {
          s.noise_tone.c = juce::dsp::IIR::ArrayCoefficients<double>::makeHighShelf(rate, std::min(rate * .45, 1800.0), .7071, tapeGain(current_[kNoiseTone][lane] * .09));
          s.noise_tone_value = current_[kNoiseTone][lane];
        }
        const double level = std::max(std::abs(processed[lane & ~1]), std::abs(processed[lane | 1]));
        s.noise_envelope += (level - s.noise_envelope) * (level > s.noise_envelope ? attack_ : release_);
        const double pitch = s.noise_pitch.get(current_[kNoisePitch][lane], [](double semitones) { return std::exp2(semitones / 12.0); });
        float n = noiseSample(*clip_, lane % 2, s.noise_position);
        if (fading_clip_ && fade_remaining_ > 0) {
          const double angle = (1 - double(fade_remaining_) / fade_length_) * tapePi * .5;
          n = static_cast<float>(noiseSample(*fading_clip_, lane % 2, s.fading_position) * std::cos(angle) + n * std::sin(angle));
          advanceNoise(*fading_clip_, s.fading_position, pitch * fading_clip_->sample_rate / rate);
        }
        noise.set(lane, n);
        advanceNoise(*clip_, s.noise_position, pitch * clip_->sample_rate / rate);
        const double follow = current_[kNoiseFollow][lane];
        noise_gain.set(lane, static_cast<float>(current_[kNoiseEnabled][lane] * s.noise_gain.get(current_[kNoiseLevel][lane], tapeGain) *
            (1 - follow + follow * std::clamp(s.noise_envelope * 4, 0.0, 1.0))));
      }
      if (fade_remaining_ > 0 && --fade_remaining_ == 0) fading_clip_ = nullptr;
      // Opus width precedes the tone shelf and the two noise cut filters.
      const auto swap = utils::swapStereo(noise);
      noise += (noise - swap) * .5f * (current_[kNoiseWidth] - 1.0f);
      for (int lane = 0; lane < poly_float::kSize; ++lane) noise.set(lane, static_cast<float>(lanes_[lane].noise_tone.tick(noise[lane])));
      auto filtered = noise_filters_.hp.tick(noise);
      noise += (filtered - noise) * current_[kNoiseHpEnabled];
      filtered = noise_filters_.lp.tick(noise);
      noise += (filtered - noise) * current_[kNoiseLpEnabled];
    }
    // Global shaping follows the complete Tape mix, including its noise branch.
    output()->buffer[sample] = filterAndWidth(processed + noise * noise_gain, global_filters_,
                                             kGlobalWidth, kGlobalHpEnabled, kGlobalLpEnabled);
    tone_counter_ = (tone_counter_ + 1) % 16;
  }
  ready_ = true;
}
}
