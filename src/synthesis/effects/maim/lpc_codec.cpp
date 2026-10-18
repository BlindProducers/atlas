// SPDX-License-Identifier: GPL-3.0-or-later
#include "lpc_codec.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace atlas::maim {
namespace {
  constexpr float kPi = 3.14159265358979323846f;
  // The reflection coefficients travel as log area ratios, which squash the
  // region near +-1, where the filter is most sensitive, into more quantizer
  // steps. Each has its own range: the first few swing widely, the later
  // ones stay close to zero.
  constexpr std::array<float, LpcCodec::kOrder> kLarRange { 5.f, 4.f, 3.f, 3.f, 2.5f, 2.5f, 2.5f, 2.5f, 2.f, 2.f };
  // Loudness is sent in dB: code 0 is silence, and code 31 is a little over
  // full scale, which the pre-emphasis can push high material to.
  constexpr float kRmsFloorDb = -80.f;
  constexpr float kRmsStepDb = 2.75f;
  // Normalized autocorrelation the best pitch candidate needs to count as voiced.
  constexpr float kVoicingThreshold = 0.5f;

  constexpr int totalFrameBits() {
    int bits = LpcCodec::kPitchBits + LpcCodec::kRmsBits;
    for (int reflection_bits : LpcCodec::kReflectionBits) bits += reflection_bits;
    return bits;
  }
  static_assert(totalFrameBits() == LpcCodec::kFrameBits);

  template <typename T> T limit(T low, T high, T value) { return value < low ? low : value > high ? high : value; }

  int quantizeReflection(float k, int coefficient) {
    k = limit(-0.9999f, 0.9999f, k);
    const float range = kLarRange[static_cast<size_t>(coefficient)];
    const int levels = 1 << LpcCodec::kReflectionBits[static_cast<size_t>(coefficient)];
    const float log_area_ratio = std::log((1 + k) / (1 - k));
    const int code = static_cast<int>(std::floor((log_area_ratio + range) / (2 * range) * static_cast<float>(levels)));
    return limit(0, levels - 1, code);
  }

  float dequantizeReflection(int code, int coefficient) {
    const float range = kLarRange[static_cast<size_t>(coefficient)];
    const int levels = 1 << LpcCodec::kReflectionBits[static_cast<size_t>(coefficient)];
    const float log_area_ratio = -range + (static_cast<float>(code) + 0.5f) * (2 * range) / static_cast<float>(levels);
    // Inverts the log area ratio, and keeps |k| < 1, so any code gives a stable filter.
    return std::tanh(log_area_ratio / 2);
  }

  int quantizeRms(float rms) {
    if (rms <= 0) return 0;
    const int code = static_cast<int>(std::lround((gainToDb(rms, -1000.f) - kRmsFloorDb) / kRmsStepDb));
    return limit(0, (1 << LpcCodec::kRmsBits) - 1, code);
  }

  float dequantizeRms(int code) {
    return code == 0 ? 0.0f : dbToGain(kRmsFloorDb + static_cast<float>(code) * kRmsStepDb);
  }

  int quantizePitch(float period8k) {
    if (period8k <= 0) return 0;
    const int max_code = (1 << LpcCodec::kPitchBits) - 1;
    const float position = std::log(period8k / LpcCodec::kMinPitchPeriod8k)
        / std::log(static_cast<float>(LpcCodec::kMaxPitchPeriod8k) / LpcCodec::kMinPitchPeriod8k);
    return limit(1, max_code, 1 + static_cast<int>(std::lround(position * static_cast<float>(max_code - 1))));
  }

  struct BitWriter {
    LpcCodec::Packed& bytes;
    int position = 0;
    void write(int value, int bits) {
      for (int bit = bits - 1; bit >= 0; --bit, ++position) {
        if ((value >> bit) & 1)
          bytes[static_cast<size_t>(position / 8)] |= static_cast<uint8_t>(0x80 >> (position % 8));
      }
    }
  };

  struct BitReader {
    const LpcCodec::Packed& bytes;
    int position = 0;
    int read(int bits) {
      int value = 0;
      for (int bit = 0; bit < bits; ++bit, ++position)
        value = (value << 1) | ((bytes[static_cast<size_t>(position / 8)] >> (7 - position % 8)) & 1);
      return value;
    }
  };
}

