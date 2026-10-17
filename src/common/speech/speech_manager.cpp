// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_manager.h"

#include "espeak_speech.h"
#include "speech_syllables.h"
#include "speech_voices.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace vital {
namespace speech {
  namespace {
    // How often the slots' type, language and variant are checked.
    constexpr int kRefreshMilliseconds = 150;

    std::string phonemeSummary(const Phrase& phrase) {
      std::string summary;
      const Layer* layer = phrase.referenceLayer();
      if (layer == nullptr)
        return summary;
      for (size_t i = 0; i < layer->syllables.size(); ++i) {
        const Syllable& syllable = layer->syllables[i];
        if (i > 0)
          summary += syllable.index_in_word == 0 ? " " : ".";
        summary += syllable.phonemes;
      }
      return summary;
    }

    // Cuts at a character boundary.
    std::string limitText(const std::string& text, size_t limit) {
      if (text.size() <= limit)
        return text;
      size_t end = limit;
      while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
      return text.substr(0, end);
    }
  } // namespace

  Manager::Manager(ApplyCallback apply, VoiceCallback voice) :
      apply_(std::move(apply)), voice_(std::move(voice)) { }

  void Manager::startWatching() {
    startTimer(kRefreshMilliseconds);
  }

  Manager::~Manager() {
    shutting_down_ = true;
    stopTimer();
    pool_.removeAllJobs(true, 30000);
    cancelPendingUpdate();
  }

  void Manager::setText(int slot, const std::string& text) {
    if (slot < 0 || slot >= kNumSlots)
      return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      slots_[static_cast<size_t>(slot)].text = limitText(text, kMaxTextLength);
    }
    refresh();
  }

  std::string Manager::text(int slot) const {
    if (slot < 0 || slot >= kNumSlots)
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_[static_cast<size_t>(slot)].text;
  }

  void Manager::clearAll() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (Slot& slot : slots_)
        slot.text.clear();
    }
    refresh();
  }

  void Manager::setStatus(Slot& slot, SlotStatus::State state, const std::string& error) {
    slot.status.state = state;
    slot.status.error = error;
    if (state != SlotStatus::kReady) {
      slot.status.syllables = 0;
      slot.status.phonemes.clear();
    }
    slot.status.revision++;
  }

  void Manager::refresh() {
    if (shutting_down_)
      return;
    bool cleared = false;
    for (int index = 0; index < kNumSlots; ++index) {
      const Voice voice = voice_ ? voice_(index) : Voice();
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& slot = slots_[static_cast<size_t>(index)];
      Key desired;
      desired.active = voice.active && !slot.text.empty();
      if (desired.active) {
        desired.text = slot.text;
        desired.language = std::clamp(voice.language, 0, kNumLanguages - 1);
        desired.variant = std::clamp(voice.variant, 0, kNumVariants - 1);
      }
      if (desired == slot.key)
        continue;
      slot.key = desired;
      const uint64_t request = next_request_++;
      slot.request = request;
      if (!desired.active) {
        // Nothing to sing: free the phrase.
        slot.pending = nullptr;
        slot.has_pending = slot.current != nullptr;
        setStatus(slot, SlotStatus::kEmpty);
        cleared = cleared || slot.has_pending;
        continue;
      }
      setStatus(slot, SlotStatus::kRendering);
      rendering_++;
      pool_.addJob([this, index, request, desired] { render(index, request, desired); });
    }
    if (cleared)
      triggerAsyncUpdate();
  }

  void Manager::render(int index, uint64_t request, Key key) {
    std::shared_ptr<Phrase> phrase;
    std::string error;
    bool superseded = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      superseded = slots_[static_cast<size_t>(index)].request != request;
    }
    if (!superseded && !shutting_down_) {
      std::vector<Recording> recordings;
      bool spoken = true;
      for (int pitch : kLayerPitches) {
        recordings.emplace_back();
        spoken = espeak::speak(key.text, voiceName(key.language, key.variant), pitch, recordings.back(), error);
        if (!spoken || shutting_down_)
          break;
      }
      if (spoken && !shutting_down_) {
        phrase = buildPhrase(recordings, kReferenceLayer);
        phrase->text = key.text;
        phrase->language = key.language;
        phrase->variant = key.variant;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Slot& slot = slots_[static_cast<size_t>(index)];
      if (slot.request == request && !superseded) {
        slot.pending = phrase;
        slot.has_pending = true;
        if (phrase == nullptr) {
          setStatus(slot, SlotStatus::kFailed, error);
        }
        else if (phrase->numSyllables() == 0) {
          setStatus(slot, SlotStatus::kFailed, "espeak-ng found nothing to sing in the text");
        }
        else {
          setStatus(slot, SlotStatus::kReady);
          slot.status.syllables = phrase->numSyllables();
          slot.status.phonemes = phonemeSummary(*phrase);
        }
      }
    }
    rendering_--;
    if (!shutting_down_)
      triggerAsyncUpdate();
  }

  SlotStatus Manager::status(int slot) const {
    if (slot < 0 || slot >= kNumSlots)
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_[static_cast<size_t>(slot)].status;
  }

  std::shared_ptr<const Phrase> Manager::phrase(int slot) const {
    if (slot < 0 || slot >= kNumSlots)
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_[static_cast<size_t>(slot)].current;
  }

  bool Manager::hasPendingWork() const {
    if (rendering_ > 0)
      return true;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Slot& slot : slots_) {
      if (slot.has_pending)
        return true;
    }
    return false;
  }

  bool Manager::waitForPendingWork(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (rendering_ > 0) {
      if (std::chrono::steady_clock::now() > deadline)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
  }

  void Manager::processPendingResults(bool blocking) {
    std::unique_lock<std::mutex> apply_lock(apply_mutex_, std::defer_lock);
    if (blocking)
      apply_lock.lock();
    else if (!apply_lock.try_lock())
      return;
    for (int index = 0; index < kNumSlots; ++index) {
      std::shared_ptr<const Phrase> phrase;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[static_cast<size_t>(index)];
        if (!slot.has_pending)
          continue;
        slot.has_pending = false;
        phrase = std::move(slot.pending);
        slot.pending = nullptr;
        slot.current = phrase;
      }
      if (apply_)
        apply_(index, std::move(phrase));
    }
  }
} // namespace speech
} // namespace vital
