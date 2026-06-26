#include "dsp/Convolver.h"

#include <algorithm>

namespace statebox
{

static int nextPow2Order (int n)
{
    int order = 0;
    while ((1 << order) < n)
        ++order;
    return order;
}

void Convolver::prepare (const std::vector<float>& kernel, int maxBlockSize)
{
    kernelLen = (int) kernel.size();
    maxBlock  = std::max (1, maxBlockSize);

    const int order = nextPow2Order (maxBlock + kernelLen - 1);
    fftSize = 1 << order;
    fft     = std::make_unique<juce::dsp::FFT> (order);

    kernelSpectrum.assign ((size_t) fftSize, C { 0.0f, 0.0f });
    in_  .assign ((size_t) fftSize, C { 0.0f, 0.0f });
    freq .assign ((size_t) fftSize, C { 0.0f, 0.0f });
    prod .assign ((size_t) fftSize, C { 0.0f, 0.0f });
    time_.assign ((size_t) fftSize, C { 0.0f, 0.0f });

    // Precompute the kernel spectrum (zero-padded).
    std::vector<C> k ((size_t) fftSize, C { 0.0f, 0.0f });
    for (int i = 0; i < kernelLen; ++i)
        k[(size_t) i] = C { kernel[(size_t) i], 0.0f };
    fft->perform (k.data(), kernelSpectrum.data(), false);

    overlap    .assign ((size_t) std::max (0, kernelLen - 1), 0.0f);
    overlapNext.assign ((size_t) std::max (0, kernelLen - 1), 0.0f);
}

void Convolver::reset() noexcept
{
    std::fill (overlap.begin(), overlap.end(), 0.0f);
    std::fill (overlapNext.begin(), overlapNext.end(), 0.0f);
}

void Convolver::process (const float* in, float* out, int n) noexcept
{
    const int L = kernelLen;

    std::fill (in_.begin(), in_.end(), C { 0.0f, 0.0f });
    for (int i = 0; i < n; ++i)
        in_[(size_t) i] = C { in[i], 0.0f };

    fft->perform (in_.data(), freq.data(), false);
    for (int i = 0; i < fftSize; ++i)
        prod[(size_t) i] = freq[(size_t) i] * kernelSpectrum[(size_t) i];
    fft->perform (prod.data(), time_.data(), true); // normalized inverse

    // Overlap-add: add the retained tail from previous blocks.
    for (int i = 0; i < n; ++i)
    {
        const float o = (i < L - 1) ? overlap[(size_t) i] : 0.0f;
        out[i] = time_[(size_t) i].real() + o;
    }

    // Build the new tail: convolution beyond the n output samples, plus any old
    // overlap not yet consumed (when n < L-1).
    for (int i = 0; i < L - 1; ++i)
    {
        const float carry = ((n + i) < (L - 1)) ? overlap[(size_t) (n + i)] : 0.0f;
        overlapNext[(size_t) i] = time_[(size_t) (n + i)].real() + carry;
    }
    overlap.swap (overlapNext);
}

} // namespace statebox