void LpcCodec::prepare() {
  const int max_rate = kLpcRates.back();
  const size_t max_history = static_cast<size_t>(samplesFor(kHistorySeconds, max_rate));
  const size_t max_window = static_cast<size_t>(samplesFor(kWindowSeconds, max_rate));
  for (auto& channel : channels_) {
    channel.encoder.emphasized.assign(max_history, 0.f);
    channel.encoder.lowpassed.assign(max_history, 0.f);
  }
  window_.assign(max_window, 0.f);
  windowed_.assign(max_window, 0.f);
  correlation_.assign(max_history, 0.f);
  reserveFrames(frameSizeFor(max_rate));
  loss_.prepare(max_rate);
  setRate(kLpcRates[kDefaultLpcRateIndex]);
}

void LpcCodec::setRate(int codec_rate) {
  if (!prepared()) return;
  rate_ = codec_rate;
  frame_size_ = frameSizeFor(rate_);
  history_length_ = samplesFor(kHistorySeconds, rate_);
  window_length_ = samplesFor(kWindowSeconds, rate_);
  min_pitch_period_ = std::max(2, static_cast<int>(std::lround(kMinPitchPeriod8k * rate_ / 8000.0)));
  max_pitch_period_ = static_cast<int>(std::lround(kMaxPitchPeriod8k * rate_ / 8000.0));
  interpolation_samples_ = std::max(1, frame_size_ / 4);
  // LPC-10's 0.9375 at 8 kHz, with its corner at the same frequency at any rate.
  preemphasis_ = std::pow(0.9375f, 8000.f / static_cast<float>(rate_));
  for (int n = 0; n < window_length_; ++n)
    window_[static_cast<size_t>(n)] = 0.54f - 0.46f * std::cos(2.0f * kPi * static_cast<float>(n) / static_cast<float>(window_length_ - 1));
  for (auto& channel : channels_) {
    std::fill(channel.encoder.emphasized.begin(), channel.encoder.emphasized.end(), 0.f);
    std::fill(channel.encoder.lowpassed.begin(), channel.encoder.lowpassed.end(), 0.f);
    channel.encoder.previous_input = 0;
    channel.encoder.pitch_filter.lowPass(static_cast<float>(rate_), std::min(900.f, static_cast<float>(rate_) * 0.4f));
    channel.encoder.pitch_filter.reset();
    channel.decoder = Decoder();
    channel.encoded = pack(Frame());
  }
  loss_.prepare(rate_);
  setFrameSize(frame_size_, rate_);
}

void LpcCodec::reset() {
  if (prepared()) setRate(rate_);
}

void LpcCodec::setParameters(float error, int loss_mode) {
  error_ = error * error * error;
  stick_ = loss_mode == kStick;
}

void LpcCodec::codeFrame(const float* in_left, const float* in_right, float* out_left, float* out_right) {
  const bool delivered = loss_.processPacket(frame_size_);
  for (int ch = 0; ch < 2; ++ch) {
    auto& channel = channels_[static_cast<size_t>(ch)];
    const float* in = ch == 0 ? in_left : in_right;
    float* out = ch == 0 ? out_left : out_right;
    // The encoder keeps listening through lost packets, so it's up to date when they return.
    const Frame frame = analyze(channel.encoder, in);
    if (delivered) {
      channel.encoded = pack(frame);
      if (error_ > 0.0f) {
        for (int byte = 0; byte < kFrameBytes; ++byte) {
          if (random_.nextFloat() < error_) {
            const int bits_in_byte = std::min(8, kFrameBits - byte * 8);
            channel.encoded[static_cast<size_t>(byte)] ^= static_cast<uint8_t>(0x80 >> random_.nextInt(bits_in_byte));
          }
        }
      }
    }
    // A lost packet is silence in Drop mode, and the next one fades back in.
    // In Stick mode the last packet that got through is decoded again: the
    // pulse train keeps its phase, so a voiced frame holds as a drone.
    if (!delivered && !stick_) {
      channel.decoder.gain = 0;
      std::memset(out, 0, sizeof(float) * static_cast<size_t>(frame_size_));
      continue;
    }
    synthesize(channel.decoder, unpack(channel.encoded), out);
  }
}

