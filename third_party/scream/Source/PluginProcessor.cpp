#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DSP/FastMath.h"
#include "DSP/Dynamics.h"
#include <cmath>

BPScreamProcessor::BPScreamProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    cutoffParam      = apvts.getRawParameterValue ("cutoff");
    screamParam      = apvts.getRawParameterValue ("scream");
    resonanceParam   = apvts.getRawParameterValue ("resonance");
    inputGainParam   = apvts.getRawParameterValue ("inputGain");
    wetParam         = apvts.getRawParameterValue ("wet");
    outputGainParam  = apvts.getRawParameterValue ("outputGain");
    lfoPattern1Param = apvts.getRawParameterValue ("lfoPattern1");
    lfoPattern2Param = apvts.getRawParameterValue ("lfoPattern2");
    lfoRateType1Param = apvts.getRawParameterValue ("lfoRateType1");
    lfoRateType2Param = apvts.getRawParameterValue ("lfoRateType2");
    lfoSyncRate1Param = apvts.getRawParameterValue ("lfoSyncRate1");
    lfoSyncRate2Param = apvts.getRawParameterValue ("lfoSyncRate2");
    lfoFreeRate1Param = apvts.getRawParameterValue ("lfoFreeRate1");
    lfoFreeRate2Param = apvts.getRawParameterValue ("lfoFreeRate2");
    gateReleaseParam  = apvts.getRawParameterValue ("gateRelease");
}

BPScreamProcessor::~BPScreamProcessor() {}

juce::AudioProcessorValueTreeState::ParameterLayout BPScreamProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Cutoff - display as Hz
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID ("cutoff", 1), "Cutoff",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 1.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [](float v, int) {
            float hz = 20.0f * std::pow (2.0f, v * 10.0f);
            hz = std::min (hz, 20000.0f);
            if (hz >= 1000.0f) return juce::String (hz / 1000.0f, 2) + " kHz";
            return juce::String (hz, 1) + " Hz";
        },
        [](const juce::String& s) {
            float hz = s.getFloatValue();
            if (s.containsIgnoreCase ("kHz")) hz *= 1000.0f;
            return (float)(std::log (hz / 20.0) / std::log (2.0) / 10.0);
        }));

    // Scream
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID ("scream", 1), "Scream",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.5f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [](float v, int) { return juce::String (v * 100.0f, 1) + "%"; },
        [](const juce::String& s) { return s.getFloatValue() / 100.0f; }));

    // Resonance
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID ("resonance", 1), "Resonance",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.5f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [](float v, int) { return juce::String (v * 100.0f, 1) + "%"; },
        [](const juce::String& s) { return s.getFloatValue() / 100.0f; }));

    // Input Gain
    auto inputDefault = (float) FastMath::normd (0.0, RANGE_INPUT_GAIN_MIN, RANGE_INPUT_GAIN_MAX);
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID ("inputGain", 1), "Input Gain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), inputDefault,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [](float v, int) {
            float dB = FastMath::lerpf (v, RANGE_INPUT_GAIN_MIN, RANGE_INPUT_GAIN_MAX);
            return juce::String (dB, 1) + " dB";
        },
        [](const juce::String& s) {
            float dB = s.getFloatValue();
            return (float) FastMath::normd (dB, RANGE_INPUT_GAIN_MIN, RANGE_INPUT_GAIN_MAX);
        }));

    // Wet
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID ("wet", 1), "Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 1.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [](float v, int) { return juce::String (v * 100.0f, 1) + "%"; },
        [](const juce::String& s) { return s.getFloatValue() / 100.0f; }));

    // Output Gain
    auto outputDefault = (float) FastMath::normd (-6.0, RANGE_OUTPUT_GAIN_MIN, RANGE_OUTPUT_GAIN_MAX);
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID ("outputGain", 1), "Output Gain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), outputDefault,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [](float v, int) {
            float dB = FastMath::lerpf (v, RANGE_OUTPUT_GAIN_MIN, RANGE_OUTPUT_GAIN_MAX);
            return juce::String (dB, 1) + " dB";
        },
        [](const juce::String& s) {
            float dB = s.getFloatValue();
            return (float) FastMath::normd (dB, RANGE_OUTPUT_GAIN_MIN, RANGE_OUTPUT_GAIN_MAX);
        }));

    // Gate Release
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID ("gateRelease", 1), "Gate Release",
        juce::NormalisableRange<float> (5.0f, 500.0f, 0.1f, 0.4f), 30.0f,
        juce::String(),
        juce::AudioProcessorParameter::genericParameter,
        [](float v, int) { return juce::String (v, 1) + " ms"; },
        [](const juce::String& s) { return s.getFloatValue(); }));

    // LFO parameters (2 LFOs)
    for (int i = 1; i <= 2; i++)
    {
        auto s = juce::String (i);

        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID ("lfoPattern" + s, 1), "LFO " + s + " Pattern", 1, 8, 1));

        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID ("lfoRateType" + s, 1), "LFO " + s + " Rate Type",
            juce::StringArray { "Sync", "Free" }, 0));

        juce::StringArray syncNames;
        for (int j = 0; j < LFO_RATE_COUNT; j++)
            syncNames.add (lfoSyncRateName (j));
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID ("lfoSyncRate" + s, 1), "LFO " + s + " Sync Rate",
            syncNames, 8));

        auto freeDefault = (float) normaliseSec (0.25);
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID ("lfoFreeRate" + s, 1), "LFO " + s + " Free Rate",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), freeDefault,
            juce::String(),
            juce::AudioProcessorParameter::genericParameter,
            [](float v, int) {
                double sec = denormaliseSec (v);
                if (sec < 0.01)       return juce::String (sec * 1000.0, 3) + " ms";
                else if (sec < 0.1)   return juce::String (sec * 1000.0, 2) + " ms";
                else if (sec < 1.0)   return juce::String (sec * 1000.0, 1) + " ms";
                else                  return juce::String (sec, 2) + " s";
            },
            [](const juce::String& s) {
                double sec = s.getDoubleValue();
                if (s.containsIgnoreCase ("ms")) sec /= 1000.0;
                return (float) normaliseSec (sec);
            }));
    }

    return { params.begin(), params.end() };
}

void BPScreamProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;
    dsp.prepare (sampleRate);
    lfoBuffer1.resize (samplesPerBlock);
    lfoBuffer2.resize (samplesPerBlock);
}

void BPScreamProcessor::releaseResources()
{
    dsp.reset();
}

bool BPScreamProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void BPScreamProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const int numFrames = buffer.getNumSamples();
    if (numFrames == 0) return;

    // Ensure stereo
    if (buffer.getNumChannels() < 2) return;

    // Get BPM from host
    if (auto* playHead = getPlayHead())
    {
        auto posInfo = playHead->getPosition();
        if (posInfo.hasValue())
        {
            if (posInfo->getBpm().hasValue())
                currentBPM = *posInfo->getBpm();
        }
    }

    // Process MIDI
    for (const auto metadata : midiMessages)
    {
        auto msg = metadata.getMessage();
        if (msg.isNoteOn())
        {
            keytrackingLastMidiNote = msg.getNoteNumber();
            for (int lfoIdx = 0; lfoIdx < 2; lfoIdx++)
            {
                if (lfos[lfoIdx].loopType == LFOLoopType::Retrig ||
                    lfos[lfoIdx].loopType == LFOLoopType::OneShot)
                {
                    lfos[lfoIdx].phase = 0;
                }
            }
        }
    }

    // Read params
    float audioParams[5] = {
        cutoffParam->load(),
        screamParam->load(),
        resonanceParam->load(),
        inputGainParam->load(),
        wetParam->load()
    };

    float keytrackingOffset = 0;
    if (midiKeytracking.load() && keytrackingLastMidiNote >= 0)
        keytrackingOffset = (float)(keytrackingLastMidiNote - 24);

    // LFO modulation
    uint8_t lfo1ModFlags = 0, lfo2ModFlags = 0;
    float localModAmounts[5][2];
    {
        std::lock_guard<std::mutex> lock (modAmountsMutex);
        std::memcpy (localModAmounts, lfoModAmounts, sizeof (localModAmounts));
    }

    for (int i = 0; i < 5; i++)
    {
        if (std::fabs (localModAmounts[i][0]) > 0) lfo1ModFlags |= (1 << i);
        if (std::fabs (localModAmounts[i][1]) > 0) lfo2ModFlags |= (1 << i);
    }

    // Resize LFO buffers if needed
    if ((int) lfoBuffer1.size() < numFrames)
    {
        lfoBuffer1.resize (numFrames);
        lfoBuffer2.resize (numFrames);
    }

    float* modBuf1 = nullptr;
    float* modBuf2 = nullptr;

    if (lfo1ModFlags)
    {
        int patIdx = (int) lfoPattern1Param->load() - 1;
        bool isSync = lfoRateType1Param->load() < 0.5f;
        int syncIdx = (int) lfoSyncRate1Param->load();
        float freeRate = lfoFreeRate1Param->load();

        lfoProcessors[0].renderLFO (lfoBuffer1.data(), numFrames,
                                     lfos[0], patIdx, isSync, syncIdx, freeRate,
                                     currentBPM, sampleRate_);
        modBuf1 = lfoBuffer1.data();
    }

    if (lfo2ModFlags)
    {
        int patIdx = (int) lfoPattern2Param->load() - 1;
        bool isSync = lfoRateType2Param->load() < 0.5f;
        int syncIdx = (int) lfoSyncRate2Param->load();
        float freeRate = lfoFreeRate2Param->load();

        lfoProcessors[1].renderLFO (lfoBuffer2.data(), numFrames,
                                     lfos[1], patIdx, isSync, syncIdx, freeRate,
                                     currentBPM, sampleRate_);
        modBuf2 = lfoBuffer2.data();
    }

    // Audio retrigger detection
    const float pdReleaseTime = (float) Dynamics::convertCompressorTime (sampleRate_ * 0.015);
    const float pdThreshold   = FastMath::fast_dB_to_gain (-54.0f);
    bool doKeytracking = midiKeytracking.load();

    float* channels[2] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };

    int startSample = 0;
    int remaining = numFrames;

    while (remaining > 0)
    {
        // Peak detect for retrigger
        int i;
        for (i = 0; i < remaining; i++)
        {
            float avgGain = 0.5f * (channels[0][startSample + i] + channels[1][startSample + i]);
            float prevPeak = retrigDetection;
            float nextPeak = Dynamics::detectPeak (avgGain, retrigDetection, 0.0f, pdReleaseTime);
            retrigDetection = nextPeak;

            if (prevPeak < pdThreshold && nextPeak >= pdThreshold)
            {
                for (int lfoIdx = 0; lfoIdx < 2; lfoIdx++)
                {
                    bool retrigOn = (lfos[lfoIdx].loopType == LFOLoopType::Retrig ||
                                     lfos[lfoIdx].loopType == LFOLoopType::OneShot);
                    if (retrigOn && !doKeytracking)
                        lfos[lfoIdx].phase = 0;
                }
                break;
            }
        }

        if (i > 0)
        {
            float* subChannels[2] = { channels[0] + startSample, channels[1] + startSample };
            const float* subMod1 = modBuf1 ? modBuf1 + startSample : nullptr;
            const float* subMod2 = modBuf2 ? modBuf2 + startSample : nullptr;

            dsp.processBlock (subChannels, i, audioParams, keytrackingOffset,
                              subMod1, subMod2, localModAmounts,
                              lfo1ModFlags, lfo2ModFlags,
                              gateReleaseParam->load());
        }

        startSample += i;
        remaining -= i;
    }

    // Autogain
    float* agChannels[2] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    dsp.applyAutogain (agChannels, numFrames, autogainOn.load());

    // Output gain
    dsp.applyOutputGain (agChannels, numFrames, outputGainParam->load());
}

void BPScreamProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Add custom state
    juce::ValueTree customState ("STATE");
    customState.setProperty ("autogain", autogainOn.load(), nullptr);
    customState.setProperty ("keytracking", midiKeytracking.load(), nullptr);

    // LFO loop types
    juce::ValueTree loopTypes ("LOOP_TYPES");
    loopTypes.setProperty ("lfo1", (int) lfos[0].loopType, nullptr);
    loopTypes.setProperty ("lfo2", (int) lfos[1].loopType, nullptr);
    customState.addChild (loopTypes, -1, nullptr);

    // Mod matrix
    juce::ValueTree modMatrix ("MOD_MATRIX");
    const char* paramNames[] = { "cutoff", "scream", "resonance", "inputGain", "wet" };
    {
        std::lock_guard<std::mutex> lock (modAmountsMutex);
        for (int i = 0; i < 5; i++)
        {
            juce::ValueTree mod ("MOD");
            mod.setProperty ("param", paramNames[i], nullptr);
            mod.setProperty ("lfo1", (double) lfoModAmounts[i][0], nullptr);
            mod.setProperty ("lfo2", (double) lfoModAmounts[i][1], nullptr);
            modMatrix.addChild (mod, -1, nullptr);
        }
    }
    customState.addChild (modMatrix, -1, nullptr);

    // LFO patterns
    for (int lfoIdx = 0; lfoIdx < 2; lfoIdx++)
    {
        juce::ValueTree lfoTree ("LFO");
        lfoTree.setProperty ("index", lfoIdx, nullptr);

        std::lock_guard<std::mutex> lock (lfos[lfoIdx].mutex);
        for (int patIdx = 0; patIdx < NUM_LFO_PATTERNS; patIdx++)
        {
            juce::ValueTree patTree ("PATTERN");
            patTree.setProperty ("index", patIdx, nullptr);
            patTree.setProperty ("gridX", lfos[lfoIdx].patterns[patIdx].gridX, nullptr);
            patTree.setProperty ("gridY", lfos[lfoIdx].patterns[patIdx].gridY, nullptr);

            for (const auto& pt : lfos[lfoIdx].patterns[patIdx].points)
            {
                juce::ValueTree ptTree ("POINT");
                ptTree.setProperty ("x", (double) pt.x, nullptr);
                ptTree.setProperty ("y", (double) pt.y, nullptr);
                ptTree.setProperty ("skew", (double) pt.skew, nullptr);
                patTree.addChild (ptTree, -1, nullptr);
            }
            lfoTree.addChild (patTree, -1, nullptr);
        }
        customState.addChild (lfoTree, -1, nullptr);
    }

    state.addChild (customState, -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BPScreamProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr) return;

    auto tree = juce::ValueTree::fromXml (*xml);
    if (!tree.isValid()) return;

    apvts.replaceState (tree);

    auto customState = tree.getChildWithName ("STATE");
    if (!customState.isValid()) return;

    autogainOn.store ((bool) customState.getProperty ("autogain", true));
    midiKeytracking.store ((bool) customState.getProperty ("keytracking", false));

    auto loopTypes = customState.getChildWithName ("LOOP_TYPES");
    if (loopTypes.isValid())
    {
        lfos[0].loopType = (LFOLoopType)(int) loopTypes.getProperty ("lfo1", 1);
        lfos[1].loopType = (LFOLoopType)(int) loopTypes.getProperty ("lfo2", 1);
    }

    auto modMatrix = customState.getChildWithName ("MOD_MATRIX");
    if (modMatrix.isValid())
    {
        std::lock_guard<std::mutex> lock (modAmountsMutex);
        for (int i = 0; i < modMatrix.getNumChildren() && i < 5; i++)
        {
            auto mod = modMatrix.getChild (i);
            lfoModAmounts[i][0] = (float)(double) mod.getProperty ("lfo1", 0.0);
            lfoModAmounts[i][1] = (float)(double) mod.getProperty ("lfo2", 0.0);
        }
    }

    for (int lfoIdx = 0; lfoIdx < 2; lfoIdx++)
    {
        for (int ci = 0; ci < customState.getNumChildren(); ci++)
        {
            auto lfoTree = customState.getChild (ci);
            if (lfoTree.hasType ("LFO") && (int) lfoTree.getProperty ("index") == lfoIdx)
            {
                std::lock_guard<std::mutex> lock (lfos[lfoIdx].mutex);
                for (int pi = 0; pi < lfoTree.getNumChildren(); pi++)
                {
                    auto patTree = lfoTree.getChild (pi);
                    if (!patTree.hasType ("PATTERN")) continue;
                    int patIdx = (int) patTree.getProperty ("index", 0);
                    if (patIdx < 0 || patIdx >= NUM_LFO_PATTERNS) continue;

                    lfos[lfoIdx].patterns[patIdx].gridX = (int) patTree.getProperty ("gridX", 4);
                    lfos[lfoIdx].patterns[patIdx].gridY = (int) patTree.getProperty ("gridY", 4);
                    lfos[lfoIdx].patterns[patIdx].points.clear();

                    for (int pti = 0; pti < patTree.getNumChildren(); pti++)
                    {
                        auto ptTree = patTree.getChild (pti);
                        if (!ptTree.hasType ("POINT")) continue;
                        LFOPoint pt;
                        pt.x    = (float)(double) ptTree.getProperty ("x", 0.0);
                        pt.y    = (float)(double) ptTree.getProperty ("y", 0.0);
                        pt.skew = (float)(double) ptTree.getProperty ("skew", 0.5);
                        lfos[lfoIdx].patterns[patIdx].points.push_back (pt);
                    }
                }
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BPScreamProcessor();
}
