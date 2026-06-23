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
    std::vector<std::vector<float>> kernels;
    kernels.reserve ((size_t) maxHarmonic);

    for (int m = 1; m <= maxHarmonic; ++m)
    {
        const int centre = harmonicIrIndex (decon, synth, m);
        const int start  = centre - kernelLength / 2;
        kernels.push_back (extractKernel (decon.full, start, kernelLength));
    }

    return kernels;
}

} // namespace statebox::capture
