#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

#include "DcPack.h"
#include "capture/RuntimeProfile.h"
#include "dsp/Convolver.h"
#include "dsp/DynamicConvolutionEngine.h"
#include "dsp/StateEstimator.h"

using namespace statebox;

namespace
{
// Direct (time-domain) convolution reference for the null test.
std::vector<float> directConv (const std::vector<float>& x, const std::vector<float>& h)
{
    std::vector<float> y (x.size(), 0.0f);
    for (int n = 0; n < (int) x.size(); ++n)
    {
        float acc = 0.0f;
        for (int k = 0; k < (int) h.size(); ++k)
            if (n - k >= 0)
                acc += h[(size_t) k] * x[(size_t) (n - k)];
        y[(size_t) n] = acc;
    }
    return y;
}

float maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    float m = 0.0f;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        m = juce::jmax (m, std::abs (a[i] - b[i]));
    return m;
}

// A tiny in-memory profile: 2 levels x 2 knobs, short kernels, for engine tests.
RuntimeProfile makeProfile (const std::vector<std::vector<float>>& kernels, int kernelLen)
{
    RuntimeProfile rp;
    rp.profile.kernelLengthSamples = kernelLen;
    rp.profile.sampleRate = 48000.0;
    rp.profile.levelAxis = capture::ProfileAxis { "level", "dBFS-at-output", { -18.0f, -6.0f } };
    rp.profile.knobAxis  = capture::ProfileAxis { "knob", "normalized", { 0.0f, 1.0f } };
    int idx = 0;
    for (int l = 0; l < 2; ++l)
        for (int k = 0; k < 2; ++k)
        {
            capture::CaptureKernels c;
            c.levelIndex = l; c.knobIndex = k; c.channel = 0;
            c.linear = kernels[(size_t) idx++];
            rp.profile.cells.push_back (c);
        }
    rp.numLevels = 2; rp.numKnobs = 2; rp.kernelLength = kernelLen; rp.sampleRate = 48000.0;
    return rp;
}
} // namespace

class ConvolverTests : public juce::UnitTest
{
public:
    ConvolverTests() : juce::UnitTest ("Convolver") {}

    void runTest() override
    {
        beginTest ("FFT overlap-add equals direct convolution across block boundaries");
        {
            std::vector<float> h (37);
            for (int i = 0; i < (int) h.size(); ++i) h[(size_t) i] = std::sin (0.3f * i) * (1.0f - i / 40.0f);

            std::vector<float> x (1000);
            for (int i = 0; i < (int) x.size(); ++i) x[(size_t) i] = std::sin (0.07f * i) + 0.5f * std::sin (0.21f * i);

            const auto ref = directConv (x, h);

            Convolver conv;
            const int block = 64;
            conv.prepare (h, block);

            std::vector<float> out (x.size(), 0.0f);
            for (int s = 0; s < (int) x.size(); s += block)
            {
                const int n = std::min (block, (int) x.size() - s);
                conv.process (x.data() + s, out.data() + s, n);
            }
            expectLessThan (maxAbsDiff (ref, out), 1.0e-4f, "overlap-add must match direct convolution");
        }

        beginTest ("ragged final block (n < block) still matches");
        {
            std::vector<float> h { 1.0f, -0.5f, 0.25f };
            std::vector<float> x (130);
            for (int i = 0; i < (int) x.size(); ++i) x[(size_t) i] = (float) ((i * 7) % 11) / 11.0f - 0.5f;
            const auto ref = directConv (x, h);

            Convolver conv; conv.prepare (h, 50);
            std::vector<float> out (x.size(), 0.0f);
            for (int s = 0; s < (int) x.size(); s += 50)
            {
                const int n = std::min (50, (int) x.size() - s);
                conv.process (x.data() + s, out.data() + s, n);
            }
            expectLessThan (maxAbsDiff (ref, out), 1.0e-4f, "ragged tail must match");
        }
    }
};

class EngineTests : public juce::UnitTest
{
public:
    EngineTests() : juce::UnitTest ("Dynamic convolution engine") {}

