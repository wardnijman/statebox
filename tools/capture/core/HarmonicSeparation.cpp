#include "HarmonicSeparation.h"

#include "SweepSynth.h"

#include <cmath>

namespace statebox::capture
{

int harmonicIrIndex (const DeconvolutionResult& decon,
                     const SweepSynth& synth,
                     int harmonic)
{
    if (harmonic <= 1)
        return decon.linearIrIndex;

    return decon.linearIrIndex - (int) std::llround (synth.harmonicOffsetSamples (harmonic));
}

std::vector<std::vector<float>> separateHarmonics (const DeconvolutionResult& decon,
                                                   const SweepSynth& synth,
                                                   int maxHarmonic,
                                                   int kernelLength)
{
    // The actual linear-IR peak may be offset from its theoretical index by the
    // recording's round-trip latency; find it so the windows track the real peaks.
    int   actual = decon.linearIrIndex;
    float best   = -1.0f;
    for (int i = 0; i < (int) decon.full.size(); ++i)
    {
        const float a = std::abs (decon.full[(size_t) i]);
        if (a > best) { best = a; actual = i; }
    }
    const int latency = actual - decon.linearIrIndex;

    std::vector<std::vector<float>> kernels;
    kernels.reserve ((size_t) maxHarmonic);

    for (int m = 1; m <= maxHarmonic; ++m)
    {
        const int centre = harmonicIrIndex (decon, synth, m) + latency;
        const int start  = centre - kernelLength / 2;
        kernels.push_back (extractKernel (decon.full, start, kernelLength));
    }

    return kernels;
}

} // namespace statebox::capture
