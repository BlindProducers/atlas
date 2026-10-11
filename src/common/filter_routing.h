// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "synth_constants.h"

namespace vital {
  enum class FilterRouting { kParallel, kSerialForward, kSerialBackward };

  inline FilterRouting filterRouting(bool filter1_on, bool filter2_on,
                                     bool filter1_input, bool filter2_input) {
    // Preserve the engine's precedence if a preset/host enables both links.
    if (filter1_on && filter1_input)
      return FilterRouting::kSerialBackward;
    if (filter2_on && filter2_input)
      return FilterRouting::kSerialForward;
    return FilterRouting::kParallel;
  }

  inline int filterSourceDestination(int destination, FilterRouting routing,
                                     bool filter1_on, bool filter2_on) {
    // A source assigned to both filters enters the serial chain only once.
    // Bypass a disabled first stage into the enabled last stage of the chain.
    if (routing == FilterRouting::kSerialForward &&
        (destination == constants::kFilter1 || destination == constants::kDualFilters))
      return filter1_on ? constants::kFilter1 : constants::kFilter2;
    if (routing == FilterRouting::kSerialBackward &&
        (destination == constants::kFilter2 || destination == constants::kDualFilters))
      return filter2_on ? constants::kFilter2 : constants::kFilter1;
    return destination;
  }
}
