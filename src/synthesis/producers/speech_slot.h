// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "speech_phrase.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace vital {
  // The Speech state of one oscillator slot, shared by every voice copy of
  // its module: the phrase it sings and which syllable the next note takes.
  // With the Word or Phrase split a note takes a word or the whole text
  // instead; "syllable" below means whichever unit that is.
  //
  // While the host plays, the syllable follows the song: the slot remembers
  // where in the song (in quarter notes) its notes started, and a note sings
  // the syllable counted by the notes before it. Each block the host plays
  // replaces what the slot remembered for that stretch of the song with what
  // it heard, and marks the stretch played, so after the song has played
  // through once, starting anywhere sings the syllable a play from the
  // beginning would, and edited notes are relearned when they play. With the
  // transport stopped, notes take the next syllable in turn.
  //
  // Hosts that chase notes send a note held across the point where playback
  // starts (or a loop returns) as a new note-on there. Where that point has
  // been played and no note was remembered starting at it, such a note
  // continues the held note's syllable and is not remembered.
  //
  // While a key is down, every new note sings the syllable that key's note
  // sings: a chord held down loops one syllable on every note pressed into
  // it, and the text moves on only once all keys are up. Notes an
  // arpeggiator plays move on as before.
  class SpeechSlot {
    public:
      // Notes that start within this many seconds of each other (a chord,
      // a strum) sing the same syllable.
      static constexpr double kChordSeconds = 0.04;
      // The most note starts and separate played stretches a slot remembers.
      static constexpr size_t kMaxSongNotes = 8192;
      static constexpr size_t kMaxPlayedRanges = 1024;
      // Song positions this close, in quarter notes, are the same place
      // (about five samples at 120 BPM and 48 kHz).
      static constexpr double kSamePlace = 2.0e-4;
      // The most key releases within one block the slot keeps apart.
      static constexpr int kMaxHeldReleases = 64;

      struct Range {
        double start = 0.0;
        double end = 0.0;
        bool operator==(const Range& other) const { return start == other.start && end == other.end; }
      };

      // Message thread, holding the engine's lock. Notes playing the phrase
      // before this one finish with it; older phrases stop.
      void setPhrase(std::shared_ptr<const speech::Phrase> phrase);
      std::shared_ptr<const speech::Phrase> phrase() const { return phrase_; }
      std::shared_ptr<const speech::Phrase> retired() const { return retired_; }

      // ---- Audio thread ----
      uint32_t generation() const { return generation_; }
      // The phrase a note started at `generation` plays, or null once freed.
      const speech::Phrase* phraseFor(uint32_t generation) const {
        if (generation == generation_)
          return phrase_.get();
        if (generation + 1 == generation_)
          return retired_.get();
        return nullptr;
      }
      // Message thread, holding the engine's lock (or with processing paused):
      // where in the song the slot's notes start and which stretches of it
      // have played, to save with the host's project and restore it.
      std::vector<double> songNotes() const { return song_notes_; }
      std::vector<Range> playedRanges() const { return played_; }
      void setSong(std::vector<double> notes, std::vector<Range> played);

      // Before each block the host plays, from the engine: the song position
      // where the block starts, in quarter notes, the tempo, the block's
      // length and the host's loop (`loop_end` <= `loop_start` when it does
      // not loop). It forgets the note starts it remembered in that stretch.
      void setSongPosition(double quarters, double quarters_per_second, double seconds,
                           double loop_start = 0.0, double loop_end = 0.0);
      // Once per block, from the voice handler. The song position lasts one block.
      void advanceClock(int num_samples, float sample_rate) {
        if (sample_rate > 0.0f)
          clock_ += num_samples / static_cast<double>(sample_rate);
        song_continues_ = song_playing_;
        song_playing_ = false;
      }
      // Before the voices render each block, from the voice handler: how many
      // keyboard notes playing the slot are down where the block starts, and
      // the samples into the block where `num_releases` of them are released.
      void setHeldNotes(int held, const int* releases, int num_releases) {
        held_notes_ = held;
        num_releases_ = std::min(num_releases, kMaxHeldReleases);
        for (int i = 0; i < num_releases_; ++i)
          releases_[static_cast<size_t>(i)] = releases[i];
      }
      // Keyboard notes still down `sample` samples into the block, not
      // counting notes that start in it.
      int heldNotesAt(int sample) const {
        int held = held_notes_;
        for (int i = 0; i < num_releases_; ++i)
          held -= releases_[static_cast<size_t>(i)] <= sample ? 1 : 0;
        return held;
      }
      // The unit of `split` (speech::Split) a note starting `offset` seconds
      // (`sample` samples) into this block sings, or -1 when there is none.
      int takeUnit(double offset, int split = speech::kSplitSyllable, int sample = 0);
      // The next note sings the first syllable (transport start, all sounds off).
      void rewind() {
        next_syllable_ = 0;
        last_syllable_ = -1;
      }

    private:
      // Where playback starts or a loop returns this block: a note-on there
      // with no note remembered starting at it, in a stretch already played,
      // is a held note the host chased.
      struct ChasePoint {
        double position = 0.0;
        bool chased = false;
      };

      int takeSongSyllable(double quarters, int count);
      double wrap(double quarters) const;
      void openChase(double position);
      // Forgets the note starts in [start, end) and marks it played.
      void replaceStretch(double start, double end);
      bool played(double position) const;
      void reserveSong() {
        if (song_notes_.capacity() < kMaxSongNotes)
          song_notes_.reserve(kMaxSongNotes);
        if (played_.capacity() < kMaxPlayedRanges)
          played_.reserve(kMaxPlayedRanges);
      }

      std::shared_ptr<const speech::Phrase> phrase_;
      std::shared_ptr<const speech::Phrase> retired_;
      uint32_t generation_ = 0;
      double clock_ = 0.0;
      double last_start_ = -1.0e9;
      int next_syllable_ = 0;
      int last_syllable_ = -1;
      // Sorted song positions of the notes that started a syllable (the
      // first note of each chord), and the sorted, separate stretches of the
      // song that have played. Reserved off the audio thread.
      std::vector<double> song_notes_;
      std::vector<Range> played_;
      bool song_playing_ = false;
      // The previous block had a song position, which ended at `song_end_`
      // (not wrapped at the loop, so a host returning to the loop start is a jump).
      bool song_continues_ = false;
      double song_end_ = 0.0;
      double song_quarters_ = 0.0;
      double quarters_per_second_ = 0.0;
      double loop_start_ = 0.0;
      double loop_end_ = 0.0;
      std::array<ChasePoint, 2> chase_ {};
      int num_chase_ = 0;
      int held_notes_ = 0;
      std::array<int, kMaxHeldReleases> releases_ {};
      int num_releases_ = 0;
  };
} // namespace vital
