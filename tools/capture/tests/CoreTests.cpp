#include <juce_core/juce_core.h>

#include "core/SweepSynth.h"
#include "core/Deconvolution.h"
#include "core/HarmonicSeparation.h"
#include "core/Alignment.h"
#include "core/DcPack.h"
#include "core/CapturePipeline.h"
#include "core/LiveCaptureEngine.h"
#include "core/GridCapture.h"

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

class AlignmentTests : public juce::UnitTest
{
public:
    AlignmentTests() : juce::UnitTest ("Sub-sample alignment") {}

    static std::vector<float> gaussianPulse (int n, double centre, double sigma)
    {
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double d = (double) i - centre;
            v[(size_t) i] = (float) std::exp (-(d * d) / (2.0 * sigma * sigma));
        }
        return v;
    }

    static std::vector<float> tone (int n, double cycles)
    {
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i)
            v[(size_t) i] = (float) std::sin (2.0 * M_PI * cycles * (double) i / (double) n);
        return v;
    }

    void runTest() override
    {
        beginTest ("fractional shift matches an analytic phase shift on a pure tone");
        {
            const int    N   = 1024;
            const double cyc  = 8.0;  // exact FFT bin -> periodic -> circular shift is exact
            const double d    = 0.37;
            const auto   x    = tone (N, cyc);
            const auto   y    = fractionalShift (x, d);

            float maxErr = 0.0f;
            for (int n = 0; n < N; ++n)
            {
                const double a = std::sin (2.0 * M_PI * cyc * ((double) n - d) / (double) N);
                maxErr = juce::jmax (maxErr, std::abs (y[(size_t) n] - (float) a));
            }
            expectLessThan (maxErr, 1.0e-3f, "shifted tone deviates from analytic");
        }

        beginTest ("peak estimate tracks a sub-sample shift");
        {
            const int  N = 512;
            const auto g = gaussianPulse (N, 256.0, 3.0);
            expectWithinAbsoluteError ((float) estimatePeakPosition (g), 256.0f, 0.02f,
                                       "unshifted peak");
            const auto gs = fractionalShift (g, 0.3);
            expectWithinAbsoluteError ((float) estimatePeakPosition (gs), 256.3f, 0.05f,
                                       "shifted peak");
        }

        beginTest ("alignPeakTo removes a known offset and recovers the waveform");
        {
            const int  N = 512;
            const auto g = gaussianPulse (N, 256.0, 3.0);
            const auto gs = fractionalShift (g, 0.42);
            const auto aligned = alignPeakTo (gs, 256.0);

            expectWithinAbsoluteError ((float) estimatePeakPosition (aligned), 256.0f, 0.05f,
                                       "aligned peak position");

            // Recovering the original waveform means two aligned copies sum
            // cleanly rather than combing (CLAUDE.md §5.4).
            float maxDiff = 0.0f;
            for (int n = 0; n < N; ++n)
                maxDiff = juce::jmax (maxDiff, std::abs (aligned[(size_t) n] - g[(size_t) n]));
            expectLessThan (maxDiff, 0.05f, "aligned waveform should match original");
        }
    }
};

class DcPackTests : public juce::UnitTest
{
public:
    DcPackTests() : juce::UnitTest ("dcpack I/O") {}

