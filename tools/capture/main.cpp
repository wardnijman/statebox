// Statebox Capture Tool — CLI entry point.
//
// Pipeline so far: exponential sweep synthesis, FFT deconvolution, and harmonic
// separation (core/). Next increments add sub-sample alignment, normalization, and
// the .dcpack writer. See CLAUDE.md §6 and §10.

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "core/SweepSynth.h"
#include "core/Deconvolution.h"
#include "core/HarmonicSeparation.h"

int main()
{
    using namespace statebox::capture;

    SweepSpec  spec;
    SweepSynth synth (spec);

    std::cout << "Statebox Capture Tool\n"
              << "  sweep : " << synth.length() << " samples @ " << spec.sampleRate
              << " Hz (" << spec.startHz << "-" << spec.endHz << " Hz, "
              << spec.durationSeconds << " s)\n";

    // Demo: push the sweep through a known polynomial nonlinearity and recover the
    // linear IR + measured h2/h3, then print the relative harmonic levels.
    const float a2 = 0.25f, a3 = 0.1f;
    std::vector<float> y (synth.sweep().size());
    for (size_t i = 0; i < y.size(); ++i)
    {
        const float s = synth.sweep()[i];
        y[i] = s + a2 * s * s + a3 * s * s * s;
    }

    const auto decon   = deconvolve (y, synth.inverseFilter(), synth.length());
    const auto kernels = separateHarmonics (decon, synth, 3, 4096);

    const auto peak = [] (const std::vector<float>& v)
    {
        float m = 0.0f;
        for (const float s : v) m = std::max (m, std::abs (s));
        return m;
    };

    const float h1 = peak (kernels[0]);
    std::cout << "  demo  : y = x + " << a2 << "x^2 + " << a3 << "x^3\n"
              << "    recovered  h2/h1 = " << (peak (kernels[1]) / h1)
              << ",  h3/h1 = " << (peak (kernels[2]) / h1) << "\n";

    return 0;
}
