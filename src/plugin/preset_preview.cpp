// SPDX-License-Identifier: GPL-3.0-or-later
#include "preset_preview.h"
#include "synth_plugin.h"
#include "tape_noise_library.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

namespace atlas::preview {
namespace {
  constexpr double kSampleRate = 44100.0;
  constexpr int kBlockSamples = 512;
  constexpr juce::uint8 kVelocity = 100;
  constexpr double kHoldSeconds = 2.0;
  // After the note, or the MIDI file, the recording runs on until the sound
  // has been quiet this long, for kTailSeconds at most, and fades out over
  // kFadeSeconds when the sound has not ended by then.
  constexpr double kQuietSeconds = 0.25;
  constexpr double kTailSeconds = 5.0;
  constexpr double kFadeSeconds = 0.5;
  constexpr float kQuietLevel = 0.0005f;   // -66 dBFS
  // The engine runs this long before the first note, unrecorded, so that its
  // smoothed controls have reached the patch's values.
  constexpr double kSettleSeconds = 0.25;
  constexpr double kMaxMidiSeconds = 60.0;
  constexpr float kMaxPeak = 0.891f;       // -1 dBFS
  // The range of the patch's tempo control.
  constexpr double kMinBpm = 20.0;
  constexpr double kMaxBpm = 300.0;
  // The tempo JUCE reads a MIDI file without one at.
  constexpr double kMidiFileBpm = 120.0;

  int samplesFor(double seconds) {
    return static_cast<int>(std::ceil(seconds * kSampleRate));
  }

  // A stopped transport at a fixed tempo, as in a host before Play: synced
  // LFOs and arpeggiators follow the tempo and start with the first note.
  class TempoPlayHead : public juce::AudioPlayHead {
    public:
      explicit TempoPlayHead(double bpm) : bpm_(bpm) { }

      juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo position;
        position.setBpm(bpm_);
        position.setIsPlaying(false);
        return position;
      }

    private:
      double bpm_;
  };
}

juce::File atlasMidiFolder() {
  return vital::bundleResourcesDirectory().getChildFile("Preview MIDI");
}

juce::Array<juce::File> atlasMidiFiles(const juce::File& folder) {
  juce::Array<juce::File> files = folder.findChildFiles(juce::File::findFiles, false, "*.mid");
  std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
    return a.getFileName().compareNatural(b.getFileName()) < 0;
  });
  return files;
}

std::unique_ptr<Renderer> Renderer::create(const juce::File& preset, const juce::File& midi_file, int octave,
                                           juce::String& error) {
  auto synth = std::make_unique<SynthPlugin>();
  synth->setNonRealtime(true);
  synth->setPlayConfigDetails(0, 2, kSampleRate, kBlockSamples);
  synth->prepareToPlay(kSampleRate, kBlockSamples);
  std::string load_error;
  if (!synth->loadFromFile(preset, load_error)) {
    error = load_error.empty() ? juce::String("Unable to load the patch") : juce::String(load_error).trimCharactersAtEnd(".");
    return nullptr;
  }
  double patch_bpm = 0.0;
  const auto& controls = synth->getControls();
  const auto tempo = controls.find("beats_per_minute");   // in beats per second
  if (tempo != controls.end())
    patch_bpm = 60.0 * tempo->second->value();
  patch_bpm = juce::jlimit(kMinBpm, kMaxBpm, patch_bpm > 0.0 ? patch_bpm : kMidiFileBpm);

  std::unique_ptr<Renderer> renderer(new Renderer());
  double bpm = patch_bpm;
  if (midi_file != juce::File()) {
    error = renderer->readMidiFile(midi_file, 12 * (octave - kWrittenOctave), patch_bpm, bpm);
    if (error.isNotEmpty())
      return nullptr;
  }
  else {
    const int note = juce::jlimit(0, 127, 12 * (octave + 1));
    renderer->events_.addEvent(juce::MidiMessage::noteOn(1, note, kVelocity));
    renderer->events_.addEvent(juce::MidiMessage::noteOff(1, note).withTimeStamp(kHoldSeconds));
    renderer->seconds_ = kHoldSeconds;
  }
  renderer->play_head_ = std::make_unique<TempoPlayHead>(juce::jlimit(kMinBpm, kMaxBpm, bpm));
  synth->setPlayHead(renderer->play_head_.get());
  renderer->synth_ = std::move(synth);
  return renderer;
}

