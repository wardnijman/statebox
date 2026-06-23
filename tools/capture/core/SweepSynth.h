#pragma once

#include <vector>

namespace statebox::capture
{

// Parameters for an exponential (logarithmic) sine sweep. Defaults match the
// capture conventions in CLAUDE.md §6.
struct SweepSpec
{
    double sampleRate      = 48000.0;
    double startHz         = 20.0;
    double endHz           = 20000.0;
    double durationSeconds = 3.0;
};

// Generates an exponential sine sweep (Farina / synchronized swept-sine) and the
// matching inverse filter. Convolving the recorded system output with the inverse
// filter yields the system's impulse response (linear part at t=0; harmonic
// products fall at earlier, known offsets — see harmonicOffsetSamples()).
//
// This type is JUCE-free on purpose so it can be reused anywhere.
class SweepSynth
{
public:
    explicit SweepSynth (const SweepSpec& spec);

    const std::vector<float>& sweep()         const noexcept { return sweep_; }
    const std::vector<float>& inverseFilter() const noexcept { return inverse_; }
    int                        length()        const noexcept { return (int) sweep_.size(); }
    const SweepSpec&           spec()          const noexcept { return spec_; }

    // After deconvolution the n-th harmonic IR appears this many samples *before*
    // the linear-IR position (n >= 2). dt_n = L * ln(n), with L = N / ln(f2/f1).
    double harmonicOffsetSamples (int harmonic) const noexcept;

private:
    SweepSpec          spec_;
    std::vector<float> sweep_;
    std::vector<float> inverse_;
};

} // namespace statebox::capture
