// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "JuceHeader.h"

#include <functional>
#include <memory>

class SynthPlugin;

// The audio preview Share patch sends with a patch (the share form in
// synth_editor.cpp): the patch loaded into an engine of its own, so the one
// the host plays goes on undisturbed, played, recorded and written as FLAC.
// It plays one C, held two seconds and left to ring until it is quiet, or a
// MIDI file: one of Atlas's own (Factory/Preview MIDI, in C with C4 as the
// root) or the user's.
namespace atlas::preview {
  // The octave Atlas's MIDI files are written in: a file played in another
  // octave moves by as many octaves from it.
  constexpr int kWrittenOctave = 4;

  // Atlas's MIDI files, which the build copies into the bundle's Resources,
  // in the order of their names.
  juce::File atlasMidiFolder();
  juce::Array<juce::File> atlasMidiFiles(const juce::File& folder = atlasMidiFolder());

  class Renderer {
    public:
      // On the message thread, where loading a patch builds its oscillators
      // and effects: an engine with `preset` loaded, to play `midi_file`, or
      // the C of `octave` when it is File(), C4 being octave 4. A MIDI file
      // moves by `octave` - kWrittenOctave octaves, and its notes that leave
      // the MIDI range are left out. The tempo is the MIDI file's, else the
      // one saved in the patch, which is the host's when it was saved. Null,
      // with `error` set, when the patch or the MIDI file cannot be read.
      static std::unique_ptr<Renderer> create(const juce::File& preset, const juce::File& midi_file, int octave,
                                              juce::String& error);
      // On the message thread too, where the engine's timers run.
      ~Renderer();

      // On one thread at a time: plays and records the preview and writes it
      // to `output` as 44.1 kHz, 16-bit stereo FLAC, turned down to a peak of
      // -1 dBFS when it is louder. Returns what went wrong, or an empty
      // string; stops, writing nothing, once `stopped` answers true.
      juce::String render(const juce::File& output, const std::function<bool()>& stopped = {});

    private:
      Renderer() = default;
      // The events of `file` moved by `transpose` semitones, in seconds from
      // its first note, into `events_` and `seconds_`; its tempo, or
      // `patch_bpm` when it has none, in `bpm`.
      juce::String readMidiFile(const juce::File& file, int transpose, double patch_bpm, double& bpm);

      std::unique_ptr<juce::AudioPlayHead> play_head_;   // outlives synth_, which reads it
      std::unique_ptr<SynthPlugin> synth_;
      // What plays before the recording waits for the sound to end.
      juce::MidiMessageSequence events_;
      double seconds_ = 0.0;
  };
}
