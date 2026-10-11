#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class LFOPatternEditor;

class LFOSection : public juce::Component,
                   public juce::ComboBox::Listener
{
public:
    LFOSection (BPScreamProcessor& proc);
    ~LFOSection() override;

    void resized() override;
    void comboBoxChanged (juce::ComboBox* box) override;

    void refreshForSelectedLFO();

    std::function<void()> onPatternChanged;

    int getSelectedLFO() const { return selectedLFO; }
    int getSelectedPattern() const { return patternCombo.getSelectedId() - 1; }

private:
    BPScreamProcessor& processor;
    int selectedLFO = 0;

    juce::TextButton lfo1Button { "LFO 1" };
    juce::TextButton lfo2Button { "LFO 2" };

    juce::Label patternLabel { {}, "Pattern:" };
    juce::ComboBox patternCombo;

    juce::Label loopTypeLabel { {}, "Loop Type:" };
    juce::ComboBox loopTypeCombo;

    juce::Label rateTypeLabel { {}, "Rate Type:" };
    juce::ComboBox rateTypeCombo;

    juce::Label syncRateLabel { {}, "Sync Rate:" };
    juce::ComboBox syncRateCombo;

    juce::Label freeRateLabel { {}, "Free Rate:" };
    juce::Slider freeRateSlider;

    juce::Label shapeLabel { {}, "Shape Preset:" };
    juce::ComboBox shapeCombo;

    std::unique_ptr<juce::SliderParameterAttachment> freeRateAttach;

    void updateVisibility();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LFOSection)
};
