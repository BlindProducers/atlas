// SPDX-License-Identifier: GPL-3.0-or-later
#include "piano_model_service.h"

#include <algorithm>
#include <chrono>

namespace pm
{

ModelService& ModelService::instance()
{
    static ModelService service;
    return service;
}

ModelService::~ModelService()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping = true;
    }
    wake.notify_all();
    if (thread.joinable())
        thread.join();
}

void ModelService::add (std::shared_ptr<Instrument> instrument)
{
    ModelService& s = instance();
    const std::lock_guard<std::mutex> lock (s.mutex);
    Entry entry;
    entry.key = instrument.get();
    entry.instrument = std::move (instrument);
    s.entries.push_back (std::move (entry));
    if (! s.running && ! s.stopping)
    {
        s.running = true;
        s.thread = std::thread ([&s] { s.run(); });
    }
    s.wake.notify_all();
}

void ModelService::remove (const Instrument* instrument)
{
    ModelService& s = instance();
    const std::lock_guard<std::mutex> lock (s.mutex);
    s.entries.erase (std::remove_if (s.entries.begin(), s.entries.end(),
                                     [instrument] (const Entry& e) { return e.key == instrument; }),
                     s.entries.end());
}

bool ModelService::waitForModel (const Instrument* instrument, int timeoutMilliseconds)
{
    ModelService& s = instance();
    std::unique_lock<std::mutex> lock (s.mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMilliseconds);
    while (true)
    {
        const auto found = std::find_if (s.entries.begin(), s.entries.end(),
                                         [instrument] (const Entry& e) { return e.key == instrument; });
        if (found == s.entries.end())
            return false;
        Instrument::Wanted wanted;
        if (found->builtOnce && instrument->readWanted (wanted) && wanted == found->built)
            return true;
        if (s.passed.wait_until (lock, deadline) == std::cv_status::timeout)
            return false;
    }
}

void ModelService::run()
{
    std::unique_lock<std::mutex> lock (mutex);
    while (! stopping)
    {
        wake.wait_for (lock, std::chrono::milliseconds (kPollMilliseconds));
        if (stopping)
            break;
        lock.unlock();
        poll();
        lock.lock();
        ++passes;
        passed.notify_all();
    }
}

std::shared_ptr<const PianoModel> ModelService::modelFor (const Instrument::Wanted& w)
{
    const ModelKey key { { w.sampleRate, w.a4, w.stretch, w.unison, w.sustain } };
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (const auto found = cache.find (key); found != cache.end())
            if (auto model = found->second.lock())
                return model;
    }
    Voicing voicing;
    voicing.sampleRate = w.sampleRate;
    voicing.a4 = w.a4;
    voicing.stretch = w.stretch;
    voicing.unison = w.unison;
    voicing.sustain = w.sustain;
    std::shared_ptr<const PianoModel> model = buildModel (voicing);
    const std::lock_guard<std::mutex> lock (mutex);
    for (auto it = cache.begin(); it != cache.end();)
        it = it->second.expired() ? cache.erase (it) : std::next (it);
    cache[key] = model;
    return model;
}

void ModelService::poll()
{
    // A snapshot, so that building never holds the lock against add and remove.
    std::vector<Entry> snapshot;
    {
        const std::lock_guard<std::mutex> lock (mutex);
        snapshot = entries;
    }
    for (Entry& entry : snapshot)
    {
        std::shared_ptr<Instrument> instrument = entry.instrument.lock();
        if (instrument == nullptr)
            continue;
        instrument->collect();
        if (instrument->hasOffer())
            continue;

        Instrument::Wanted wanted;
        if (! instrument->readWanted (wanted) || wanted.sampleRate <= 0.0)
            continue;
        // The same on two polls running: the user has stopped moving the control.
        const bool settled = entry.seenOnce && wanted == entry.seen;
        entry.seen = wanted;
        entry.seenOnce = true;
        if (! settled || (entry.builtOnce && wanted == entry.built))
        {
            const std::lock_guard<std::mutex> lock (mutex);
            for (Entry& e : entries)
                if (e.key == entry.key) { e.seen = entry.seen; e.seenOnce = true; }
            continue;
        }

        std::shared_ptr<const Instrument::Prepared> prepared = Instrument::Prepared::make (modelFor (wanted));
        instrument->offer (std::move (prepared));

        const std::lock_guard<std::mutex> lock (mutex);
        for (Entry& e : entries)
            if (e.key == entry.key)
            {
                e.seen = wanted;
                e.seenOnce = true;
                e.built = wanted;
                e.builtOnce = true;
            }
    }
}

} // namespace pm
