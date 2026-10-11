// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_instrument.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace vital {
namespace multisample {

  float Region::baseGain() const {
    return std::pow(10.0f, volume_db / 20.0f) * amplitude;
  }

  void Region::buildVelocityCurve() {
    // Points from amp_velcurve_N, with the SFZ defaults at both ends.
    std::map<int, float> points;
    for (const auto& point : velocity_points)
      points[std::max(0, std::min(127, point.first))] = point.second;

    const bool custom = !points.empty();
    if (custom) {
      if (points.count(0) == 0)
        points[0] = 0.0f;
      if (points.count(127) == 0)
        points[127] = 1.0f;
    }

    const float track = std::max(-1.0f, std::min(1.0f, amp_veltrack));
    for (int velocity = 0; velocity < 128; ++velocity) {
      float curve;
      if (custom) {
        auto upper = points.lower_bound(velocity);
        if (upper->first == velocity)
          curve = upper->second;
        else {
          auto lower = std::prev(upper);
          const float t = (velocity - lower->first) / static_cast<float>(upper->first - lower->first);
          curve = lower->second + (upper->second - lower->second) * t;
        }
      }
      else {
        const float normalized = velocity / 127.0f;
        curve = normalized * normalized;
      }
      curve = std::max(0.0f, std::min(1.0f, curve));
      float gain = std::fabs(track) * (1.0f - curve);
      velocity_gain[velocity] = track < 0.0f ? gain : 1.0f - gain;
    }
  }

  void Instrument::build() {
    for (auto& keys : attack_by_key)
      keys.clear();
    for (auto& keys : release_by_key)
      keys.clear();
    for (auto& ccs : cc_triggered)
      ccs.clear();

    std::map<int64_t, int> group_slots;
    auto slotFor = [&group_slots](int64_t group) {
      auto found = group_slots.find(group);
      if (found != group_slots.end())
        return found->second;
      const int slot = static_cast<int>(group_slots.size());
      group_slots[group] = slot;
      return slot;
    };

    uses_cc_state = false;
    for (int index = 0; index < static_cast<int>(regions.size()); ++index) {
      Region& region = regions[index];
      region.group_slot = region.group != 0 ? slotFor(region.group) : -1;
      region.off_by_slot = region.off_by != 0 ? slotFor(region.off_by) : -1;
      if (!region.cc_gates.empty() || !region.amplitude_cc.empty() || !region.volume_cc.empty() ||
          !region.pan_cc.empty() || !region.ampeg_cc.empty()) {
        uses_cc_state = true;
      }

      if (region.sw_lokey >= 0 && region.sw_hikey >= region.sw_lokey) {
        sw_lokey = std::min(sw_lokey, region.sw_lokey);
        sw_hikey = std::max(sw_hikey, region.sw_hikey);
      }
      if (region.sw_last >= 0) {
        sw_lokey = std::min(sw_lokey, region.sw_last);
        sw_hikey = std::max(sw_hikey, region.sw_last);
      }

      if (region.trigger == Trigger::kCc) {
        if (region.on_cc >= 0 && region.on_cc < kNumCcs) {
          cc_triggered[region.on_cc].push_back(index);
          uses_cc_state = true;
        }
        continue;
      }

      const int low = std::max(0, region.lokey);
      const int high = std::min(kNumKeys - 1, region.hikey);
      for (int key = low; key <= high; ++key) {
        if (region.trigger == Trigger::kRelease)
          release_by_key[key].push_back(index);
        else
          attack_by_key[key].push_back(index);
      }
    }
    num_group_slots = static_cast<int>(group_slots.size());
  }

  int parseNoteName(const std::string& text) {
    if (text.empty())
      return -1;
    static const int kOffsets[] = { 9, 11, 0, 2, 4, 5, 7 };
    const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
    if (letter < 'a' || letter > 'g')
      return -1;
    int note = kOffsets[letter - 'a'];
    size_t position = 1;
    if (position < text.size() && (text[position] == '#' || text[position] == 's')) {
      note++;
      position++;
    }
    else if (position < text.size() && text[position] == 'b') {
      note--;
      position++;
    }
    if (position >= text.size())
      return -1;
    char* end = nullptr;
    const long octave = std::strtol(text.c_str() + position, &end, 10);
    if (end == text.c_str() + position || *end != '\0')
      return -1;
    return static_cast<int>((octave + 1) * 12 + note);
  }

  int parseKey(const std::string& text, int fallback) {
    if (text.empty())
      return fallback;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end != text.c_str() && *end == '\0')
      return static_cast<int>(value);
    const int note = parseNoteName(text);
    return note >= 0 ? note : fallback;
  }
} // namespace multisample
} // namespace vital
