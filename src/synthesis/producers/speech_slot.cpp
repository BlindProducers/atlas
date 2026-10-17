// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_slot.h"

#include <algorithm>
#include <cmath>

namespace vital {
  std::shared_ptr<const atlas::openevv::Utterance> SpeechSlot::setUtterance(
      std::shared_ptr<const atlas::openevv::Utterance> utterance, uint64_t ticket) {
    if (ticket != 0 && ticket < ticket_)
      return utterance;
    ticket_ = std::max(ticket_, ticket);
    // A render of the same words in another voice carries on where the keys
    // were; new words start from the top.
    const bool same_words = utterance_ != nullptr && utterance != nullptr &&
                            utterance_->request.text == utterance->request.text &&
                            utterance_->request.language == utterance->request.language;
    std::shared_ptr<const atlas::openevv::Utterance> dropped = std::move(retired_);
    retired_ = std::move(utterance_);
    utterance_ = std::move(utterance);
    ++generation_;
    if (!same_words)
      next_ = 0;
    last_unit_ = -1;
    last_time_ = -1.0;
    last_played_.store(-1);
    return dropped;
  }

  int SpeechSlot::takeUnit(int split, bool loop, double time) {
    if (restart_.exchange(false)) {
      next_ = 0;
      last_unit_ = -1;
    }
    const atlas::openevv::Utterance* utterance = utterance_.get();
    if (utterance == nullptr)
      return -1;
    const int count = static_cast<int>(utterance->unitsFor(split).size());
    if (count == 0)
      return -1;

    if (last_unit_ >= 0 && last_split_ == split && last_generation_ == generation_ &&
        std::abs(time - last_time_) <= kChordSeconds)
      return last_unit_;

    // Another split counts in its own pieces; start it from the top.
    if (split != last_split_)
      next_ = 0;
    int unit = next_;
    if (unit >= count) {
      if (!loop)
        return -1;
      unit = 0;
    }
    next_ = unit + 1;
    last_unit_ = unit;
    last_split_ = split;
    last_generation_ = generation_;
    last_time_ = time;
    last_played_of_.store(count);
    last_played_split_.store(split);
    last_played_generation_.store(generation_);
    last_played_.store(unit);
    return unit;
  }

  void SpeechSlot::keyUp(bool restart) {
    if (keys_held_ > 0)
      --keys_held_;
    if (restart && keys_held_ == 0) {
      next_ = 0;
      last_unit_ = -1;
    }
  }
} // namespace vital
