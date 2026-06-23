#pragma once

#include <vector>

namespace statebox::capture
{

// Linear convolution of two real signals via FFT (zero-padded to a power of two).
std::vector<float> linearConvolveFFT (const std::vector<float>& a,
                                      const std::vector<float>& b);

struct DeconvolutionResult
{
    std::vector<float> full;          // recording * inverseFilter (full convolution)
    int                linearIrIndex; // index of the linear IR's t=0 within `full`
};

// Deconvolve a recording with a sweep's inverse filter. The linear impulse
// response begins at index (sweepLength - 1); harmonic IRs precede it.
DeconvolutionResult deconvolve (const std::vector<float>& recording,
                                const std::vector<float>& inverseFilter,
                                int sweepLength);

// Copy `length` samples starting at `startIndex` (out-of-range -> zero).
std::vector<float> extractKernel (const std::vector<float>& full,
                                  int startIndex, int length);

} // namespace statebox::capture
