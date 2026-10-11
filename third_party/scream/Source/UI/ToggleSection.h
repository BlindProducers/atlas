#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class ToggleSection : public juce::Component
{
public:
    ToggleSection (BPScreamProcessor& proc);

    void resized() override;

private:
    BPScreamProcessor& processor;

    juce::ToggleButton autogainToggle { "Auto Gain" };
    juce::ToggleButton keytrackToggle { "MIDI Key Tracking" };

    juce::Label gateReleaseLabel { {}, "Gate Release:" };
    juce::Slider gateReleaseSlider;
    std::unique_ptr<juce::SliderParameterAttachment> gateReleaseAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToggleSection)
};
