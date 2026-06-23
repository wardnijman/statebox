#pragma once

#include <string>
#include <vector>

#include "SweepSynth.h"
#include "DcPack.h"

namespace statebox::capture
{

// Process one recorded sweep (the unit's response to synth.sweep()) into measured
// kernels: deconvolve -> separate h1/h2/h3 -> latency + sub-sample align each
// kernel so the whole grid shares a common origin. Harmonic orders captured are
// 2..maxHarmonic. The kernel peaks are aligned to kernelLength/2.
CaptureKernels processRecording (const std::vector<float>& recording,
                                 const SweepSynth& synth,
                                 int maxHarmonic,
                                 int kernelLength,
                                 int levelIndex = 0,
                                 int knobIndex  = 0,
                                 int channel    = 0);

// Wrap a single set of kernels in a one-cell CaptureProfile (single level/knob).
CaptureProfile buildSingleCellProfile (CaptureKernels kernels,
                                       const SweepSpec& spec,
                                       int maxHarmonic,
                                       int kernelLength,
                                       const std::string& name);

// Quality summary of one captured return — drives both the CLI readout and the
// per-cell grid quality gate. Levels are linear amplitudes (0..1); convert to dBFS
// for display.
struct CaptureStats
{
    float peak       = 0.0f; // full record
    float sweepRms   = 0.0f; // sweep portion
    float noiseFloor = 0.0f; // trailing tail (post-sweep silence)
    bool  clipping   = false; // peak >= ~0 dBFS
    bool  silent     = false; // peak below ~-80 dBFS (likely no signal / no mic permission)
    bool  snrValid   = false;
    float snrDb      = 0.0f;
};

// Analyze a captured return. `tailLen` = trailing post-sweep silence (the noise-floor
// window); the rest is treated as the sweep response.
CaptureStats analyzeRecording (const std::vector<float>& recording, int tailLen);

} // namespace statebox::capture
