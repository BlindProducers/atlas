// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "piano_instrument.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pm
{

/** Builds piano models on one background thread for every instrument in the process.

    Each instrument says what it wants (sample rate, tuning, octave stretch, unison width, sustain
    length) from the audio thread; the service polls every 60 ms and builds once the settings have
    stopped changing, as the original plugin's worker does, so a moving slider does not queue a
    build for every step. Models are shared: two instruments that want the same voicing (two slots,
    two plugin instances) get one PianoModel, each with its own Prepared tables.
*/
class ModelService
{
public:
    static constexpr int kPollMilliseconds = 60;

    /** Message thread: the service watches the instrument until it is removed. */
    static void add (std::shared_ptr<Instrument>);
    static void remove (const Instrument*);

    /** Tests and offline rendering: waits until the instrument has been offered a model for what
        it last asked for (it takes the offer at the end of its next block). */
    static bool waitForModel (const Instrument*, int timeoutMilliseconds);

private:
    struct Entry
    {
        std::weak_ptr<Instrument> instrument;
        const Instrument* key = nullptr;
        Instrument::Wanted seen, built;
        bool seenOnce = false, builtOnce = false;
    };

    struct ModelKey
    {
        double values[5];
        bool operator< (const ModelKey& o) const
        {
            for (int i = 0; i < 5; ++i)
                if (values[i] != o.values[i])
                    return values[i] < o.values[i];
            return false;
        }
    };

    ModelService() = default;
    ~ModelService();
    static ModelService& instance();

    void run();
    void poll();
    std::shared_ptr<const PianoModel> modelFor (const Instrument::Wanted&);

    std::mutex mutex;
    std::condition_variable wake, passed;
    std::vector<Entry> entries;
    std::map<ModelKey, std::weak_ptr<const PianoModel>> cache;
    std::thread thread;
    bool running = false, stopping = false;
    uint64_t passes = 0;
};

} // namespace pm
