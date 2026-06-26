#pragma once

#include <algorithm>
#include <cmath>

namespace statebox
{

// Minimal Milestone-0 state estimator: a one-pole peak envelope follower that maps
// the input level onto the profile's level axis. (The full estimator — crest,
// transient, recovery, hysteresis per CLAUDE.md §5.3 — comes once the core is
// validated.) Returns a fractional position in [0, numLevels-1].
class StateEstimator
{
public:
    void prepare (double sampleRate, float attackMs, float releaseMs,
                  float levelMinDb, float levelMaxDb, int numLevels) noexcept
    {
        minDb   = levelMinDb;
        maxDb   = levelMaxDb;
        levels  = std::max (1, numLevels);
        attCoef = coef (attackMs,  sampleRate);
        relCoef = coef (releaseMs, sampleRate);
        env     = 0.0f;
    }

    void reset() noexcept { env = 0.0f; }

    // Advance the envelope over a block and return the level-axis position.
    float process (const float* in, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = std::abs (in[i]);
            const float c = (x > env) ? attCoef : relCoef;
            env += c * (x - env);
        }

        const float db  = 20.0f * std::log10 (std::max (env, 1.0e-9f));
        const float t   = (maxDb > minDb) ? (db - minDb) / (maxDb - minDb) : 0.0f;
        return std::clamp (t, 0.0f, 1.0f) * (float) (levels - 1);
    }

private:
    static float coef (float ms, double sr)
    {
        if (ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp (-1.0f / ((float) sr * 0.001f * ms));
    }

    float minDb = -24.0f, maxDb = -12.0f;
    int   levels = 1;
    float attCoef = 1.0f, relCoef = 1.0f, env = 0.0f;
};

} // namespace statebox
