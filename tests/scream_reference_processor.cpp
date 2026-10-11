// Build the complete supplied BP Scream processor as an independent oracle.
// Only its plugin factory name and unused editor entry point are adapted.
#ifndef JucePlugin_Name
#define JucePlugin_Name "BP Scream"
#endif
#define createPluginFilter createBPScreamReferenceFilter
#include "../third_party/scream/Source/PluginProcessor.cpp"

juce::AudioProcessorEditor* BPScreamProcessor::createEditor() { return nullptr; }
