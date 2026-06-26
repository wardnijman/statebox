#pragma once

#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>

namespace statebox
{

// Single-kernel FFT overlap-add convolver. prepare() with a fixed kernel and the
// maximum block size; process() is then allocation-free (all scratch preallocated).
// Used as one voice in the engine's warm pool (CLAUDE.md §5.1).
class Convolver
{
public:
    void prepare (const std::vector<float>& kernel, int maxBlockSize);
    void reset() noexcept;

    // Convolve n (<= maxBlockSize) input samples into out. [RT-safe after prepare]
    void process (const float* in, float* out, int n) noexcept;

    int kernelLength() const noexcept { return kernelLen; }

private:
    using C = juce::dsp::Complex<float>;

    std::unique_ptr<juce::dsp::FFT> fft;
    int fftSize   = 0;
    int kernelLen = 0;
    int maxBlock  = 0;

    std::vector<C> kernelSpectrum; // Kf
    std::vector<C> in_, freq, prod, time_;
    std::vector<float> overlap, overlapNext;
};

} // namespace statebox
