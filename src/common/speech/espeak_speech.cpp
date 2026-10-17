// SPDX-License-Identifier: GPL-3.0-or-later
#include "espeak_speech.h"

#include "tape_noise_library.h"

#include <espeak-ng/speak_lib.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace vital {
namespace speech {
namespace espeak {
  namespace {
    std::mutex& engineMutex() {
      static std::mutex mutex;
      return mutex;
    }

    juce::File& chosenDirectory() {
      static juce::File directory;
      return directory;
    }

    // Initialised once per process; espeak-ng cannot be reinitialised safely.
    bool initialised = false;
    bool failed = false;
    double engine_sample_rate = 22050.0;

    // The recording the synthesis callback appends to (under engineMutex).
    Recording* current = nullptr;
    int current_word = -1;

    int synthCallback(short* samples, int num_samples, espeak_EVENT* events) {
      if (current == nullptr)
        return 1;
      for (espeak_EVENT* event = events; event != nullptr && event->type != espeakEVENT_LIST_TERMINATED; ++event) {
        if (event->type == espeakEVENT_WORD) {
          ++current_word;
        }
        else if (event->type == espeakEVENT_PHONEME) {
          // An IPA name fills up to eight bytes, with no terminator then.
          const char* name = event->id.string;
          const size_t length = strnlen(name, sizeof(event->id.string));
          current->phonemes.push_back({ std::string(name, length), event->sample, current_word });
        }
        else if (event->type == espeakEVENT_END) {
          // A clause ends with a pause; it ends the word before it.
          current->phonemes.push_back({ std::string(), event->sample, current_word });
        }
      }
      if (samples != nullptr) {
        for (int i = 0; i < num_samples; ++i)
          current->audio.push_back(samples[i] * (1.0f / 32768.0f));
      }
      return 0;
    }

    bool initialise(std::string& error) {
      if (initialised)
        return true;
      if (failed) {
        error = "espeak-ng could not start";
        return false;
      }
      const juce::File directory = dataDirectory();
      if (!directory.getChildFile("espeak-ng-data").getChildFile("phontab").existsAsFile()) {
        failed = true;
        error = "espeak-ng data is missing from " + directory.getFullPathName().toStdString();
        return false;
      }
      const std::string path = directory.getFullPathName().toStdString();
      const int sample_rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, path.c_str(),
                                                espeakINITIALIZE_PHONEME_EVENTS | espeakINITIALIZE_PHONEME_IPA |
                                                espeakINITIALIZE_DONT_EXIT);
      if (sample_rate <= 0) {
        failed = true;
        error = "espeak-ng could not start";
        return false;
      }
      engine_sample_rate = sample_rate;
      espeak_SetSynthCallback(synthCallback);
      initialised = true;
      return true;
    }
  } // namespace

  juce::File dataDirectory() {
    if (chosenDirectory() != juce::File())
      return chosenDirectory();
    return bundleResourcesDirectory();
  }

  void setDataDirectory(const juce::File& directory) {
    std::lock_guard<std::mutex> lock(engineMutex());
    if (!initialised) {
      chosenDirectory() = directory;
      failed = false;
    }
  }

  bool speak(const std::string& text, const std::string& voice, int pitch, Recording& recording,
             std::string& error) {
    std::lock_guard<std::mutex> lock(engineMutex());
    recording = Recording();
    if (!initialise(error))
      return false;
    if (espeak_SetVoiceByName(voice.c_str()) != EE_OK) {
      error = "espeak-ng has no voice " + voice;
      return false;
    }
    // A monotone at the voice's own pitch: the notes give the melody.
    espeak_SetParameter(espeakRANGE, 0, 0);
    espeak_SetParameter(espeakPITCH, std::clamp(pitch, 0, kMaxPitch), 0);
    espeak_SetParameter(espeakPUNCTUATION, espeakPUNCT_NONE, 0);

    recording.sample_rate = engine_sample_rate;
    current = &recording;
    current_word = -1;
    const espeak_ERROR result = espeak_Synth(text.c_str(), text.size() + 1, 0, POS_CHARACTER, 0,
                                             espeakCHARS_UTF8, nullptr, nullptr);
    espeak_Synchronize();
    current = nullptr;
    if (result != EE_OK) {
      error = "espeak-ng could not speak the text";
      return false;
    }
    return true;
  }
} // namespace espeak
} // namespace speech
} // namespace vital