    static CaptureProfile makeSampleProfile()
    {
        CaptureProfile p;
        p.name                = "Test Unit";
        p.vendor              = "Statebox";
        p.sampleRate          = 48000.0;
        p.bitDepth            = 24;
        p.kernelLengthSamples = 8;
        p.channels            = 2;
        p.levelAxis           = ProfileAxis { "level", "dBFS-at-unit-input", { -18.0f, -6.0f } };
        p.knobAxis            = ProfileAxis { "knob", "normalized", { 0.0f, 1.0f } };
        p.harmonicOrders      = { 2, 3 };

        int idx = 0;
        for (int level = 0; level < 2; ++level)
            for (int knob = 0; knob < 2; ++knob)
                for (int ch = 0; ch < 2; ++ch)
                {
                    CaptureKernels c;
                    c.levelIndex = level;
                    c.knobIndex  = knob;
                    c.channel    = ch;
                    c.gain       = 1.0f;

                    c.linear.resize (8);
                    for (int s = 0; s < 8; ++s)
                        c.linear[(size_t) s] = (float) (idx * 0.1 + s * 0.01) - 0.2f;

                    // Two harmonic kernels (h2, h3) with distinctive values.
                    c.harmonics.resize (2);
                    for (int order = 0; order < 2; ++order)
                    {
                        c.harmonics[(size_t) order].resize (8);
                        for (int s = 0; s < 8; ++s)
                            c.harmonics[(size_t) order][(size_t) s]
                                = (float) ((order + 2) * 0.001 * (s + 1) + idx * 0.0005);
                    }
                    p.cells.push_back (c);
                    ++idx;
                }

        // An out-of-range value to confirm kernels are stored as float (not clipped).
        p.cells[0].linear[0] = 2.5f;
        return p;
    }

    void runTest() override
    {
        beginTest ("normalizeProfile peak-normalizes and scales harmonics together");
        {
            CaptureProfile p;
            CaptureKernels c;
            c.linear    = { 0.0f, 2.0f, -1.0f };
            c.harmonics = { { 0.4f, -0.2f } };
            p.cells.push_back (c);

            normalizeProfile (p, 1.0f);

            float peak = 0.0f;
            for (const float s : p.cells[0].linear) peak = juce::jmax (peak, std::abs (s));
            expectWithinAbsoluteError (peak, 1.0f, 1.0e-6f, "linear peak normalized to 1");
            expectWithinAbsoluteError (p.cells[0].gain, 0.5f, 1.0e-6f, "gain recorded");
            expectWithinAbsoluteError (p.cells[0].harmonics[0][0], 0.2f, 1.0e-6f, "harmonics scaled by same gain");
        }

        beginTest ("write/read round-trips metadata and kernel samples (float storage)");
        {
            const auto p = makeSampleProfile();
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("statebox_dcpack_" + juce::Uuid().toString());

            std::string err;
            expect (writeDcPack (p, dir.getFullPathName().toStdString(), &err),
                    "write failed: " + juce::String (err));

            CaptureProfile q;
            std::string err2;
            expect (readDcPack (dir.getFullPathName().toStdString(), q, &err2),
                    "read failed: " + juce::String (err2));

            expectEquals (q.formatVersion, p.formatVersion);
            expectEquals (juce::String (q.name), juce::String (p.name));
            expectWithinAbsoluteError ((float) q.sampleRate, (float) p.sampleRate, 0.5f);
            expectEquals (q.kernelLengthSamples, p.kernelLengthSamples);
            expectEquals (q.channels, p.channels);
            expectEquals ((int) q.levelAxis.values.size(), (int) p.levelAxis.values.size());
            expectEquals (juce::String (q.levelAxis.unit), juce::String (p.levelAxis.unit));
            expectEquals ((int) q.harmonicOrders.size(), (int) p.harmonicOrders.size());
            expectEquals ((int) q.cells.size(), (int) p.cells.size());

            float maxDiff  = 0.0f;
            bool  foundBig = false;
            for (size_t i = 0; i < q.cells.size() && i < p.cells.size(); ++i)
            {
                const auto& a = p.cells[i];
                const auto& b = q.cells[i];
                expectEquals (b.levelIndex, a.levelIndex);
                expectEquals (b.knobIndex, a.knobIndex);
                expectEquals (b.channel, a.channel);
                expectEquals ((int) b.harmonics.size(), (int) a.harmonics.size());

                for (size_t s = 0; s < a.linear.size(); ++s)
                {
                    maxDiff = juce::jmax (maxDiff, std::abs (a.linear[s] - b.linear[s]));
                    if (a.linear[s] > 2.4f) foundBig = true;
                }
                for (size_t h = 0; h < a.harmonics.size() && h < b.harmonics.size(); ++h)
                    for (size_t s = 0; s < a.harmonics[h].size(); ++s)
                        maxDiff = juce::jmax (maxDiff, std::abs (a.harmonics[h][s] - b.harmonics[h][s]));
            }
            expectLessThan (maxDiff, 1.0e-5f, "kernel samples should round-trip");
            expect (foundBig, "out-of-range value should survive (float WAV storage)");

            dir.deleteRecursively();
        }
    }
};

