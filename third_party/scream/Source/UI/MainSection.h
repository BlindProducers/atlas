#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

class MainSection : public juce::Component
{
public:
    MainSection (juce::AudioProcessorValueTreeState& apvts);

    void resized() override;

private:
    juce::GroupComponent filterGroup { {}, "Filter" };
    juce::GroupComponent gainGroup   { {}, "Gain" };

    juce::Slider cutoffSlider, screamSlider, resonanceSlider;
    juce::Slider inputGainSlider, wetSlider, outputGainSlider;

    juce::Label cutoffLabel, screamLabel, resonanceLabel;
    juce::Label inputGainLabel, wetLabel, outputGainLabel;

    std::unique_ptr<juce::SliderParameterAttachment> cutoffAttach, screamAttach, resonanceAttach;
    std::unique_ptr<juce::SliderParameterAttachment> inputGainAttach, wetAttach, outputGainAttach;

    void setupSlider (juce::Slider& slider, juce::Label& label, const juce::String& name,
                      juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                      std::unique_ptr<juce::SliderParameterAttachment>& attachment);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainSection)
};