Renderer::~Renderer() = default;

// The notes and controllers of `file`, from its first note on and for at most
// kMaxMidiSeconds, each note still on at the end ended there.
juce::String Renderer::readMidiFile(const juce::File& file, int transpose, double patch_bpm, double& bpm) {
  const juce::String name = file.getFileName();
  juce::FileInputStream input(file);
  if (!input.openedOk())
    return "Unable to read " + name;
  juce::MidiFile midi;
  if (!midi.readFrom(input))
    return name + " is not a MIDI file";
  midi.convertTimestampTicksToSeconds();

  // A file without a tempo, as Atlas's own are, plays at the patch's, which
  // JUCE timed at 120 BPM; one timed in SMPTE frames has no tempo to follow.
  juce::MidiMessageSequence tempos;
  midi.findAllTempoEvents(tempos);
  const double seconds_per_quarter =
      tempos.getNumEvents() > 0 ? tempos.getEventPointer(0)->message.getTempoSecondsPerQuarterNote() : 0.0;
  bpm = seconds_per_quarter > 0.0 ? 60.0 / seconds_per_quarter : patch_bpm;
  const double stretch = tempos.getNumEvents() == 0 && midi.getTimeFormat() > 0 ? kMidiFileBpm / patch_bpm : 1.0;

  std::vector<juce::MidiMessage> messages;
  for (int track = 0; track < midi.getNumTracks(); ++track) {
    for (const auto* event : *midi.getTrack(track)) {
      juce::MidiMessage message = event->message;
      if (message.isNoteOnOrOff() || message.isAftertouch()) {
        const int note = message.getNoteNumber() + transpose;
        if (!juce::isPositiveAndBelow(note, 128))
          continue;
        message.setNoteNumber(note);
      }
      else if (!message.isController() && !message.isPitchWheel() && !message.isChannelPressure()) {
        continue;
      }
      message.setTimeStamp(message.getTimeStamp() * stretch);
      messages.push_back(message);
    }
  }
  std::stable_sort(messages.begin(), messages.end(), [](const juce::MidiMessage& a, const juce::MidiMessage& b) {
    return a.getTimeStamp() < b.getTimeStamp();
  });
  const auto first_note = std::find_if(messages.begin(), messages.end(),
                                       [](const juce::MidiMessage& message) { return message.isNoteOn(); });
  if (first_note == messages.end())
    return name + " has no notes";

  const double start = first_note->getTimeStamp();
  std::set<std::pair<int, int>> sounding;   // channel and note
  std::set<int> sustained;                  // channels with the sustain pedal down
  seconds_ = 0.0;
  for (auto message : messages) {
    const double at = std::max(0.0, message.getTimeStamp() - start);
    if (at >= kMaxMidiSeconds) {
      seconds_ = kMaxMidiSeconds;
      break;
    }
    const int channel = message.getChannel();
    if (message.isNoteOn())
      sounding.insert({ channel, message.getNoteNumber() });
    else if (message.isNoteOff())
      sounding.erase({ channel, message.getNoteNumber() });
    else if (message.isSustainPedalOn())
      sustained.insert(channel);
    else if (message.isSustainPedalOff())
      sustained.erase(channel);
    message.setTimeStamp(at);
    events_.addEvent(message);
    seconds_ = std::max(seconds_, at);
  }
  for (const auto& [channel, note] : sounding)
    events_.addEvent(juce::MidiMessage::noteOff(channel, note).withTimeStamp(seconds_));
  for (int channel : sustained)
    events_.addEvent(juce::MidiMessage::controllerEvent(channel, 64, 0).withTimeStamp(seconds_));
  return {};
}