class CapturePipelineTests : public juce::UnitTest
{
public:
    CapturePipelineTests() : juce::UnitTest ("Capture pipeline") {}

    void runTest() override
    {
        SweepSpec spec;
        spec.durationSeconds = 1.0;
        SweepSynth synth (spec);

        const int K      = 512;
        const int target = K / 2;

        beginTest ("pipeline recovers an aligned linear IR + harmonics");
        {
            const auto rec = polyNonlinearity (synth.sweep(), 0.25f, 0.1f);
            const auto c   = processRecording (rec, synth, 3, K);

            expectEquals ((int) c.harmonics.size(), 2);
            expect (std::abs (argMaxAbs (c.linear) - target) <= 2, "linear IR peak should be centred");

            const float h1 = maxAbs (c.linear);
            expectGreaterThan (maxAbs (c.harmonics[0]) / h1, 0.01f, "h2 present");
            expectGreaterThan (maxAbs (c.harmonics[1]) / h1, 0.005f, "h3 present");
        }

        beginTest ("pipeline compensates round-trip latency");
        {
            const int  D = 40;
            const auto y = polyNonlinearity (synth.sweep(), 0.25f, 0.1f);
            std::vector<float> rec ((size_t) ((int) y.size() + D), 0.0f);
            for (size_t i = 0; i < y.size(); ++i)
                rec[i + (size_t) D] = y[i];

            const auto c = processRecording (rec, synth, 3, K);
            expect (std::abs (argMaxAbs (c.linear) - target) <= 2,
                    "linear IR peak should be centred despite latency");
            expectGreaterThan (maxAbs (c.harmonics[0]) / maxAbs (c.linear), 0.01f, "h2 present");
        }

        beginTest ("end-to-end: process -> writeDcPack -> readDcPack");
        {
            const auto rec     = polyNonlinearity (synth.sweep(), 0.25f, 0.1f);
            auto       kernels = processRecording (rec, synth, 3, K);
            auto       profile = buildSingleCellProfile (kernels, spec, 3, K, "E2E Unit");
            normalizeProfile (profile, 1.0f);

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("statebox_e2e_" + juce::Uuid().toString());
            std::string err;
            expect (writeDcPack (profile, dir.getFullPathName().toStdString(), &err),
                    "write: " + juce::String (err));

            CaptureProfile q;
            std::string err2;
            expect (readDcPack (dir.getFullPathName().toStdString(), q, &err2),
                    "read: " + juce::String (err2));

            expectEquals ((int) q.cells.size(), 1);
            if (! q.cells.empty())
            {
                expect (std::abs (argMaxAbs (q.cells[0].linear) - target) <= 2,
                        "read-back linear peak centred");
                expectEquals ((int) q.cells[0].harmonics.size(), 2);
            }
            dir.deleteRecursively();
        }

        beginTest ("analyzeRecording is latency-aware: finds onset, measures clean noise floor");
        {
            // A return shifted by `latency`, with quiet noise filling the silent regions.
            const int latency = 100, sweepLen = 1000, tail = 1000;
            const int n = sweepLen + tail;
            std::vector<float> rec ((size_t) n, 0.0f);
            for (int i = 0; i < sweepLen; ++i) rec[(size_t) (latency + i)] = 0.5f * std::sin (0.1f * i);
            for (int i = 0; i < n; ++i)        rec[(size_t) i]            += 1.0e-4f * std::sin (0.37f * i);

            const auto st = analyzeRecording (rec, tail);
            expect (std::abs (st.latencySamples - latency) <= 8,
                    "onset ~ latency (" + juce::String (st.latencySamples) + ")");
            expectWithinAbsoluteError (st.peak, 0.5f, 2.0e-3f, "peak measured");
            expect (! st.clipping && ! st.silent, "flags clear");
            expect (st.noiseFloorValid, "tail long enough to measure noise floor");
            expect (st.snrValid && st.snrDb > 50.0f,
                    "clean post-return silence -> high SNR (" + juce::String (st.snrDb, 1) + " dB)");
        }

        beginTest ("analyzeRecording flags a too-short tail, clipping, and silence");
        {
            // Tail shorter than latency+guard -> no clean silence to measure.
            std::vector<float> shortTail (1200, 0.2f);
            const auto stShort = analyzeRecording (shortTail, 100);
            expect (! stShort.noiseFloorValid, "short tail -> noise floor not measurable");

            expect (analyzeRecording (std::vector<float> (500, 1.0f), 100).clipping, "full-scale -> clipping");
            expect (analyzeRecording (std::vector<float> (500, 0.0f), 100).silent, "zeros -> silent");
        }

        beginTest ("analyzeKernel scores a clean impulse high and a spread kernel low");
        {
            std::vector<float> impulse (256, 0.0f);
            impulse[128] = 1.0f;
            const auto a = analyzeKernel (impulse);
            expectEquals (a.peakIndex, 128, "impulse peak located");
            expectWithinAbsoluteError (a.peakValue, 1.0f, 1.0e-6f, "impulse peak value");
            expectGreaterThan (a.sharpnessDb, 60.0f, "delta -> very sharp");

            std::vector<float> spread (256, 0.1f);
            spread[128] = 0.2f;
            const auto b = analyzeKernel (spread);
            expectLessThan (b.sharpnessDb, 20.0f, "energy everywhere -> not sharp");
        }
    }
};

