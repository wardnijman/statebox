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
    float peak           = 0.0f; // full record
    float sweepRms       = 0.0f; // measured over the latency-shifted sweep return
    float noiseFloor     = 0.0f; // genuinely-silent region after the return ends
    int   latencySamples = 0;    // detected onset = round-trip latency of the return
    bool  clipping       = false; // peak >= ~0 dBFS
    bool  silent         = false; // peak below ~-80 dBFS (likely no signal / no mic permission)
    bool  noiseFloorValid = false; // false if the tail is too short to contain clean silence
    bool  snrValid       = false;
    float snrDb          = 0.0f;
};

// Analyze a captured return. `tailLen` = nominal trailing silence in the recording.
// Latency-aware: detects where the sweep return actually begins (round-trip latency),
// measures the sweep over [onset, onset+sweepLen], and the noise floor only over the
// silence *after* the return ends — so the SNR isn't polluted by the sweep bleeding
// past the nominal boundary (see capture readout). noiseFloorValid is false when the
// tail is too short to hold any clean silence (increase --settle).
CaptureStats analyzeRecording (const std::vector<float>& recording, int tailLen);

// Per-kernel quality: peak position/value and impulse "sharpness" = peak vs the RMS
// outside a guard around it. A wire loopback deconvolves to a near-delta -> very high
// sharpness; a real IR has spread -> lower. Used by the `inspect` command.
struct KernelStats
{
    int   peakIndex   = 0;
    float peakValue   = 0.0f; // signed
    float sharpnessDb = 0.0f; // 20*log10(|peak| / rms-outside-guard)
};

KernelStats analyzeKernel (const std::vector<float>& kernel, int guard = 16);

} // namespace statebox::capture
