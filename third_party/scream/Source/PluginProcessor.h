#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/ScreamDSP.h"
#include "LFO/LFOData.h"
#include "LFO/LFOProcessor.h"

class BPScreamProcessor : public juce::AudioProcessor
{
public:
    BPScreamProcessor();
    ~BPScreamProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    // Non-APVTS state
    std::atomic<bool> autogainOn { true };
    std::atomic<bool> midiKeytracking { false };

    LFOData lfos[2];
    float lfoModAmounts[5][2] = {};  // [param][lfo] -> -1..1
    std::mutex modAmountsMutex;

    // LFO processors
    LFOProcessor lfoProcessors[2];

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    ScreamDSP dsp;

    // Cached param pointers
    std::atomic<float>* cutoffParam      = nullptr;
    std::atomic<float>* screamParam      = nullptr;
    std::atomic<float>* resonanceParam   = nullptr;
    std::atomic<float>* inputGainParam   = nullptr;
    std::atomic<float>* wetParam         = nullptr;
    std::atomic<float>* outputGainParam  = nullptr;
    std::atomic<float>* lfoPattern1Param = nullptr;
    std::atomic<float>* lfoPattern2Param = nullptr;
    std::atomic<float>* lfoRateType1Param = nullptr;
    std::atomic<float>* lfoRateType2Param = nullptr;
    std::atomic<float>* lfoSyncRate1Param = nullptr;
    std::atomic<float>* lfoSyncRate2Param = nullptr;
    std::atomic<float>* lfoFreeRate1Param = nullptr;
    std::atomic<float>* lfoFreeRate2Param = nullptr;
    std::atomic<float>* gateReleaseParam  = nullptr;

    // Retrigger state
    float retrigDetection = 0;
    int   keytrackingLastMidiNote = -1;

    double currentBPM = 120.0;
    double sampleRate_ = 44100.0;

    // Temp buffers for LFO
    std::vector<float> lfoBuffer1, lfoBuffer2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BPScreamProcessor)
};
