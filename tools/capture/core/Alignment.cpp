#include "Alignment.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

namespace statebox::capture
{

double estimatePeakPosition (const std::vector<float>& ir)
{
    if (ir.empty())
        return 0.0;

    int   k    = 0;
    float best = -1.0f;
    for (int i = 0; i < (int) ir.size(); ++i)
    {
        const float a = std::abs (ir[(size_t) i]);
        if (a > best) { best = a; k = i; }
    }

    if (k <= 0 || k >= (int) ir.size() - 1)
        return (double) k;

    const float ym1 = std::abs (ir[(size_t) (k - 1)]);
    const float y0  = std::abs (ir[(size_t) k]);
    const float yp1 = std::abs (ir[(size_t) (k + 1)]);

    const float denom = ym1 - 2.0f * y0 + yp1;
    double delta = 0.0;
    if (std::abs (denom) > 1.0e-20f)
        delta = 0.5 * (double) ((ym1 - yp1) / denom);

    delta = std::max (-0.5, std::min (0.5, delta));
    return (double) k + delta;
}

static int nextPow2Order (int n)
{
    int order = 0;
    while ((1 << order) < n)
        ++order;
    return order;
}

static std::vector<float> integerShift (const std::vector<float>& v, int s)
{
    std::vector<float> r (v.size(), 0.0f);
    for (int i = 0; i < (int) v.size(); ++i)
    {
        const int j = i + s; // positive s = delay
        if (j >= 0 && j < (int) v.size())
            r[(size_t) j] = v[(size_t) i];
    }
    return r;
}

std::vector<float> fractionalShift (const std::vector<float>& ir, double deltaSamples)
{
    const int M = (int) ir.size();
    if (M == 0)
        return {};

    const int order   = nextPow2Order (M);
    const int fftSize = 1 << order;
    const int half    = fftSize / 2;

    juce::dsp::FFT fft (order);
    using C = juce::dsp::Complex<float>;

    std::vector<C> in   ((size_t) fftSize, C { 0.0f, 0.0f });
    std::vector<C> spec ((size_t) fftSize);
    std::vector<C> out  ((size_t) fftSize);
    for (int i = 0; i < M; ++i)
        in[(size_t) i] = C { ir[(size_t) i], 0.0f };

    fft.perform (in.data(), spec.data(), false);

    const double twoPi = 2.0 * M_PI;
    for (int k = 0; k <= half; ++k)
    {
        const double w  = twoPi * (double) k / (double) fftSize;
        const double ph = -w * deltaSamples;

        // Force the Nyquist bin real so the inverse transform stays real-valued.
        const C mult = (k == half) ? C { (float) std::cos (ph), 0.0f }
                                   : C { (float) std::cos (ph), (float) std::sin (ph) };

        spec[(size_t) k] *= mult;
        if (k != 0 && k != half)
            spec[(size_t) (fftSize - k)] *= std::conj (mult);
    }

    fft.perform (spec.data(), out.data(), true);

    // perform() normalises the inverse transform (1/N), so no extra scaling.
    std::vector<float> result ((size_t) M);
    for (int i = 0; i < M; ++i)
        result[(size_t) i] = out[(size_t) i].real();

    return result;
}

std::vector<float> alignPeakTo (const std::vector<float>& ir, double targetIndex)
{
    const double delta   = targetIndex - estimatePeakPosition (ir);
    const int    intPart = (int) std::lround (delta);
    const double frac    = delta - (double) intPart;

    return fractionalShift (integerShift (ir, intPart), frac);
}

} // namespace statebox::capture
