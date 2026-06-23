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

CaptureStats analyzeRecording (const std::vector<float>& rec, int tailLen)
{
    CaptureStats st;
    if (rec.empty())
        return st;

    const int n      = (int) rec.size();
    const int tail   = std::min (std::max (0, tailLen), n);
    const int sweepN = n - tail;

    for (const float v : rec)
        st.peak = std::max (st.peak, std::abs (v));

    const auto rms = [] (const float* p, int count) -> float
    {
        if (count <= 0) return 0.0f;
        double acc = 0.0;
        for (int i = 0; i < count; ++i) acc += (double) p[i] * (double) p[i];
        return (float) std::sqrt (acc / count);
    };

    st.sweepRms   = rms (rec.data(), sweepN);
    st.noiseFloor = rms (rec.data() + sweepN, tail);
    st.clipping   = st.peak >= 0.99f;
    st.silent     = st.peak < 1.0e-4f;

    if (st.sweepRms > 1.0e-9f && st.noiseFloor > 1.0e-9f)
    {
        st.snrValid = true;
        st.snrDb    = 20.0f * std::log10 (st.sweepRms / st.noiseFloor);
    }
    return st;
}

} // namespace statebox::capture
