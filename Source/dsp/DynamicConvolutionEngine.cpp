#include "dsp/DynamicConvolutionEngine.h"

#include <algorithm>
#include <cmath>

namespace statebox
{

void DynamicConvolutionEngine::prepare (const RuntimeProfile& profile, int maxBlockSize, int channel)
{
    L        = profile.numLevels;
    K        = profile.numKnobs;
    maxBlock = std::max (1, maxBlockSize);

    convs.clear();
    convs.resize ((size_t) (L * K));

    for (int l = 0; l < L; ++l)
        for (int k = 0; k < K; ++k)
        {
            if (const auto* ker = profile.linearKernel (l, k, channel))
                at (l, k).prepare (*ker, maxBlock);
            else
                at (l, k).prepare (std::vector<float> ((size_t) std::max (1, profile.kernelLength), 0.0f), maxBlock);
        }

    scratch.assign ((size_t) maxBlock, 0.0f);
    accum  .assign ((size_t) maxBlock, 0.0f);
    levelPos = knobPos = 0.0f;
}

void DynamicConvolutionEngine::setState (float lp, float kp) noexcept
{
    levelPos = std::clamp (lp, 0.0f, (float) std::max (0, L - 1));
    knobPos  = std::clamp (kp, 0.0f, (float) std::max (0, K - 1));
}

float DynamicConvolutionEngine::weightFor (int l, int k) const noexcept
{
    const auto axisW = [] (float pos, int idx, int n) -> float
    {
        int i0 = std::clamp ((int) std::floor (pos), 0, n - 1);
        const int i1 = std::min (i0 + 1, n - 1);
        const float f = pos - (float) i0;
        if (i0 == i1) return (idx == i0) ? 1.0f : 0.0f;
        if (idx == i0) return 1.0f - f;
        if (idx == i1) return f;
        return 0.0f;
    };

    return axisW (levelPos, l, L) * axisW (knobPos, k, K);
}

void DynamicConvolutionEngine::process (const float* in, float* out, int n) noexcept
{
    n = std::min (n, maxBlock);
    std::fill (accum.begin(), accum.begin() + n, 0.0f);

    for (int l = 0; l < L; ++l)
        for (int k = 0; k < K; ++k)
        {
            at (l, k).process (in, scratch.data(), n); // always run -> stays warm
            const float w = weightFor (l, k);
            if (w != 0.0f)
                for (int i = 0; i < n; ++i)
                    accum[(size_t) i] += w * scratch[(size_t) i];
        }

    for (int i = 0; i < n; ++i)
        out[i] = accum[(size_t) i];
}

void DynamicConvolutionEngine::reset() noexcept
{
    for (auto& c : convs)
        c.reset();
}

} // namespace statebox
