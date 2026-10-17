// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_slot.h"

#include <algorithm>
#include <cmath>

namespace vital {
  void SpeechSlot::setPhrase(std::shared_ptr<const speech::Phrase> phrase) {
    if (phrase != nullptr)
      reserveSong();
    retired_ = std::move(phrase_);
    phrase_ = std::move(phrase);
    generation_++;
    rewind();
  }

  void SpeechSlot::setSong(std::vector<double> notes, std::vector<Range> played) {
    std::sort(notes.begin(), notes.end());
    if (notes.size() > kMaxSongNotes)
      notes.resize(kMaxSongNotes);
    song_notes_ = std::move(notes);
    played_.clear();
    if (!song_notes_.empty() || !played.empty() || phrase_ != nullptr)
      reserveSong();
    std::sort(played.begin(), played.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
    for (const Range& range : played) {
      if (range.end <= range.start)
        continue;
      if (!played_.empty() && range.start <= played_.back().end)
        played_.back().end = std::max(played_.back().end, range.end);
      else if (played_.size() < kMaxPlayedRanges)
        played_.push_back(range);
    }
  }

  double SpeechSlot::wrap(double quarters) const {
    if (loop_end_ <= loop_start_ || quarters < loop_end_)
      return quarters;
    return loop_start_ + std::fmod(quarters - loop_end_, loop_end_ - loop_start_);
  }

  bool SpeechSlot::played(double position) const {
    // The stretch played on both sides of `position`.
    auto range = std::lower_bound(played_.begin(), played_.end(), position,
                                  [](const Range& r, double value) { return r.end < value; });
    return range != played_.end() && range->start <= position - kSamePlace && range->end >= position + kSamePlace;
  }

  void SpeechSlot::openChase(double position) {
    if (num_chase_ >= static_cast<int>(chase_.size()))
      return;
    // Checked before this block forgets the notes it replaces.
    auto near = std::lower_bound(song_notes_.begin(), song_notes_.end(), position - kSamePlace);
    const bool note_starts_here = near != song_notes_.end() && *near <= position + kSamePlace;
    chase_[static_cast<size_t>(num_chase_++)] = { position, played(position) && !note_starts_here };
  }

  void SpeechSlot::replaceStretch(double start, double end) {
    if (end <= start)
      return;
    auto first = std::lower_bound(song_notes_.begin(), song_notes_.end(), start);
    auto last = std::lower_bound(first, song_notes_.end(), end);
    song_notes_.erase(first, last);

    // Merge [start, end) into the played stretches; never allocates.
    auto range = std::lower_bound(played_.begin(), played_.end(), start,
                                  [](const Range& r, double value) { return r.end < value; });
    if (range == played_.end() || range->start > end) {
      if (played_.size() < played_.capacity())
        played_.insert(range, { start, end });
      return;
    }
    range->start = std::min(range->start, start);
    range->end = std::max(range->end, end);
    auto next = range + 1;
    while (next != played_.end() && next->start <= range->end) {
      range->end = std::max(range->end, next->end);
      ++next;
    }
    played_.erase(range + 1, next);
  }

  void SpeechSlot::setSongPosition(double quarters, double quarters_per_second, double seconds,
                                   double loop_start, double loop_end) {
    song_playing_ = quarters_per_second > 0.0;
    quarters_per_second_ = quarters_per_second;
    loop_start_ = loop_start;
    loop_end_ = loop_end;
    num_chase_ = 0;
    if (!song_playing_)
      return;
    quarters = wrap(quarters);
    song_quarters_ = quarters;
    const double end = quarters + seconds * quarters_per_second;
    if (!song_continues_ || std::abs(quarters - song_end_) > kSamePlace)
      openChase(quarters);
    song_end_ = end;
    // Notes that play in this block take the place of those remembered here.
    if (loop_end_ > loop_start_ && quarters < loop_end_ && end > loop_end_) {
      // The loop returns inside this block.
      replaceStretch(quarters, loop_end_);
      const double after = std::min(loop_start_ + (end - loop_end_), loop_end_);
      openChase(loop_start_);
      replaceStretch(loop_start_, after);
      song_end_ = after;
    }
    else
      replaceStretch(quarters, end);
  }

  int SpeechSlot::takeSongSyllable(double quarters, int count) {
    quarters = wrap(quarters);
    for (int i = 0; i < num_chase_; ++i) {
      const ChasePoint& point = chase_[static_cast<size_t>(i)];
      if (point.chased && std::abs(quarters - point.position) <= kSamePlace) {
        // The held note's syllable: that of the last note before it.
        auto after = std::lower_bound(song_notes_.begin(), song_notes_.end(), point.position - kSamePlace);
        const size_t index = static_cast<size_t>(after - song_notes_.begin());
        return index == 0 ? 0 : static_cast<int>((index - 1) % static_cast<size_t>(count));
      }
    }

    // A note in a chord sings the syllable of the chord's first note.
    const double chord = kChordSeconds * quarters_per_second_;
    auto chord_start = std::lower_bound(song_notes_.begin(), song_notes_.end(), quarters - chord);
    size_t index = 0;
    if (chord_start != song_notes_.end() && *chord_start <= quarters)
      index = static_cast<size_t>(chord_start - song_notes_.begin());
    else {
      auto after = std::lower_bound(chord_start, song_notes_.end(), quarters);
      index = static_cast<size_t>(after - song_notes_.begin());
      // Inserting never allocates: the capacity was reserved off the audio thread.
      if (song_notes_.size() < song_notes_.capacity())
        song_notes_.insert(after, quarters);
    }
    return static_cast<int>(index % static_cast<size_t>(count));
  }

  int SpeechSlot::takeSyllable(double offset) {
    const speech::Phrase* phrase = phrase_.get();
    if (phrase == nullptr || phrase->numSyllables() == 0)
      return -1;
    const int count = phrase->numSyllables();
    const double start = clock_ + offset;
    if (song_playing_) {
      last_syllable_ = takeSongSyllable(song_quarters_ + offset * quarters_per_second_, count);
      last_start_ = start;
      next_syllable_ = (last_syllable_ + 1) % count;
      return last_syllable_;
    }
    if (last_syllable_ >= 0 && last_syllable_ < count && start - last_start_ < kChordSeconds)
      return last_syllable_;
    if (next_syllable_ < 0 || next_syllable_ >= count)
      next_syllable_ = 0;
    last_syllable_ = next_syllable_;
    last_start_ = start;
    next_syllable_ = (next_syllable_ + 1) % count;
    return last_syllable_;
  }
} // namespace vital
