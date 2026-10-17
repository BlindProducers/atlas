// SPDX-License-Identifier: GPL-3.0-or-later
#include "tape_noise_library.h"
#include "load_save.h"
#include <mutex>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

namespace vital {
juce::File bundleResourcesDirectory() {
  static int anchor;
 #if JUCE_WINDOWS
  HMODULE module = nullptr;
  wchar_t path[32768] {};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&anchor), &module) ||
      !GetModuleFileNameW(module, path, 32768)) return {};
  const juce::File binary { juce::String(path) };
 #else
  Dl_info info {};
  if (dladdr(&anchor, &info) == 0 || !info.dli_fname) return {};
  const juce::File binary { juce::String(info.dli_fname) };
 #endif
  return binary.getParentDirectory().getSiblingFile("Resources");
}

namespace {
juce::File bundledDirectory() { return bundleResourcesDirectory().getChildFile("Tape"); }
}

std::shared_ptr<TapeNoiseLibrary> TapeNoiseLibrary::shared() {
  static std::mutex mutex;
  static std::weak_ptr<TapeNoiseLibrary> resource;
  std::lock_guard<std::mutex> lock(mutex);
  auto result = resource.lock();
  if (!result) { result = std::make_shared<TapeNoiseLibrary>(); resource = result; }
  return result;
}

TapeNoiseLibrary::TapeNoiseLibrary(juce::File directory) :
    juce::Thread("Atlas Tape noise loader"), directory_(directory), explicit_directory_(directory != juce::File()) {
  for (auto& pointer : published_) pointer.store(nullptr);
  for (auto& state : status_) state.store(0);
  startThread(juce::Thread::Priority::low);
}
TapeNoiseLibrary::~TapeNoiseLibrary() {
  signalThreadShouldExit(); notify(); stopThread(-1);
}
juce::File TapeNoiseLibrary::factoryDirectory() {
  return LoadSave::getDataDirectory().getChildFile("Factory").getChildFile("Samples").getChildFile("Tape");
}
const TapeNoiseLibrary::Clip* TapeNoiseLibrary::request(int index) noexcept {
  index = std::clamp(index, 0, kNumClips - 1);
  const auto* clip = published_[index].load(std::memory_order_acquire);
  if (!clip) requested_.fetch_or(uint32_t(1) << index, std::memory_order_relaxed);
  return clip;
}
int TapeNoiseLibrary::status(int index) const noexcept {
  return status_[std::clamp(index, 0, kNumClips - 1)].load(std::memory_order_acquire);
}
std::unique_ptr<TapeNoiseLibrary::Clip> TapeNoiseLibrary::load(const juce::File& file) {
  // A WAV header alone is 44 bytes. The size cap keeps a damaged header from
  // requesting gigabytes; the longest recording shipped stays far below it.
  if (!file.existsAsFile() || file.getSize() < 44 || file.getSize() > 128 * 1024 * 1024) return {};
  auto stream = file.createInputStream();
  if (!stream) return {};
  juce::WavAudioFormat format;
  std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(stream.release(), true));
  if (!reader || reader->lengthInSamples < 2 || reader->lengthInSamples > 32 * 1024 * 1024 ||
      reader->numChannels < 1 || reader->numChannels > 2 || reader->sampleRate < 8000 || reader->sampleRate > 384000) return {};
  auto clip = std::make_unique<Clip>();
  clip->sample_rate = reader->sampleRate;
  clip->audio.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
  if (!reader->read(&clip->audio, 0, clip->audio.getNumSamples(), 0, true, true)) return {};
  return clip;
}
void TapeNoiseLibrary::run() {
  bool located = false;
  // The recordings inside the plug-in bundle. They are read directly when the
  // factory folder cannot be written (a sandboxed host, or a Documents folder
  // the host was refused) or a copy there is missing or damaged.
  juce::File bundled;
  std::array<double, kNumClips> attempted {};
  while (!threadShouldExit()) {
    const uint32_t requested = requested_.exchange(0, std::memory_order_relaxed);
    if (requested && !located) {
      if (!explicit_directory_) {
        directory_ = factoryDirectory();
        bundled = bundledDirectory();
        // The factory folder is the playback source when it can be written;
        // otherwise the bundle is.
        if (bundled.isDirectory() && directory_.createDirectory().wasOk()) {
          for (int index = 0; index < kNumClips && !threadShouldExit(); ++index) {
            const auto source = bundled.getChildFile(fileName(index));
            const auto destination = directory_.getChildFile(fileName(index));
            if (source.existsAsFile() && !source.hasIdenticalContentTo(destination)) {
              juce::TemporaryFile replacement(destination);
              if (source.copyFileTo(replacement.getFile())) replacement.overwriteTargetFileWithTemporary();
            }
          }
          // Earlier 2.0.0 betas copied 27 encrypted "Vinyl N.atape" clips here.
          // Nothing reads them any more.
          for (int index = 1; index <= 27; ++index)
            directory_.getChildFile("Vinyl " + juce::String(index) + ".atape").deleteFile();
        }
      }
      located = true;
    }
    for (int i = 0; i < kNumClips && !threadShouldExit(); ++i) {
      if (!(requested & (uint32_t(1) << i)) || clips_[i]) continue;
      const double now = juce::Time::getMillisecondCounterHiRes();
      if (attempted[i] != 0 && now - attempted[i] < 1000) continue;
      attempted[i] = now;
      clips_[i] = load(directory_.getChildFile(fileName(i)));
      if (!clips_[i] && bundled.isDirectory())
        clips_[i] = load(bundled.getChildFile(fileName(i)));
      if (clips_[i]) {
        published_[i].store(clips_[i].get(), std::memory_order_release);
        status_[i].store(1, std::memory_order_release);
      }
      else if (status_[i].exchange(-1) != -1)
        juce::Logger::writeToLog("Atlas Tape: missing or invalid noise recording " + fileName(i));
    }
    wait(50);
  }
}
}
