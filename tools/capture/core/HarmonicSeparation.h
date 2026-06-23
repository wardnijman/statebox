#pragma once

#include <vector>

#include "Deconvolution.h"

namespace statebox::capture
{

class SweepSynth;

// Index within decon.full where the given harmonic's IR is centred.
// harmonic 1 = linear IR (h1); 2,3,... = higher-order IRs (earlier in time, the
// Farina ln(m) offsets). These higher IRs are the measured-Hammerstein kernels.
int harmonicIrIndex (const DeconvolutionResult& decon,
                     const SweepSynth& synth,
                     int harmonic);

// Extract per-harmonic IR windows: result[0] = linear (h1), [1] = h2, [2] = h3 ...
// Each window is `kernelLength` samples centred on its harmonic position. Window
// length must be smaller than the spacing between adjacent harmonics (true for the
// default sweep with maxHarmonic <= a few and kernelLength <= 4096).
std::vector<std::vector<float>> separateHarmonics (const DeconvolutionResult& decon,
                                                   const SweepSynth& synth,
                                                   int maxHarmonic,
                                                   int kernelLength);

} // namespace statebox::capture
