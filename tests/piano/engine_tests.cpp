// The piano engine on its own, without JUCE: the port (pm::Instrument, one lane per note plus the
// shared body) against the original engine (tests/piano/reference_engine.*) on the same model and
// the same events, and the behaviour the original's own tests check.
//
//   AtlasPianoEngineTests --reference | --behaviour | --cpu

#include "Denormals.h"
#include "piano_instrument.h"
#include "reference_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void expect (bool condition, const std::string& what)
    {
        std::printf ("%s  %s\n", condition ? "ok  " : "FAIL", what.c_str());
        failures += condition ? 0 : 1;
    }

    constexpr double kRate = 48000.0;

    struct Event
    {
        enum Type { kOn, kOff, kSustain, kSostenuto, kSoft };
        double seconds;
        Type type;
        int note = 0;           // MIDI
        float value = 0.0f;     // velocity, lift or pedal amount
    };

    struct Render
    {
        std::vector<float> left, right;

        double peak() const
        {
            double p = 0.0;
            for (size_t i = 0; i < left.size(); ++i)
                p = std::max ({ p, (double) std::fabs (left[i]), (double) std::fabs (right[i]) });
            return p;
        }

        double rms (double from, double to) const
        {
            double sum = 0.0;
            const auto a = (size_t) (from * kRate), b = std::min (left.size(), (size_t) (to * kRate));
            for (size_t i = a; i < b; ++i) sum += (double) left[i] * left[i] + (double) right[i] * right[i];
            return b > a ? std::sqrt (sum / (double) (2 * (b - a))) : 0.0;
        }

        bool finite() const
        {
            for (size_t i = 0; i < left.size(); ++i)
                if (! std::isfinite (left[i]) || ! std::isfinite (right[i])) return false;
            return true;
        }
    };

    std::shared_ptr<const pm::PianoModel> sharedModel (double rate = kRate)
    {
        static std::map<double, std::shared_ptr<const pm::PianoModel>> models;
        auto& model = models[rate];
        if (model == nullptr)
        {
            pm::Voicing voicing;
            voicing.sampleRate = rate;
            model = pm::buildModel (voicing);
        }
        return model;
    }

    /** The events in time order, the block split at each one, as the original plugin splits its blocks. */
    template <typename Segment, typename Apply>
    void play (const std::vector<Event>& events, double seconds, int block, Segment&& segment, Apply&& apply)
    {
        const auto total = (size_t) (seconds * kRate);
        size_t next = 0;
        for (size_t pos = 0; pos < total;)
        {
            const size_t end = std::min (total, pos + (size_t) block);
            size_t at = pos;
            while (at < end)
            {
                size_t stop = end;
                while (next < events.size() && (size_t) (events[next].seconds * kRate) <= at)
                    apply (events[next++], (int) (at - pos));
                if (next < events.size())
                    stop = std::min (end, std::max (at + 1, (size_t) (events[next].seconds * kRate)));
                segment ((int) (at - pos), (int) (stop - at), (int) (end - pos));
                at = stop;
            }
            pos = end;
        }
    }

    Render renderReference (const std::vector<Event>& events, double seconds, int block)
    {
        pm::ReferenceEngine engine;
        engine.setModel (sharedModel());
        engine.reset();
        Render r;
        const auto total = (size_t) (seconds * kRate);
        r.left.resize (total); r.right.resize (total);
        size_t written = 0;
        play (events, seconds, block,
              [&] (int, int n, int) { engine.process (&r.left[written], &r.right[written], n); written += (size_t) n; },
              [&] (const Event& e, int)
              {
                  switch (e.type)
                  {
                      case Event::kOn: engine.noteOn (e.note, e.value); break;
                      case Event::kOff: engine.noteOff (e.note, e.value); break;
                      case Event::kSustain: engine.setSustain (e.value); break;
                      case Event::kSostenuto: engine.setSostenuto (e.value >= 0.5f); break;
                      case Event::kSoft: engine.setSoft (e.value); break;
                  }
              });
        return r;
    }

    /** The port played as Atlas plays it: a lane per note, each rendered for the block in turn, then
        the body. Notes keep their lane while they sound, as a voice keeps its note; with `freshLanes`
        every note-on takes a new lane, announced and handed over as the voice handler does it. */
    Render renderPort (const std::vector<Event>& events, double seconds, int block, bool freshLanes = false)
    {
        pm::Instrument instrument;
        instrument.offer (pm::Instrument::Prepared::make (sharedModel()));
        {
            float l[pm::Instrument::kMaxBlock], r[pm::Instrument::kMaxBlock];
            instrument.renderBody (l, r, 32);         // takes the model
        }
        std::map<int, std::unique_ptr<pm::Instrument::Lane>> lanes;
        std::vector<std::unique_ptr<pm::Instrument::Lane>> spare;     // freshLanes: lanes let go of
        auto lane = [&] (int midi) -> pm::Instrument::Lane&
        {
            auto& p = lanes[midi];
            if (p == nullptr)
                p = std::make_unique<pm::Instrument::Lane>();
            return *p;
        };
        // freshLanes: the strikes of the block, announced before any lane renders.
        std::vector<std::pair<int, int>> strikes;

        Render r;
        const auto total = (size_t) (seconds * kRate);
        r.left.resize (total); r.right.resize (total);
        size_t blockStart = 0;
        int blockSize = 0;
        float l[pm::Instrument::kMaxBlock], rr[pm::Instrument::kMaxBlock];
        const pm::Instrument::Blow blow;

        size_t nextEvent = 0;
        play (events, seconds, block,
              [&] (int offset, int n, int size)
              {
                  blockSize = size;
                  if (freshLanes && offset == 0)
                  {
                      // The voice handler knows the block's note-ons before any voice renders.
                      const size_t first = (size_t) (blockStart / 1);
                      for (size_t k = nextEvent; k < events.size() && (size_t) (events[k].seconds * kRate) < blockStart + (size_t) size; ++k)
                          if (events[k].type == Event::kOn)
                              instrument.announceStrike (events[k].note - pm::kLowestMidi, (int) ((size_t) (events[k].seconds * kRate) - blockStart));
                      (void) first;
                  }
                  for (auto& entry : lanes)
                  {
                      pm::Instrument::Lane& v = *entry.second;
                      if (v.note < 0)
                          continue;
                      instrument.renderNote (v, l, rr, offset, n, size);
                      for (int i = 0; i < n; ++i)
                      {
                          r.left[blockStart + (size_t) offset + (size_t) i] += l[i];
                          r.right[blockStart + (size_t) offset + (size_t) i] += rr[i];
                      }
                  }
                  if (offset + n == size)
                  {
                      instrument.renderBody (l, rr, size);
                      for (int i = 0; i < size; ++i)
                      {
                          r.left[blockStart + (size_t) i] += l[i];
                          r.right[blockStart + (size_t) i] += rr[i];
                      }
                      blockStart += (size_t) size;
                      while (nextEvent < events.size() && (size_t) (events[nextEvent].seconds * kRate) < blockStart)
                          ++nextEvent;
                      for (auto& entry : lanes)
                          if (entry.second->note >= 0 && ! entry.second->busy())
                              instrument.clear (*entry.second);
                  }
              },
              [&] (const Event& e, int offset)
              {
                  switch (e.type)
                  {
                      case Event::kOn:
                          if (freshLanes && lanes[e.note] != nullptr && lanes[e.note]->note >= 0)
                          {
                              // A new voice for the note: it takes the strings over from the old lane, which
                              // has rendered up to the strike (announced) and now falls silent.
                              auto fresh = std::make_unique<pm::Instrument::Lane>();
                              const int taken = instrument.takeOver (*fresh, e.note - pm::kLowestMidi, 480);
                              if (taken > offset)
                                  std::printf ("      (hand-over after the old lane rendered past the strike: %d > %d)\n", taken, offset);
                              spare.push_back (std::move (lanes[e.note]));
                              lanes[e.note] = std::move (fresh);
                          }
                          instrument.strike (lane (e.note), e.note - pm::kLowestMidi, e.value, blow, 480, offset);
                          break;
                      case Event::kOff: instrument.release (lane (e.note), e.value, 1.0f, offset); break;
                      case Event::kSustain: instrument.pedalEvent (pm::Instrument::kSustainPedal, e.value, offset); break;
                      case Event::kSostenuto: instrument.pedalEvent (pm::Instrument::kSostenutoPedal, e.value, offset); break;
                      case Event::kSoft: instrument.pedalEvent (pm::Instrument::kSoftPedal, e.value, offset); break;
                  }
              });
        return r;
    }

    /** The largest difference between two renders, in dB below the reference's peak. */
    double compare (const Render& a, const Render& b, const char* what, double bound)
    {
        double difference = 0.0;
        for (size_t i = 0; i < a.left.size(); ++i)
            difference = std::max ({ difference, (double) std::fabs (a.left[i] - b.left[i]), (double) std::fabs (a.right[i] - b.right[i]) });
        const double db = -20.0 * std::log10 (difference / a.peak() + 1.0e-30);
        std::printf ("      %s: largest difference %.1f dB below the peak\n", what, db);
        expect (db >= bound, std::string (what) + ": the port matches the original");
        return db;
    }

    int referenceTest()
    {
        // A4 held for two seconds, then released.
        const std::vector<Event> single { { 0.1, Event::kOn, 69, 90.0f }, { 2.1, Event::kOff, 69, 64.0f } };
        compare (renderReference (single, 4.0, 128), renderPort (single, 4.0, 128), "single note", 60.0);
        compare (renderReference (single, 4.0, 128), renderPort (single, 4.0, 17), "single note, 17-sample blocks", 60.0);

        // A pedalled chord with a bass note: thud, knocks and sympathetic strings all over the keyboard.
        const std::vector<Event> chord {
            { 0.05, Event::kSustain, 0, 1.0f },
            { 0.1, Event::kOn, 36, 110.0f }, { 0.11, Event::kOn, 60, 95.0f }, { 0.12, Event::kOn, 64, 90.0f }, { 0.13, Event::kOn, 67, 100.0f },
            { 0.9, Event::kOff, 36, 40.0f }, { 1.0, Event::kOff, 60, 70.0f }, { 1.0, Event::kOff, 64, 70.0f }, { 1.05, Event::kOff, 67, 100.0f },
            { 2.5, Event::kSustain, 0, 0.5f }, { 2.8, Event::kSustain, 0, 0.0f }
        };
        compare (renderReference (chord, 4.0, 256), renderPort (chord, 4.0, 256), "pedalled chord", 50.0);

        // A note struck again while it still rings, with the soft pedal, and a sostenuto-held note.
        const std::vector<Event> repeats {
            { 0.05, Event::kSoft, 0, 1.0f },
            { 0.1, Event::kOn, 57, 100.0f }, { 0.4, Event::kOff, 57, 90.0f }, { 0.45, Event::kOn, 57, 60.0f },
            { 0.5, Event::kSostenuto, 0, 1.0f }, { 0.7, Event::kOff, 57, 30.0f },
            { 0.8, Event::kOn, 76, 120.0f }, { 1.0, Event::kOff, 76, 60.0f },
            { 1.6, Event::kSostenuto, 0, 0.0f }, { 2.0, Event::kSoft, 0, 0.0f }, { 2.2, Event::kOn, 100, 80.0f }, { 2.6, Event::kOff, 100, 64.0f }
        };
        compare (renderReference (repeats, 3.5, 128), renderPort (repeats, 3.5, 128), "repeats, soft and sostenuto pedals", 50.0);

        // The opening of a prelude, as written: keys held under the pedal, restruck while held, and the
        // pedal lifted and pressed again around a release. Once with a lane per note, once with a fresh
        // lane for every note-on, handed the strings over as the voice handler does it.
        const std::vector<Event> prelude {
            { 0.9698, Event::kSustain, 0, 0.0f }, { 0.9969, Event::kOn, 60, 62.0f }, { 1.0500, Event::kSustain, 0, 1.0f },
            { 1.2115, Event::kOn, 64, 59.0f }, { 1.4292, Event::kOn, 67, 58.0f }, { 1.6229, Event::kOff, 67, 64.0f },
            { 1.6448, Event::kOn, 72, 54.0f }, { 1.8385, Event::kOff, 72, 64.0f }, { 1.8594, Event::kOn, 76, 54.0f },
            { 2.0531, Event::kOff, 76, 64.0f }, { 2.0719, Event::kOn, 67, 54.0f }, { 2.2656, Event::kOff, 67, 64.0f },
            { 2.2927, Event::kOn, 72, 57.0f }, { 2.4865, Event::kOff, 72, 64.0f }, { 2.5031, Event::kOn, 76, 54.0f },
            { 2.6958, Event::kOff, 76, 64.0f }, { 2.7167, Event::kOff, 64, 64.0f }, { 2.7177, Event::kOff, 60, 64.0f },
            { 2.7240, Event::kOn, 60, 65.0f }, { 2.9344, Event::kOn, 64, 58.0f }, { 3.1469, Event::kOn, 67, 61.0f },
            { 3.3396, Event::kOff, 67, 64.0f }, { 3.3635, Event::kOn, 72, 60.0f }, { 3.5573, Event::kOff, 72, 64.0f },
            { 3.5771, Event::kOn, 76, 55.0f }, { 3.7708, Event::kOff, 76, 64.0f }, { 3.7979, Event::kOn, 67, 56.0f },
            { 3.9906, Event::kOff, 67, 64.0f }, { 4.0104, Event::kOn, 72, 55.0f }, { 4.2042, Event::kOff, 72, 64.0f },
            { 4.2240, Event::kOn, 76, 59.0f }, { 4.4104, Event::kSustain, 0, 0.0f }, { 4.4177, Event::kOff, 76, 64.0f },
            { 4.4365, Event::kOn, 60, 65.0f }, { 4.4396, Event::kOff, 64, 64.0f }, { 4.4437, Event::kOff, 60, 64.0f },
            { 4.4896, Event::kSustain, 0, 1.0f }, { 4.6531, Event::kOn, 62, 56.0f }, { 4.8698, Event::kOn, 69, 60.0f }
        };
        const Render reference = renderReference (prelude, 6.0, 128);
        compare (reference, renderPort (prelude, 6.0, 128), "a prelude's opening, a lane per note", 50.0);
        compare (reference, renderPort (prelude, 6.0, 128, true), "a prelude's opening, a fresh lane per note-on", 50.0);
        return failures == 0 ? 0 : 1;
    }

    int behaviourTest()
    {
        const std::vector<Event> single { { 0.1, Event::kOn, 69, 90.0f }, { 2.1, Event::kOff, 69, 64.0f } };
        const Render a = renderPort (single, 4.0, 256);
        expect (a.finite(), "output is finite");
        expect (a.rms (0.2, 0.6) > 1.0e-3, "a struck note sounds");
        expect (a.rms (2.8, 3.2) < 0.01 * a.rms (1.6, 2.0), "the damper stops the note after release");
        expect (a.rms (3.7, 4.0) < 1.0e-6, "the engine returns to silence");

        const std::vector<Event> pedalled { { 0.05, Event::kSustain, 0, 1.0f }, { 0.1, Event::kOn, 69, 90.0f },
                                            { 0.6, Event::kOff, 69, 64.0f }, { 3.0, Event::kSustain, 0, 0.0f } };
        const Render c = renderPort (pedalled, 4.0, 256);
        expect (c.rms (1.6, 2.0) > 0.2 * a.rms (1.6, 2.0), "the sustain pedal holds a released note");
        expect (c.rms (3.7, 4.0) < 0.05 * c.rms (2.6, 2.9), "lifting the pedal damps it");

        // A voice stolen for another note: the old note fades out in the lane over the fade length
        // while the new one starts, as if both played on their own. Two instruments, so that the
        // knocks draw the same random values.
        {
            pm::Instrument a, b;
            for (pm::Instrument* instrument : { &a, &b })
            {
                instrument->offer (pm::Instrument::Prepared::make (sharedModel()));
                float l[pm::Instrument::kMaxBlock], r[pm::Instrument::kMaxBlock];
                instrument->renderBody (l, r, 32);
            }
            float l[pm::Instrument::kMaxBlock], r[pm::Instrument::kMaxBlock];
            pm::Instrument::Lane stolen, old, fresh;
            const pm::Instrument::Blow blow;
            constexpr int fade = 480, block = 256;
            a.strike (stolen, 60 - pm::kLowestMidi, 100.0f, blow, fade, 0);
            b.strike (old, 60 - pm::kLowestMidi, 100.0f, blow, fade, 0);
            for (int i = 0; i < 100; ++i)
            {
                a.renderNote (stolen, l, r, 0, block, block);
                a.renderBody (l, r, block);
                b.renderNote (old, l, r, 0, block, block);
                b.renderBody (l, r, block);
            }
            a.strike (stolen, 72 - pm::kLowestMidi, 100.0f, blow, fade, 0);
            b.strike (fresh, 72 - pm::kLowestMidi, 100.0f, blow, fade, 0);
            expect (stolen.fade >= 0 && stolen.note == 72 - pm::kLowestMidi,
                    "a lane struck with another note hands the old one to a fade slot");
            double difference = 0.0, peak = 0.0;
            for (int n = 0; n < 4; ++n)
            {
                float sl[block], sr[block], ol[block], orr[block], fl[block], fr[block];
                a.renderNote (stolen, sl, sr, 0, block, block);
                a.renderBody (l, r, block);
                b.renderNote (old, ol, orr, 0, block, block);
                b.renderNote (fresh, fl, fr, 0, block, block);
                b.renderBody (l, r, block);
                for (int i = 0; i < block; ++i)
                {
                    const float g = (float) std::max (0, fade - n * block - i) / (float) fade;
                    const float expected = ol[i] * g + fl[i];
                    difference = std::max (difference, (double) std::fabs (sl[i] - expected));
                    peak = std::max (peak, (double) std::fabs (expected));
                }
            }
            std::printf ("      stolen voice: %.1f dB below the peak from the two notes on their own\n",
                         -20.0 * std::log10 (difference / peak + 1.0e-30));
            expect (difference < 1.0e-3 * peak, "the stolen note fades out as it would on its own");
            expect (stolen.fade < 0, "the fade slot is free again");
        }
        // A note repeated on another voice (as under the sustain pedal) takes the ringing strings over,
        // and sounds as a restrike in the same lane does. Here the first lane has rendered the block
        // already, so the new lane strikes at the block's end and skips it.
        {
            pm::Instrument a, b;
            for (pm::Instrument* instrument : { &a, &b })
            {
                instrument->offer (pm::Instrument::Prepared::make (sharedModel()));
                float l[pm::Instrument::kMaxBlock], r[pm::Instrument::kMaxBlock];
                instrument->renderBody (l, r, 32);
            }
            float l[pm::Instrument::kMaxBlock], r[pm::Instrument::kMaxBlock];
            pm::Instrument::Lane first, second, same;
            const pm::Instrument::Blow blow;
            constexpr int block = 256;
            a.strike (first, 39, 100.0f, blow, 480, 0);
            b.strike (same, 39, 100.0f, blow, 480, 0);
            for (int i = 0; i < 100; ++i)
            {
                a.renderNote (first, l, r, 0, block, block);
                a.renderBody (l, r, block);
                b.renderNote (same, l, r, 0, block, block);
                b.renderBody (l, r, block);
            }
            // Block 100: the first lane renders, then the second takes the note over.
            a.renderNote (first, l, r, 0, block, block);
            const int taken = a.takeOver (second, 39, 480);
            expect (taken == block && first.note < 0 && second.note == 39, "the strings change hands after the old lane's block");
            a.strike (second, 39, 100.0f, blow, 480, 0);
            a.renderBody (l, r, block);
            b.renderNote (same, l, r, 0, block, block);
            b.strike (same, 39, 100.0f, blow, 480, 0);
            b.renderBody (l, r, block);
            double difference = 0.0, peak = 0.0;
            for (int n = 0; n < 20; ++n)
            {
                float al[block], ar[block], bl[block], br[block];
                a.renderNote (first, l, r, 0, block, block);
                a.renderNote (second, al, ar, 0, block, block);
                a.renderBody (l, r, block);
                b.renderNote (same, bl, br, 0, block, block);
                b.renderBody (l, r, block);
                for (int i = 0; i < block; ++i)
                {
                    difference = std::max (difference, (double) std::fabs (al[i] - bl[i]));
                    peak = std::max (peak, (double) std::fabs (bl[i]));
                }
            }
            expect (difference < 1.0e-6 * peak, "a note taken over sounds as a restrike in one lane does");

            // Announced, the strike changes hands at its own sample: the first lane stops there.
            constexpr int at = 100;
            pm::Instrument::Lane third;
            a.announceStrike (39, at);
            b.announceStrike (39, at);           // announced to a lane that strikes itself: no effect
            float al[block], ar[block], bl[block], br[block];
            a.renderNote (second, al, ar, 0, block, block);
            expect (second.renderedTo == at, "the lane sounding an announced note stops at the strike");
            const int handed = a.takeOver (third, 39, 480);
            expect (handed == at, "the strings change hands at the announced sample");
            a.strike (third, 39, 90.0f, blow, 480, at);
            a.renderNote (third, l, r, at, block - at, block);
            for (int i = at; i < block; ++i) { al[i] += l[i - at]; ar[i] += r[i - at]; }
            a.renderBody (l, r, block);
            b.renderNote (same, bl, br, 0, at, block);
            b.strike (same, 39, 90.0f, blow, 480, at);
            b.renderNote (same, bl + at, br + at, at, block - at, block);
            b.renderBody (l, r, block);
            difference = 0.0; peak = 0.0;
            for (int i = 0; i < block; ++i)
            {
                difference = std::max (difference, (double) std::fabs (al[i] - bl[i]));
                peak = std::max (peak, (double) std::fabs (bl[i]));
            }
            for (int n = 0; n < 20; ++n)
            {
                a.renderNote (third, al, ar, 0, block, block);
                a.renderBody (l, r, block);
                b.renderNote (same, bl, br, 0, block, block);
                b.renderBody (l, r, block);
                for (int i = 0; i < block; ++i)
                {
                    difference = std::max (difference, (double) std::fabs (al[i] - bl[i]));
                    peak = std::max (peak, (double) std::fabs (bl[i]));
                }
            }
            expect (difference < 1.0e-6 * peak, "an announced hand-over sounds as a restrike in one lane does");

            // Announced but not taken: the body plays the rest of the lane's block (both bodies carry
            // the same sympathetic strings, so their difference is that rest).
            float bodyA[2][block], bodyB[2][block];
            a.announceStrike (39, at);
            a.renderNote (third, al, ar, 0, block, block);
            a.renderBody (bodyA[0], bodyA[1], block);
            b.renderNote (same, bl, br, 0, block, block);
            b.renderBody (bodyB[0], bodyB[1], block);
            difference = 0.0;
            for (int i = 0; i < block; ++i)
                difference = std::max (difference, (double) std::fabs (al[i] + bodyA[0][i] - bodyB[0][i] - bl[i]));
            std::printf ("      announced and not taken: %.1f dB below the peak\n", -20.0 * std::log10 (difference / peak + 1.0e-30));
            expect (difference < 1.0e-4 * peak, "a strike announced and not taken loses nothing");
        }
        return failures == 0 ? 0 : 1;
    }

    int cpuTest()
    {
        // Every key at full force with the pedal down: the heaviest load, timed over its ring-out.
        std::vector<Event> all { { 0.0, Event::kSustain, 0, 1.0f } };
        for (int n = 21; n <= 108; ++n)
        {
            all.push_back ({ 0.05 + 0.01 * (n - 21), Event::kOn, n, 127.0f });
            all.push_back ({ 1.0 + 0.01 * (n - 21), Event::kOff, n, 64.0f });
        }
        const auto start = std::chrono::steady_clock::now();
        const Render a = renderPort (all, 6.0, 256);
        const double port = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        const auto middle = std::chrono::steady_clock::now();
        const Render b = renderReference (all, 6.0, 256);
        const double reference = std::chrono::duration<double> (std::chrono::steady_clock::now() - middle).count();
        std::printf ("      all 88 keys, pedal down, 6 s: port %.0f%% of one core, original %.0f%%\n", 100.0 * port / 6.0,
                     100.0 * reference / 6.0);
        expect (a.finite() && b.finite(), "both stay finite");
        return failures == 0 ? 0 : 1;
    }
}

int main (int argc, char** argv)
{
    const std::string flag = argc > 1 ? argv[1] : "";
    if (flag == "--reference") return referenceTest();
    if (flag == "--behaviour") return behaviourTest();
    if (flag == "--cpu") return cpuTest();
    std::fprintf (stderr, "usage: AtlasPianoEngineTests --reference | --behaviour | --cpu\n");
    return 2;
}
