#pragma once

#include <vector>

#include "capture/RuntimeProfile.h"
#include "dsp/Convolver.h"

namespace statebox
{

// The dynamic convolution core: a warm pool with one convolver per grid cell, all
// kept running (CLAUDE.md §5.1). Each block, bilinear interpolation weights are
// computed from the (level, knob) state and the convolver outputs are summed in the
// output domain. Modulating the weights is what produces the non-LTI movement.
class DynamicConvolutionEngine
{
public:
    void prepare (const RuntimeProfile& profile, int maxBlockSize, int channel = 0);

    // State positions are in axis-index space: level in [0, numLevels-1],
    // knob in [0, numKnobs-1]. [RT-safe]
    void setState (float levelPos, float knobPos) noexcept;

    void process (const float* in, float* out, int n) noexcept;
    void reset() noexcept;

    int numLevels() const noexcept { return L; }
    int numKnobs()  const noexcept { return K; }

private:
    Convolver&       at (int l, int k) noexcept { return convs[(size_t) (l * K + k)]; }
    float weightFor (int l, int k) const noexcept;

    int L = 0, K = 0, maxBlock = 0;
    float levelPos = 0.0f, knobPos = 0.0f;

    std::vector<Convolver> convs;  // L*K, kept warm
    std::vector<float>     scratch; // one convolver's output
    std::vector<float>     accum;   // weighted sum
};

} // namespace statebox
