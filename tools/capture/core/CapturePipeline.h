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

} // namespace statebox::capture