LpcCodec::Frame LpcCodec::analyze(Encoder& encoder, const float* in) {
  const int old_samples = history_length_ - frame_size_;
  std::copy(encoder.emphasized.begin() + frame_size_, encoder.emphasized.begin() + history_length_, encoder.emphasized.begin());
  std::copy(encoder.lowpassed.begin() + frame_size_, encoder.lowpassed.begin() + history_length_, encoder.lowpassed.begin());
  for (int n = 0; n < frame_size_; ++n) {
    // Pre-emphasis tilts the spectrum up, so the filter doesn't spend all its poles on the lows.
    encoder.emphasized[static_cast<size_t>(old_samples + n)] = in[n] - preemphasis_ * encoder.previous_input;
    encoder.previous_input = in[n];
    encoder.lowpassed[static_cast<size_t>(old_samples + n)] = encoder.pitch_filter.process(in[n]);
  }

  // Autocorrelation of the windowed tail of the history...
  double window_energy = 0.0;
  const float* recent = encoder.emphasized.data() + (history_length_ - window_length_);
  for (int n = 0; n < window_length_; ++n) {
    windowed_[static_cast<size_t>(n)] = recent[n] * window_[static_cast<size_t>(n)];
    window_energy += static_cast<double>(window_[static_cast<size_t>(n)]) * window_[static_cast<size_t>(n)];
  }
  std::array<double, kOrder + 1> autocorrelation {};
  for (int lag = 0; lag < static_cast<int>(autocorrelation.size()); ++lag)
    for (int n = lag; n < window_length_; ++n)
      autocorrelation[static_cast<size_t>(lag)] += static_cast<double>(windowed_[static_cast<size_t>(n)]) * windowed_[static_cast<size_t>(n - lag)];

  Frame frame;
  frame.rms = quantizeRms(static_cast<float>(std::sqrt(autocorrelation[0] / window_energy)));
  if (frame.rms == 0) {
    for (int i = 0; i < kOrder; ++i) frame.reflection[static_cast<size_t>(i)] = quantizeReflection(0, i);
    return frame;
  }

  // ...and Levinson-Durbin recursion turns it into reflection coefficients.
  // A touch of white noise keeps the recursion well behaved on very pure tones.
  autocorrelation[0] *= 1.0001;
  std::array<double, kOrder + 1> predictor {};
  predictor[0] = 1;
  double prediction_error = autocorrelation[0];
  for (int i = 1; i <= kOrder; ++i) {
    double accumulator = autocorrelation[static_cast<size_t>(i)];
    for (int j = 1; j < i; ++j) accumulator += predictor[static_cast<size_t>(j)] * autocorrelation[static_cast<size_t>(i - j)];
    const double k = limit(-0.9999, 0.9999, -accumulator / prediction_error);
    const std::array<double, kOrder + 1> previous = predictor;
    for (int j = 1; j < i; ++j) predictor[static_cast<size_t>(j)] = previous[static_cast<size_t>(j)] + k * previous[static_cast<size_t>(i - j)];
    predictor[static_cast<size_t>(i)] = k;
    prediction_error *= 1 - k * k;
    frame.reflection[static_cast<size_t>(i - 1)] = quantizeReflection(static_cast<float>(k), i - 1);
  }

  frame.pitch = quantizePitch(static_cast<float>(estimatePitchPeriod(encoder.lowpassed)) * 8000.f / static_cast<float>(rate_));
  return frame;
}

int LpcCodec::estimatePitchPeriod(const std::vector<float>& signal) {
  // Compare the newest stretch of the signal with itself a pitch period earlier.
  const int start = max_pitch_period_;
  float energy = 0.f;
  for (int n = start; n < history_length_; ++n) energy += signal[static_cast<size_t>(n)] * signal[static_cast<size_t>(n)];
  if (energy < 1e-8f) return 0;

  int best_period = 0;
  float best_correlation = 0.f;
  for (int period = min_pitch_period_; period <= max_pitch_period_; ++period) {
    float cross = 0.f, lagged = 0.f;
    for (int n = start; n < history_length_; ++n) {
      cross += signal[static_cast<size_t>(n)] * signal[static_cast<size_t>(n - period)];
      lagged += signal[static_cast<size_t>(n - period)] * signal[static_cast<size_t>(n - period)];
    }
    correlation_[static_cast<size_t>(period)] = lagged > 0 ? cross / std::sqrt(energy * lagged) : 0;
    if (correlation_[static_cast<size_t>(period)] > best_correlation) {
      best_correlation = correlation_[static_cast<size_t>(period)];
      best_period = period;
    }
  }
  if (best_correlation < kVoicingThreshold) return 0;

  // A signal that repeats every period also repeats every two periods, and the
  // longer lag sometimes wins by a hair. Prefer the shortest period that
  // correlates nearly as well.
  for (int divisor = 4; divisor >= 2; --divisor) {
    const int candidate = static_cast<int>(std::lround(static_cast<float>(best_period) / static_cast<float>(divisor)));
    if (candidate - 1 < min_pitch_period_) continue;
    for (int period = candidate - 1; period <= candidate + 1; ++period)
      if (correlation_[static_cast<size_t>(period)] > 0.85f * best_correlation) return period;
  }
  return best_period;
}