juce::String Renderer::render(const juce::File& output, const std::function<bool()>& stopped) {
  // With room for the delay the patch's effects report, dropped below.
  // And for the blocks running past the events and the tail.
  const int most_samples = samplesFor(seconds_ + kTailSeconds + 1.0);
  juce::AudioBuffer<float> recording(2, most_samples + std::max(0, synth_->getLatencySamples()));
  juce::AudioBuffer<float> block(2, kBlockSamples);
  juce::MidiBuffer midi;
  int recorded = 0;

  // One block, with the events from `cursor` on that fall in it when `from`
  // samples into them, or none. Returns the block's peak.
  int cursor = 0;
  auto play = [&](bool with_events, int from, bool record) {
    midi.clear();
    for (; with_events && cursor < events_.getNumEvents(); ++cursor) {
      const auto& message = events_.getEventPointer(cursor)->message;
      const int at = static_cast<int>(std::llround(message.getTimeStamp() * kSampleRate)) - from;
      if (at >= kBlockSamples)
        break;
      midi.addEvent(message, std::max(0, at));
    }
    block.clear();
    {
      const juce::ScopedLock lock(synth_->getCallbackLock());
      synth_->processBlock(block, midi);
    }
    if (record) {
      const int samples = std::min(kBlockSamples, recording.getNumSamples() - recorded);
      for (int channel = 0; channel < 2; ++channel)
        recording.copyFrom(channel, recorded, block, channel, 0, samples);
      recorded += samples;
    }
    return block.getMagnitude(0, kBlockSamples);
  };

  for (int at = 0; at < samplesFor(kSettleSeconds); at += kBlockSamples)
    play(false, 0, false);

  const int played = samplesFor(seconds_);
  for (int from = 0; from < played || cursor < events_.getNumEvents(); from += kBlockSamples) {
    if (stopped && stopped())
      return "Cancelled";
    play(true, from, true);
  }
  const int quiet_samples = samplesFor(kQuietSeconds);
  int quiet = 0;
  for (int tail = 0; tail < samplesFor(kTailSeconds) && quiet < quiet_samples; tail += kBlockSamples) {
    if (stopped && stopped())
      return "Cancelled";
    quiet = play(false, 0, true) < kQuietLevel ? quiet + kBlockSamples : 0;
  }
  const bool ended_quiet = quiet >= quiet_samples;

  // The patch's effects delay what it plays by its reported latency.
  const int start = juce::jlimit(0, recorded, synth_->getLatencySamples());
  const int length = recorded - start;
  const float peak = length > 0 ? recording.getMagnitude(start, length) : 0.0f;
  if (!std::isfinite(peak))
    return "The patch's output is not a number";
  if (peak < kQuietLevel)
    return "The patch made no sound";
  if (peak > kMaxPeak)
    recording.applyGain(start, length, kMaxPeak / peak);
  if (!ended_quiet) {
    const int fade = std::min(length, samplesFor(kFadeSeconds));
    recording.applyGainRamp(start + length - fade, fade, 1.0f, 0.0f);
  }

  output.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = output.createOutputStream();
  if (stream == nullptr)
    return "Unable to write " + output.getFileName();
  juce::FlacAudioFormat flac;
  auto writer = flac.createWriterFor(stream, juce::AudioFormatWriterOptions {}
                                                 .withSampleRate(kSampleRate)
                                                 .withNumChannels(2)
                                                 .withBitsPerSample(16)
                                                 .withQualityOptionIndex(5));
  if (writer == nullptr)
    return "Unable to write " + output.getFileName();
  const bool written = writer->writeFromAudioSampleBuffer(recording, start, length);
  writer.reset();
  if (!written) {
    output.deleteFile();
    return "Unable to write " + output.getFileName();
  }
  return {};
}
}
