#pragma once

#include <vector>

#include "SweepSynth.h"

namespace statebox::capture
{

struct LiveCaptureConfig
{
    SweepSpec sweep;
    double    conditioningSeconds = 2.0;   // sine tone to set the unit's operating state
    double    settleSeconds       = 0.5;   // silence before the sweep, and tail after it
    int       repetitions         = 5;
    double    conditioningHz      = 1000.0;
    float     conditioningLevel   = 0.25f; // operating-level tone (~ -12 dBFS)
    float     sweepLevel          = 1.0f;  // sweep amplitude
};

// Hardware-agnostic capture sequencer. Drive processBlock() from an audio callback
// (real device) or a virtual loopback (tests). Per repetition it plays:
//   [conditioning tone][settle silence][sweep][tail silence]
// recording the input over [sweep .. sweep+tail]. After all reps, finalize()
// combines the recordings into a mean (used for kernels) and a per-sample std
// (the "where is the unit alive" map, CLAUDE.md §6). No allocation in processBlock.
class LiveCaptureEngine
{
public:
    explicit LiveCaptureEngine (const LiveCaptureConfig& cfg);

    // Real-time safe: reads `input` (may be null), writes mono `output`.
    void processBlock (const float* input, float* output, int numSamples) noexcept;

    bool isFinished()           const noexcept { return finished_; }
    int  currentRepetition()    const noexcept { return rep_; }
    int  recordLengthSamples()  const noexcept { return recordLen_; }
    int  sweepLengthSamples()   const noexcept { return sweepLen_; }
    int  tailLengthSamples()    const noexcept { return tail_; }   // post-sweep silence = noise-floor window
    int  totalRepetitions()     const noexcept { return cfg_.repetitions; }

    void finalize();
    const std::vector<float>&              meanRecording() const noexcept { return mean_; }
    const std::vector<float>&              stdRecording()  const noexcept { return std_; }
    const std::vector<std::vector<float>>& rawRecordings() const noexcept { return raw_; }

private:
    LiveCaptureConfig cfg_;
    SweepSynth        synth_;

    int sweepLen_    = 0;
    int cond_        = 0;
    int settle_      = 0;
    int tail_        = 0;
    int recordStart_ = 0;
    int recordLen_   = 0;
    int repLen_      = 0;

    int    i_        = 0;     // sample index within the current repetition
    int    rep_      = 0;     // current repetition
    bool   finished_ = false;
    double phase_    = 0.0;   // conditioning oscillator phase

    std::vector<std::vector<float>> raw_;  // [rep][recordLen]
    std::vector<float>              mean_;
    std::vector<float>              std_;
};

} // namespace statebox::capture
