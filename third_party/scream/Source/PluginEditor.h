#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "UI/MainSection.h"
#include "UI/ToggleSection.h"
#include "UI/LFOSection.h"
#include "UI/LFOPatternEditor.h"
#include "UI/ModMatrixSection.h"

class BPScreamEditor : public juce::AudioProcessorEditor
{
public:
    BPScreamEditor (BPScreamProcessor&);
    ~BPScreamEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    BPScreamProcessor& processorRef;

    juce::Label titleLabel;
    MainSection mainSection;
    ToggleSection toggleSection;
    LFOSection lfoSection;
    LFOPatternEditor patternEditor;
    ModMatrixSection modMatrix;

    void updatePatternEditor();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BPScreamEditor)
};
