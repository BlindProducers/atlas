#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace vital::scream {
  inline bool parameterIs(const std::string& name, const std::string& suffix) {
    const std::string id = "scream_" + suffix;
    return name == id || (name.size() > id.size() &&
        name.compare(name.size() - id.size() - 1, id.size() + 1, "_" + id) == 0);
  }

  // BP Scream's display intentionally uses ten octaves, while its DSP uses
  // the supplied MIDI-note endpoints. Keep both original mappings intact.
  inline float cutoffToHz(float normalized) {
    return std::min(20000.0f, 20.0f * std::pow(2.0f, normalized * 10.0f));
  }
  inline float cutoffFromHz(float hz) {
    return std::clamp(static_cast<float>(std::log(std::max(20.0f, hz) / 20.0) /
                                        std::log(2.0) / 10.0), 0.0f, 1.0f);
  }
}
