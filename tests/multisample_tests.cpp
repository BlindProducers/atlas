// Multi-sample oscillator tests: SFZ, DecentSampler and SF2 parsing, decoding, playback,
// presets and host state. Fixtures are written to a temporary folder; no test
// depends on the large libraries the feature is meant for. `--manual <file>`
// loads a real instrument and prints what the loader found.

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_editor.h"
#include "sound_engine.h"
#include "source_types.h"
#include "multisample_slot.h"
#include "source_slot_module.h"
#include "multisample_instrument.h"
#include "multisample_loader.h"
#include "multisample_container.h"
#include "multisample_manager.h"
#include "load_save.h"
#include "decentsampler_parser.h"
#include "sf2_parser.h"
#include "sfz_parser.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vital::multisample;
namespace container = vital::multisample::container;

namespace {
  int failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void close(double actual, double expected, const std::string& message, double tolerance = 1.0e-4) {
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
           message + " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
  }

  int finish(const char* name) {
    if (failures == 0)
      std::cout << name << ": all tests passed\n";
    return failures == 0 ? 0 : 1;
  }

  // A fresh folder under the system temp directory, removed on exit.
  struct TempFolder {
    TempFolder() {
      folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getNonexistentChildFile("atlas-multisample-test", "", false);
      folder.createDirectory();
    }
    ~TempFolder() { folder.deleteRecursively(); }
    juce::File folder;
  };

  void writeText(const juce::File& file, const std::string& text) {
    file.getParentDirectory().createDirectory();
    file.replaceWithText(juce::String(text), false, false, "\n");
  }

