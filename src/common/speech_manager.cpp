// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech_manager.h"

#include <chrono>

namespace atlas::openevv {
  namespace {
    // How often the worker looks at the voice controls for a change.
    constexpr auto kPollInterval = std::chrono::milliseconds(40);
  }

  SpeechManager::SpeechManager(RequestReader reader, Deliver deliver) :
      reader_(std::move(reader)), deliver_(std::move(deliver)) {
    if (!available()) {
      for (Status& status : statuses_)
        status.state = State::kUnavailable;
    }
    thread_ = std::thread([this] { run(); });
  }

  SpeechManager::~SpeechManager() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  void SpeechManager::setText(int slot, const std::string& text) {
    if (slot < 0 || slot >= vital::kNumOscillators)
      return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      texts_[static_cast<size_t>(slot)] = text;
      poked_ = true;
    }
    wake_.notify_all();
  }

  std::string SpeechManager::text(int slot) const {
    if (slot < 0 || slot >= vital::kNumOscillators)
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return texts_[static_cast<size_t>(slot)];
  }

  SpeechManager::Status SpeechManager::status(int slot) const {
    if (slot < 0 || slot >= vital::kNumOscillators)
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return statuses_[static_cast<size_t>(slot)];
  }

  void SpeechManager::poke() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      poked_ = true;
    }
    wake_.notify_all();
  }

  void SpeechManager::renderPendingNow() {
    for (int slot = 0; slot < vital::kNumOscillators; ++slot)
      update(slot);
  }

  void SpeechManager::update(int slot) {
    if (!available())
      return;
    std::unique_lock<std::mutex> render_lock(render_mutex_);
    const std::string text = this->text(slot);
    const std::optional<RenderRequest> request = reader_(slot, text);
    if (!request.has_value())
      return;
    uint64_t ticket = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto& rendered = rendered_[static_cast<size_t>(slot)];
      if (rendered.has_value() && *rendered == *request)
        return;
      ticket = ++tickets_[static_cast<size_t>(slot)];
      statuses_[static_cast<size_t>(slot)].state = State::kRendering;
    }

    std::string error;
    std::shared_ptr<const Utterance> utterance;
    if (!request->text.empty())
      utterance = render(*request, error);
    else
      utterance = std::make_shared<Utterance>();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      Status& status = statuses_[static_cast<size_t>(slot)];
      rendered_[static_cast<size_t>(slot)] = request;
      ++status.renders;
      status.ticket = ticket;
      status.error = error;
      if (utterance == nullptr)
        status.state = State::kFailed;
      else {
        status.state = request->text.empty() ? State::kEmpty : State::kReady;
        status.utterance = utterance;
      }
    }
    // The hand-off takes the synth's lock, which a preset load on another
    // thread may hold while it renders here too: never wait for it holding ours.
    render_lock.unlock();
    if (utterance != nullptr)
      deliver_(slot, std::move(utterance), ticket);
  }

  void SpeechManager::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      wake_.wait_for(lock, kPollInterval, [this] { return stop_ || poked_; });
      if (stop_)
        break;
      poked_ = false;
      lock.unlock();
      for (int slot = 0; slot < vital::kNumOscillators; ++slot)
        update(slot);
      lock.lock();
    }
  }
} // namespace atlas::openevv