class GridCaptureTests : public juce::UnitTest
{
public:
    GridCaptureTests() : juce::UnitTest ("Grid capture orchestration") {}

    static GridPlan makePlan()
    {
        GridPlan plan;
        plan.name                  = "Grid Unit";
        plan.sweep.durationSeconds = 0.2;   // short, for a fast test
        plan.repetitions           = 1;
        plan.maxHarmonic           = 3;
        plan.kernelLength          = 256;
        plan.levelsDb              = { -12.0f, -6.0f };       // 2 levels
        plan.knobName              = "cutoff";
        plan.knobValues            = { 0.0f, 0.5f, 1.0f };    // 3 knobs -> 6 cells
        return plan;
    }

    void runTest() override
    {
        const auto plan = makePlan();
        SweepSynth synth (plan.sweep); // identity unit: captureCell returns the sweep itself

        beginTest ("walks the full grid: one cell per (level,knob), axes + indices correct");
        {
            int calls = 0, doneCount = 0;
            auto cap = [&] (int, int, float, float, float) -> CellCapture
            {
                ++calls;
                return { true, false, synth.sweep() };
            };
            auto onDone = [&] (const CaptureProfile&) { ++doneCount; };

            const auto profile = assembleProfile (plan, cap, {}, onDone);

            expectEquals (calls, 6, "captureCell called once per cell");
            expectEquals (doneCount, 6, "onCellDone called once per cell");
            expectEquals ((int) profile.cells.size(), 6);
            expectEquals ((int) profile.levelAxis.values.size(), 2);
            expectEquals ((int) profile.knobAxis.values.size(), 3);
            expectEquals (juce::String (profile.knobAxis.name), juce::String ("cutoff"));
            expectEquals (profile.channels, 1, "mono capture");
            expectEquals (profile.latencyReferenceSamples, 0, "no latency ref by default");

            std::vector<int> seen (6, 0);
            for (const auto& c : profile.cells)
                seen[(size_t) (c.knobIndex * 2 + c.levelIndex)]++;
            for (int i = 0; i < 6; ++i)
                expectEquals (seen[(size_t) i], 1, "each (level,knob) present exactly once");

            for (const auto& c : profile.cells)
            {
                float pk = 0.0f;
                for (const float s : c.linear) pk = juce::jmax (pk, std::abs (s));
                expectWithinAbsoluteError (pk, 1.0f, 1.0e-3f, "each cell normalized to unity peak");
            }
        }

        beginTest ("resume: existing cells are kept and their column is not recaptured");
        {
            CaptureProfile existing;
            existing.levelAxis = ProfileAxis { "level", "dBFS-at-output", plan.levelsDb };
            existing.knobAxis  = ProfileAxis { plan.knobName, plan.knobUnit, plan.knobValues };
            for (int li = 0; li < 2; ++li) // knob column 0 already captured
            {
                CaptureKernels c;
                c.levelIndex = li;
                c.knobIndex  = 0;
                c.linear.assign ((size_t) plan.kernelLength, 0.0f);
                c.linear[(size_t) (plan.kernelLength / 2)] = 1.0f;
                existing.cells.push_back (c);
            }

            int calls = 0;
            std::vector<std::pair<int, int>> captured;
            auto cap = [&] (int li, int ki, float, float, float) -> CellCapture
            {
                ++calls;
                captured.push_back ({ li, ki });
                return { true, false, synth.sweep() };
            };

            const auto profile = assembleProfile (plan, cap, {}, {}, &existing);

            expectEquals (calls, 4, "only the 4 missing cells are captured");
            expectEquals ((int) profile.cells.size(), 6, "existing + new = full grid");
            for (const auto& p : captured)
                expect (p.second != 0, "the already-captured knob-0 column is skipped");
        }

        beginTest ("abort stops the grid and keeps cells captured so far");
        {
            int calls = 0;
            auto cap = [&] (int, int, float, float, float) -> CellCapture
            {
                ++calls;
                if (calls == 3) return { false, true, {} }; // abort on the 3rd cell
                return { true, false, synth.sweep() };
            };

            const auto profile = assembleProfile (plan, cap, {}, {});
            expectEquals (calls, 3, "stops as soon as abort is returned");
            expectEquals ((int) profile.cells.size(), 2, "2 cells banked before the abort");
        }

        beginTest ("plan JSON round-trips (incl. latency reference)");
        {
            GridPlan src = plan;
            src.latencyReferenceSamples = 1528;

            const auto path = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("statebox_plan_" + juce::Uuid().toString() + ".json");

            std::string e;
            expect (saveGridPlan (src, path.getFullPathName().toStdString(), &e), "save: " + juce::String (e));

            GridPlan q;
            expect (loadGridPlan (path.getFullPathName().toStdString(), q, &e), "load: " + juce::String (e));
            expectEquals (juce::String (q.name), juce::String (src.name));
            expectEquals (q.repetitions, src.repetitions);
            expectEquals (q.kernelLength, src.kernelLength);
            expectEquals ((int) q.levelsDb.size(), 2);
            expectEquals ((int) q.knobValues.size(), 3);
            expectEquals (juce::String (q.knobName), juce::String ("cutoff"));
            expectWithinAbsoluteError (q.levelsDb[1], -6.0f, 1.0e-4f, "level value preserved");
            expectWithinAbsoluteError (q.knobValues[1], 0.5f, 1.0e-4f, "knob value preserved");
            expectEquals (q.latencyReferenceSamples, 1528, "latency reference preserved");

            path.deleteFile();
        }

        beginTest ("assembleProfile carries the plan's latency reference into the profile");
        {
            GridPlan src = plan;
            src.latencyReferenceSamples = 1590;
            auto cap = [&] (int, int, float, float, float) -> CellCapture
            {
                return { true, false, synth.sweep() };
            };
            const auto profile = assembleProfile (src, cap, {}, {});
            expectEquals (profile.latencyReferenceSamples, 1590, "latency ref propagated to profile");
        }

        beginTest ("dbToLinear maps dBFS to amplitude and clamps at 0 dBFS");
        {
            expectWithinAbsoluteError (dbToLinear (0.0f),   1.0f,        1.0e-4f, "0 dBFS = 1.0");
            expectWithinAbsoluteError (dbToLinear (-6.0f),  0.501187f,   1.0e-3f, "-6 dBFS ~ 0.5");
            expectWithinAbsoluteError (dbToLinear (-20.0f), 0.1f,        1.0e-3f, "-20 dBFS = 0.1");
            expectWithinAbsoluteError (dbToLinear (6.0f),   1.0f,        1.0e-4f, "positive clamps to 1.0");
        }
    }
};

