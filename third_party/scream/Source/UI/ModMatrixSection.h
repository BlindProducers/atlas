#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class ModMatrixSection : public juce::Component
{
public:
    ModMatrixSection (BPScreamProcessor& proc);

    void resized() override;

private:
    BPScreamProcessor& processor;

    juce::GroupComponent group { {}, "Modulation Matrix" };

    static constexpr int NUM_PARAMS = 5;
    static constexpr int NUM_LFOS = 2;

    juce::Label paramLabels[NUM_PARAMS];
    juce::Slider modSliders[NUM_PARAMS][NUM_LFOS];
    juce::Label lfo1Header, lfo2Header;

    static constexpr const char* paramNames[] = { "Cutoff", "Scream", "Resonance", "Input", "Wet" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModMatrixSection)
};