void LpcCodec::synthesize(Decoder& decoder, const Frame& frame, float* out) {
  std::array<float, kOrder> reflection {};
  float residual_ratio = 1.f;
  for (size_t i = 0; i < reflection.size(); ++i) {
    reflection[i] = dequantizeReflection(frame.reflection[i], static_cast<int>(i));
    residual_ratio *= 1 - reflection[i] * reflection[i];
  }
  // The loudness is of the signal, so scale the excitation down by how much
  // the filter will boost it.
  const float gain = dequantizeRms(frame.rms) * std::sqrt(residual_ratio);
  const float period = frame.pitch == 0 ? 0.f : pitchPeriod8kFor(frame.pitch) * static_cast<float>(rate_) / 8000.f;

  std::array<float, kOrder> current_reflection = reflection;
  float current_gain = gain;
  for (int n = 0; n < frame_size_; ++n) {
    if (n < interpolation_samples_) {
      const float amount = static_cast<float>(n + 1) / static_cast<float>(interpolation_samples_);
      for (size_t i = 0; i < reflection.size(); ++i)
        current_reflection[i] = decoder.reflection[i] + (reflection[i] - decoder.reflection[i]) * amount;
      current_gain = decoder.gain + (gain - decoder.gain) * amount;
    }
    else {
      current_reflection = reflection;
      current_gain = gain;
    }

    // A pulse per period for voiced frames, white noise for unvoiced ones,
    // both with an RMS of the gain.
    float excitation = 0;
    if (period > 0) {
      decoder.pulse_phase += 1.f / period;
      if (decoder.pulse_phase >= 1) {
        decoder.pulse_phase -= 1;
        excitation = current_gain * std::sqrt(period);
      }
    }
    else {
      // Uniform noise on [-1, 1] has an RMS of 1/sqrt(3).
      excitation = current_gain * (random_.nextFloat() * 2 - 1) * std::sqrt(3.f);
    }

    // All-pole lattice filter, which stays stable for any |k| < 1.
    auto& state = decoder.lattice;
    float sample = excitation;
    for (int i = kOrder; i >= 1; --i) {
      sample -= current_reflection[static_cast<size_t>(i - 1)] * state[static_cast<size_t>(i - 1)];
      state[static_cast<size_t>(i)] = state[static_cast<size_t>(i - 1)] + current_reflection[static_cast<size_t>(i - 1)] * sample;
    }
    state[0] = sample;

    // Undo the pre-emphasis.
    decoder.previous_output = sample + preemphasis_ * decoder.previous_output;
    out[n] = limit(-1.f, 1.f, decoder.previous_output);
  }
  decoder.reflection = reflection;
  decoder.gain = gain;
}

float LpcCodec::pitchPeriod8kFor(int code) {
  const int max_code = (1 << kPitchBits) - 1;
  return kMinPitchPeriod8k * std::pow(static_cast<float>(kMaxPitchPeriod8k) / kMinPitchPeriod8k,
                                      static_cast<float>(code - 1) / static_cast<float>(max_code - 1));
}

LpcCodec::Packed LpcCodec::pack(const Frame& frame) {
  Packed packed {};
  BitWriter writer { packed };
  writer.write(frame.pitch, kPitchBits);
  writer.write(frame.rms, kRmsBits);
  for (size_t i = 0; i < frame.reflection.size(); ++i) writer.write(frame.reflection[i], kReflectionBits[i]);
  return packed;
}

LpcCodec::Frame LpcCodec::unpack(const Packed& packed) {
  Frame frame;
  BitReader reader { packed };
  frame.pitch = reader.read(kPitchBits);
  frame.rms = reader.read(kRmsBits);
  for (size_t i = 0; i < frame.reflection.size(); ++i) frame.reflection[i] = reader.read(kReflectionBits[i]);
  return frame;
}
}
