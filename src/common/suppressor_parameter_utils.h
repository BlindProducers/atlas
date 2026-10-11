#pragma once
#include <algorithm>
#include <cmath>
#include <string>

namespace vital::suppressor {
  inline bool isFrequency(const std::string& name) {
    constexpr const char* suffix = "suppressor_frequency";
    constexpr size_t length = sizeof("suppressor_frequency") - 1;
    return name.size() >= length && name.compare(name.size() - length, length, suffix) == 0;
  }
  inline double positionHz(double position, double sample_rate) {
    return std::pow(1.0 + position / 384.0, 12.0) * sample_rate / 8192.0;
  }
  inline double hzPosition(double hz, double sample_rate) {
    return 384.0 * (std::pow(std::max(0.0, hz) * 8192.0 / sample_rate, 1.0 / 12.0) - 1.0);
  }
}
