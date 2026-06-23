#include "LiveCaptureEngine.h"

#include <algorithm>
#include <cmath>

namespace statebox::capture
{

LiveCaptureEngine::LiveCaptureEngine (const LiveCaptureConfig& cfg)
    : cfg_ (cfg), synth_ (cfg.sweep)
{
    const double fs = cfg_.sweep.sampleRate;

    sweepLen_    = synth_.length();
    cond_        = (int) std::llround (cfg_.conditioningSeconds * fs);
    settle_      = (int) std::llround (cfg_.settleSeconds * fs);
    tail_        = settle_;
    recordStart_ = cond_ + settle_;
    recordLen_   = sweepLen_ + tail_;
    repLen_      = cond_ + settle_ + sweepLen_ + tail_;

    const int reps = std::max (0, cfg_.repetitions);
    raw_.assign ((size_t) reps, std::vector<float> ((size_t) recordLen_, 0.0f));

    if (reps == 0)
        finished_ = true;
}

void LiveCaptureEngine::processBlock (const float* input, float* output, int numSamples) noexcept
{
    const double fs    = cfg_.sweep.sampleRate;
    const double condW = 2.0 * M_PI * cfg_.conditioningHz / fs;
    const int    sweepStart = cond_ + settle_;

    for (int s = 0; s < numSamples; ++s)
    {
        float out = 0.0f;

        if (! finished_)
        {
            if (i_ < cond_)
            {
                out = cfg_.conditioningLevel * (float) std::sin (phase_);
                phase_ += condW;
            }
            else if (i_ < sweepStart)
            {
                out = 0.0f; // settle before sweep
            }
            else if (i_ < sweepStart + sweepLen_)
            {
                out = cfg_.sweepLevel * synth_.sweep()[(size_t) (i_ - sweepStart)];
            }
            // else: tail silence

            if (i_ >= recordStart_ && i_ < recordStart_ + recordLen_ && rep_ < (int) raw_.size())
                raw_[(size_t) rep_][(size_t) (i_ - recordStart_)] = (input != nullptr) ? input[s] : 0.0f;

            if (++i_ >= repLen_)
            {
                i_     = 0;
                phase_ = 0.0;
                if (++rep_ >= cfg_.repetitions)
                    finished_ = true;
            }
        }

        output[s] = out;
    }
}

void LiveCaptureEngine::finalize()
{
    const int reps = (int) raw_.size();
    if (reps == 0 || recordLen_ == 0)
        return;

    mean_.assign ((size_t) recordLen_, 0.0f);
    std_.assign ((size_t) recordLen_, 0.0f);

    for (int j = 0; j < recordLen_; ++j)
    {
        double m = 0.0;
        for (int r = 0; r < reps; ++r)
            m += raw_[(size_t) r][(size_t) j];
        m /= reps;

        double v = 0.0;
        for (int r = 0; r < reps; ++r)
        {
            const double d = raw_[(size_t) r][(size_t) j] - m;
            v += d * d;
        }
        v /= reps;

        mean_[(size_t) j] = (float) m;
        std_[(size_t) j]  = (float) std::sqrt (v);
    }
}

} // namespace statebox::capture