class LiveCaptureTests : public juce::UnitTest
{
public:
    LiveCaptureTests() : juce::UnitTest ("Live capture") {}

    void runTest() override
    {
        LiveCaptureConfig cfg;
        cfg.sweep.durationSeconds = 0.2; // short, for a fast test
        cfg.conditioningSeconds   = 0.05;
        cfg.settleSeconds         = 0.05;
        cfg.repetitions           = 3;

        beginTest ("virtual loopback: drive engine, recover IR + harmonics, get a std map");
        {
            LiveCaptureEngine engine (cfg);

            // Virtual device: a known nonlinearity, a round-trip latency, and a tiny
            // per-sample perturbation (so the std across reps is non-zero).
            const int D     = 128; // latency, >= block so input comes only from history
            const int block = 64;
            auto nonlin = [] (float x) { return x + 0.25f * x * x + 0.1f * x * x * x; };

            std::vector<float> outAll;
            outAll.reserve (1u << 20);
            std::vector<float> inB ((size_t) block), outB ((size_t) block);

            int g = 0;
            while (! engine.isFinished())
            {
                for (int i = 0; i < block; ++i)
                {
                    const int src = g + i - D;
                    const float o = (src >= 0 && src < (int) outAll.size()) ? outAll[(size_t) src] : 0.0f;
                    inB[(size_t) i] = nonlin (o) + 1.0e-4f * (float) std::sin (0.013 * (g + i));
                }
                engine.processBlock (inB.data(), outB.data(), block);
                for (int i = 0; i < block; ++i) outAll.push_back (outB[(size_t) i]);
                g += block;
                if (g > 5'000'000) break; // safety
            }

            expect (engine.isFinished(), "engine should finish");
            engine.finalize();

            expectEquals ((int) engine.rawRecordings().size(), 3);
            expectEquals ((int) engine.meanRecording().size(), engine.recordLengthSamples());

            float maxStd = 0.0f;
            for (const float s : engine.stdRecording()) maxStd = juce::jmax (maxStd, s);
            expectGreaterThan (maxStd, 0.0f, "std map should capture inter-rep variation");
            expectLessThan (maxStd, 0.1f, "std should be small");

            // The averaged recording should still yield a clean IR + harmonics.
            SweepSynth synth (cfg.sweep);
            const int  K      = 256;
            const int  target = K / 2;
            const auto c      = processRecording (engine.meanRecording(), synth, 3, K);

            expect (std::abs (argMaxAbs (c.linear) - target) <= 3, "recovered linear IR centred");
            expectEquals ((int) c.harmonics.size(), 2);
            expectGreaterThan (maxAbs (c.harmonics[0]) / maxAbs (c.linear), 0.01f, "h2 present from loopback");
        }
    }
};

static SweepDeconvTests        sweepDeconvTests;
static HarmonicSeparationTests harmonicSeparationTests;
static AlignmentTests          alignmentTests;
static DcPackTests             dcPackTests;
static CapturePipelineTests    capturePipelineTests;
static LiveCaptureTests        liveCaptureTests;
static GridCaptureTests        gridCaptureTests;

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
