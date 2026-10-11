#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// Accessible table-based LFO point editor
// Each point has Position (%), Value (%), and Curve (%) sliders
class LFOPatternEditor : public juce::Component,
                         public juce::TableListBoxModel
{
public:
    LFOPatternEditor (BPScreamProcessor& proc);

    void setLFOAndPattern (int lfoIndex, int patternIndex);
    void refreshFromData();

    void resized() override;

    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell (juce::Graphics&, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    juce::Component* refreshComponentForCell (int rowNumber, int columnId, bool isRowSelected,
                                               juce::Component* existingComponentToUpdate) override;

private:
    BPScreamProcessor& processor;
    int currentLFO = 0;
    int currentPattern = 0;

    juce::TableListBox table;
    juce::TextButton addButton { "Add Point" };
    juce::TextButton deleteButton { "Delete Point" };

    // Custom cell component with a slider
    class SliderCell : public juce::Component
    {
    public:
        SliderCell (LFOPatternEditor& owner);
        void resized() override;
        void setRowAndColumn (int row, int col);

        juce::Slider slider;
    private:
        LFOPatternEditor& owner;
        int row = 0, col = 0;
    };

    void pointValueChanged (int row, int col, float newValue);
    void addPoint();
    void deletePoint();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LFOPatternEditor)
};
