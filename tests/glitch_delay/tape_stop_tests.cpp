#include "glitch_delay/glitch_delay_engine.h"

namespace
{
    namespace dsp = atlas::glitch_delay;

    constexpr double tapeSampleRate = 48000.0;
    constexpr int tapeBlockSize = 256;

    /** Runs a mono signal through the engine in blocks and returns the left output. */
    std::vector<float> runTape (dsp::Engine& engine, const std::vector<float>& input)
    {
        std::vector<float> left (input), right (input.size(), 0.0f);

        for (size_t start = 0; start < input.size(); start += tapeBlockSize)
        {
            const auto n = (int) std::min ((size_t) tapeBlockSize, input.size() - start);
            engine.process (left.data() + start, nullptr, left.data() + start, right.data() + start, n);
        }

        return left;
    }

    std::vector<float> sine (int length, int soundingLength, float frequency)
    {
        std::vector<float> signal ((size_t) length, 0.0f);

        for (int i = 0; i < soundingLength; ++i)
            signal[(size_t) i] = 0.5f * std::sin (juce::MathConstants<float>::twoPi * frequency * (float) i / (float) tapeSampleRate);

        return signal;
    }

    float level (const std::vector<float>& signal, int start, int length)
    {
        double sum = 0.0;

        for (int i = start; i < start + length; ++i)
            sum += (double) signal[(size_t) i] * signal[(size_t) i];

        return (float) std::sqrt (sum / length);
    }

    float peak (const std::vector<float>& signal, int start, int length)
    {
        auto highest = 0.0f;

        for (int i = start; i < start + length; ++i)
            highest = std::max (highest, std::abs (signal[(size_t) i]));

        return highest;
    }

    int zeroCrossings (const std::vector<float>& signal, int start, int length)
    {
        int count = 0;

        for (int i = start + 1; i < start + length; ++i)
            if ((signal[(size_t) i - 1] < 0.0f) != (signal[(size_t) i] < 0.0f))
                ++count;

        return count;
    }

    dsp::GlobalSettings wetTape()
    {
        dsp::GlobalSettings g;
        g.glideSeconds = 0.0f;
        g.mix = 1.0f;
        g.outputGain = 1.0f;
        return g;
    }

    dsp::TapSettings loopTap (float delaySamples, float feedback)
    {
        dsp::TapSettings t;
        t.enabled = true;
        t.delaySamples = delaySamples;
        t.feedback = feedback;
        return t;
    }

    void setUpTape (dsp::Engine& engine, const dsp::GlobalSettings& global, const dsp::TapSettings& tap)
    {
        engine.prepare (tapeSampleRate, tapeBlockSize, 2.0);
        engine.setGlobalSettings (global);
        engine.setTapSettings (0, tap);
        engine.reset();
    }
}

class TapeStopTests final : public juce::UnitTest
{
public:
    TapeStopTests() : juce::UnitTest ("Tape stop", "Glitch Delay") {}

