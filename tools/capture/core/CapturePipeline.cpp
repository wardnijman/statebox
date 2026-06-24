#include "CapturePipeline.h"

#include <algorithm>
#include <cmath>

#include "Deconvolution.h"
#include "HarmonicSeparation.h"
#include "Alignment.h"

namespace statebox::capture
{

CaptureKernels processRecording (const std::vector<float>& recording,
                                 const SweepSynth& synth,
                                 int maxHarmonic,
                                 int kernelLength,
                                 int levelIndex,
                                 int knobIndex,
                                 int channel)
{
    const auto   decon   = deconvolve (recording, synth.inverseFilter(), synth.length());
    const auto   windows = separateHarmonics (decon, synth, maxHarmonic, kernelLength);
    const double target  = kernelLength / 2.0;

    CaptureKernels c;
    c.levelIndex = levelIndex;
    c.knobIndex  = knobIndex;
    c.channel    = channel;
    c.gain       = 1.0f;

    if (! windows.empty())
        c.linear = alignPeakTo (windows[0], target);
    else
        c.linear.assign ((size_t) kernelLength, 0.0f);

    for (size_t h = 1; h < windows.size(); ++h)
        c.harmonics.push_back (alignPeakTo (windows[h], target));

    return c;
}

CaptureProfile buildSingleCellProfile (CaptureKernels kernels,
                                       const SweepSpec& spec,
                                       int maxHarmonic,
                                       int kernelLength,
                                       const std::string& name)
{
    CaptureProfile p;
    p.name                = name;
    p.vendor              = "User Capture";
    p.sampleRate          = spec.sampleRate;
    p.bitDepth            = 24;
    p.kernelLengthSamples = kernelLength;
    p.channels            = 1;
    p.levelAxis           = ProfileAxis { "level", "dBFS-at-unit-input", { 0.0f } };
    p.knobAxis            = ProfileAxis { "knob", "normalized", { 0.0f } };

    for (int m = 2; m <= maxHarmonic; ++m)
        p.harmonicOrders.push_back (m);

    p.cells.push_back (std::move (kernels));
    return p;
}

namespace
{
float rmsOf (const float* p, int count)
{
    if (count <= 0) return 0.0f;
    double acc = 0.0;
    for (int i = 0; i < count; ++i) acc += (double) p[i] * (double) p[i];
    return (float) std::sqrt (acc / count);
}

// First index where the signal rises above 5% of peak (the sweep return onset).
int detectOnset (const std::vector<float>& rec, float peak)
{
    if (peak <= 0.0f) return 0;
    const float thr = 0.05f * peak;
    const int   win = 8; // require a short run, so a lone noise spike doesn't trigger
    for (int i = 0; i + win <= (int) rec.size(); ++i)
    {
        float e = 0.0f;
        for (int j = 0; j < win; ++j) e = std::max (e, std::abs (rec[(size_t) (i + j)]));
        if (e > thr) return i;
    }
    return 0;
}
} // namespace

CaptureStats analyzeRecording (const std::vector<float>& rec, int tailLen)
{
    CaptureStats st;
    if (rec.empty())
        return st;

    const int n        = (int) rec.size();
    const int tail     = std::min (std::max (0, tailLen), n);
    const int sweepLen = n - tail;          // nominal sweep length (output-time)
    const int guard    = 256;               // skip any short post-sweep decay

    for (const float v : rec)
        st.peak = std::max (st.peak, std::abs (v));
    st.clipping = st.peak >= 0.99f;
    st.silent   = st.peak < 1.0e-4f;

    // Where the return actually begins = round-trip latency.
    const int onset = detectOnset (rec, st.peak);
    st.latencySamples = onset;

    // Sweep return occupies [onset, onset+sweepLen]; measure RMS there.
    const int sigEnd = std::min (n, onset + sweepLen);
    st.sweepRms = rmsOf (rec.data() + onset, std::max (0, sigEnd - onset));

    // Noise floor = the genuinely silent region after the return + a guard.
    const int nfStart = onset + sweepLen + guard;
    if (nfStart < n)
    {
        st.noiseFloor      = rmsOf (rec.data() + nfStart, n - nfStart);
        st.noiseFloorValid = true;
    }

    if (st.sweepRms > 1.0e-9f && st.noiseFloorValid && st.noiseFloor > 1.0e-9f)
    {
        st.snrValid = true;
        st.snrDb    = 20.0f * std::log10 (st.sweepRms / st.noiseFloor);
    }
    return st;
}

KernelStats analyzeKernel (const std::vector<float>& k, int guard)
{
    KernelStats s;
    if (k.empty())
        return s;

    const int n = (int) k.size();
    for (int i = 0; i < n; ++i)
        if (std::abs (k[(size_t) i]) > std::abs (s.peakValue))
        {
            s.peakValue = k[(size_t) i];
            s.peakIndex = i;
        }

    if (std::abs (s.peakValue) < 1.0e-9f)
        return s; // all-zero kernel -> sharpness stays 0

    double acc = 0.0;
    int    cnt = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs (i - s.peakIndex) > guard)
        {
            acc += (double) k[(size_t) i] * (double) k[(size_t) i];
            ++cnt;
        }

    const float outside = cnt > 0 ? (float) std::sqrt (acc / cnt) : 0.0f;
    s.sharpnessDb = 20.0f * std::log10 (std::abs (s.peakValue) / std::max (outside, 1.0e-9f));
    return s;
}

} // namespace statebox::capture