    void runTest() override
    {
        const int kl = 16;

        beginTest ("null test: at a grid corner the engine equals plain convolution");
        {
            std::vector<float> h (kl, 0.0f);
            h[0] = 1.0f; h[1] = -0.4f; h[3] = 0.2f; // arbitrary IR at this corner
            std::vector<std::vector<float>> ks (4, std::vector<float> (kl, 0.0f));
            ks[0] = h; // L0K0; others zero

            auto rp = makeProfile (ks, kl);
            DynamicConvolutionEngine eng; eng.prepare (rp, 64);
            eng.setState (0.0f, 0.0f); // exactly on L0K0 -> weight 1 there, 0 elsewhere

            std::vector<float> x (200);
            for (int i = 0; i < (int) x.size(); ++i) x[(size_t) i] = std::sin (0.11f * i);
            std::vector<float> out (x.size(), 0.0f);
            for (int s = 0; s < (int) x.size(); s += 64)
            {
                const int n = std::min (64, (int) x.size() - s);
                eng.process (x.data() + s, out.data() + s, n);
            }
            expectLessThan (maxAbsDiff (directConv (x, h), out), 1.0e-4f,
                            "engine at a corner == convolution with that corner's kernel");
        }

        beginTest ("interpolating identical kernels is a no-op (no comb filtering)");
        {
            std::vector<float> h (kl, 0.0f);
            h[2] = 1.0f; h[5] = -0.3f; // same IR in all four cells
            std::vector<std::vector<float>> ks (4, h);

            auto rp = makeProfile (ks, kl);
            DynamicConvolutionEngine eng; eng.prepare (rp, 64);
            eng.setState (0.5f, 0.5f); // dead centre -> equal weights on all four

            std::vector<float> x (200);
            for (int i = 0; i < (int) x.size(); ++i) x[(size_t) i] = std::sin (0.13f * i) + 0.3f * std::cos (0.05f * i);
            std::vector<float> out (x.size(), 0.0f);
            for (int s = 0; s < (int) x.size(); s += 64)
            {
                const int n = std::min (64, (int) x.size() - s);
                eng.process (x.data() + s, out.data() + s, n);
            }
            // Equal blend of identical kernels must equal that single kernel.
            expectLessThan (maxAbsDiff (directConv (x, h), out), 1.0e-4f,
                            "blend of identical kernels must equal the kernel");
        }

        beginTest ("interpolation weight crossing is continuous (bilinear)");
        {
            // Two different kernels along the knob axis; at the midpoint the output is
            // the average of the two single-kernel outputs.
            std::vector<float> hA (kl, 0.0f), hB (kl, 0.0f);
            hA[0] = 1.0f; hB[4] = 1.0f;
            std::vector<std::vector<float>> ks { hA, hB, hA, hB }; // K0=hA, K1=hB on both levels

            auto rp = makeProfile (ks, kl);
            DynamicConvolutionEngine eng; eng.prepare (rp, 64);

            std::vector<float> x (128);
            for (int i = 0; i < (int) x.size(); ++i) x[(size_t) i] = std::sin (0.2f * i);

            auto render = [&] (float knobPos)
            {
                eng.reset(); eng.setState (0.0f, knobPos);
                std::vector<float> o (x.size(), 0.0f);
                for (int s = 0; s < (int) x.size(); s += 64)
                {
                    const int n = std::min (64, (int) x.size() - s);
                    eng.process (x.data() + s, o.data() + s, n);
                }
                return o;
            };

            const auto mid = render (0.5f);
            const auto a   = directConv (x, hA);
            const auto b   = directConv (x, hB);
            std::vector<float> avg (x.size());
            for (size_t i = 0; i < avg.size(); ++i) avg[i] = 0.5f * (a[i] + b[i]);

            expectLessThan (maxAbsDiff (mid, avg), 1.0e-4f, "midpoint == average of the two kernels");
        }
    }
};

class StateEstimatorTests : public juce::UnitTest
{
public:
    StateEstimatorTests() : juce::UnitTest ("State estimator") {}

    void runTest() override
    {
        beginTest ("louder input maps to a higher level-axis position");
        {
            StateEstimator s;
            s.prepare (48000.0, 1.0f, 1.0f, -24.0f, -12.0f, 3); // axis 0..2

            std::vector<float> quiet (4800, 0.06f);  // ~ -24 dBFS
            std::vector<float> loud  (4800, 0.25f);  // ~ -12 dBFS

            const float pQuiet = s.process (quiet.data(), (int) quiet.size());
            s.reset();
            const float pLoud  = s.process (loud.data(), (int) loud.size());

            expectGreaterThan (pLoud, pQuiet, "louder -> higher position");
            expect (pLoud <= 2.0f && pQuiet >= 0.0f, "position stays within axis bounds");
        }
    }
};

static ConvolverTests      convolverTests;
static EngineTests         engineTests;
static StateEstimatorTests stateEstimatorTests;

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    if (failures > 0) { std::cerr << failures << " test failure(s)\n"; return 1; }
    std::cout << "All engine tests passed.\n";
    return 0;
}
