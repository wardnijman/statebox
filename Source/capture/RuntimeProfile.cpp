#include "capture/RuntimeProfile.h"

#include <cmath>
#include <vector>

#include <juce_dsp/juce_dsp.h>

namespace statebox
{

void RuntimeProfile::normalizeMaxGain()
{
    if (profile.cells.empty())
        return;

    const int len = std::max (1, kernelLength);
    int order = 0;
    while ((1 << order) < len) ++order;
    const int n = 1 << order;

    juce::dsp::FFT fft (order);
    using C = juce::dsp::Complex<float>;
    std::vector<C> in ((size_t) n), out ((size_t) n);

    for (auto& c : profile.cells)
    {
        std::fill (in.begin(), in.end(), C { 0.0f, 0.0f });
        for (int i = 0; i < (int) c.linear.size() && i < n; ++i)
            in[(size_t) i] = C { c.linear[(size_t) i], 0.0f };

        fft.perform (in.data(), out.data(), false);

        float maxMag = 0.0f;
        for (int i = 0; i <= n / 2; ++i) // DC..Nyquist
            maxMag = std::max (maxMag, std::abs (out[(size_t) i]));

        if (maxMag > 1.0e-9f)
        {
            const float g = 1.0f / maxMag;
            for (auto& s : c.linear) s *= g;
        }
    }
}

} // namespace statebox