    void runTest() override
    {

        beginTest ("The tape takes the stop time to stop and the start time to start");
        {
            dsp::TapeClock clock;
            clock.prepare (tapeSampleRate);
            clock.reset (false);
            expect (clock.isAtFullSpeed());

            clock.setSettings (true, 0.5f, 0.25f);
            int samples = 0, tapeSamples = 0;

            while (! clock.isStopped() && samples < 100000)
            {
                tapeSamples += clock.advance() ? 1 : 0;
                ++samples;
            }

            expectWithinAbsoluteError (samples, 24000, 1);

            // A straight line in speed covers half the distance.
            expectWithinAbsoluteError (tapeSamples, 12000, 2);
            expectEquals (clock.getGain(), 0.0f);
            expect (! clock.advance());

            clock.setSettings (false, 0.5f, 0.25f);
            samples = 0;

            while (clock.isSlowed() && samples < 100000)
            {
                clock.advance();
                ++samples;
            }

            expectWithinAbsoluteError (samples, 12000, 1);

            // Then it settles onto whole samples, where the output is the tape's newest sample.
            samples = 0;

            while (! clock.isAtFullSpeed() && samples < 100000)
            {
                expect (clock.advance());
                ++samples;
            }

            expect (samples <= 501);
            expectEquals (clock.getFraction(), 1.0f);
            expectEquals (clock.getGain(), 1.0f);
        }

        beginTest ("Let go part of the way down, the tape starts again from the speed it reached");
        {
            dsp::TapeClock clock;
            clock.prepare (tapeSampleRate);
            clock.reset (false);
            clock.setSettings (true, 0.5f, 0.25f);

            for (int i = 0; i < 12000; ++i)
                clock.advance();

            expectWithinAbsoluteError (clock.getSpeed(), 0.5, 1.0e-3);

            clock.setSettings (false, 0.5f, 0.25f);
            int samples = 0;

            while (clock.isSlowed() && samples < 100000)
            {
                clock.advance();
                ++samples;
            }

            expectWithinAbsoluteError (samples, 6000, 2);

            // And stopped again part of the way up, it slows from there.
            clock.reset (true);
            clock.setSettings (false, 0.5f, 0.25f);

            for (int i = 0; i < 6000; ++i)
                clock.advance();

            clock.setSettings (true, 0.5f, 0.25f);
            samples = 0;

            while (! clock.isStopped() && samples < 100000)
            {
                clock.advance();
                ++samples;
            }

            expectWithinAbsoluteError (samples, 12000, 2);
        }

        beginTest ("A tape that is never stopped is not changed by the stop and start times");
        {
            const auto input = sine (48000, 24000, 440.0f);
            auto global = wetTape();

            dsp::Engine first, second;
            setUpTape (first, global, loopTap (4800.0f, 0.6f));

            global.tapeStopSeconds = 2.0f;
            global.tapeStartSeconds = 0.05f;
            setUpTape (second, global, loopTap (4800.0f, 0.6f));

            const auto a = runTape (first, input);
            const auto b = runTape (second, input);

            expect (a == b);
            expect (level (a, 24000, 24000) > 0.01f);
            expect (first.getTape().isAtFullSpeed());
        }

        beginTest ("A stopped tape is silent, and the dry signal still passes");
        {
            auto global = wetTape();
            global.mix = 0.5f;
            global.tapeStopped = true;

            dsp::Engine engine;
            setUpTape (engine, global, loopTap (2400.0f, 0.6f));
            expect (engine.getTape().isStopped());

            const auto input = sine (24000, 24000, 440.0f);
            const auto output = runTape (engine, input);
            const auto dryGain = std::cos (juce::MathConstants<float>::halfPi * 0.5f);
            auto worst = 0.0f;

            for (size_t i = 0; i < input.size(); ++i)
                worst = std::max (worst, std::abs (output[i] - input[i] * dryGain));

            expect (worst < 1.0e-6f);
        }

        beginTest ("What is played into a stopped tape is not recorded");
        {
            const auto playAndListen = [] (bool stopped)
            {
                auto global = wetTape();
                global.tapeStopped = stopped;
                global.tapeStartSeconds = 0.05f;

                dsp::Engine engine;
                const auto tap = loopTap (4800.0f, 0.7f);
                setUpTape (engine, global, tap);
                runTape (engine, sine (24000, 24000, 440.0f));

                global.tapeStopped = false;
                engine.setGlobalSettings (global);
                engine.setTapSettings (0, tap);
                return runTape (engine, std::vector<float> (48000, 0.0f));
            };

            const auto afterStopped = playAndListen (true);
            const auto afterRunning = playAndListen (false);

            expect (peak (afterStopped, 0, 48000) < 1.0e-6f);
            expect (level (afterRunning, 0, 48000) > 0.01f);
        }

        beginTest ("Stopping drops the pitch to silence, and starting carries on from there");
        {
            auto global = wetTape();
            global.tapeStopSeconds = 2.0f;
            global.tapeStartSeconds = 0.25f;

            // A loop that closes on itself: the burst is twice as long as the delay.
            const auto tap = loopTap (12000.0f, 0.9f);

            dsp::Engine engine;
            setUpTape (engine, global, tap);

            const auto running = runTape (engine, sine (48000, 24000, 1000.0f));
            const auto crossingsBefore = zeroCrossings (running, 36000, 9600);
            const auto levelBefore = level (running, 36000, 9600);
            expectWithinAbsoluteError (crossingsBefore, 400, 8);

            global.tapeStopped = true;
            engine.setGlobalSettings (global);
            engine.setTapSettings (0, tap);

            const auto stopping = runTape (engine, std::vector<float> (105600, 0.0f));

            // Half way down, at half speed, what was recorded at full speed is an octave lower.
            const auto crossingsHalfway = zeroCrossings (stopping, 43200, 9600);
            expect (crossingsHalfway > 170 && crossingsHalfway < 230,
                    juce::String (crossingsHalfway) + " zero crossings half way down");

            expect (engine.getTape().isStopped());
            expectEquals (peak (stopping, 96100, 9500), 0.0f);

            global.tapeStopped = false;
            engine.setGlobalSettings (global);
            engine.setTapSettings (0, tap);

            const auto starting = runTape (engine, std::vector<float> (24000, 0.0f));
            expect (engine.getTape().isAtFullSpeed());

            // The loop is where it was left: at pitch again, having lost only what it would have
            // in the time the tape actually ran.
            expectWithinAbsoluteError (zeroCrossings (starting, 14400, 9600), 400, 8);
            expect (level (starting, 14400, 9600) > 0.3f * levelBefore);

            auto finite = true;

            for (const auto* signal : { &running, &stopping, &starting })
                for (const auto sample : *signal)
                    finite = finite && std::isfinite (sample);

            expect (finite);
        }

        beginTest ("What is played into a slowing tape doesn't come back as a squeal");
        {
            dsp::TapeClock clock;
            clock.prepare (tapeSampleRate);
            clock.reset (false);
            expectEquals (clock.getRecordGain(), 1.0f);

            clock.setSettings (true, 0.5f, 0.25f);

            for (int i = 0; i < 6000; ++i)
                clock.advance();

            expectWithinAbsoluteError (clock.getRecordGain(), 0.25f, 0.01f);

            for (int i = 0; i < 6000; ++i)
                clock.advance();

            expectWithinAbsoluteError (clock.getRecordGain(), 0.0f, 1.0e-4f);

            // A steady tone into one repeat, with the tape stopped for a second in the middle.
            // What was recorded on the way down and up returns once the tape is fast again.
            auto global = wetTape();
            const auto tap = loopTap (12000.0f, 0.0f);

            dsp::Engine engine;
            setUpTape (engine, global, tap);

            const auto tone = sine (48000, 48000, 440.0f);
            const auto before = runTape (engine, tone);
            const auto reference = level (before, 43200, 4800);

            global.tapeStopped = true;
            engine.setGlobalSettings (global);
            engine.setTapSettings (0, tap);
            runTape (engine, tone);

            global.tapeStopped = false;
            engine.setGlobalSettings (global);
            engine.setTapSettings (0, tap);
            const auto after = runTape (engine, tone);

            // In 10 ms windows from the tape reaching full speed: nothing audible is an octave
            // sharp, and nothing near full level is more than about 4 semitones sharp.
            auto highestAudible = 0, highestLoud = 0;

            for (int start = 12480; start + 480 <= 48000; start += 480)
            {
                const auto relative = level (after, start, 480) / reference;
                const auto pitch = zeroCrossings (after, start, 480) * 50;

                if (relative > 0.01f)
                    highestAudible = std::max (highestAudible, pitch);

                if (relative > 0.5f)
                    highestLoud = std::max (highestLoud, pitch);
            }

            expect (highestAudible < 880, "Audible up to " + juce::String (highestAudible) + " Hz");
            expect (highestLoud > 0 && highestLoud <= 550, "Loud up to " + juce::String (highestLoud) + " Hz");

            // And the repeat is back in full afterwards.
            expectWithinAbsoluteError (level (after, 43200, 4800) / reference, 1.0f, 0.02f);
        }
    }
};

static TapeStopTests tapeStopTests;
