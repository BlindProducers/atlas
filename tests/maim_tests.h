// SPDX-License-Identifier: GPL-3.0-or-later
// The Maim effect: its resampler, the latency each codec reports, encoder
// and bitrate changes, the circuit bends, packet loss, the LPC vocoder, the
// module in the effect chain and the editor's labels.
#pragma once
#include "maim/maim_engine.h"
#include "maim/lpc_codec.h"
#include "maim/opus_codec.h"
#include "maim/resampler.h"
#include "maim_module.h"
#include "synth_parameters.h"
#include "synth_strings.h"
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace maim_tests {
  namespace maim = atlas::maim;
  int failures = 0;
  void expect(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
  }
  struct Stereo { std::vector<float> left, right; };

  Stereo noise(int samples, float amplitude, uint32_t seed = 7) {
    maim::Random random(seed);
    Stereo signal { std::vector<float>(size_t(samples)), std::vector<float>(size_t(samples)) };
    for (int i = 0; i < samples; ++i) {
      signal.left[size_t(i)] = (random.nextFloat() * 2.0f - 1.0f) * amplitude;
      signal.right[size_t(i)] = (random.nextFloat() * 2.0f - 1.0f) * amplitude;
    }
    return signal;
  }

  // A sawtooth has a harmonic at every multiple of its pitch, like a voice,
  // so it gives every codec, the vocoder included, something to work on.
  Stereo sawtooth(int samples, float frequency, int rate, float amplitude = 0.5f) {
    Stereo signal { std::vector<float>(size_t(samples)), std::vector<float>(size_t(samples)) };
    for (int i = 0; i < samples; ++i) {
      // The fractional part by hand: MSVC 19.44's optimizer crashes (C1001)
      // on std::fmod in this loop.
      const float cycles = float(i) * frequency / float(rate);
      const float phase = cycles - std::floor(cycles);
      signal.left[size_t(i)] = signal.right[size_t(i)] = amplitude * (2.0f * phase - 1.0f);
    }
    return signal;
  }

  // The pitch of a signal: the first autocorrelation peak near the best one,
  // since a pulse train a quantizer step off its period matches itself
  // better two periods on than one.
  double pitchOf(const std::vector<float>& signal, int rate) {
    std::vector<double> correlation(size_t(rate / 51) + 1, 0.0);
    double best = 0.0;
    for (int lag = rate / 400; lag < rate / 51; ++lag) {
      double sum = 0.0;
      for (size_t i = 0; i + size_t(lag) < signal.size(); ++i) sum += double(signal[i]) * signal[i + size_t(lag)];
      correlation[size_t(lag)] = sum;
      best = std::max(best, sum);
    }
    for (int lag = rate / 400 + 1; lag < rate / 51 - 1; ++lag)
      if (correlation[size_t(lag)] > 0.9 * best && correlation[size_t(lag)] >= correlation[size_t(lag - 1)] &&
          correlation[size_t(lag)] >= correlation[size_t(lag + 1)])
        return double(rate) / lag;
    return 0.0;
  }

  double rms(const std::vector<float>& signal, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > signal.size()) to = signal.size();
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) sum += double(signal[i]) * signal[i];
    return to > from ? std::sqrt(sum / double(to - from)) : 0.0;
  }

  bool finite(const Stereo& signal) {
    for (const auto* channel : { &signal.left, &signal.right })
      for (float sample : *channel)
        if (!std::isfinite(sample)) return false;
    return true;
  }

  // Runs the engine over a signal in blocks, setting the parameters before
  // each block; `realtime` false builds MP3 encoders synchronously.
  Stereo render(maim::Engine& engine, const Stereo& input, const maim::Parameters& parameters,
                bool realtime = false, int block = 512) {
    Stereo out { std::vector<float>(input.left.size()), std::vector<float>(input.right.size()) };
    for (size_t i = 0; i < input.left.size(); i += size_t(block)) {
      const int size = int(std::min<size_t>(size_t(block), input.left.size() - i));
      engine.setParameters(parameters, realtime);
      engine.process(input.left.data() + i, input.right.data() + i, out.left.data() + i, out.right.data() + i, size);
    }
    return out;
  }

  // The lag at which the output correlates best with the input.
  int measuredLag(const std::vector<float>& input, const std::vector<float>& output, int from, int to, int max_lag) {
    int best_lag = 0;
    double best = -1.0;
    for (int lag = 0; lag <= max_lag; ++lag) {
      double sum = 0.0;
      for (int i = from; i < to && i + lag < int(output.size()); ++i) sum += double(input[size_t(i)]) * output[size_t(i + lag)];
      if (sum > best) { best = sum; best_lag = lag; }
    }
    return best_lag;
  }

  // ---------------------------------------------------------------- Resampler

  void testResampler() {
    for (const auto rates : { std::pair<int, int> { 88200, 44100 }, { 44100, 48000 }, { 8000, 44100 }, { 352800, 44100 } }) {
      maim::ResamplerKernel kernel(rates.first, rates.second);
      maim::Resampler stream;
      stream.reserve(kernel.half(), (rates.first + rates.second - 1) / rates.second);
      stream.bind(&kernel);
      // Unity gain at DC, through every phase.
      std::vector<float> out;
      for (int i = 0; i < rates.first; ++i) {
        stream.push(1.0f);
        float value;
        while (stream.pull(value)) out.push_back(value);
      }
      double worst = 0.0;
      for (size_t i = out.size() / 2; i < out.size(); ++i) worst = std::max(worst, std::abs(double(out[i]) - 1.0));
      expect(worst < 1.0e-4, "resampler " + std::to_string(rates.first) + " to " + std::to_string(rates.second) +
             " keeps DC at unity (worst " + std::to_string(worst) + ")");
      // The last half kernel of outputs waits for input that never comes.
      const int held_back = int(std::ceil(double(kernel.half()) * rates.second / rates.first)) + 2;
      expect(std::abs(int(out.size()) - rates.second) <= held_back,
             "resampler " + std::to_string(rates.first) + " to " + std::to_string(rates.second) +
             " produces one second of output for one second of input (" + std::to_string(out.size()) + ")");
    }

    // A tone well inside the band survives a trip down to 44.1 kHz and back;
    // one above it does not. Each output stands for its own input time: the
    // kernels' half widths only delay when an output can be computed.
    const int rate = 88200;
    maim::ResamplerKernel down(rate, 44100), up(44100, rate);
    for (const float hz : { 1000.0f, 10000.0f, 30000.0f }) {
      maim::Resampler down_stream, up_stream;
      down_stream.reserve(down.half(), 2); up_stream.reserve(up.half(), 1);
      down_stream.bind(&down); up_stream.bind(&up);
      std::vector<float> out;
      const int samples = rate / 2;
      for (int i = 0; i < samples; ++i) {
        down_stream.push(std::sin(2.0f * 3.14159265f * hz * float(i) / float(rate)));
        float value;
        while (down_stream.pull(value)) {
          up_stream.push(value);
          float back;
          while (up_stream.pull(back)) out.push_back(back);
        }
      }
      double error = 0.0, level = 0.0;
      for (int i = samples / 4; i < int(out.size()); ++i) {
        const float expected = std::sin(2.0f * 3.14159265f * hz * float(i) / float(rate));
        error += std::pow(double(out[size_t(i)]) - expected, 2.0);
        level += double(expected) * expected;
      }
      const double ratio = std::sqrt(error / std::max(level, 1e-9));
      if (hz < 20000.0f)
        expect(ratio < 0.02, "a " + std::to_string(int(hz)) + " Hz tone survives 88.2 to 44.1 kHz and back (error " + std::to_string(ratio) + ")");
      else
        expect(ratio > 0.9, "a 30 kHz tone is removed on the way down (error " + std::to_string(ratio) + ")");
    }
  }

  // ------------------------------------------------------------------ Latency

  void testLatency() {
    struct Case { int encoder; const char* name; int tolerance; };
    for (const int rate : { 44100, 48000, 88200, 96000 }) {
      for (const Case c : { Case { maim::kLame, "LAME", 1 }, Case { maim::kBlade, "Blade", 1 },
                            Case { maim::kOpus, "Opus", 2 }, Case { maim::kGsm, "GSM", 3 } }) {
        maim::Engine engine;
        engine.prepare(rate);
        maim::Parameters p;
        p.encoder = c.encoder;
        p.bitrate_index = maim::kNumBitrates - 1;  // 320 kbps: the least damage
        const Stereo input = noise(rate * 3, 0.5f);
        const Stereo out = render(engine, input, p);
        const int expected = maim::Engine::latencyFor(c.encoder, maim::kDefaultLpcRateIndex, rate);
        const int lag = measuredLag(input.left, out.left, rate, rate * 2, expected + rate / 4);
        expect(std::abs(lag - expected) <= c.tolerance, std::string(c.name) + " at " + std::to_string(rate) +
               " Hz reports its latency (reported " + std::to_string(expected) + ", measured " + std::to_string(lag) + ")");
        expect(engine.latencySamples() == expected, "the engine reports the same latency as the static query");
      }
    }
    maim::Engine engine;
    engine.prepare(48000);
    expect(engine.opusReportedLookahead() == maim::OpusCodec::kLookahead,
           "Opus reports the lookahead the latency assumes (" + std::to_string(engine.opusReportedLookahead()) + ")");
    expect(maim::Engine::latencyFor(maim::kLame, 0, 44100) == 2881 && maim::Engine::latencyFor(maim::kBlade, 0, 48000) == 2209,
           "the MP3 encoders report their measured latencies at their own rates");
    expect(maim::Engine::maxLatencyFor(384000) <= vital::MaimModule::kMaxLatencySamples,
           "every setting's latency fits the shells' dry history at 384 kHz (" +
           std::to_string(maim::Engine::maxLatencyFor(384000)) + ")");
    expect(maim::Engine::mp3RateFor(88200) == 44100 && maim::Engine::mp3RateFor(96000) == 48000 &&
           maim::Engine::mp3RateFor(192000) == 48000 && maim::Engine::mp3RateFor(64000) == 48000 &&
           maim::Engine::mp3RateFor(22050) == 32000 && maim::Engine::mp3RateFor(32000) == 32000,
           "the MP3 rate for a processing rate");
  }

  // ---------------------------------------------------------- MP3 switching

  void testMp3Switching() {
    const int rate = 44100;
    maim::Engine engine;
    engine.prepare(rate);
    maim::Parameters p;
    const Stereo input = sawtooth(rate / 2, 150.0f, rate);
    Stereo out = render(engine, input, p);
    expect(engine.mp3Settled() && engine.mp3CurrentBitrate() == 112, "LAME starts at the default 112 kbps");
    expect(rms(out.left, out.left.size() / 2) > 0.1, "LAME passes a sawtooth");

    p.bitrate_index = 7;  // 64 kbps
    out = render(engine, input, p);
    expect(engine.mp3Settled() && engine.mp3CurrentBitrate() == 64, "a bitrate change swaps the encoder (offline)");
    expect(finite(out) && rms(out.left, out.left.size() / 2) > 0.1, "audio keeps flowing through a bitrate change");

    p.encoder = maim::kBlade;
    p.bitrate_index = 0;  // 8 kbps: Blade's lowest is 32
    out = render(engine, input, p);
    expect(engine.mp3Settled() && engine.mp3CurrentBitrate() == 32, "Blade takes the nearest bitrate it has");
    expect(finite(out) && rms(out.left, out.left.size() / 2) > 0.05, "Blade passes a sawtooth");

    // In realtime the new encoder is built on the manager's own thread.
    p.encoder = maim::kLame;
    p.bitrate_index = 16;
    const auto start = std::chrono::steady_clock::now();
    bool settled = false;
    while (!settled && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
      out = render(engine, input, p, true);
      settled = engine.mp3Settled() && engine.mp3CurrentBitrate() == 320;
    }
    expect(settled, "a realtime encoder change is built off thread and swapped in");
    expect(finite(out) && rms(out.left, out.left.size() / 2) > 0.1, "audio keeps flowing through a realtime change");
  }

  // ---------------------------------------------------------- Codec changes

  void testFamilySwitching() {
    const int rate = 48000;
    maim::Engine engine;
    engine.prepare(rate);
    const Stereo input = sawtooth(rate * 2, 150.0f, rate);
    const double in_level = rms(input.left);
    struct Case { int encoder; const char* name; double low, high; };
    for (const Case c : { Case { maim::kLame, "LAME", 0.3, 3.0 }, Case { maim::kOpus, "Opus", 0.3, 3.0 },
                          Case { maim::kGsm, "GSM", 0.2, 3.0 }, Case { maim::kLpc, "LPC", 0.3, 3.0 },
                          Case { maim::kBlade, "Blade", 0.3, 3.0 }, Case { maim::kLpc, "LPC again", 0.3, 3.0 } }) {
      maim::Parameters p;
      p.encoder = c.encoder;
      const Stereo out = render(engine, input, p);
      expect(finite(out), std::string(c.name) + " output is finite");
      const double level = rms(out.left, out.left.size() / 2) / in_level;
      expect(level > c.low && level < c.high, std::string(c.name) + " keeps the level of a sawtooth (ratio " + std::to_string(level) + ")");
      expect(!engine.switching(), std::string(c.name) + " has finished its crossfade after two seconds");
    }
    // The LPC rate is a change of codec too: a new path warms up and fades in.
    maim::Parameters p;
    p.encoder = maim::kLpc;
    for (const int index : { 0, 2, 9, 5 }) {
      p.lpc_rate_index = index;
      const Stereo out = render(engine, input, p);
      expect(finite(out) && rms(out.left, out.left.size() / 2) / in_level > 0.2,
             "LPC at rate index " + std::to_string(index) + " keeps the level");
      expect(engine.currentCodecRate() == maim::LpcCodec::rateFor(index, rate), "the LPC path runs at its rate");
    }
  }

  // ---------------------------------------------------------------- Bends

  // Noise keeps LAME in long blocks, where every bend has something to do;
  // a sawtooth's edges, even band-limited, put it in short blocks, where the
  // tilt and butterfly bends do nothing by design.
  void testBends() {
    const int rate = 44100;
    const Stereo input = noise(rate * 2, 0.3f, 11);
    const auto renderWith = [&](const maim::Parameters& p) {
      maim::Engine engine;
      engine.prepare(rate);
      return render(engine, input, p);
    };
    const auto difference = [&](const Stereo& a, const Stereo& b) {
      std::vector<float> diff(a.left.size());
      for (size_t i = 0; i < diff.size(); ++i) diff[i] = a.left[i] - b.left[i];
      const double base = rms(a.left, a.left.size() / 2);
      return base > 0.0 ? rms(diff, diff.size() / 2) / base : 0.0;
    };
    for (const int encoder : { maim::kLame, maim::kBlade }) {
      maim::Parameters base;
      base.encoder = encoder;
      const Stereo plain = renderWith(base);
      const std::string name = encoder == maim::kLame ? "LAME" : "Blade";
      struct Bend { const char* name; std::function<void(maim::Parameters&)> apply; bool lame_only; };
      for (const Bend bend : {
             Bend { "turbo", [](maim::Parameters& p) { p.turbo = 0.9f; }, false },
             Bend { "tilt", [](maim::Parameters& p) { p.tilt = -0.8f; }, false },
             Bend { "error", [](maim::Parameters& p) { p.error = 0.5f; }, false },
             Bend { "frequency shift", [](maim::Parameters& p) { p.shift = 20; }, false },
             Bend { "level shift", [](maim::Parameters& p) { p.level_shift = -0.5f; }, false },
             Bend { "feedback", [](maim::Parameters& p) { p.feedback = 0.8f; }, false },
             Bend { "butterfly", [](maim::Parameters& p) { p.butterfly_straight = 0.2f; p.butterfly_crossed = 0.9f; }, false },
             Bend { "band shift", [](maim::Parameters& p) { p.band_shift = 3; }, false },
             Bend { "band reverse", [](maim::Parameters& p) { p.band_reverse = true; }, false },
             Bend { "band step", [](maim::Parameters& p) { p.band_step = 10; }, true },
             Bend { "band invert", [](maim::Parameters& p) { p.band_invert = true; }, true },
             Bend { "window step", [](maim::Parameters& p) { p.window_step = 8; }, true } }) {
        maim::Parameters p = base;
        bend.apply(p);
        const Stereo bent = renderWith(p);
        const double change = difference(plain, bent);
        expect(finite(bent), name + " with " + bend.name + " stays finite");
        if (bend.lame_only && encoder == maim::kBlade)
          expect(change < 1.0e-6, name + " ignores " + bend.name + ", a LAME-only bend (change " + std::to_string(change) + ")");
        else
          expect(change > 0.02, name + " responds to " + bend.name + " (change " + std::to_string(change) + ")");
      }
      // Drive, Makeup and the filters around the codec.
      maim::Parameters loud = base;
      loud.makeup_db = 6.0f;
      expect(std::abs(rms(renderWith(loud).left, size_t(rate)) / rms(plain.left, size_t(rate)) - 2.0) < 0.15,
             name + ": 6 dB of makeup doubles the level");
      maim::Parameters dry = base;
      dry.mix = 0.0f;
      const Stereo dry_out = renderWith(dry);
      double worst = 0.0;
      for (size_t i = size_t(rate); i < dry_out.left.size(); ++i)
        worst = std::max(worst, double(std::abs(dry_out.left[i] - input.left[i - size_t(maim::Engine::latencyFor(encoder, 0, rate))])));
      expect(worst < 1.0e-5, name + " at 0% mix returns the dry signal, delayed by the latency (worst " + std::to_string(worst) + ")");
      maim::Parameters filtered = base;
      filtered.high_cut_hz = 60.0f;
      expect(rms(renderWith(filtered).left, size_t(rate)) < 0.5 * rms(plain.left, size_t(rate)), name + ": the high cut removes level");
      filtered = base;
      filtered.low_cut_hz = 18000.0f;
      expect(rms(renderWith(filtered).left, size_t(rate)) < 0.5 * rms(plain.left, size_t(rate)), name + ": the low cut removes level");
    }
  }

  // ------------------------------------------------------------ Packet loss

  void testPacketLoss() {
    const int rate = 48000;
    const Stereo input = sawtooth(rate * 2, 150.0f, rate);
    for (const int encoder : { maim::kOpus, maim::kGsm, maim::kLpc }) {
      const std::string name = encoder == maim::kOpus ? "Opus" : encoder == maim::kGsm ? "GSM" : "LPC";
      maim::Parameters p;
      p.encoder = encoder;
      maim::Engine engine;
      engine.prepare(rate);
      const Stereo plain = render(engine, input, p);
      const double level = rms(plain.left, plain.left.size() / 2);

      // Nothing gets through: Drop leaves silence (Opus conceals, then fades out).
      // The pattern delivers one packet a cycle whatever the width, so the
      // longest cycle loses everything within these two seconds.
      maim::Parameters lost = p;
      lost.loss_width = 0.0f;
      lost.loss_period = 2.05f;
      const Stereo dropped = render(engine, input, lost);
      expect(rms(dropped.left, dropped.left.size() / 2) < 0.1 * level,
             name + " with every packet dropped goes quiet (" + std::to_string(rms(dropped.left, dropped.left.size() / 2) / level) + ")");
      // Stick keeps decoding the last packet that arrived.
      lost.loss_mode = maim::kStick;
      lost.loss_width = 0.3f;
      lost.loss_period = 0.5f;
      const Stereo stuck = render(engine, input, lost);
      expect(finite(stuck) && rms(stuck.left, stuck.left.size() / 2) > 0.2 * level, name + " in Stick mode keeps sounding");

      maim::Parameters errors = p;
      errors.error = 1.0f;
      const Stereo corrupted = render(engine, input, errors);
      std::vector<float> diff(corrupted.left.size());
      for (size_t i = 0; i < diff.size(); ++i) diff[i] = corrupted.left[i] - plain.left[i];
      expect(finite(corrupted) && rms(diff, diff.size() / 2) > 0.05 * level, name + " responds to Error");
    }
  }

  // ---------------------------------------------------------------- LPC

  void testLpc() {
    maim::LpcCodec::Frame frame;
    frame.pitch = 93;
    frame.rms = 17;
    frame.reflection = { 31, 0, 12, 29, 15, 7, 3, 8, 5, 2 };
    const auto unpacked = maim::LpcCodec::unpack(maim::LpcCodec::pack(frame));
    expect(unpacked.pitch == frame.pitch && unpacked.rms == frame.rms && unpacked.reflection == frame.reflection,
           "LPC packs frames losslessly");
    expect(maim::LpcCodec::rateFor(9, 44100) == 22050 && maim::LpcCodec::rateFor(9, 16000) == 16000,
           "the LPC rate is capped by the processing rate");

    const int rate = 44100;
    for (const int index : { 0, 5, 9 }) {
      maim::Engine engine;
      engine.prepare(rate);
      maim::Parameters p;
      p.encoder = maim::kLpc;
      p.lpc_rate_index = index;
      const Stereo input = sawtooth(rate * 2, 150.0f, rate);
      const Stereo out = render(engine, input, p);
      const std::vector<float> tail(out.left.begin() + rate, out.left.end());
      const double pitch = pitchOf(tail, rate);
      expect(std::abs(pitch - 150.0) < 150.0 * 0.03,
             "LPC at rate index " + std::to_string(index) + " keeps the pitch of a voiced sound (" + std::to_string(pitch) + " Hz)");
      const double level = rms(tail) / rms(input.left);
      expect(level > 0.3 && level < 3.0, "LPC at rate index " + std::to_string(index) + " keeps the level (" + std::to_string(level) + ")");
    }
    maim::Engine engine;
    engine.prepare(rate);
    maim::Parameters p;
    p.encoder = maim::kLpc;
    Stereo silence { std::vector<float>(size_t(rate), 0.0f), std::vector<float>(size_t(rate), 0.0f) };
    const Stereo out = render(engine, silence, p);
    expect(rms(out.left) == 0.0, "LPC is silent on silence");
  }

  // ------------------------------------------------------------- The module

  void testModule() {
    vital::MaimModule module;
    module.init();
    module.setSampleRate(48000);
    module.setOversampleAmount(1);
    auto controls = module.getControls();
    expect(controls.count("maim_encoder") == 1 && controls.count("maim_bitrate") == 1 &&
           controls.count("maim_lpc_rate") == 1 && controls.count("maim_turbo") == 1 &&
           controls.count("maim_high_cut") == 1 && controls.count("maim_loss_mode") == 1,
           "the module has its controls");
    expect(module.getLatencySamples() == maim::Engine::latencyFor(maim::kLame, 5, 48000), "the module reports LAME's latency");
    controls.at("maim_encoder")->set(float(maim::kOpus));
    expect(module.getLatencySamples() == maim::Engine::latencyFor(maim::kOpus, 5, 48000), "the module's latency follows the encoder");
    controls.at("maim_encoder")->set(float(maim::kLpc));
    controls.at("maim_lpc_rate")->set(2.0f);
    expect(module.getLatencySamples() == maim::Engine::latencyFor(maim::kLpc, 2, 48000), "the module's latency follows the LPC rate");
    controls.at("maim_encoder")->set(float(maim::kLame));

    std::vector<vital::poly_float> input(static_cast<size_t>(vital::kMaxBufferSize));
    for (int i = 0; i < vital::kMaxBufferSize; ++i) {
      const float left = 0.5f * std::sin(0.05f * float(i)), right = 0.25f * std::sin(0.07f * float(i));
      input[size_t(i)] = vital::poly_float(left, right, 0.0f, 0.0f);
    }
    bool mirrored = true;
    for (int block = 0; block < 60; ++block) {
      module.processWithInput(input.data(), vital::kMaxBufferSize);
      for (int i = 0; i < vital::kMaxBufferSize; ++i) {
        const vital::poly_float sample = module.output()->buffer[i];
        if (sample[0] != sample[2] || sample[1] != sample[3] || !std::isfinite(sample[0]) || !std::isfinite(sample[1]))
          mirrored = false;
      }
    }
    expect(mirrored, "lanes 2 and 3 mirror left and right");
    double energy = 0.0;
    for (int i = 0; i < vital::kMaxBufferSize; ++i) energy += double(module.output()->buffer[i][0]) * module.output()->buffer[i][0];
    expect(energy > 0.0, "the module produces audio");
  }

  // Through the plug-in: a Maim slot in the chain, prepared on demand.
  void testPlugin() {
    SynthPlugin plugin;
    plugin.setNonRealtime(true);
    plugin.setPlayConfigDetails(0, 2, 48000, 128);
    plugin.prepareToPlay(48000, 128);
    const auto controls = plugin.getControls();
    controls.at("osc_1_on")->set(1.0f);
    controls.at("fx_slot_1_type")->set(float(vital::constants::kMaim + 1));
    controls.at("fx_slot_1_enabled")->set(1.0f);
    plugin.checkOversampling();
    AudioSampleBuffer audio(2, 128);
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(1, 60, 0.8f), 0);
    double energy = 0.0;
    bool finite_output = true;
    for (int block = 0; block < 600; ++block) {
      audio.clear();
      plugin.processBlock(audio, midi);
      midi.clear();
      if (block > 400)
        for (int i = 0; i < 128; ++i) {
          const float sample = audio.getSample(0, i);
          if (!std::isfinite(sample)) finite_output = false;
          energy += double(sample) * sample;
        }
    }
    expect(finite_output && energy > 0.0, "a Maim slot in the chain plays a note (energy " + std::to_string(energy) + ")");
    const int lame_latency = plugin.getLatencySamples();
    expect(lame_latency > 0, "the plug-in reports Maim's latency (" + std::to_string(lame_latency) + ")");
    controls.at("fx_slot_1_maim_encoder")->set(float(maim::kOpus));
    plugin.processBlock(audio, midi);
    expect(plugin.getLatencySamples() > 0 && plugin.getLatencySamples() != lame_latency,
           "the plug-in's latency follows the encoder (" + std::to_string(plugin.getLatencySamples()) + ")");
  }

  // ----------------------------------------------------------------- Labels

  void testLabels() {
    const auto& encoder = vital::Parameters::getDetails("maim_encoder");
    expect(encoder.max == maim::kNumEncoders - 1 && encoder.string_lookup != nullptr &&
           encoder.string_lookup[maim::kBlade] == "Blade" && encoder.string_lookup[maim::kLame] == "LAME" &&
           encoder.string_lookup[maim::kOpus] == "Opus" && encoder.string_lookup[maim::kGsm] == "GSM" &&
           encoder.string_lookup[maim::kLpc] == "LPC", "the encoder labels match the engine");
    const auto& bitrate = vital::Parameters::getDetails("maim_bitrate");
    expect(bitrate.max == maim::kNumBitrates - 1 && bitrate.string_lookup[0] == "8 kbps" &&
           bitrate.string_lookup[maim::kNumBitrates - 1] == "320 kbps" && bitrate.default_value == maim::kDefaultBitrateIndex,
           "the bitrate labels match the table");
    const auto& lpc_rate = vital::Parameters::getDetails("maim_lpc_rate");
    expect(lpc_rate.max == maim::kNumLpcRates - 1 && lpc_rate.string_lookup[6] == "11.025 kHz" &&
           lpc_rate.string_lookup[9] == "22.05 kHz" && lpc_rate.default_value == maim::kDefaultLpcRateIndex,
           "the LPC rate labels match the table");
    const auto& loss_mode = vital::Parameters::getDetails("maim_loss_mode");
    expect(loss_mode.max == maim::kNumLossModes - 1 && loss_mode.string_lookup[maim::kDrop] == "Drop" &&
           loss_mode.string_lookup[maim::kStick] == "Stick", "the loss mode labels match the engine");
    expect(std::abs(std::pow(vital::Parameters::getDetails("maim_loss_period").max, 3.0f) - 2.05f) < 1e-4f &&
           std::abs(std::pow(vital::Parameters::getDetails("maim_loss_period").min, 3.0f) - 0.05f) < 1e-4f,
           "the loss period runs from 50 ms to 2.05 s");
    expect(strings::kEffectSlotTypeNames[vital::constants::kMaim + 1] == "Maim" &&
           strings::kEffectOrder[vital::constants::kMaim] == "maim", "the effect's names");
  }

  int run() {
    testResampler();
    testLatency();
    testMp3Switching();
    testFamilySwitching();
    testBends();
    testPacketLoss();
    testLpc();
    testModule();
    testPlugin();
    testLabels();
    if (failures == 0) std::cout << "Atlas Maim tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}
