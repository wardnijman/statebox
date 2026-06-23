#include <juce_core/juce_core.h>

#include "core/SweepSynth.h"
#include "core/Deconvolution.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace statebox::capture;

namespace
{
int argMaxAbs (const std::vector<float>& v)
{
    int idx = 0;
    float best = -1.0f;
    for (int i = 0; i < (int) v.size(); ++i)
    {
        const float a = std::abs (v[(size_t) i]);
        if (a > best) { best = a; idx = i; }
    }
    return idx;
}
} // namespace

class SweepDeconvTests : public juce::UnitTest
{
public:
    SweepDeconvTests() : juce::UnitTest ("Sweep + deconvolution") {}

    void runTest() override
    {
        SweepSpec spec;
        spec.sampleRate      = 48000.0;
        spec.startHz         = 20.0;
        spec.endHz           = 20000.0;
        spec.durationSeconds = 1.0; // short, for a fast test
        SweepSynth synth (spec);
        const int N = synth.length();

        beginTest ("self-deconvolution yields a sharp peak at index N-1");
        {
            const auto r = deconvolve (synth.sweep(), synth.inverseFilter(), N);
            const int peakIdx = argMaxAbs (r.full);
            expect (std::abs (peakIdx - (N - 1)) <= 2,
                    "peak at " + juce::String (peakIdx) + ", expected ~" + juce::String (N - 1));

            const float peak = std::abs (r.full[(size_t) peakIdx]);
            double sum = 0.0;
            int    cnt = 0;
            const int guard = 64;
            for (int i = 0; i < (int) r.full.size(); ++i)
                if (std::abs (i - peakIdx) > guard) { sum += std::abs (r.full[(size_t) i]); ++cnt; }
            const float meanOutside = (float) (sum / juce::jmax (1, cnt));
            expectGreaterThan (peak / juce::jmax (1.0e-12f, meanOutside), 50.0f,
                               "peak should dominate the floor");
        }

        beginTest ("linear recovery: delay + gain reproduced in position and amplitude");
        {
            const auto r0      = deconvolve (synth.sweep(), synth.inverseFilter(), N);
            const int  peak0Id = argMaxAbs (r0.full);
            const float peak0  = r0.full[(size_t) peak0Id];

            const int   d = 50;
            const float g = 0.5f;
            std::vector<float> rec ((size_t) (N + d), 0.0f);
            for (int n = 0; n < N; ++n)
                rec[(size_t) (n + d)] = g * synth.sweep()[(size_t) n];

            const auto r1      = deconvolve (rec, synth.inverseFilter(), N);
            const int  peak1Id = argMaxAbs (r1.full);
            const float peak1  = r1.full[(size_t) peak1Id];

            expect (std::abs (peak1Id - (peak0Id + d)) <= 2,
                    "delayed peak at " + juce::String (peak1Id) + ", expected ~" + juce::String (peak0Id + d));
            expectWithinAbsoluteError (peak1 / peak0, g, 0.02f,
                                       "recovered gain ratio " + juce::String (peak1 / peak0));
        }

        beginTest ("harmonic offsets increase with order");
        {
            const double h2 = synth.harmonicOffsetSamples (2);
            const double h3 = synth.harmonicOffsetSamples (3);
            expectGreaterThan (h2, 0.0);
            expectGreaterThan (h3, h2);
        }
    }
};

static SweepDeconvTests sweepDeconvTests;

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    if (failures > 0)
    {
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
