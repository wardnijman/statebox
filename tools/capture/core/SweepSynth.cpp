#include "SweepSynth.h"

#include <cmath>

namespace statebox::capture
{

SweepSynth::SweepSynth (const SweepSpec& spec)
    : spec_ (spec)
{
    const double fs = spec_.sampleRate;
    const int    N  = (int) std::llround (spec_.durationSeconds * fs);
    const double R  = std::log (spec_.endHz / spec_.startHz); // ln(f2/f1)
    const double L  = (double) N / R;                          // sweep rate constant, in samples

    sweep_.resize ((size_t) N);
    inverse_.resize ((size_t) N);

    // x[n] = sin( 2*pi*f1 * (L/fs) * (exp(n/L) - 1) )
    // Instantaneous frequency sweeps f1 -> f2 over the duration.
    const double k = 2.0 * M_PI * spec_.startHz * (L / fs);
    for (int n = 0; n < N; ++n)
    {
        const double phase = k * (std::exp ((double) n / L) - 1.0);
        sweep_[(size_t) n] = (float) std::sin (phase);
    }

    // Inverse filter: time-reversed sweep with a decaying amplitude envelope that
    // whitens the sweep's 1/f energy distribution (Farina). exp(n*R/N) == exp(n/L).
    for (int n = 0; n < N; ++n)
    {
        const double env = std::exp ((double) n * R / (double) N);
        inverse_[(size_t) n] = sweep_[(size_t) (N - 1 - n)] / (float) env;
    }
}

double SweepSynth::harmonicOffsetSamples (int harmonic) const noexcept
{
    const double N = (double) sweep_.size();
    const double R = std::log (spec_.endHz / spec_.startHz);
    const double L = N / R;
    return L * std::log ((double) harmonic);
}

} // namespace statebox::capture
