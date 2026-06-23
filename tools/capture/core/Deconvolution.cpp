#include "Deconvolution.h"

#include <juce_dsp/juce_dsp.h>

namespace statebox::capture
{

static int nextPow2Order (int n)
{
    int order = 0;
    while ((1 << order) < n)
        ++order;
    return order;
}

std::vector<float> linearConvolveFFT (const std::vector<float>& a,
                                      const std::vector<float>& b)
{
    const int convLen = (int) a.size() + (int) b.size() - 1;
    const int order   = nextPow2Order (convLen);
    const int fftSize = 1 << order;

    juce::dsp::FFT fft (order);
    using C = juce::dsp::Complex<float>;

    std::vector<C> A ((size_t) fftSize, C { 0.0f, 0.0f });
    std::vector<C> B ((size_t) fftSize, C { 0.0f, 0.0f });
    std::vector<C> Af ((size_t) fftSize);
    std::vector<C> Bf ((size_t) fftSize);

    for (size_t i = 0; i < a.size(); ++i) A[i] = C { a[i], 0.0f };
    for (size_t i = 0; i < b.size(); ++i) B[i] = C { b[i], 0.0f };

    fft.perform (A.data(), Af.data(), false);
    fft.perform (B.data(), Bf.data(), false);

    std::vector<C> product ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        product[(size_t) i] = Af[(size_t) i] * Bf[(size_t) i];

    std::vector<C> out ((size_t) fftSize);
    fft.perform (product.data(), out.data(), true);

    // perform()'s inverse transform is unnormalised; scale by 1/fftSize so the
    // result equals a true linear convolution.
    const float scale = 1.0f / (float) fftSize;
    std::vector<float> result ((size_t) convLen);
    for (int i = 0; i < convLen; ++i)
        result[(size_t) i] = out[(size_t) i].real() * scale;

    return result;
}

DeconvolutionResult deconvolve (const std::vector<float>& recording,
                                const std::vector<float>& inverseFilter,
                                int sweepLength)
{
    DeconvolutionResult r;
    r.full          = linearConvolveFFT (recording, inverseFilter);
    r.linearIrIndex = sweepLength - 1;
    return r;
}

std::vector<float> extractKernel (const std::vector<float>& full,
                                  int startIndex, int length)
{
    std::vector<float> k ((size_t) length, 0.0f);
    for (int i = 0; i < length; ++i)
    {
        const int idx = startIndex + i;
        if (idx >= 0 && idx < (int) full.size())
            k[(size_t) i] = full[(size_t) idx];
    }
    return k;
}

} // namespace statebox::capture
