#include <juce_core/juce_core.h>

#include "core/SweepSynth.h"
#include "core/Deconvolution.h"
#include "core/HarmonicSeparation.h"

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

float maxAbs (const std::vector<float>& v)
{
    float m = 0.0f;
    for (const float s : v)
        m = juce::jmax (m, std::abs (s));
    return m;
}

// Push the sweep through a memoryless polynomial y = x + a2*x^2 + a3*x^3.
std::vector<float> polyNonlinearity (const std::vector<float>& x, float a2, float a3)
{
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        const float s = x[i];
        y[i] = s + a2 * s * s + a3 * s * s * s;
    }
    return y;
}
} // namespace

class SweepDeconvTests : public juce::UnitTest
{
public:
    SweepDeconvTests() : juce::UnitTest ("Sweep + deconvolution") {}

    void runTest() override
    {
        SweepSpec spec;
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

class HarmonicSeparationTests : public juce::UnitTest
{
public:
    HarmonicSeparationTests() : juce::UnitTest ("Harmonic separation") {}

    void runTest() override
    {
        SweepSpec spec;
        spec.durationSeconds = 1.0;
        SweepSynth synth (spec);

        const int kernelLen = 512;
        const int centre    = kernelLen / 2;

        beginTest ("h2/h3 land at the predicted offsets, centred in their windows");
        {
            const auto y      = polyNonlinearity (synth.sweep(), 0.25f, 0.1f);
            const auto decon  = deconvolve (y, synth.inverseFilter(), synth.length());
            const auto k      = separateHarmonics (decon, synth, 3, kernelLen);

            // The peak of each harmonic window should sit at the window centre,
            // i.e. exactly at the predicted ln(m) offset.
            expect (std::abs (argMaxAbs (k[1]) - centre) <= 4, "h2 peak off-centre");
            expect (std::abs (argMaxAbs (k[2]) - centre) <= 4, "h3 peak off-centre");

            // Both harmonics are present relative to the linear IR.
            const float h1 = maxAbs (k[0]);
            expectGreaterThan (maxAbs (k[1]) / h1, 0.01f, "h2 should be present");
            expectGreaterThan (maxAbs (k[2]) / h1, 0.01f, "h3 should be present");
        }

        beginTest ("x^2 produces only even harmonics; x^3 only odd");
        {
            // Square only -> 2nd harmonic present, 3rd absent.
            const auto kSq = separateHarmonics (
                deconvolve (polyNonlinearity (synth.sweep(), 0.25f, 0.0f),
                            synth.inverseFilter(), synth.length()),
                synth, 3, kernelLen);
            expectGreaterThan (maxAbs (kSq[1]), 10.0f * maxAbs (kSq[2]),
                               "x^2: h2 should dominate h3");

            // Cube only -> 3rd harmonic present, 2nd absent.
            const auto kCu = separateHarmonics (
                deconvolve (polyNonlinearity (synth.sweep(), 0.0f, 0.1f),
                            synth.inverseFilter(), synth.length()),
                synth, 3, kernelLen);
            expectGreaterThan (maxAbs (kCu[2]), 10.0f * maxAbs (kCu[1]),
                               "x^3: h3 should dominate h2");
        }

        beginTest ("3rd-harmonic amplitude scales linearly with cubic coefficient");
        {
            const auto kA = separateHarmonics (
                deconvolve (polyNonlinearity (synth.sweep(), 0.0f, 0.1f),
                            synth.inverseFilter(), synth.length()),
                synth, 3, kernelLen);
            const auto kB = separateHarmonics (
                deconvolve (polyNonlinearity (synth.sweep(), 0.0f, 0.3f),
                            synth.inverseFilter(), synth.length()),
                synth, 3, kernelLen);

            const float ratio = maxAbs (kB[2]) / maxAbs (kA[2]);
            expectWithinAbsoluteError (ratio, 3.0f, 0.3f,
                                       "h3 should scale ~3x for 3x cubic coeff (got "
                                       + juce::String (ratio) + ")");
        }
    }
};

static SweepDeconvTests        sweepDeconvTests;
static HarmonicSeparationTests harmonicSeparationTests;

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
