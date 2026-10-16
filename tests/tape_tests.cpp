#include "JuceHeader.h"
#include "tape_module.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { thread_local bool track = false; thread_local size_t allocation_count = 0; }
void* operator new(std::size_t n) { if (track) ++allocation_count; if (void* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
constexpr double pi = 3.14159265358979323846;
using Signal = std::vector<vital::poly_float>;
int failures = 0;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
struct Fixture {
  vital::TapeModule module;
  vital::control_map controls;
  Fixture(std::shared_ptr<vital::TapeNoiseLibrary> library, int rate = 48000, int factor = 1) : module("", library) {
    module.init(); module.setSampleRate(rate); module.setOversampleAmount(factor); controls = module.getControls();
  }
  void set(const char* name, float value) { controls.at(std::string("tape_") + name)->set(value); }
};
Signal sine(int rate, int size, double hz, double amplitude = .5) {
  Signal result(size);
  for (int i = 0; i < size; ++i)
    for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
      result[i].set(lane, static_cast<float>(amplitude * std::sin(2 * pi * hz * i / rate) * (lane + 1) / 4));
  return result;
}
Signal render(vital::TapeModule& module, const Signal& input, int block = 127) {
  expect(block <= vital::kMaxBufferSize * module.getOversampleAmount(), "test block capacity");
  Signal result(input.size()); allocation_count = 0; track = true;
  for (int i = 0; i < static_cast<int>(input.size()); i += block) {
    const int n = std::min(block, static_cast<int>(input.size()) - i);
    module.processWithInput(input.data() + i, n);
    std::copy_n(module.output()->buffer, n, result.data() + i);
  }
  track = false; expect(allocation_count == 0, "audio callbacks must not allocate, including first use and noise changes");
  return result;
}
double difference(const Signal& a, const Signal& b) {
  double max = 0;
  for (size_t i = 0; i < a.size(); ++i)
    for (int l = 0; l < vital::poly_float::kSize; ++l) max = std::max(max, double(std::abs(a[i][l] - b[i][l])));
  return max;
}
double rms(const Signal& a, int lane = 0) {
  double sum = 0;
  for (size_t i = a.size() / 2; i < a.size(); ++i) sum += a[i][lane] * a[i][lane];
  return std::sqrt(sum / (a.size() - a.size() / 2));
}
bool finite(const Signal& a) {
  for (const auto& frame : a) for (int lane = 0; lane < vital::poly_float::kSize; ++lane)
    if (!std::isfinite(frame[lane]) || std::abs(frame[lane]) > 16) return false;
  return true;
}
}
int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  if (argc < 2) { std::cerr << "Pass Factory/Samples/Tape\n"; return 2; }
  const juce::File folder { juce::String(argv[1]) };
  auto library = std::make_shared<vital::TapeNoiseLibrary>(folder);
  constexpr int clips = vital::TapeNoiseLibrary::kNumClips;
  for (int i = 0; i < clips; ++i) library->request(i);
  for (int attempt = 0; attempt < 400; ++attempt) {
    bool loaded = true;
    for (int i = 0; i < clips; ++i) loaded &= library->status(i) == 1;
    if (loaded) break;
    juce::Thread::sleep(25);
  }
  const auto clipIsSane = [](const vital::TapeNoiseLibrary::Clip& clip) {
    bool sane = clip.audio.getNumChannels() == 2 && clip.sample_rate >= 44100 && clip.audio.getNumSamples() > clip.sample_rate;
    for (int channel = 0; channel < clip.audio.getNumChannels(); ++channel) {
      const float* data = clip.audio.getReadPointer(channel);
      for (int i = 0; i < clip.audio.getNumSamples(); ++i) sane &= std::isfinite(data[i]) && std::abs(data[i]) <= 1.0f;
    }
    return sane;
  };
  for (int i = 0; i < clips; ++i) {
    const auto* clip = library->request(i);
    expect(clip != nullptr, "Vinyl 1 to Vinyl 8 load from Factory/Samples/Tape");
    if (!clip) return 1;
    expect(clipIsSane(*clip), "recordings are stereo, at least 44.1 kHz, longer than a second and within full scale");
  }
  // Damaged or foreign files must fail safely instead of crashing.
  const auto temporary = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("atlas-tape-test", ".wav");
  temporary.replaceWithText("this is not a WAV file, but it is longer than a WAV header");
  expect(!vital::TapeNoiseLibrary::load(temporary), "a file that is not a WAV is rejected");
  juce::MemoryBlock original;
  folder.getChildFile(vital::TapeNoiseLibrary::fileName(0)).loadFileAsData(original);
  temporary.replaceWithData(original.getData(), original.getSize() / 2);
  if (auto truncated = vital::TapeNoiseLibrary::load(temporary))
    expect(clipIsSane(*truncated), "a recording cut short loads only finite samples");
  temporary.replaceWithData(original.getData(), 40);
  expect(!vital::TapeNoiseLibrary::load(temporary), "a recording cut inside its header is rejected");
  temporary.deleteFile();
  expect(!vital::TapeNoiseLibrary::load(folder.getChildFile("missing.wav")), "missing recordings fail safely");

  for (int rate : { 44100, 48000, 96000 }) for (int factor : { 1, 2, 4, 8 }) {
    Fixture f(library, rate, factor);
    auto input = sine(rate * factor, 4000, 997);
    expect(difference(input, render(f.module, input)) == 0, "all submodules off is bit-transparent on every lane and sample rate");
    f.set("drive_enabled", 1); f.set("drive_mix", 0); f.module.hardReset();
    expect(difference(input, render(f.module, input)) == 0, "Drive Mix zero, unity output and neutral width preserve input");
    f.set("drive_mix", 1); f.set("drive_amount", 10); f.set("motion_enabled", 1);
    f.set("wow", 1); f.set("wow_rate", .1); f.set("flutter", 1); f.set("flutter_rate", 14);
    f.set("dropouts", 1); f.set("dropout_density", 1); f.set("noise_enabled", 1); f.set("noise_pitch", 24);
    f.set("global_hp_enabled", 1); f.set("global_hp_cutoff", 2000); f.set("global_hp_slope", 3);
    f.set("global_lp_enabled", 1); f.set("global_lp_cutoff", 1000); f.set("global_lp_slope", 3);
    f.set("noise_tone", 100); f.set("noise_width", 2);
    f.module.hardReset();
    expect(finite(render(f.module, input)), "maximum Tape processing stays finite at 44.1/48/96 kHz with 1/2/4/8x oversampling");
    track = true; allocation_count = 0; f.module.enable(false); f.module.enable(true); track = false;
    expect(allocation_count == 0, "bypass and reset must not allocate");
  }
  // Independent Opus drive reference: same saturation, tone and RMS auto gain.
  {
    Fixture f(library); f.set("drive_enabled", 1);
    auto input = sine(48000, 48000, 997, .9), result = render(f.module, input);
    auto coefficients = juce::dsp::IIR::Coefficients<double>::makeLowPass(48000, 2500 * std::pow(5.6, .55), .7071);
    double error = 0;
    for (int lane = 0; lane < vital::poly_float::kSize; ++lane) {
      juce::dsp::IIR::Filter<double> tone(coefficients);
      double dry_rms = 0, wet_rms = 0, gain = 1;
      const double ec = std::exp(-1 / (.12 * 48000)), gc = std::exp(-1 / (.04 * 48000));
      for (size_t i = 0; i < input.size(); ++i) {
        const double dry = input[i][lane];
        double wet = std::tanh(dry * std::pow(10.0, 9.0 / 20) + .035) - std::tanh(.035);
        wet = tone.processSample(wet + .06 * wet * wet * wet);
        dry_rms = ec * dry_rms + (1 - ec) * dry * dry; wet_rms = ec * wet_rms + (1 - ec) * wet * wet;
        if (dry_rms > 1e-8 && wet_rms > 1e-8) gain = gc * gain + (1 - gc) * std::clamp(std::sqrt(dry_rms / wet_rms), .1, 2.0);
        error = std::max(error, std::abs(result[i][lane] - wet * gain));
      }
    }
    expect(error < 2e-6, "Tape Drive matches the Opus saturation, tone and automatic gain reference");
    expect(difference(input, result) > .01, "Tape Drive must audibly distort the signal");
  }
  // Butterworth response is checked against its analytical digital response.
  { // Isolate the shared filter implementation from pitch motion and noise.
    for (bool hp : { false, true }) for (int slope = 0; slope < 4; ++slope) for (double hz : { 500.0, 1000.0, 2000.0 }) {
      Fixture f(library); // Global filters also work with all three processing sections off.
      const std::string prefix = hp ? "global_hp_" : "global_lp_";
      f.set((prefix + "enabled").c_str(), 1); f.set((prefix + "cutoff").c_str(), 1000); f.set((prefix + "slope").c_str(), float(slope));
      const auto input = sine(48000, 24000, hz), output = render(f.module, input);
      double ratio = std::tan(pi * hz / 48000) / std::tan(pi * 1000 / 48000);
      if (hp) ratio = 1 / ratio;
      const double expected = 1 / std::sqrt(1 + std::pow(ratio, 4 * (slope + 1)));
      expect(std::abs(rms(output) / rms(input) - expected) < 5e-4, "12/24/36/48 dB Butterworth slopes and cutoff match analytical response");
    }
  }
  {
    Fixture a(library), b(library);
    for (auto* f : { &a, &b }) { f->set("motion_enabled", 1); f->set("wow", .8); f->set("flutter", .7); f->set("dropouts", 0); }
    const auto input = sine(48000, 96000, 1000);
    const auto one = render(a.module, input, 1), many = render(b.module, input, 127);
    expect(difference(one, many) < 1e-7, "Wow/flutter timing is independent of callback size");
    expect(difference(input, many) > .1 && rms(many) > .07, "Wow/flutter changes phase while preserving sustained audio");
    double shortest = 100, longest = 0, previous = 0;
    for (size_t i = 12000; i < many.size(); ++i) if (many[i-1][0] <= 0 && many[i][0] > 0) {
      const double crossing = i - many[i][0] / (many[i][0] - many[i-1][0]);
      if (previous > 0) { shortest = std::min(shortest, crossing - previous); longest = std::max(longest, crossing - previous); }
      previous = crossing;
    }
    expect(shortest > 44 && longest < 52 && longest - shortest > .3, "Wow/flutter produces bounded time-varying pitch around 1 kHz");
    for (auto* f : { &a, &b }) { f->set("wow_rate", 1.5); f->set("flutter_rate", 14); f->set("dropouts", 1); f->set("dropout_density", 1); }
    expect(difference(render(a.module, input, 17), render(b.module, input, 113)) < 1e-6, "rate changes and dropouts retain timing across callback sizes");
  }
  {
    Fixture f(library); f.set("global_width", 0);
    const auto output = render(f.module, sine(48000, 10000, 700));
    bool mono = true;
    for (const auto& x : output) for (int lane = 0; lane < vital::poly_float::kSize; lane += 2) mono &= std::abs(x[lane] - x[lane+1]) < 1e-7;
    expect(mono, "global width works with all processing sections off and collapses each stereo pair to mono");
  }
  {
    Fixture f(library); f.set("noise_enabled", 1); f.set("noise_level", -12);
    Signal silence(24000, vital::poly_float(0));
    const auto first = render(f.module, silence);
    expect(rms(first) > 1e-5 && finite(first), "noise sounds without input when Follow is zero");
    f.set("noise_source", clips - 1);
    const auto changed = render(f.module, silence);
    expect(difference(first, changed) > 1e-4 && finite(changed), "noise selection switches recordings safely");
    f.set("noise_follow", 1); f.module.hardReset();
    expect(rms(render(f.module, silence)) == 0, "full Noise Follow is silent without input");
    f.set("noise_follow", 0); f.set("noise_width", 0); f.set("noise_pitch", -24);
    expect(finite(render(f.module, silence)), "noise pitch and width changes are safe");
    f.set("noise_enabled", 0); render(f.module, silence);
    expect(rms(render(f.module, silence)) == 0, "Noise Enabled off fully silences playback after its fade");
  }
  // The final section must shape the complete sum, never just one branch.
  for (int mode = 0; mode < 4; ++mode) {
    Fixture unshaped(library), shaped(library), final_only(library);
    for (auto* f : { &unshaped, &shaped }) {
      f->set("drive_enabled", mode == 0 || mode == 3);
      f->set("motion_enabled", mode == 1 || mode == 3);
      f->set("noise_enabled", mode == 2 || mode == 3);
      f->set("noise_level", -6); f->set("noise_follow", 0);
    }
    for (auto* f : { &shaped, &final_only }) {
      f->set("global_width", .4f);
      f->set("global_hp_enabled", 1); f->set("global_hp_cutoff", 300); f->set("global_hp_slope", 2);
      f->set("global_lp_enabled", 1); f->set("global_lp_cutoff", 4000); f->set("global_lp_slope", 3);
    }
    const auto input = mode == 2 ? Signal(16384, vital::poly_float(0)) : sine(48000, 16384, 997);
    const auto before = render(unshaped.module, input);
    const auto expected = render(final_only.module, before);
    const auto actual = render(shaped.module, input);
    expect(difference(expected, actual) < 2e-6, "Global width and filters must follow Drive, motion, noise, and their combined output");
    expect(difference(before, actual) > 1e-4, "Global shaping must audibly affect every Tape branch");
  }
  {
    Fixture a(library), b(library);
    for (auto* f : { &a, &b }) { f->set("drive_enabled", 1); f->set("motion_enabled", 1); }
    b.set("noise_width", 0);
    b.set("noise_hp_cutoff", 2000); b.set("noise_hp_slope", 3);
    b.set("noise_lp_cutoff", 2000); b.set("noise_lp_slope", 3);
    const auto input = sine(48000, 16384, 997);
    expect(difference(render(a.module, input), render(b.module, input)) == 0,
           "Noise width and filters cannot change the main signal with Noise disabled");
  }
  {
    Fixture f(library); f.set("noise_enabled", 1); f.set("noise_width", 0);
    const auto input = sine(48000, 16384, 997), output = render(f.module, input);
    double error = 0;
    for (size_t i = 0; i < input.size(); ++i) for (int lane = 0; lane < vital::poly_float::kSize; lane += 2)
      error = std::max(error, double(std::abs((output[i][lane] - output[i][lane+1]) - (input[i][lane] - input[i][lane+1]))));
    expect(error < 2e-7, "Noise Width zero makes only the noise mono and preserves the input stereo difference");
    f.set("global_width", 0);
    render(f.module, input); // Allow global width automation to settle.
    const auto mono = render(f.module, input);
    bool same = true;
    for (size_t i = mono.size() / 2; i < mono.size(); ++i) for (int lane = 0; lane < vital::poly_float::kSize; lane += 2)
      same &= std::abs(mono[i][lane] - mono[i][lane+1]) < 2e-7;
    expect(same, "Global Width zero makes the complete input-plus-noise output mono");
  }
  std::cout << "Atlas Tape tests: " << failures << " failures\n";
  return failures ? 1 : 0;
}
