// Compile the unchanged supplied processor as an independent audio oracle.
// Only rename its plugin factory to coexist with Atlas in the test executable.
#define createPluginFilter createSpectralLockReferenceFilter
#include "../third_party/spectral_lock/PluginProcessor.cpp"