  // A sine of `frequency` Hz, mono or stereo (right channel inverted).
  void writeWav(const juce::File& file, double frequency, int frames, int sample_rate = 48000, int channels = 1,
                int bits = 16, double amplitude = 0.5) {
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::AudioBuffer<float> buffer(channels, frames);
    for (int i = 0; i < frames; ++i) {
      const float value = static_cast<float>(amplitude * std::sin(2.0 * M_PI * frequency * i / sample_rate));
      buffer.setSample(0, i, value);
      if (channels > 1)
        buffer.setSample(1, i, -value);
    }
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                     .withSampleRate(sample_rate)
                                                     .withNumChannels(channels)
                                                     .withBitsPerSample(bits));
    expect(writer != nullptr, "WAV writer for " + file.getFileName().toStdString());
    if (writer)
      writer->writeFromAudioSampleBuffer(buffer, 0, frames);
  }

  void writeFlac(const juce::File& file, double frequency, int frames, int sample_rate = 48000) {
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::AudioBuffer<float> buffer(1, frames);
    for (int i = 0; i < frames; ++i)
      buffer.setSample(0, i, static_cast<float>(0.5 * std::sin(2.0 * M_PI * frequency * i / sample_rate)));
    juce::FlacAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                     .withSampleRate(sample_rate)
                                                     .withNumChannels(1)
                                                     .withBitsPerSample(24));
    expect(writer != nullptr, "FLAC writer");
    if (writer)
      writer->writeFromAudioSampleBuffer(buffer, 0, frames);
  }

  // ---- A minimal SoundFont writer ------------------------------------------

  struct Sf2Builder {
    struct Gen { uint16_t oper; uint16_t amount; };
    struct Zone { std::vector<Gen> gens; };
    struct PresetSpec { std::string name; uint16_t program; uint16_t bank; std::vector<Zone> zones; };
    struct InstrumentSpec { std::string name; std::vector<Zone> zones; };
    struct SampleSpec { std::string name; std::vector<int16_t> data; uint32_t rate; uint8_t pitch;
                        int8_t correction; uint32_t loop_start; uint32_t loop_end; };

    std::vector<PresetSpec> presets;
    std::vector<InstrumentSpec> instruments;
    std::vector<SampleSpec> samples;
    std::vector<uint8_t> sm24;

    static void id(juce::MemoryOutputStream& out, const char* text) { out.write(text, 4); }
    static void name20(juce::MemoryOutputStream& out, const std::string& text) {
      char buffer[20] = {};
      std::memcpy(buffer, text.data(), std::min<size_t>(19, text.size()));
      out.write(buffer, 20);
    }
    static void chunk(juce::MemoryOutputStream& out, const char* chunk_id, const juce::MemoryBlock& data) {
      id(out, chunk_id);
      out.writeInt(static_cast<int>(data.getSize()));
      out.write(data.getData(), data.getSize());
      if (data.getSize() & 1)
        out.writeByte(0);
    }
    static void list(juce::MemoryOutputStream& out, const char* type, const juce::MemoryBlock& data) {
      id(out, "LIST");
      out.writeInt(static_cast<int>(data.getSize() + 4));
      id(out, type);
      out.write(data.getData(), data.getSize());
    }

    juce::MemoryBlock build() const {
      // Sample data with 46 zero frames after each sample.
      juce::MemoryOutputStream smpl;
      std::vector<uint32_t> starts;
      for (const SampleSpec& sample : samples) {
        starts.push_back(static_cast<uint32_t>(smpl.getDataSize() / 2));
        for (int16_t value : sample.data)
          smpl.writeShort(value);
        for (int i = 0; i < 46; ++i)
          smpl.writeShort(0);
      }

      juce::MemoryOutputStream info;
      {
        juce::MemoryOutputStream ifil;
        ifil.writeShort(2);
        ifil.writeShort(4);
        chunk(info, "ifil", ifil.getMemoryBlock());
        juce::MemoryBlock inam("Test Font\0\0", 10);
        chunk(info, "INAM", inam);
      }

      juce::MemoryOutputStream sdta;
      chunk(sdta, "smpl", smpl.getMemoryBlock());
      if (!sm24.empty())
        chunk(sdta, "sm24", juce::MemoryBlock(sm24.data(), sm24.size()));

      juce::MemoryOutputStream phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
      int bag = 0, gen = 0;
      for (const PresetSpec& preset : presets) {
        name20(phdr, preset.name);
        phdr.writeShort(static_cast<short>(preset.program));
        phdr.writeShort(static_cast<short>(preset.bank));
        phdr.writeShort(static_cast<short>(bag));
        phdr.writeInt(0); phdr.writeInt(0); phdr.writeInt(0);
        for (const Zone& zone : preset.zones) {
          pbag.writeShort(static_cast<short>(gen));
          pbag.writeShort(0);
          for (const Gen& g : zone.gens) {
            pgen.writeShort(static_cast<short>(g.oper));
            pgen.writeShort(static_cast<short>(g.amount));
            gen++;
          }
          bag++;
        }
      }
      name20(phdr, "EOP");
      phdr.writeShort(0); phdr.writeShort(0); phdr.writeShort(static_cast<short>(bag));
      phdr.writeInt(0); phdr.writeInt(0); phdr.writeInt(0);
      pbag.writeShort(static_cast<short>(gen)); pbag.writeShort(0);
      pgen.writeShort(0); pgen.writeShort(0);
      for (int i = 0; i < 10; ++i) pmod.writeByte(0);

      bag = 0; gen = 0;
      for (const InstrumentSpec& instrument : instruments) {
        name20(inst, instrument.name);
        inst.writeShort(static_cast<short>(bag));
        for (const Zone& zone : instrument.zones) {
          ibag.writeShort(static_cast<short>(gen));
          ibag.writeShort(0);
          for (const Gen& g : zone.gens) {
            igen.writeShort(static_cast<short>(g.oper));
            igen.writeShort(static_cast<short>(g.amount));
            gen++;
          }
          bag++;
        }
      }
      name20(inst, "EOI");
      inst.writeShort(static_cast<short>(bag));
      ibag.writeShort(static_cast<short>(gen)); ibag.writeShort(0);
      igen.writeShort(0); igen.writeShort(0);
      for (int i = 0; i < 10; ++i) imod.writeByte(0);

      for (size_t i = 0; i < samples.size(); ++i) {
        const SampleSpec& sample = samples[i];
        name20(shdr, sample.name);
        shdr.writeInt(static_cast<int>(starts[i]));
        shdr.writeInt(static_cast<int>(starts[i] + sample.data.size()));
        shdr.writeInt(static_cast<int>(starts[i] + sample.loop_start));
        shdr.writeInt(static_cast<int>(starts[i] + sample.loop_end));
        shdr.writeInt(static_cast<int>(sample.rate));
        shdr.writeByte(static_cast<char>(sample.pitch));
        shdr.writeByte(static_cast<char>(sample.correction));
        shdr.writeShort(0);
        shdr.writeShort(1);
      }
      name20(shdr, "EOS");
      for (int i = 0; i < 26; ++i) shdr.writeByte(0);

      juce::MemoryOutputStream pdta;
      chunk(pdta, "phdr", phdr.getMemoryBlock());
      chunk(pdta, "pbag", pbag.getMemoryBlock());
      chunk(pdta, "pmod", pmod.getMemoryBlock());
      chunk(pdta, "pgen", pgen.getMemoryBlock());
      chunk(pdta, "inst", inst.getMemoryBlock());
      chunk(pdta, "ibag", ibag.getMemoryBlock());
      chunk(pdta, "imod", imod.getMemoryBlock());
      chunk(pdta, "igen", igen.getMemoryBlock());
      chunk(pdta, "shdr", shdr.getMemoryBlock());

      juce::MemoryOutputStream body;
      id(body, "sfbk");
      list(body, "INFO", info.getMemoryBlock());
      list(body, "sdta", sdta.getMemoryBlock());
      list(body, "pdta", pdta.getMemoryBlock());

      juce::MemoryOutputStream file;
      id(file, "RIFF");
      file.writeInt(static_cast<int>(body.getDataSize()));
      file.write(body.getData(), body.getDataSize());
      return file.getMemoryBlock();
    }
  };

  std::vector<int16_t> sineData(int frames, double cycles_per_frame, double amplitude = 16000.0) {
    std::vector<int16_t> data(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
      data[static_cast<size_t>(i)] = static_cast<int16_t>(amplitude * std::sin(2.0 * M_PI * cycles_per_frame * i));
    return data;
  }

  // Two presets: "Piano" (bank 0 program 0) with a global zone and two key
  // splits, and "Hats" (bank 128 program 0) with an exclusive class.
  Sf2Builder testFont() {
    Sf2Builder font;
    font.samples.push_back({ "Low", sineData(4800, 0.01), 48000, 57, 0, 1000, 3000 });
    font.samples.push_back({ "High", sineData(4800, 0.02), 44100, 69, -5, 0, 0 });

    Sf2Builder::InstrumentSpec piano { "PianoInst", {} };
    // Global zone: attack 0.5 s (-1200 tc), sustain 60 cB.
    piano.zones.push_back({ { { 34, static_cast<uint16_t>(-1200) }, { 37, 60 } } });
    piano.zones.push_back({ { { 43, (59 << 8) | 0 }, { 54, 1 }, { 53, 0 } } });
    piano.zones.push_back({ { { 43, (127 << 8) | 60 }, { 58, 72 }, { 51, 1 }, { 52, 10 }, { 17, 250 },
                              { 48, 100 }, { 53, 1 } } });
    font.instruments.push_back(piano);

    Sf2Builder::InstrumentSpec hats { "HatInst", {} };
    hats.zones.push_back({ { { 43, (42 << 8) | 42 }, { 57, 3 }, { 53, 1 } } });
    hats.zones.push_back({ { { 43, (46 << 8) | 46 }, { 57, 3 }, { 53, 1 } } });
    font.instruments.push_back(hats);

    // Preset 0: a global zone (fine tune +3) and an instrument zone limited to key 70 and below.
    font.presets.push_back({ "Piano", 0, 0, { { { { 52, 3 } } },
                                              { { { 43, (70 << 8) | 0 }, { 41, 0 } } } } });
    font.presets.push_back({ "Hats", 0, 128, { { { { 41, 1 } } } } });
    return font;
  }

  // Another SoundFont with other presets: "Strings", "Kit" and "Bass" in bank 0.
  Sf2Builder otherFont() {
    Sf2Builder font = testFont();
    font.presets[0].name = "Strings";
    font.presets[1] = { "Kit", 1, 0, font.presets[1].zones };
    font.presets.push_back({ "Bass", 2, 0, font.presets[0].zones });
    return font;
  }

  void writeFont(const Sf2Builder& font, const juce::File& file) {
    auto block = font.build();
    file.replaceWithData(block.getData(), block.getSize());
  }

  // ---- SFZ parser -------------------------------------------------------------

  int sfzParser() {
    TempFolder temp;
    const juce::File root = temp.folder.getChildFile("Lib");
    const juce::File programs = root.getChildFile("Programs");
    writeWav(root.getChildFile("Samples/a b.wav"), 440.0, 4800);
    writeWav(root.getChildFile("Samples/c.wav"), 440.0, 4800);
    writeText(programs.getChildFile("keymap/keys.sfz"), "#define $KICK 36\n#define $KICKER 40\n");
    writeText(programs.getChildFile("mappings/inner.sfz"),
              "<region> sample=..\\Samples\\c.wav key=$KICKER\n");
    writeText(programs.getChildFile("mappings/map.sfz"),
              "<master> amplitude_cc70=100 locc70=1\n"
              "<group> group=2 off_by=3\n"
              "<region> sample=../Samples/a b.wav key=$KICK seq_length=2 seq_position=2 // comment\n"
              "#include \"mappings/inner.sfz\"\n");
    writeText(programs.getChildFile("main.sfz"),
              "/* block\n comment */\n"
              "<control> set_cc70=100 label_cc70=Kick mic\n"
              "#include \"keymap/keys.sfz\"\n"
              "<global> loop_mode=one_shot ampeg_release=0.25 amp_velcurve_1=0.4\n"
              "#include \"mappings/map.sfz\"\n"
              "<master> transpose=12 volume=-6 tune=-20\n"
              "<region> sample=../Samples/c.wav lokey=c4 hikey=e4 pitch_keycenter=d4 lovel=64 trigger=release rt_decay=6\n"
              "<effect> type=com.mda.Limiter\n"
              "<curve> curve_index=7 v000=0\n"
              "<region> sample=*sine key=50\n"
              "<global> fil_type=lpf_2p cutoff=500 eq1_gain_oncc77=-18 eq2_gain_oncc80=1\n"
              "<region> sample=../Samples/c.wav lokey=-1 hikey=-1 on_locc64=126 on_hicc64=127 group=1 off_by=2\n");

    SfzParser::Result result = SfzParser::parse(programs.getChildFile("main.sfz"));
    expect(result.error.empty(), "SFZ parses: " + result.error);
    const Instrument& instrument = *result.instrument;
    expect(instrument.regions.size() == 4, "four regions (got " + std::to_string(instrument.regions.size()) + ")");
    expect(result.text_files.size() == 4, "main file and three includes read");
    expect(result.samples.size() == 2, "two distinct samples");
    close(instrument.cc_defaults[70], 100.0, "set_cc70");
    expect(instrument.cc_labels.count(70) && instrument.cc_labels.at(70) == "Kick mic", "label_cc70 keeps spaces");

    if (instrument.regions.size() == 4) {
      const Region& kick = instrument.regions[0];
      expect(kick.lokey == 36 && kick.hikey == 36, "#define substituted from an included file");
      close(kick.pitch_keycenter, 36.0, "key= sets the key centre");
      expect(juce::File(kick.sample_path).getFileName() == "a b.wav", "sample names may contain spaces");
      expect(kick.loop_mode == LoopMode::kOneShot && kick.loop_mode_set, "global loop_mode inherited");
      close(kick.ampeg.release, 0.25, "global ampeg_release inherited");
      expect(kick.amplitude_cc.size() == 1 && kick.amplitude_cc[0].cc == 70, "master amplitude_cc70");
      close(kick.amplitude_cc[0].amount, 1.0, "amplitude_cc70 is 100%");
      expect(kick.cc_gates.size() == 1 && kick.cc_gates[0].low == 1.0f && kick.cc_gates[0].high == 127.0f,
             "locc70 gate");
      expect(kick.group == 2 && kick.off_by == 3, "group opcodes inherited");
      expect(kick.seq_length == 2 && kick.seq_position == 2, "round robin");
      close(kick.velocity_gain[1], 0.4, "amp_velcurve_1");
      close(kick.velocity_gain[127], 1.0, "velcurve default end");

      const Region& inner = instrument.regions[1];
      expect(inner.lokey == 40, "longest #define wins ($KICKER over $KICK)");
      expect(juce::File(inner.sample_path).getFileName() == "c.wav", "backslash paths normalised");
      expect(inner.group == 2, "group keeps applying to later regions");

      const Region& release = instrument.regions[2];
      expect(release.lokey == 60 && release.hikey == 64, "note names");
      close(release.pitch_keycenter, 62.0, "pitch_keycenter note name");
      expect(release.trigger == Trigger::kRelease, "trigger=release");
      close(release.rt_decay, 6.0, "rt_decay");
      expect(release.transpose == 12, "transpose");
      close(release.tune_cents, -20.0, "tune");
      close(release.volume_db, -6.0, "volume");
      expect(release.group == 0 && release.amplitude_cc.empty(), "a new <master> resets the group level");
      expect(release.lovel == 64, "lovel");

      const Region& pedal = instrument.regions[3];
      expect(pedal.trigger == Trigger::kCc && pedal.on_cc == 64, "on_locc64 makes a CC-triggered region");
      close(pedal.on_low, 126.0, "on_locc");
      close(pedal.on_high, 127.0, "on_hicc");
      expect(pedal.lokey == -1 && pedal.hikey == -1, "hikey=-1");
      expect(pedal.loop_mode_set == false, "a new <global> resets inherited opcodes");
    }

    bool ignored_eq = false;
    bool ignored_effect = false;
    for (const std::string& warning : instrument.warnings) {
      ignored_eq = ignored_eq || warning.find("eqN_gain_onccN (2 uses)") != std::string::npos;
      ignored_effect = ignored_effect || warning.find("<effect>") != std::string::npos;
    }
    expect(ignored_eq, "unsupported opcodes are counted by family");
    expect(ignored_effect, "ignored blocks are reported");

    // Circular includes are reported, not followed forever.
    writeText(root.getChildFile("loop.sfz"), "#include \"loop.sfz\"\n<region> sample=Samples/c.wav\n");
    SfzParser::Result looped = SfzParser::parse(root.getChildFile("loop.sfz"));
    expect(looped.instrument->regions.size() == 1, "circular include parsed once");

    // default_path and a case-insensitive fallback.
    writeText(root.getChildFile("paths.sfz"), "<control> default_path=samples/\n<region> sample=C.WAV\n");
    SfzParser::Result paths = SfzParser::parse(root.getChildFile("paths.sfz"));
    expect(paths.samples.size() == 1 && paths.samples[0].file.existsAsFile(),
           "default_path with a case-insensitive fallback");

    // Search roots: an edited working copy first, samples from the original.
    const juce::File working = temp.folder.getChildFile("Working");
    writeText(working.getChildFile("paths.sfz"), "<region> sample=Samples/c.wav key=99\n");
    SfzParser::Options options;
    options.search_roots = { working, root };
    SfzParser::Result overlaid = SfzParser::parse(working.getChildFile("paths.sfz"), options);
    expect(overlaid.samples.size() == 1 && overlaid.samples[0].file == root.getChildFile("Samples/c.wav"),
           "search roots fall back to the original folder for samples");
    return finish("sfz parser");
  }

  // ---- SF2 parser -------------------------------------------------------------

  int sf2Parser() {
    TempFolder temp;
    const juce::File file = temp.folder.getChildFile("test.sf2");
    Sf2Builder font = testFont();
    file.replaceWithData(font.build().getData(), font.build().getSize());

    std::string error;
    std::unique_ptr<Sf2File> sf2 = Sf2File::open(file, error);
    expect(sf2 != nullptr, "SF2 opens: " + error);
    if (sf2 == nullptr)
      return finish("sf2 parser");
    expect(sf2->name() == "Test Font", "INAM name");
    expect(sf2->presets().size() == 2, "two presets");
    expect(sf2->presets()[0].name == "Piano" && sf2->presets()[1].bank == 128, "presets sorted by bank");
    expect(sf2->findPreset(128, 0, "", -1) == 1, "find preset by bank and program");
    expect(sf2->findPreset(5, 5, "Hats", -1) == 1, "find preset by name");

    auto pool = sf2->readSamplePool(nullptr, error);
    expect(pool != nullptr && pool->size() == 2 * (4800 + 46), "sample pool size");
    auto piano = sf2->buildPreset(0, pool);
    expect(piano->regions.size() == 2, "piano zones");
    if (piano->regions.size() == 2) {
      const Region& low = piano->regions[0];
      expect(low.lokey == 0 && low.hikey == 59, "instrument key range");
      close(low.pitch_keycenter, 57.0, "original pitch is the root");
      close(low.tune_cents, 3.0, "preset fine tune adds to the instrument");
      close(low.ampeg.attack, 0.5, "global zone attack -1200 timecents", 1e-4);
      close(low.ampeg.sustain, std::pow(10.0, -6.0 / 20.0), "sustain 60 cB", 1e-4);
      expect(low.loop_mode == LoopMode::kContinuous, "sampleModes 1 loops");
      expect(low.loop_start == 1000 && low.loop_end == 3000, "loop points");
      expect(low.offset == 0 && low.end == 4800, "sample range");
      close(low.sample_rate, 48000.0, "sample rate");

      const Region& high = piano->regions[1];
      expect(high.lokey == 60 && high.hikey == 70, "preset and instrument key ranges intersect");
      close(high.pitch_keycenter, 72.0, "overridingRootKey");
      close(high.tune_cents, 100.0 + 10.0 + 3.0 - 5.0, "coarse, fine, preset tune and pitch correction");
      close(high.pan, 0.5, "pan 250 is half right");
      close(high.volume_db, -0.4 * 10.0, "initialAttenuation 100 cB with the 0.4 factor");
      expect(high.offset == 4800 + 46, "second sample offset in the pool");
      expect(high.loop_mode == LoopMode::kNoLoop, "no loop by default");
      close(high.velocity_gain[64], (64.0 / 127.0) * (64.0 / 127.0), "default velocity curve", 1e-5);
    }

    auto hats = sf2->buildPreset(1, pool);
    expect(hats->regions.size() == 2, "hat zones");
    if (hats->regions.size() == 2) {
      expect(hats->regions[0].group != 0 && hats->regions[0].group == hats->regions[0].off_by &&
             hats->regions[0].group == hats->regions[1].group, "exclusiveClass chokes its own class");
      expect(hats->regions[0].group_slot == hats->regions[1].off_by_slot, "dense group slots");
    }
    expect(hats->attack_by_key[42].size() == 1 && hats->attack_by_key[46].size() == 1, "key lookup tables");

    // 24-bit data rounds to 16 bits.
    Sf2Builder wide;
    wide.samples.push_back({ "W", { 100, -100, 32767, 0 }, 44100, 60, 0, 0, 0 });
    wide.sm24.resize(4 + 46);
    wide.sm24[0] = 0x80;
    wide.sm24[1] = 0x7f;
    wide.sm24[2] = 0xff;
    wide.instruments.push_back({ "I", { { { { 53, 0 } } } } });
    wide.presets.push_back({ "P", 0, 0, { { { { 41, 0 } } } } });
    const juce::File wide_file = temp.folder.getChildFile("wide.sf2");
    auto wide_block = wide.build();
    wide_file.replaceWithData(wide_block.getData(), wide_block.getSize());
    auto wide_sf2 = Sf2File::open(wide_file, error);
    auto wide_pool = wide_sf2 ? wide_sf2->readSamplePool(nullptr, error) : nullptr;
    expect(wide_pool != nullptr, "24-bit SF2 opens");
    if (wide_pool) {
      expect((*wide_pool)[0] == 101, "sm24 0x80 rounds up");
      expect((*wide_pool)[1] == -100, "sm24 0x7f rounds down");
      expect((*wide_pool)[2] == 32767, "sm24 saturates");
    }

    // Damaged files fail cleanly.
    const juce::File broken = temp.folder.getChildFile("broken.sf2");
    broken.replaceWithText("RIFF1234sfbk");
    expect(Sf2File::open(broken, error) == nullptr && !error.empty(), "truncated SF2 is rejected");
    return finish("sf2 parser");
  }

  // ---- Decoding and loading ---------------------------------------------------

  int decode() {
    TempFolder temp;
    const juce::File folder = temp.folder;

    // 24-bit WAV rounds to the nearest 16-bit value.
    writeWav(folder.getChildFile("w24.wav"), 1000.0, 4800, 48000, 2, 24, 0.5);
    juce::MemoryBlock bytes;
    folder.getChildFile("w24.wav").loadFileAsData(bytes);
    std::string error;
    auto data = decodeSample(bytes.getData(), bytes.getSize(), error);
    expect(data != nullptr, "24-bit WAV decodes: " + error);
    if (data) {
      expect(data->num_channels == 2 && data->frames == 4800, "channels and length");
      const double expected = 0.5 * std::sin(2.0 * M_PI * 1000.0 * 10 / 48000.0) * 32767.0;
      close(data->channels[0][10], expected, "24-bit value rounded", 1.01);
      close(data->channels[1][10], -expected, "right channel", 1.01);
    }

    writeFlac(folder.getChildFile("f.flac"), 500.0, 4800);
    juce::MemoryBlock flac;
    folder.getChildFile("f.flac").loadFileAsData(flac);
    auto flac_data = decodeSample(flac.getData(), flac.getSize(), error);
    expect(flac_data != nullptr && flac_data->frames == 4800, "FLAC decodes");

    // CRC32 of "123456789" is the standard check value.
    expect(crc32("123456789", 9) == 0xCBF43926u, "CRC32 check value");

    // A full SFZ load with a missing sample and a reload that reuses audio.
    writeWav(folder.getChildFile("lib/s/one.wav"), 440.0, 4800);
    writeText(folder.getChildFile("lib/p/inst.sfz"),
              "<region> sample=../s/one.wav key=60\n<region> sample=../s/missing.wav key=61\n");
    LoadRequest request;
    request.file = folder.getChildFile("lib/p/inst.sfz");
    auto result = loadInstrument(request, nullptr);
    expect(result && result->error.empty(), "SFZ loads with a missing sample: " + (result ? result->error : ""));
    if (result && result->instrument) {
      expect(result->instrument->regions.size() == 1, "the missing sample's region is dropped");
      expect(result->instrument->missing_files.size() == 1, "missing file reported");
      expect(result->entry == "p/inst.sfz", "entry relative to the common root");
      expect(result->files.size() == 2, "text and sample files listed");
      expect(result->hash.size() == 32, "hash is 32 hex characters");

      LoadRequest again = request;
      again.previous = result;
      auto reloaded = loadInstrument(again, nullptr);
      expect(reloaded && reloaded->instrument &&
             reloaded->instrument->regions[0].sample == result->instrument->regions[0].sample,
             "a reload shares decoded audio");
      expect(reloaded && reloaded->hash == result->hash, "the hash is stable");
    }

    // Moving the whole library keeps the hash.
    folder.getChildFile("lib").copyDirectoryTo(folder.getChildFile("moved"));
    LoadRequest moved;
    moved.file = folder.getChildFile("moved/p/inst.sfz");
    auto moved_result = loadInstrument(moved, nullptr);
    expect(moved_result && result && moved_result->hash == result->hash, "hash ignores the library's location");

    // Loops from the file are used when the SFZ names none.
    {
      juce::WavAudioFormat format;
      std::unordered_map<juce::String, juce::String> metadata {
        { "NumSampleLoops", "1" }, { "Loop0Start", "100" }, { "Loop0End", "199" }, { "Loop0Type", "0" },
        { "MidiUnityNote", "64" } };
      juce::AudioBuffer<float> buffer(1, 1000);
      buffer.clear();
      const juce::File looped = folder.getChildFile("looped.wav");
      std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(looped);
      auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate(48000)
                                                       .withNumChannels(1)
                                                       .withBitsPerSample(16)
                                                       .withMetadataValues(metadata));
      if (writer)
        writer->writeFromAudioSampleBuffer(buffer, 0, 1000);
      writer.reset();
      writeText(folder.getChildFile("looped.sfz"), "<region> sample=looped.wav pitch_keycenter=sample\n");
      LoadRequest looped_request;
      looped_request.file = folder.getChildFile("looped.sfz");
      auto looped_result = loadInstrument(looped_request, nullptr);
      expect(looped_result && looped_result->instrument && looped_result->instrument->regions.size() == 1,
             "looped sample loads");
      if (looped_result && looped_result->instrument && !looped_result->instrument->regions.empty()) {
        const Region& region = looped_result->instrument->regions[0];
        expect(region.loop_mode == LoopMode::kContinuous, "file loop makes the region loop");
        expect(region.loop_start == 100 && region.loop_end == 200, "file loop points (end exclusive)");
        close(region.pitch_keycenter, 64.0, "pitch_keycenter=sample reads the unity note");
      }
    }

    // SF2 loads, then switches presets sharing the pool.
    Sf2Builder font = testFont();
    const juce::File sf2_file = folder.getChildFile("font.sf2");
    auto block = font.build();
    sf2_file.replaceWithData(block.getData(), block.getSize());
    LoadRequest sf2_request;
    sf2_request.file = sf2_file;
    sf2_request.sf2_bank = 128;
    sf2_request.sf2_program = 0;
    auto sf2_result = loadInstrument(sf2_request, nullptr);
    expect(sf2_result && sf2_result->error.empty() && sf2_result->instrument, "SF2 loads");
    if (sf2_result && sf2_result->instrument) {
      expect(sf2_result->instrument->preset_index == 1, "requested preset chosen");
      auto switched = withSf2Preset(*sf2_result, 0);
      expect(switched->sf2_pool == sf2_result->sf2_pool, "preset switch shares the pool");
      expect(switched->instrument->regions.size() == 2, "switched preset regions");
      expect(switched->hash == sf2_result->hash, "preset switch keeps the hash");
    }
    return finish("decode");
  }


  // ---- Playback through the plugin ---------------------------------------------

  constexpr int kRate = 48000;

  struct MidiEvent {
    int sample;
    juce::MidiMessage message;
  };

  void setControl(SynthPlugin& plugin, const std::string& name, float value) {
    auto& controls = plugin.getControls();
    auto found = controls.find(name);
    expect(found != controls.end(), "control exists: " + name);
    if (found != controls.end())
      found->second->set(value);
  }

  // Slot 1 is a Multi-sample oscillator straight to the effects (no filter),
  // every effect and other oscillator off, Env 1 with a near-zero release.
  void preparePlayback(SynthPlugin& plugin) {
    plugin.setPlayConfigDetails(0, 2, kRate, 128);
    plugin.prepareToPlay(kRate, 128);
    setControl(plugin, "oversampling", 0.0f);
    for (int chain = 0; chain < 4; ++chain) {
      const auto prefix = chain == 0 ? std::string() : "bus_" + std::to_string(chain) + "_";
      for (int i = 1; i <= 20; ++i)
        setControl(plugin, prefix + "fx_slot_" + std::to_string(i) + "_type", 0.0f);
    }
    for (int i = 1; i <= 8; ++i)
      setControl(plugin, "osc_" + std::to_string(i) + "_on", 0.0f);
    plugin.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kMultiSample));
    setControl(plugin, "osc_1_on", 1.0f);
    setControl(plugin, "osc_1_destination", static_cast<float>(vital::constants::kEffects));
    setControl(plugin, "env_1_attack", 0.0f);
    setControl(plugin, "env_1_release", 0.0f);
    plugin.checkOversampling();
    expect(plugin.getEngine()->getSourceSlot(0)->activeType() == vital::source_types::kMultiSample,
           "slot 1 runs the Multi-sample type");
  }

  void setInstrument(SynthPlugin& plugin, int slot, std::shared_ptr<const Instrument> instrument) {
    plugin.pauseProcessing(true);
    plugin.getEngine()->getMultiSample(slot)->setInstrument(std::move(instrument));
    plugin.pauseProcessing(false);
  }

  std::shared_ptr<const Instrument> loadSfz(const juce::File& file) {
    LoadRequest request;
    request.file = file;
    auto result = loadInstrument(request, nullptr);
    expect(result && result->error.empty() && result->instrument,
           "instrument loads: " + (result ? result->error : std::string("cancelled")));
    return result ? result->instrument : nullptr;
  }

  // Renders `samples` frames (left channel) with MIDI events at given samples.
  std::vector<float> render(SynthPlugin& plugin, int samples, std::vector<MidiEvent> events) {
    std::vector<float> result(static_cast<size_t>(samples));
    for (int offset = 0; offset < samples;) {
      const int count = std::min(128, samples - offset);
      juce::AudioBuffer<float> audio(2, count);
      audio.clear();
      juce::MidiBuffer midi;
      for (const MidiEvent& event : events) {
        if (event.sample >= offset && event.sample < offset + count)
          midi.addEvent(event.message, event.sample - offset);
      }
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i)
        result[static_cast<size_t>(offset + i)] = audio.getSample(0, i);
      offset += count;
    }
    return result;
  }

  double rms(const std::vector<float>& signal, int start, int end) {
    double sum = 0.0;
    end = std::min<int>(end, static_cast<int>(signal.size()));
    for (int i = start; i < end; ++i)
      sum += double(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
    return end > start ? std::sqrt(sum / (end - start)) : 0.0;
  }

  double frequency(const std::vector<float>& signal, int start, int end) {
    int first = -1;
    int last = -1;
    int crossings = 0;
    for (int i = start + 1; i < end; ++i) {
      if (signal[static_cast<size_t>(i - 1)] <= 0.0f && signal[static_cast<size_t>(i)] > 0.0f) {
        if (first < 0)
          first = i;
        last = i;
        crossings++;
      }
    }
    return crossings > 1 ? (crossings - 1) * double(kRate) / (last - first) : 0.0;
  }

  int pitch() {
    TempFolder temp;
    writeWav(temp.folder.getChildFile("a440.wav"), 440.0, kRate * 2);
    writeText(temp.folder.getChildFile("pitch.sfz"),
              "<region> sample=a440.wav lokey=30 pitch_keycenter=69 loop_mode=no_loop\n"
              "<region> sample=a440.wav key=20 pitch_keytrack=0\n"
              "<region> sample=a440.wav key=21 pitch_keycenter=69 tune=100\n"
              "<region> sample=a440.wav key=22 pitch_keycenter=69 transpose=-12\n");
    auto instrument = loadSfz(temp.folder.getChildFile("pitch.sfz"));
    if (!instrument)
      return finish("pitch");

    auto measure = [&](int note) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setInstrument(plugin, 0, instrument);
      auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, note, 1.0f) } });
      return frequency(audio, kRate / 10, kRate / 2);
    };
    close(measure(69), 440.0, "the key centre plays at the recorded pitch", 1.0);
    close(measure(81), 880.0, "an octave up plays an octave higher", 2.0);
    close(measure(57), 220.0, "an octave down", 1.0);
    // key=20 also sets the key centre to 20; pitch_keytrack=0 keeps it fixed.
    close(measure(20), 440.0, "key= with no keytracking", 1.0);
    close(measure(21), 440.0 * std::pow(2.0, (21 - 69 + 1) / 12.0), "tune=100 adds a semitone", 0.5);
    close(measure(22), 440.0 * std::pow(2.0, (22 - 69 - 12) / 12.0), "transpose", 0.5);

    // Slot transpose shifts every region.
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setInstrument(plugin, 0, instrument);
      setControl(plugin, "osc_1_multisample_transpose", 12.0f);
      auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 69, 1.0f) } });
      close(frequency(audio, kRate / 10, kRate / 2), 880.0, "slot transpose", 2.0);
    }

    // SF2: root key and tuning from the font.
    {
      Sf2Builder font;
      font.samples.push_back({ "S", sineData(kRate, 440.0 / kRate), kRate, 69, 0, 0, 0 });
      font.instruments.push_back({ "I", { { { { 53, 0 } } } } });
      font.presets.push_back({ "P", 0, 0, { { { { 41, 0 } } } } });
      const juce::File file = temp.folder.getChildFile("pitch.sf2");
      auto block = font.build();
      file.replaceWithData(block.getData(), block.getSize());
      LoadRequest request;
      request.file = file;
      auto result = loadInstrument(request, nullptr);
      expect(result && result->instrument, "pitch SF2 loads");
      if (result && result->instrument) {
        SynthPlugin plugin;
        preparePlayback(plugin);
        setInstrument(plugin, 0, result->instrument);
        auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 76, 1.0f) } });
        close(frequency(audio, kRate / 20, kRate / 4), 440.0 * std::pow(2.0, 7 / 12.0), "SF2 transposes from its root", 2.0);
      }
    }
    return finish("pitch");
  }

  int select() {
    TempFolder temp;
    writeWav(temp.folder.getChildFile("a.wav"), 300.0, kRate / 4);
    writeWav(temp.folder.getChildFile("b.wav"), 600.0, kRate / 4);
    writeWav(temp.folder.getChildFile("c.wav"), 1200.0, kRate / 4);
    writeText(temp.folder.getChildFile("select.sfz"),
              "<global> pitch_keycenter=60 pitch_keytrack=0 loop_mode=one_shot\n"
              "<group> key=60 seq_length=2\n"
              "<region> sample=a.wav seq_position=1\n"
              "<region> sample=b.wav seq_position=2\n"
              "<group> key=62 pitch_keycenter=62\n"
              "<region> sample=a.wav hivel=63\n"
              "<region> sample=c.wav lovel=64\n"
              "<group> key=64 pitch_keycenter=64\n"
              "<region> sample=b.wav locc1=64\n"
              "<group> key=65 pitch_keycenter=65\n"
              "<region> sample=a.wav sw_last=24\n"
              "<region> sample=c.wav sw_last=25\n"
              "<control> sw_default=24\n"
              "<region> key=24\n<region> key=25\n");
    auto instrument = loadSfz(temp.folder.getChildFile("select.sfz"));
    if (!instrument)
      return finish("select");

    SynthPlugin plugin;
    preparePlayback(plugin);
    setInstrument(plugin, 0, instrument);
    auto hit = [&](int note, float velocity) {
      auto audio = render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOn(1, note, velocity) },
                                               { kRate / 5, juce::MidiMessage::noteOff(1, note) } });
      render(plugin, kRate / 5, {});
      return frequency(audio, kRate / 50, kRate / 6);
    };
    close(hit(60, 1.0f), 300.0, "round robin first", 2.0);
    close(hit(60, 1.0f), 600.0, "round robin second", 2.0);
    close(hit(60, 1.0f), 300.0, "round robin wraps", 2.0);
    close(hit(62, 0.3f), 300.0, "soft velocity layer", 2.0);
    close(hit(62, 1.0f), 1200.0, "loud velocity layer", 3.0);

    auto quiet = render(plugin, kRate / 4, { { 0, juce::MidiMessage::noteOn(1, 64, 1.0f) },
                                             { kRate / 5, juce::MidiMessage::noteOff(1, 64) } });
    expect(rms(quiet, 0, kRate / 5) < 1.0e-6, "a CC-gated region is silent while the CC is low");
    render(plugin, kRate / 5, {});
    auto loud = render(plugin, kRate / 4, { { 0, juce::MidiMessage::controllerEvent(1, 1, 100) },
                                            { 10, juce::MidiMessage::noteOn(1, 64, 1.0f) },
                                            { kRate / 5, juce::MidiMessage::noteOff(1, 64) } });
    expect(rms(loud, 0, kRate / 5) > 0.01, "the region plays once the CC is in range");
    render(plugin, kRate / 10, { { 0, juce::MidiMessage::controllerEvent(1, 1, 0) } });

    close(hit(65, 1.0f), 300.0, "sw_default selects the first articulation", 2.0);
    hit(25, 1.0f);
    close(hit(65, 1.0f), 1200.0, "a keyswitch selects the second articulation", 3.0);
    return finish("select");
  }

  int envelopeMode() {
    TempFolder temp;
    writeWav(temp.folder.getChildFile("long.wav"), 220.0, kRate);
    writeText(temp.folder.getChildFile("shot.sfz"),
              "<region> sample=long.wav key=60 loop_mode=one_shot\n"
              "<region> sample=long.wav key=62 ampeg_release=0.3\n");
    auto instrument = loadSfz(temp.folder.getChildFile("shot.sfz"));
    if (!instrument)
      return finish("envelope mode");

    for (int mode = 0; mode < 2; ++mode) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setControl(plugin, "osc_1_multisample_amp_env", static_cast<float>(mode));
      setInstrument(plugin, 0, instrument);
      auto audio = render(plugin, kRate * 3 / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                   { kRate / 20, juce::MidiMessage::noteOff(1, 60) } });
      const double tail = rms(audio, kRate / 2, kRate * 9 / 10);
      if (mode == 0) {
        expect(tail > 0.01, "Instrument mode: a one-shot plays out after a short note");
        expect(rms(audio, kRate * 11 / 10, kRate * 3 / 2) == 0.0, "the voice ends with the sample");
        expect(plugin.getEngine()->getNumActiveVoices() == 0, "no voice is left after the one-shot");
      }
      else
        expect(tail < 1.0e-4, "Envelope 1 mode: Env 1 cuts the one-shot");

      auto released = render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, 62, 1.0f) },
                                              { kRate / 10, juce::MidiMessage::noteOff(1, 62) } });
      const double during = rms(released, kRate / 10 + kRate / 20, kRate / 10 + kRate / 10);
      const double held = rms(released, kRate / 20, kRate / 10);
      // A 0.3 s release to -96 dB is about 24 dB down 50-100 ms after note-off.
      if (mode == 0)
        close(20.0 * std::log10(during / held), -24.0, "Instrument mode: the region's own release sounds", 3.0);
      else
        expect(during < held * 1.0e-3, "Envelope 1 mode: Env 1's release wins");
    }
    // Other oscillators keep following Env 1 while an instrument holds the voice.
    for (int mode = 0; mode < 2; ++mode) {
      auto run = [&](bool instrument_on, bool wavetable_on) {
        SynthPlugin plugin;
        preparePlayback(plugin);
        setControl(plugin, "osc_1_multisample_amp_env", static_cast<float>(mode));
        setControl(plugin, "osc_1_on", instrument_on ? 1.0f : 0.0f);
        setControl(plugin, "osc_2_on", wavetable_on ? 1.0f : 0.0f);
        setControl(plugin, "osc_2_random_phase", 0.0f);
        setControl(plugin, "osc_2_destination", static_cast<float>(vital::constants::kEffects));
        setControl(plugin, "env_1_sustain", 0.25f);
        setInstrument(plugin, 0, instrument);
        return render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                       { kRate / 2, juce::MidiMessage::noteOff(1, 60) } });
      };
      const auto both = run(true, true);
      const auto instrument_only = run(true, false);
      const auto oscillator_only = run(false, true);
      std::vector<float> difference(both.size());
      for (size_t i = 0; i < both.size(); ++i)
        difference[i] = both[i] - instrument_only[i] - oscillator_only[i];
      const std::string label = mode == 0 ? "Instrument mode" : "Envelope 1 mode";
      const double held = rms(oscillator_only, kRate / 4, kRate / 2);
      expect(held > 0.01, label + ": the wavetable oscillator sounds");
      expect(rms(difference, kRate / 4, kRate / 2) < held * 1.0e-3,
             label + ": another oscillator keeps Env 1's sustain while the instrument holds the voice");
      expect(rms(both, kRate / 2 + kRate / 10, kRate * 9 / 10) <=
                 rms(instrument_only, kRate / 2 + kRate / 10, kRate * 9 / 10) * 1.001 + 1.0e-6,
             label + ": another oscillator stops at Env 1's release");
    }
    return finish("envelope mode");
  }

  int releaseTrigger() {
    TempFolder temp;
    writeWav(temp.folder.getChildFile("body.wav"), 220.0, kRate * 2);
    writeWav(temp.folder.getChildFile("rel.wav"), 880.0, kRate / 2);
    writeText(temp.folder.getChildFile("rel.sfz"),
              "<region> sample=body.wav key=60 ampeg_release=0.01\n"
              "<region> sample=rel.wav key=60 trigger=release rt_decay=20\n");
    auto instrument = loadSfz(temp.folder.getChildFile("rel.sfz"));
    if (!instrument)
      return finish("release trigger");

    auto run = [&](int held) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setInstrument(plugin, 0, instrument);
      return render(plugin, held + kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                { held, juce::MidiMessage::noteOff(1, 60) } });
    };
    auto short_note = run(kRate / 5);
    close(frequency(short_note, kRate / 5 + kRate / 20, kRate / 5 + kRate / 4), 880.0,
          "the release sample plays after note-off", 3.0);
    expect(rms(short_note, kRate / 10, kRate / 5 - 100) > 0.01, "the body plays while held");
    auto long_note = run(kRate);
    const double short_level = rms(short_note, kRate / 5 + kRate / 20, kRate / 5 + kRate / 10);
    const double long_level = rms(long_note, kRate + kRate / 20, kRate + kRate / 10);
    close(20.0 * std::log10(long_level / short_level), -16.0, "rt_decay lowers later releases by 20 dB/s", 1.0);

    // The sustain pedal holds the note: the release sample waits for pedal up.
    SynthPlugin plugin;
    preparePlayback(plugin);
    setInstrument(plugin, 0, instrument);
    auto pedal = render(plugin, kRate, { { 0, juce::MidiMessage::controllerEvent(1, 64, 127) },
                                         { 10, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                         { kRate / 10, juce::MidiMessage::noteOff(1, 60) },
                                         { kRate / 2, juce::MidiMessage::controllerEvent(1, 64, 0) } });
    close(frequency(pedal, kRate / 5, kRate * 2 / 5), 220.0, "the body sustains under the pedal", 2.0);
    close(frequency(pedal, kRate / 2 + kRate / 20, kRate * 3 / 4), 880.0, "release sample at pedal up", 3.0);
    return finish("release trigger");
  }

  int choke() {
    TempFolder temp;
    writeWav(temp.folder.getChildFile("open.wav"), 500.0, kRate);
    writeText(temp.folder.getChildFile("hat.sfz"),
              "<group> loop_mode=one_shot\n"
              "<region> sample=open.wav key=46 group=1 off_by=2\n"
              "<region> sample=*silence key=42 group=2\n"
              "<region> sample=open.wav key=36 group=3 off_by=3 off_mode=normal ampeg_release=0.2\n");
    auto instrument = loadSfz(temp.folder.getChildFile("hat.sfz"));
    if (!instrument)
      return finish("choke");

    SynthPlugin plugin;
    preparePlayback(plugin);
    setInstrument(plugin, 0, instrument);
    auto open = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 46, 1.0f) },
                                            { 100, juce::MidiMessage::noteOff(1, 46) } });
    expect(rms(open, kRate / 4, kRate / 2) > 0.01, "the open hat rings on its own");
    render(plugin, kRate, {});

    auto choked = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 46, 1.0f) },
                                              { 100, juce::MidiMessage::noteOff(1, 46) },
                                              { kRate / 5, juce::MidiMessage::noteOn(1, 42, 1.0f) },
                                              { kRate / 5 + 100, juce::MidiMessage::noteOff(1, 42) } });
    expect(rms(choked, kRate / 10, kRate / 5 - 200) > 0.01, "the open hat sounds before the closed one");
    expect(rms(choked, kRate / 5 + kRate / 50, kRate / 2) < 1.0e-5, "a silent closed hat chokes the open hat");
    render(plugin, kRate, {});

    // Self-choke with off_mode=normal fades over the region's release.
    auto self = render(plugin, kRate, { { 0, juce::MidiMessage::noteOn(1, 36, 1.0f) },
                                        { 100, juce::MidiMessage::noteOff(1, 36) },
                                        { kRate / 4, juce::MidiMessage::noteOn(1, 36, 1.0f) },
                                        { kRate / 4 + 100, juce::MidiMessage::noteOff(1, 36) } });
    expect(rms(self, kRate / 2, kRate) > 0.01, "the second hit plays");
    expect(plugin.getEngine()->getNumActiveVoices() >= 1, "voices still sound");

    // SF2 exclusive classes choke each other.
    Sf2Builder font = testFont();
    const juce::File file = temp.folder.getChildFile("hats.sf2");
    auto block = font.build();
    file.replaceWithData(block.getData(), block.getSize());
    LoadRequest request;
    request.file = file;
    request.sf2_bank = 128;
    request.sf2_program = 0;
    auto result = loadInstrument(request, nullptr);
    if (result && result->instrument) {
      SynthPlugin sf2_plugin;
      preparePlayback(sf2_plugin);
      setInstrument(sf2_plugin, 0, result->instrument);
      auto first = render(sf2_plugin, kRate / 10, { { 0, juce::MidiMessage::noteOn(1, 46, 1.0f) } });
      expect(rms(first, 0, kRate / 10) > 0.001, "SF2 hat plays");
      expect(sf2_plugin.getEngine()->getNumActiveVoices() == 1, "one voice");
    }
    return finish("choke");
  }

  int ccTrigger() {
    TempFolder temp;
    writeWav(temp.folder.getChildFile("pedal.wav"), 330.0, kRate / 4);
    writeText(temp.folder.getChildFile("pedal.sfz"),
              "<region> sample=pedal.wav hikey=-1 lokey=-1 on_locc64=64 on_hicc64=127 pitch_keycenter=60\n");
    auto instrument = loadSfz(temp.folder.getChildFile("pedal.sfz"));
    if (!instrument)
      return finish("cc trigger");

    SynthPlugin plugin;
    preparePlayback(plugin);
    setInstrument(plugin, 0, instrument);
    auto audio = render(plugin, kRate, { { kRate / 10, juce::MidiMessage::controllerEvent(1, 64, 127) },
                                         { kRate / 2, juce::MidiMessage::controllerEvent(1, 64, 127) } });
    expect(rms(audio, 0, kRate / 10) == 0.0, "silent before the pedal");
    close(frequency(audio, kRate / 10 + 100, kRate / 10 + kRate / 5), 330.0, "pedal noise plays with no notes", 2.0);
    expect(plugin.getEngine()->getNumActiveVoices() == 0, "no voice was used");
    expect(rms(audio, kRate / 2, kRate) == 0.0, "a repeated value inside the range does not retrigger");
    auto after = render(plugin, kRate / 4, {});
    expect(rms(after, 0, kRate / 4) == 0.0, "the outputs are clean after the noise ends");

    // Routed to direct out, the noise still reaches the output.
    setControl(plugin, "osc_1_destination", static_cast<float>(vital::constants::kDirectOut));
    auto direct = render(plugin, kRate / 2, { { 0, juce::MidiMessage::controllerEvent(1, 64, 0) },
                                              { kRate / 10, juce::MidiMessage::controllerEvent(1, 64, 127) } });
    expect(rms(direct, kRate / 10, kRate / 4) > 0.01, "direct-out destination");

    // Mic-mix CC: amplitude_cc scales the level linearly.
    writeWav(temp.folder.getChildFile("mic.wav"), 440.0, kRate);
    writeText(temp.folder.getChildFile("mic.sfz"),
              "<control> set_cc70=127\n<region> sample=mic.wav key=60 amplitude_cc70=100 pitch_keycenter=60\n");
    auto mic = loadSfz(temp.folder.getChildFile("mic.sfz"));
    if (mic) {
      SynthPlugin mic_plugin;
      preparePlayback(mic_plugin);
      setInstrument(mic_plugin, 0, mic);
      auto full = render(mic_plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 1.0f) },
                                                  { kRate / 4, juce::MidiMessage::controllerEvent(1, 70, 64) } });
      const double before = rms(full, kRate / 10, kRate / 4);
      const double later = rms(full, kRate / 4 + kRate / 20, kRate / 2);
      close(later / before, 64.0 / 127.0, "amplitude_cc follows the controller", 0.02);
    }
    return finish("cc trigger");
  }


  // ---- Manager, presets and host state ------------------------------------------

  vital::multisample::Manager& manager(SynthPlugin& plugin) {
    return *plugin.getMultiSampleManager();
  }

  void settle(SynthPlugin& plugin) {
    expect(manager(plugin).waitForPendingLoads(60000), "loads finish");
    manager(plugin).processPendingResults();
  }

  std::vector<float> renderPhrase(SynthPlugin& plugin) {
    render(plugin, kRate / 10, {});
    return render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, 60, 0.8f) },
                                       { 4000, juce::MidiMessage::noteOn(1, 64, 0.6f) },
                                       { kRate / 4, juce::MidiMessage::noteOff(1, 60) },
                                       { kRate / 4, juce::MidiMessage::noteOff(1, 64) } });
  }

  bool same(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i] != b[i])
        return false;
    }
    return true;
  }
  // ---- DecentSampler ------------------------------------------------------------

  const Region* regionWithKey(const Instrument& instrument, int key, int position = 1) {
    for (const Region& region : instrument.regions) {
      if (region.lokey <= key && region.hikey >= key && region.seq_position == position &&
          region.trigger != Trigger::kCc)
        return &region;
    }
    return nullptr;
  }

  bool hasWarning(const Instrument& instrument, const std::string& text) {
    for (const std::string& warning : instrument.warnings) {
      if (warning.find(text) != std::string::npos)
        return true;
    }
    return false;
  }

  int decentSamplerParser() {
    TempFolder temp;
    const juce::File root = temp.folder;
    writeWav(root.getChildFile("Samples/Low.wav"), 220.0, 4800);
    writeWav(root.getChildFile("Samples/high.wav"), 440.0, 4800);
    const juce::File preset = root.getChildFile("Piano.dspreset");
    writeText(preset, R"(<?xml version="1.0" encoding="UTF-8"?>
<DecentSampler minVersion="1.0.0">
  <ui>
    <tab name="main">
      <labeled-knob label="Body" minValue="0" maxValue="100" value="25">
        <binding type="amp" level="group" position="0" parameter="AMP_VOLUME"
                 translation="linear" translationOutputMin="0" translationOutputMax="2"/>
      </labeled-knob>
      <labeled-knob label="RT" minValue="0" maxValue="100" value="60">
        <binding type="amp" level="group" position="1" parameter="AMP_VOLUME"
                 translation="linear" translationOutputMin="0" translationOutputMax="1"/>
      </labeled-knob>
      <labeled-knob label="Release" minValue="0" maxValue="10" value="2.5">
        <binding type="amp" level="instrument" position="0" parameter="ENV_RELEASE"/>
      </labeled-knob>
      <labeled-knob label="Tone" minValue="0" maxValue="1" value="0.5">
        <binding type="effect" level="instrument" position="0" parameter="FX_FILTER_FREQUENCY"/>
      </labeled-knob>
      <menu value="2">
        <option name="Close">
          <binding type="amp" level="group" position="2" parameter="ENABLED" translation="fixed_value" translationValue="true"/>
        </option>
        <option name="Room">
          <binding type="amp" level="group" position="2" parameter="ENABLED" translation="fixed_value" translationValue="false"/>
        </option>
      </menu>
      <labeled-knob label="Hats" minValue="0" maxValue="1" value="0.5">
        <binding type="amp" level="tag" identifier="hats" parameter="TAG_VOLUME"
                 translation="table" translationTable="0,0;0.5,0.25;1,1"/>
      </labeled-knob>
    </tab>
  </ui>
  <groups volume="-6dB" attack="0.01" decay="2" sustain="0.5" globalTuning="1" globalPan="-20">
    <group groupVolume="0.8" ampVelTrack="0.5" seqMode="round_robin" tags="body">
      <sample path="Samples\low.wav" rootNote="48" loNote="0" hiNote="59" loVel="1" hiVel="100" seqPosition="1"
              tuning="0.5" pan="40" start="10" end="4000" loopEnabled="true" loopStart="100" loopEnd="3999"/>
      <sample path="Samples/Low.wav" rootNote="48" loNote="0" hiNote="59" seqPosition="2" volume="6dB"/>
      <sample path="Samples/high.wav" rootNote="64" loNote="60" hiNote="127" pitchKeyTrack="0"
              tuning="2" loopEnabled="true" loopDuringRelease="false" glideTime="0.2"/>
    </group>
    <group trigger="release" releaseTriggerDecay="3dB" ampEnvEnabled="false" volume="20dB">
      <sample path="Samples/high.wav" rootNote="64" loNote="60" hiNote="62" loCC64="0" hiCC64="63"/>
      <sample path="Samples/low.wav" rootNote="48" onLoCC64="64" onHiCC64="127"/>
    </group>
    <group>
      <sample path="Samples/high.wav" rootNote="70" loNote="70" hiNote="70"/>
    </group>
    <group seqMode="random" tags="hats" silencingMode="normal">
      <sample path="Samples/high.wav" rootNote="42" loNote="42" hiNote="42" seqPosition="1" tags="closed"/>
      <sample path="Samples/high.wav" rootNote="42" loNote="42" hiNote="42" seqPosition="2" tags="closed"/>
      <sample path="Samples/high.wav" rootNote="46" loNote="46" hiNote="46" silencedByTags="closed"
              silencingDecay="0.05" delay="0.02"/>
    </group>
    <group enabled="false">
      <sample path="Samples/high.wav" rootNote="80" loNote="80" hiNote="80"/>
    </group>
    <group tags="muted">
      <sample path="Samples/high.wav" rootNote="90" loNote="90" hiNote="90"/>
    </group>
  </groups>
  <tags>
    <tag name="muted" enabled="false"/>
  </tags>
  <effects>
    <effect type="reverb" wetLevel="0.5"/>
  </effects>
</DecentSampler>
)");
    SfzParser::Result result = DecentSamplerParser::parse(preset);
    expect(result.error.empty(), "the preset parses: " + result.error);
    if (!result.instrument)
      return finish("decentsampler parser");
    const Instrument& instrument = *result.instrument;
    expect(instrument.format == "dspreset", "format is dspreset");
    expect(instrument.name == "Piano", "named after the file");
    expect(result.text_files.size() == 1 && result.text_files[0] == preset, "the preset is the only text file");
    expect(result.samples.size() == 2, "two sample files, each once (got " + std::to_string(result.samples.size()) + ")");
    // Group 2 is turned off by the menu, group 4 by setting enabled="false", group 5 by its tag.
    expect(instrument.regions.size() == 8, "eight regions (got " + std::to_string(instrument.regions.size()) + ")");
    for (const Region& region : instrument.regions)
      expect(region.lokey != 70 && region.lokey != 80 && region.lokey != 90, "disabled groups and tags play nothing");

    const Region* low = regionWithKey(instrument, 50, 1);
    expect(low != nullptr, "round robin position 1 maps below middle C");
    if (low) {
      expect(juce::File(juce::String(low->sample_path)) == root.getChildFile("Samples/Low.wav"),
             "a backslash path is found ignoring case");
      close(low->pitch_keycenter, 48.0, "rootNote is the key centre");
      expect(low->lovel == 1 && low->hivel == 100, "velocity range");
      expect(low->seq_length == 2 && low->seq_position == 1,
             "round robin length comes from the positions of samples sharing its keys");
      close(low->tune_cents, 150.0, "global and sample tuning add up in cents");
      close(low->pan, 0.2, "pan plus globalPan");
      // -6 dB for the instrument, times the group's 0.8 and its knob's 0.5 (25 of 100 mapped to 0..2).
      close(low->amplitude, std::pow(10.0, -6.0 / 20.0) * 0.8 * 0.5, "volumes multiply, with the knob's value", 1.0e-5);
      close(low->amp_veltrack, 0.5, "ampVelTrack");
      close(low->velocity_gain[127], 1.0, "full velocity is full gain");
      close(low->velocity_gain[0], 0.5, "velocity tracking is half");
      close(low->velocity_gain[64], 0.5 + 0.5 * 64.0 / 127.0, "gain follows velocity linearly", 1.0e-5);
      expect(low->offset == 10 && low->end == 4001, "start and end, end inclusive");
      expect(low->loop_mode_set && low->loop_mode == LoopMode::kContinuous, "loopEnabled loops");
      expect(low->loop_start == 100 && low->loop_end == 4000, "loop points, end inclusive");
      close(low->ampeg.attack, 0.01, "attack inherits from groups");
      close(low->ampeg.decay, 2.0, "decay");
      close(low->ampeg.sustain, 0.5, "sustain");
      close(low->ampeg.release, 2.5, "an instrument knob sets the release");
    }
    const Region* second = regionWithKey(instrument, 50, 2);
    if (second)
      close(second->amplitude, std::pow(10.0, -6.0 / 20.0) * 0.8 * 0.5 * std::pow(10.0, 6.0 / 20.0), "sample volume in dB", 1.0e-5);
    else
      expect(false, "round robin position 2");
    const Region* high = regionWithKey(instrument, 100);
    expect(high != nullptr, "the high sample maps above middle C");
    if (high) {
      expect(high->seq_length == 1, "a sample alone on its keys is not a round robin");
      close(high->pitch_keytrack, 0.0, "pitchKeyTrack 0");
      close(high->tune_cents, 0.0, "tuning scales with key tracking");
      expect(high->loop_mode == LoopMode::kSustain, "loopDuringRelease=false loops until release");
    }

    int release_regions = 0;
    int cc_regions = 0;
    for (const Region& region : instrument.regions) {
      if (region.trigger == Trigger::kRelease) {
        release_regions++;
        close(region.rt_decay, 3.0, "releaseTriggerDecay in dB per second");
        close(region.amplitude, std::pow(10.0, -6.0 / 20.0) * 10.0 * 0.6,
              "a group volume knob multiplies the group's own volume", 1.0e-4);
        expect(region.loop_mode == LoopMode::kOneShot, "ampEnvEnabled=false plays to the end");
        expect(region.cc_gates.size() == 1 && region.cc_gates[0].cc == 64 && region.cc_gates[0].high == 63.0f,
               "loCC64/hiCC64 gate the region");
      }
      if (region.trigger == Trigger::kCc) {
        cc_regions++;
        expect(region.on_cc == 64 && region.on_low == 64.0f && region.on_high == 127.0f, "onLoCC64/onHiCC64 trigger");
      }
    }
    expect(release_regions == 1, "one release-triggered region");
    expect(cc_regions == 1, "one CC-triggered region");

    // Hats: random round robin, tag volume from a table binding, and choking by tag.
    std::vector<const Region*> closed;
    const Region* open = nullptr;
    for (const Region& region : instrument.regions) {
      if (region.lokey == 42)
        closed.push_back(&region);
      if (region.lokey == 46)
        open = &region;
    }
    expect(closed.size() == 2, "two closed hats");
    if (closed.size() == 2) {
      close(closed[0]->lorand, 0.0, "random positions split the range");
      close(closed[0]->hirand, 0.5, "first half");
      close(closed[1]->lorand, 0.5, "second half");
      close(closed[0]->amplitude, std::pow(10.0, -6.0 / 20.0) * 0.25, "a tag volume from a table binding", 1.0e-5);
      expect(closed[0]->group != 0 && closed[0]->group == closed[1]->group, "closed hats share a choke group");
      expect(closed[0]->off_mode == OffMode::kNormal, "silencingMode=normal");
    }
    expect(open != nullptr, "the open hat");
    if (open && !closed.empty()) {
      expect(open->off_by == closed[0]->group, "silencedByTags chokes the open hat");
      expect(open->off_mode == OffMode::kTime, "silencingDecay fades");
      close(open->off_time, 0.05, "silencingDecay time");
      close(open->delay, 0.02, "delay in seconds");
    }

    expect(hasWarning(instrument, "<effects>"), "effects are reported");
    expect(hasWarning(instrument, "glidetime"), "unsupported attributes are reported");
    expect(hasWarning(instrument, "FX_FILTER_FREQUENCY"), "unsupported bindings are reported");

    // seqPosition without seqMode takes turns; an explicit "always" stacks.
    {
      auto takes = [&](const std::string& mode) {
        const std::string attribute = mode.empty() ? std::string() : " seqMode=\"" + mode + "\"";
        return DecentSamplerParser::parseText(
            "<DecentSampler><groups><group" + attribute + ">"
            "<sample path=\"Samples/low.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\" seqPosition=\"1\"/>"
            "<sample path=\"Samples/low.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\" seqPosition=\"2\"/>"
            "<sample path=\"Samples/low.wav\" rootNote=\"60\" loNote=\"60\" hiNote=\"60\" seqPosition=\"3\"/>"
            "<sample path=\"Samples/high.wav\" rootNote=\"72\" loNote=\"72\" hiNote=\"72\" seqPosition=\"1\"/>"
            "</group></groups></DecentSampler>", root.getChildFile("takes.dspreset"));
      };
      SfzParser::Result implied = takes("");
      expect(implied.instrument && implied.instrument->regions.size() == 4, "four takes parse");
      if (implied.instrument && implied.instrument->regions.size() == 4) {
        const auto& regions = implied.instrument->regions;
        expect(regions[0].seq_length == 3 && regions[1].seq_position == 2 && regions[2].seq_position == 3,
               "numbered takes without seqMode take turns");
        expect(regions[3].seq_length == 1, "a lone take with position 1 always plays");
        expect(hasWarning(*implied.instrument, "no seqMode play as round robins"), "the inferred round robin is reported");
      }
      SfzParser::Result stacked = takes("always");
      expect(stacked.instrument && stacked.instrument->regions.size() == 4 &&
             stacked.instrument->regions[0].seq_length == 1 && stacked.instrument->regions[1].seq_length == 1 &&
             !hasWarning(*stacked.instrument, "round robins"), "an explicit seqMode=always stacks");
    }

    // Paths resolve through overrides and search roots like SFZ.
    SfzParser::Options options;
    options.path_overrides["Samples/high.wav"] = root.getChildFile("Samples/Low.wav");
    SfzParser::Result overridden = DecentSamplerParser::parseText(
        "<DecentSampler><groups><group><sample path=\"Samples/high.wav\" rootNote=\"60\"/></group></groups></DecentSampler>",
        root.getChildFile("text.dspreset"), options);
    expect(overridden.samples.size() == 1 && overridden.samples[0].file == root.getChildFile("Samples/Low.wav") &&
           overridden.samples[0].written == "Samples/high.wav", "path overrides apply");

    // Defaults: full key and velocity range, key centre 60, linear velocity, a short release.
    SfzParser::Result plain = DecentSamplerParser::parseText(
        "<DecentSampler><groups><sample path=\"x.wav\"/><group><sample path=\"Samples/low.wav\"/></group></groups></DecentSampler>",
        root.getChildFile("plain.dspreset"));
    expect(plain.error.empty() && plain.instrument && plain.instrument->regions.size() == 1, "a bare sample parses");
    if (plain.instrument && plain.instrument->regions.size() == 1) {
      const Region& region = plain.instrument->regions[0];
      expect(region.lokey == 0 && region.hikey == 127, "full key range");
      close(region.pitch_keycenter, 60.0, "key centre defaults to 60");
      close(region.velocity_gain[64], 64.0 / 127.0, "linear velocity by default", 1.0e-5);
      close(region.ampeg.release, 0.1, "default release");
      expect(!region.loop_mode_set, "no loopEnabled leaves loops to the file");
      expect(hasWarning(*plain.instrument, "<sample>"), "a sample outside a group is reported");
    }

    // Errors.
    expect(!DecentSamplerParser::parseText("<DecentSampler><groups>", root.getChildFile("bad.dspreset")).error.empty(),
           "broken XML is an error");
    expect(!DecentSamplerParser::parseText("<Other/>", root.getChildFile("other.dspreset")).error.empty(),
           "another root element is an error");
    expect(!DecentSamplerParser::parseText("<DecentSampler/>", root.getChildFile("empty.dspreset")).error.empty(),
           "no groups is an error");
    expect(!DecentSamplerParser::parse(root.getChildFile("missing.dspreset")).error.empty(), "a missing file is an error");
    return finish("decentsampler parser");
  }

  // Loading, playing, saving into a container and restoring a DecentSampler preset.
  int decentSamplerLoad() {
    TempFolder temp;
    container::setCacheRootForTesting(temp.folder.getChildFile("cache"));
    const juce::File library = temp.folder.getChildFile("Library");
    writeWav(library.getChildFile("Samples/a440.wav"), 440.0, kRate * 2);
    writeWav(library.getChildFile("Samples/a220.wav"), 220.0, kRate * 2);
    const juce::File preset = library.getChildFile("Presets/Keys.dspreset");
    writeText(preset,
              "<DecentSampler><groups release=\"0.01\">"
              "<group><sample path=\"../Samples/a440.wav\" rootNote=\"69\" loNote=\"60\" hiNote=\"127\"/>"
              "<sample path=\"../Samples/a220.wav\" rootNote=\"57\" loNote=\"0\" hiNote=\"59\" volume=\"0.5\"/>"
              "<sample path=\"../Samples/gone.wav\" rootNote=\"30\" loNote=\"30\" hiNote=\"30\"/></group>"
              "</groups></DecentSampler>");

    LoadRequest request;
    request.file = preset;
    auto result = loadInstrument(request, nullptr);
    expect(result && result->error.empty(), "the preset loads: " + (result ? result->error : std::string()));
    if (!result || !result->instrument)
      return finish("decentsampler load");
    expect(result->instrument->regions.size() == 2, "the missing sample's region is dropped");
    expect(result->instrument->missing_files.size() == 1, "the missing sample is reported");
    expect(result->entry == "Presets/Keys.dspreset", "entry relative to the library");
    expect(result->files.size() == 3 && result->files[0].text, "the preset text and two samples are listed");

    auto measure = [&](int note) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      setInstrument(plugin, 0, result->instrument);
      auto audio = render(plugin, kRate / 2, { { 0, juce::MidiMessage::noteOn(1, note, 1.0f) } });
      return std::make_pair(frequency(audio, kRate / 10, kRate / 2), rms(audio, kRate / 10, kRate / 2));
    };
    const auto a4 = measure(69);
    close(a4.first, 440.0, "rootNote plays at the recorded pitch", 1.0);
    close(measure(81).first, 880.0, "an octave up", 2.0);
    const auto a3 = measure(57);
    close(a3.first, 220.0, "the low sample", 1.0);
    close(a3.second / a4.second, 0.5, "volume=0.5 halves the level", 0.02);

    // A preset with the instrument is a self-contained container.
    const juce::File saved = temp.folder.getChildFile("Saved/Keys.atp");
    std::vector<float> reference;
    std::string hash;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      manager(plugin).loadFromUser(0, preset);
      settle(plugin);
      const auto status = manager(plugin).status(0);
      expect(status.state == vital::multisample::SlotStatus::kLoaded, "user load succeeds: " + status.error);
      expect(manager(plugin).spec(0).format == "dspreset", "the slot records the DecentSampler format");
      hash = manager(plugin).result(0) ? manager(plugin).result(0)->hash : "";
      reference = renderPhrase(plugin);
      expect(rms(reference, 0, kRate / 4) > 0.01, "the instrument plays");
      expect(plugin.saveToFile(saved) && container::isContainer(saved), "the preset saves as a container");
      juce::ZipFile zip(saved);
      const std::string root = "multisample/" + hash + "/";
      expect(zip.getIndexOfFileName(juce::String(root + "Presets/Keys.dspreset")) >= 0, "the .dspreset is stored");
      expect(zip.getIndexOfFileName(juce::String(root + "Samples/a440.wav")) >= 0, "samples keep their layout");
      expect(manager(plugin).editableFile(0) == preset, "the original .dspreset is edited in place");
    }

    library.deleteRecursively();
    container::setCacheRootForTesting(temp.folder.getChildFile("cache2"));
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      std::string error;
      expect(plugin.loadFromFile(saved, error), "the container loads: " + error);
      settle(plugin);
      const auto status = manager(plugin).status(0);
      expect(status.state == vital::multisample::SlotStatus::kLoaded, "the instrument comes from the container: " + status.error);
      plugin.checkOversampling();
      expect(same(renderPhrase(plugin), reference), "the container plays exactly like the original");

      // A cached preset is edited through a working copy and reloads.
      const juce::File editable = manager(plugin).editableFile(0);
      expect(editable.existsAsFile() && editable.hasFileExtension("dspreset") &&
             editable.isAChildOf(container::cacheRoot().getChildFile("Working")),
             "cached DecentSampler text is copied out for editing");
      editable.replaceWithText(editable.loadFileAsString().replace("loNote=\"60\"", "loNote=\"61\""));
      manager(plugin).reload(0);
      settle(plugin);
      auto edited = manager(plugin).result(0);
      const auto edited_status = manager(plugin).status(0);
      expect(edited && edited->instrument && edited->instrument->attack_by_key[60].empty() &&
             !edited->instrument->attack_by_key[61].empty(),
             "the edited copy reloads: " + edited_status.error);
    }
    return finish("decentsampler load");
  }


  // An SFZ library with includes, a sample above the SFZ's folder, a
  // backslash path and a sample outside the library (an absolute path).
  juce::File writeLibrary(const juce::File& root, const juce::File& outside) {
    writeFlac(root.getChildFile("Samples/low.flac"), 220.0, kRate);
    writeWav(root.getChildFile("Samples/high.wav"), 660.0, kRate);
    writeWav(outside.getChildFile("far away.wav"), 330.0, kRate);
    writeText(root.getChildFile("Programs/parts/high.sfz"), "<region> sample=..\\Samples\\high.wav key=64 pitch_keycenter=64\n");
    writeText(root.getChildFile("Programs/main.sfz"),
              "<region> sample=../Samples/low.flac key=60 pitch_keycenter=60\n"
              "#include \"parts/high.sfz\"\n"
              "<region> sample=" + outside.getChildFile("far away.wav").getFullPathName().toStdString() +
              " key=62 pitch_keycenter=62\n");
    return root.getChildFile("Programs/main.sfz");
  }

  int containerTest() {
    TempFolder temp;
    container::setCacheRootForTesting(temp.folder.getChildFile("cache"));
    const juce::File library = temp.folder.getChildFile("Library");
    const juce::File outside = temp.folder.getChildFile("Elsewhere");
    const juce::File main = writeLibrary(library, outside);

    std::vector<float> reference;
    const juce::File preset = temp.folder.getChildFile("Presets/Piano.atp");
    std::string hash;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      manager(plugin).loadFromUser(0, main);
      settle(plugin);
      const auto status = manager(plugin).status(0);
      expect(status.state == vital::multisample::SlotStatus::kLoaded, "user load succeeds: " + status.error);
      auto result = manager(plugin).result(0);
      expect(result && result->instrument && result->instrument->regions.size() == 3, "three regions");
      hash = result ? result->hash : "";
      reference = renderPhrase(plugin);
      expect(rms(reference, 0, kRate / 4) > 0.01, "the instrument plays");

      plugin.setAuthor("Tester");
      expect(plugin.saveToFile(preset), "preset saves");
      expect(container::isContainer(preset), "a multi-sample preset is a container");

      juce::ZipFile zip(preset);
      expect(zip.getNumEntries() == 6, "preset.json plus five instrument files (got " +
                                       std::to_string(zip.getNumEntries()) + ")");
      expect(zip.getEntry(0) && zip.getEntry(0)->filename == "preset.json", "preset.json comes first");
      const std::string root = "multisample/" + hash + "/";
      expect(zip.getIndexOfFileName(juce::String(root + "Programs/main.sfz")) >= 0, "main SFZ stored in place");
      expect(zip.getIndexOfFileName(juce::String(root + "Programs/parts/high.sfz")) >= 0, "included SFZ stored");
      expect(zip.getIndexOfFileName(juce::String(root + "Samples/low.flac")) >= 0, "samples keep their layout");
      expect(zip.getIndexOfFileName(juce::String(root + "_external/0/far away.wav")) >= 0, "outside sample stored");
      const juce::File flac = library.getChildFile("Samples/low.flac");
      juce::MemoryBlock stored;
      {
        std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(
            zip.getIndexOfFileName(juce::String(root + "Samples/low.flac"))));
        stream->readIntoMemoryBlock(stored);
      }
      juce::MemoryBlock original;
      flac.loadFileAsData(original);
      expect(stored == original, "FLAC is stored as it was");
      expect(LoadSave::getAuthorFromFile(preset) == "Tester", "author reads from a container");

      // Plain presets stay JSON.
      setControl(plugin, "osc_1_type", 0.0f);
      const juce::File plain = temp.folder.getChildFile("Presets/Plain.atp");
      expect(plugin.saveToFile(plain) && !container::isContainer(plain), "a preset without instruments is JSON");
      expect(plain.loadFileAsString().startsWith("{"), "plain JSON text");
    }

    // Without the library the container still plays, bit for bit.
    library.moveFileTo(temp.folder.getChildFile("Moved"));
    outside.deleteRecursively();
    container::setCacheRootForTesting(temp.folder.getChildFile("cache2"));
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      std::string error;
      expect(plugin.loadFromFile(preset, error), "container preset loads: " + error);
      settle(plugin);
      const auto status = manager(plugin).status(0);
      expect(status.state == vital::multisample::SlotStatus::kLoaded, "instrument from the container: " + status.error);
      expect(container::isCached(hash), "the container was unpacked into the cache");
      plugin.checkOversampling();
      const auto audio = renderPhrase(plugin);
      expect(same(audio, reference), "the container plays exactly like the original");

      // Saving again keeps it self-contained.
      const juce::File again = temp.folder.getChildFile("Presets/Again.atp");
      expect(plugin.saveToFile(again) && container::isContainer(again), "a reloaded container saves as a container");
      juce::ZipFile zip(again);
      expect(zip.getNumEntries() == 6, "the re-saved container holds every file");

      // Editing a cached SFZ goes through a working copy.
      const juce::File editable = manager(plugin).editableFile(0);
      expect(editable.existsAsFile() && editable.isAChildOf(container::cacheRoot().getChildFile("Working")),
             "cached SFZ text is copied out for editing");
      editable.replaceWithText(editable.loadFileAsString().replace("key=60", "key=61"));
      manager(plugin).reload(0);
      settle(plugin);
      auto edited = manager(plugin).result(0);
      expect(edited && edited->instrument && !edited->instrument->attack_by_key[61].empty(),
             "the reload reads the edited text");
      expect(edited && edited->hash != hash, "an edit changes the hash");
    }

    // A metadata edit keeps the instruments.
    std::string error;
    std::string text;
    expect(container::readPresetText(preset, text), "read preset text");
    expect(container::replacePresetText(preset, text, error) && container::isContainer(preset),
           "rewriting the JSON keeps the container");
    {
      juce::ZipFile zip(preset);
      expect(zip.getNumEntries() == 6, "every instrument file survives a metadata edit");
    }
    container::setCacheRootForTesting({});
    return finish("container");
  }

  int hostState() {
    TempFolder temp;
    container::setCacheRootForTesting(temp.folder.getChildFile("cache"));
    const juce::File library = temp.folder.getChildFile("Library");
    const juce::File main = writeLibrary(library, temp.folder.getChildFile("Elsewhere"));

    juce::MemoryBlock state;
    std::vector<float> reference;
    std::string hash;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      manager(plugin).loadFromUser(0, main);
      settle(plugin);
      // The direct load is linked into the cache in the background.
      for (int i = 0; i < 200 && !container::isCached(manager(plugin).spec(0).hash); ++i)
        juce::Thread::sleep(10);
      hash = manager(plugin).spec(0).hash;
      expect(container::isCached(hash), "a direct load is cached for later");
      reference = renderPhrase(plugin);
      plugin.getStateInformation(state);
      juce::MemoryBlock without;
      {
        SynthPlugin empty;
        preparePlayback(empty);
        empty.getStateInformation(without);
      }
      expect(state.getSize() < without.getSize() + 4096,
             "host state holds no audio (" + std::to_string(state.getSize()) + " vs " +
             std::to_string(without.getSize()) + " bytes)");
      const juce::String text = state.toString();
      expect(text.contains(main.getFullPathName().replace("\\", "\\\\")) || text.contains("main.sfz"),
             "host state names the instrument's path");
    }

    auto restore = [&](const std::string& message) {
      SynthPlugin plugin;
      preparePlayback(plugin);
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      settle(plugin);
      const auto status = manager(plugin).status(0);
      expect(status.state == vital::multisample::SlotStatus::kLoaded, message + ": " + status.error);
      plugin.checkOversampling();
      return std::make_pair(renderPhrase(plugin), status);
    };

    expect(same(restore("restores from the original path").first, reference), "path restore is exact");

    // The library moves: the cache stands in.
    library.moveFileTo(temp.folder.getChildFile("Moved"));
    expect(same(restore("restores from the cache").first, reference), "cache restore is exact");

    // The library comes back edited: the saved copy plays and the user is told.
    temp.folder.getChildFile("Moved").moveFileTo(library);
    main.replaceWithText(main.loadFileAsString() + "\n<region> sample=../Samples/high.wav key=70\n");
    auto changed = restore("restores a changed library");
    expect(same(changed.first, reference), "a changed file plays the saved copy");
    expect(!changed.second.notice.empty(), "the change is reported");

    // Nothing to load from: the reference is kept for re-saving.
    container::setCacheRootForTesting(temp.folder.getChildFile("empty-cache"));
    library.deleteRecursively();
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      settle(plugin);
      const auto status = manager(plugin).status(0);
      expect(status.state == vital::multisample::SlotStatus::kFailed &&
             status.error.find("not found") != std::string::npos, "a missing instrument is reported");
      juce::MemoryBlock again;
      plugin.getStateInformation(again);
      expect(again.toString().contains(juce::String(hash)), "the missing instrument stays in the saved state");
    }
    container::setCacheRootForTesting({});
    return finish("host state");
  }

  int sf2PresetSwitch() {
    TempFolder temp;
    container::setCacheRootForTesting(temp.folder.getChildFile("cache"));
    Sf2Builder font = testFont();
    const juce::File file = temp.folder.getChildFile("font.sf2");
    auto block = font.build();
    file.replaceWithData(block.getData(), block.getSize());

    juce::MemoryBlock state;
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      manager(plugin).loadFromUser(0, file);
      settle(plugin);
      auto first = manager(plugin).result(0);
      expect(first && first->instrument && first->instrument->preset_names.size() == 2, "two presets listed");
      manager(plugin).selectSf2Preset(0, 1);
      auto second = manager(plugin).result(0);
      expect(second && second->sf2_pool == first->sf2_pool, "switching presets shares the sample pool");
      expect(manager(plugin).spec(0).sf2_bank == 128 && manager(plugin).spec(0).sf2_name == "Hats",
             "the spec records the chosen preset");

      // Choosing a preset of the playing file while another file loads keeps
      // the playing file, and the spec names it rather than the other file.
      const juce::File other = temp.folder.getChildFile("other.sf2");
      writeFont(otherFont(), other);
      manager(plugin).loadFromUser(0, other);
      manager(plugin).selectSf2Preset(0, 0);
      settle(plugin);
      auto kept = manager(plugin).result(0);
      const auto kept_spec = manager(plugin).spec(0);
      expect(kept && kept->hash == first->hash && kept->instrument->preset_index == 0,
             "a preset chosen during a load keeps the playing SoundFont");
      expect(kept_spec.path == file.getFullPathName().toStdString() && kept_spec.hash == first->hash &&
             kept_spec.sf2_name == "Piano", "the spec names the playing SoundFont and preset");
      manager(plugin).selectSf2Preset(0, 1);
      plugin.getStateInformation(state);
    }
    {
      SynthPlugin plugin;
      preparePlayback(plugin);
      plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      settle(plugin);
      auto restored = manager(plugin).result(0);
      expect(restored && restored->instrument && restored->instrument->preset_index == 1,
             "the chosen SF2 preset is restored");
    }
    container::setCacheRootForTesting({});
    return finish("sf2 preset switch");
  }


  // ---- Editor ---------------------------------------------------------------------

  Component* findTitled(Component& parent, const std::function<bool(const String&)>& match) {
    if (match(parent.getTitle()))
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = findTitled(*child, match))
        return found;
    }
    return nullptr;
  }

  Component* titledExactly(Component& parent, const String& title) {
    return findTitled(parent, [&title](const String& text) { return text == title; });
  }

  Component* oscillatorSection(Component& parent) {
    if (parent.getProperties()["SectionName"].toString() == "Oscillator 1")
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = oscillatorSection(*child))
        return found;
    }
    return nullptr;
  }

  template <typename Predicate>
  void pumpUntil(Predicate done, int timeout_ms = 5000) {
    const uint32 start = juce::Time::getMillisecondCounter();
    while (!done() && juce::Time::getMillisecondCounter() - start < static_cast<uint32>(timeout_ms))
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  }

  void expectAccessibleButton(Component* component, const String& title) {
    expect(component != nullptr, ("button exists: " + title).toStdString());
    if (component == nullptr)
      return;
    expect(component->getWantsKeyboardFocus(), ("keyboard focus: " + title).toStdString());
    expect(component->getDescription().isNotEmpty() && component->getHelpText().isNotEmpty(),
           ("description and help: " + title).toStdString());
  }

  int editorTest() {
    TempFolder temp;
    container::setCacheRootForTesting(temp.folder.getChildFile("cache"));
    writeWav(temp.folder.getChildFile("tone.wav"), 440.0, kRate / 4);
    writeText(temp.folder.getChildFile("tone.sfz"), "<region> sample=tone.wav\n");
    Sf2Builder font = testFont();
    const juce::File sf2 = temp.folder.getChildFile("font.sf2");
    auto block = font.build();
    sf2.replaceWithData(block.getData(), block.getSize());

    SynthPlugin synth;
    synth.parameterChanged("osc_1_type", static_cast<float>(vital::source_types::kMultiSample));
    SynthEditor editor(synth);
    editor.updateFullGui();
    pumpUntil([] { return false; }, 200);

    auto section = [&] { return oscillatorSection(editor); };
    expect(section() != nullptr, "Oscillator 1 section exists");
    if (section() == nullptr)
      return finish("editor");
    expect(titledExactly(*section(), "Instrument") != nullptr, "an Instrument group");
    expectAccessibleButton(titledExactly(*section(), "Load instrument"), "Load instrument");
    expectAccessibleButton(titledExactly(*section(), "No instrument loaded"), "status");
    expectAccessibleButton(titledExactly(*section(), "Clear instrument"), "Clear instrument");
    if (auto* clear = dynamic_cast<Button*>(titledExactly(*section(), "Clear instrument"))) {
      clear->triggerClick();
      pumpUntil([] { return false; }, 200);
      expect(synth.getMultiSampleManager()->result(0) == nullptr && section() != nullptr &&
             titledExactly(*section(), "No instrument loaded") != nullptr, "Clear instrument with nothing loaded");
    }
    expect(titledExactly(*section(), "Open in text editor") == nullptr, "no SFZ buttons before an SFZ loads");
    expect(findTitled(*section(), [](const String& t) { return t.containsIgnoreCase("distortion"); }) == nullptr,
           "a Multi-sample slot shows no FM or RM rows");
    expect(findTitled(*section(), [](const String& t) { return t.startsWith("1 Multi-sample Amplitude Envelope"); }) != nullptr,
           "the amplitude envelope row is in the Instrument group");
    expect(findTitled(*section(), [](const String& t) { return t.startsWith("Oscillator 1") &&
                                                               t.contains("multi-sample"); }) != nullptr,
           "the section title names the type");

    synth.getMultiSampleManager()->loadFromUser(0, temp.folder.getChildFile("tone.sfz"));
    pumpUntil([&] { return section() != nullptr && titledExactly(*section(), "Reload SFZ") != nullptr; });
    expectAccessibleButton(section() ? titledExactly(*section(), "Open in text editor") : nullptr, "Open in text editor");
    expectAccessibleButton(section() ? titledExactly(*section(), "Reload SFZ") : nullptr, "Reload SFZ");
    expect(section() && findTitled(*section(), [](const String& t) { return t.startsWith("Instrument: tone, SFZ, 1 regions"); }),
           "the status names the loaded SFZ");

    synth.getMultiSampleManager()->loadFromUser(0, sf2);
    pumpUntil([&] { return section() != nullptr && titledExactly(*section(), "1 Instrument preset") != nullptr; });
    auto* combo = section() ? dynamic_cast<ComboBox*>(titledExactly(*section(), "1 Instrument preset")) : nullptr;
    expect(combo != nullptr, "an SF2 with several presets offers a preset menu");
    expect(section() && titledExactly(*section(), "Reload SFZ") == nullptr, "SFZ buttons go away for an SF2");
    if (combo != nullptr) {
      expect(combo->getNumItems() == 2 && combo->getItemText(0) == "Piano, bank 0 program 0" &&
             combo->getItemText(1) == "Hats, bank 128 program 0", "presets read name, bank and program");
      expect(combo->getWantsKeyboardFocus() && combo->getDescription().isNotEmpty(), "the preset menu is accessible");
      combo->setSelectedId(2, juce::sendNotificationSync);
      auto result = synth.getMultiSampleManager()->result(0);
      expect(result && result->instrument && result->instrument->preset_index == 1, "choosing a preset switches it");
    }
    auto* next = section() ? dynamic_cast<Button*>(titledExactly(*section(), "Next preset")) : nullptr;
    auto* previous = section() ? dynamic_cast<Button*>(titledExactly(*section(), "Previous preset")) : nullptr;
    expectAccessibleButton(next, "Next preset");
    expectAccessibleButton(previous, "Previous preset");
    if (next != nullptr && previous != nullptr) {
      auto preset = [&] {
        auto result = synth.getMultiSampleManager()->result(0);
        return result && result->instrument ? result->instrument->preset_index : -1;
      };
      next->triggerClick();
      pumpUntil([&] { return preset() == 0; }, 1000);
      expect(preset() == 0, "Next preset wraps from the last preset to the first");
      expect(combo == nullptr || combo->getSelectedId() == 1, "the preset menu follows Next preset");
      previous->triggerClick();
      pumpUntil([&] { return preset() == 1; }, 1000);
      expect(preset() == 1, "Previous preset wraps from the first preset to the last");
    }

    // Another SoundFont with several presets keeps the same rows, but the
    // menu must list its presets rather than the first file's.
    auto* multisample = synth.getMultiSampleManager();
    auto preset_menu = [&] {
      return section() ? dynamic_cast<ComboBox*>(titledExactly(*section(), "1 Instrument preset")) : nullptr;
    };
    const juce::File other_sf2 = temp.folder.getChildFile("other.sf2");
    writeFont(otherFont(), other_sf2);
    multisample->loadFromUser(0, other_sf2);
    multisample->waitForPendingLoads(5000);
    multisample->processPendingResults();
    // The old menu is still up until the editor's timer runs: it must not
    // send one of the first file's presets to the new one.
    auto loaded_other = multisample->result(0);
    const int other_default = loaded_other && loaded_other->instrument ? loaded_other->instrument->preset_index : -1;
    if (auto* stale = preset_menu()) {
      stale->setSelectedId(stale->getSelectedId() == 1 ? 2 : 1, juce::sendNotificationSync);
      auto after = multisample->result(0);
      expect(after == loaded_other, "a menu built for the previous SoundFont changes nothing");
    }
    pumpUntil([&] { auto* menu = preset_menu(); return menu != nullptr && menu->getNumItems() == 3; });
    combo = preset_menu();
    expect(combo != nullptr && combo->getNumItems() == 3 && combo->getItemText(0) == "Strings, bank 0 program 0" &&
           combo->getItemText(1) == "Kit, bank 0 program 1" && combo->getItemText(2) == "Bass, bank 0 program 2",
           "loading another SoundFont rebuilds the preset menu with its presets");
    if (combo != nullptr && loaded_other != nullptr && loaded_other->instrument != nullptr) {
      const auto& names = loaded_other->instrument->preset_names;
      int selected = 0;
      for (size_t i = 0; i < names.size(); ++i) {
        if (names[i].index == other_default)
          selected = static_cast<int>(i) + 1;
      }
      expect(selected > 0 && combo->getSelectedId() == selected, "the rebuilt menu selects the playing preset");
      combo->setSelectedId(3, juce::sendNotificationSync);
      auto chosen = multisample->result(0);
      expect(chosen && chosen->instrument && chosen->hash == loaded_other->hash &&
             chosen->instrument->preset_index == names[2].index, "the rebuilt menu chooses the new file's presets");

      // A preset chosen elsewhere (host state, another control) moves the menu.
      auto* kept = combo;
      multisample->selectSf2Preset(0, names[1].index);
      pumpUntil([&] { return preset_menu() != kept || kept->getSelectedId() == 2; }, 1000);
      expect(preset_menu() == kept && kept->getSelectedId() == 2,
             "the menu follows a preset chosen elsewhere without a rebuild");
    }

    // Clear instrument empties the slot and drops the preset menu.
    auto* clear = section() ? dynamic_cast<Button*>(titledExactly(*section(), "Clear instrument")) : nullptr;
    expectAccessibleButton(clear, "Clear instrument");
    if (clear != nullptr) {
      clear->triggerClick();
      pumpUntil([&] { return section() != nullptr && preset_menu() == nullptr; });
      expect(multisample->result(0) == nullptr && multisample->spec(0).empty(), "Clear instrument empties the slot");
      expect(multisample->status(0).state == vital::multisample::SlotStatus::kEmpty, "the slot status is empty");
      expect(section() != nullptr && preset_menu() == nullptr && titledExactly(*section(), "Next preset") == nullptr,
             "the preset menu goes away with the instrument");
      expect(section() != nullptr && titledExactly(*section(), "No instrument loaded") != nullptr,
             "the status reads no instrument loaded");
      expectAccessibleButton(section() ? titledExactly(*section(), "Clear instrument") : nullptr,
                             "Clear instrument after clearing");
    }

    // A large SoundFont's menu is grouped so every submenu fits on screen.
    std::vector<vital::multisample::PresetName> many;
    for (int bank : { 0, 8, 128 }) {
      for (int program = 0; program < (bank == 0 ? 128 : bank == 8 ? 10 : 50); ++program)
        many.push_back({ bank, program, "Preset " + std::to_string(program), static_cast<int>(many.size()) });
    }
    std::vector<int> seen(many.size() + 1, 0);
    int largest = 0;
    std::function<void(const PopupMenu&)> walk = [&](const PopupMenu& menu) {
      int items = 0;
      for (PopupMenu::MenuItemIterator it(menu); it.next();) {
        const auto& item = it.getItem();
        items++;
        if (item.subMenu != nullptr)
          walk(*item.subMenu);
        else if (item.itemID > 0 && item.itemID <= static_cast<int>(many.size()))
          seen[static_cast<size_t>(item.itemID)]++;
      }
      largest = std::max(largest, items);
    };
    const PopupMenu grouped = SynthEditor::createInstrumentPresetMenu(many, 140);
    walk(grouped);
    bool every_once = true;
    for (size_t i = 1; i < seen.size(); ++i)
      every_once = every_once && seen[i] == 1;
    expect(every_once, "every preset appears exactly once in the grouped menu");
    expect(largest <= 24, "no menu or submenu holds more than 24 items (largest " + std::to_string(largest) + ")");
    int top = 0;
    for (PopupMenu::MenuItemIterator it(grouped); it.next();)
      top++;
    expect(top == 3, "one submenu per bank");
    container::setCacheRootForTesting({});
    return finish("editor");
  }


  // Plays a short phrase through the plugin with a real instrument, writes it
  // to `output` and reports levels and how fast it rendered.
  int manualRender(const juce::File& file, const juce::File& output, int preset) {
    SynthPlugin plugin;
    preparePlayback(plugin);
    if (preset >= 0) {
      vital::multisample::InstrumentSpec spec;
      spec.path = file.getFullPathName().toStdString();
      spec.format = "sf2";
      spec.sf2_index = preset;
      manager(plugin).applySpec(0, spec, {});
    }
    else
      manager(plugin).loadFromUser(0, file);
    settle(plugin);
    const auto status = manager(plugin).status(0);
    if (status.state != vital::multisample::SlotStatus::kLoaded) {
      std::cout << "error: " << status.error << "\n";
      return 1;
    }
    std::vector<MidiEvent> events;
    const int notes[] = { 36, 38, 42, 46, 48, 52, 55, 60, 64, 67, 72, 76, 84 };
    int at = 0;
    for (int note : notes) {
      events.push_back({ at, juce::MidiMessage::noteOn(1, note, 0.85f) });
      events.push_back({ at + kRate / 3, juce::MidiMessage::noteOff(1, note) });
      at += kRate / 4;
    }
    events.push_back({ at, juce::MidiMessage::controllerEvent(1, 64, 127) });
    for (int i = 0; i < 4; ++i)
      events.push_back({ at + 10 + i, juce::MidiMessage::noteOn(1, 60 + i * 4, 0.6f) });
    for (int i = 0; i < 4; ++i)
      events.push_back({ at + kRate / 2, juce::MidiMessage::noteOff(1, 60 + i * 4) });
    events.push_back({ at + kRate * 2, juce::MidiMessage::controllerEvent(1, 64, 0) });
    const int length = at + kRate * 4;

    std::vector<float> left(static_cast<size_t>(length)), right(static_cast<size_t>(length));
    const auto start = std::chrono::steady_clock::now();
    for (int offset = 0; offset < length;) {
      const int count = std::min(256, length - offset);
      juce::AudioBuffer<float> audio(2, count);
      audio.clear();
      juce::MidiBuffer midi;
      for (const MidiEvent& event : events) {
        if (event.sample >= offset && event.sample < offset + count)
          midi.addEvent(event.message, event.sample - offset);
      }
      plugin.processBlock(audio, midi);
      for (int i = 0; i < count; ++i) {
        left[static_cast<size_t>(offset + i)] = audio.getSample(0, i);
        right[static_cast<size_t>(offset + i)] = audio.getSample(1, i);
      }
      offset += count;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    float peak = 0.0f;
    bool finite = true;
    for (size_t i = 0; i < left.size(); ++i) {
      peak = std::max(peak, std::max(std::abs(left[i]), std::abs(right[i])));
      finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
    }
    std::cout << "rendered " << length / double(kRate) << " s in " << seconds << " s (" << (length / double(kRate)) / seconds
              << "x realtime), peak " << peak << ", finite " << finite << ", rms " << rms(left, 0, length) << "\n";

    juce::AudioBuffer<float> buffer(2, length);
    buffer.copyFrom(0, 0, left.data(), length);
    buffer.copyFrom(1, 0, right.data(), length);
    output.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(output);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(kRate)
                                                     .withNumChannels(2).withBitsPerSample(24));
    if (writer)
      writer->writeFromAudioSampleBuffer(buffer, 0, length);
    return finite ? 0 : 1;
  }

  // ---- Manual inspection of a real instrument ----------------------------------

  int manual(const juce::File& file, int preset) {
    LoadRequest request;
    request.file = file;
    request.sf2_index = preset;
    const auto start = std::chrono::steady_clock::now();
    int last_percent = -10;
    auto result = loadInstrument(request, [&last_percent](float amount, const std::string& phase) {
      const int percent = static_cast<int>(amount * 100.0f);
      if (percent >= last_percent + 10) {
        std::cout << phase << " " << percent << "%\n";
        last_percent = percent;
      }
      return true;
    });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (result == nullptr) {
      std::cout << "cancelled\n";
      return 1;
    }
    if (!result->error.empty()) {
      std::cout << "error: " << result->error << "\n";
      return 1;
    }
    const Instrument& instrument = *result->instrument;
    std::cout << "name: " << instrument.name << " (" << instrument.format << ")\n"
              << "regions: " << instrument.regions.size() << "\n"
              << "memory: " << juce::File::descriptionOfSizeInBytes(static_cast<int64_t>(instrument.memory_bytes))
              << "\nfiles: " << result->files.size() << "\nentry: " << result->entry
              << "\nhash: " << result->hash << "\nload time: " << seconds << " s\n";
    int release = 0, cc = 0, rr = 0, gated = 0, groups = 0;
    for (const Region& region : instrument.regions) {
      release += region.trigger == Trigger::kRelease;
      cc += region.trigger == Trigger::kCc;
      rr += region.seq_length > 1;
      gated += !region.cc_gates.empty();
      groups += region.group != 0;
    }
    std::cout << "release triggers: " << release << ", CC triggers: " << cc << ", round robin: " << rr
              << ", CC gated: " << gated << ", grouped: " << groups << "\n";
    if (!instrument.preset_names.empty()) {
      std::cout << "presets: " << instrument.preset_names.size() << " (playing index " << instrument.preset_index
                << ")\n";
      for (size_t i = 0; i < std::min<size_t>(8, instrument.preset_names.size()); ++i) {
        const auto& p = instrument.preset_names[i];
        std::cout << "  " << p.bank << ":" << p.program << " " << p.name << "\n";
      }
    }
    std::cout << "missing files: " << instrument.missing_files.size() << "\n";
    for (size_t i = 0; i < std::min<size_t>(10, instrument.missing_files.size()); ++i)
      std::cout << "  " << instrument.missing_files[i] << "\n";
    for (const std::string& warning : instrument.warnings)
      std::cout << "warning: " << warning << "\n";
    return 0;
  }
} // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  const juce::String flag = argc > 1 ? juce::String(argv[1]) : juce::String();
  if (flag == "--manual" && argc > 2)
    return manual(juce::File(juce::String(argv[2])), argc > 3 ? juce::String(argv[3]).getIntValue() : -1);
  if (flag == "--render" && argc > 3)
    return manualRender(juce::File(juce::String(argv[2])), juce::File(juce::String(argv[3])),
                        argc > 4 ? juce::String(argv[4]).getIntValue() : -1);
  if (flag == "--sfz-parser")
    return sfzParser();
  if (flag == "--sf2-parser")
    return sf2Parser();
  if (flag == "--decentsampler-parser")
    return decentSamplerParser();
  if (flag == "--decentsampler-load")
    return decentSamplerLoad();
  if (flag == "--decode")
    return decode();
  if (flag == "--pitch")
    return pitch();
  if (flag == "--select")
    return select();
  if (flag == "--envelope-mode")
    return envelopeMode();
  if (flag == "--release-trigger")
    return releaseTrigger();
  if (flag == "--choke")
    return choke();
  if (flag == "--cc-trigger")
    return ccTrigger();
  if (flag == "--container")
    return containerTest();
  if (flag == "--host-state")
    return hostState();
  if (flag == "--sf2-preset-switch")
    return sf2PresetSwitch();
  if (flag == "--editor")
    return editorTest();
  std::cerr << "usage: AtlasMultiSampleTests --sfz-parser | --sf2-parser | --decentsampler-parser | --decode | --manual <file> [preset]\n";
  return 2;
}
