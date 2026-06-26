#pragma once

#include <string>
#include <vector>

#include "DcPack.h" // statebox::capture format reader (tools/capture/core)

namespace statebox
{

// Immutable runtime view of a loaded .dcpack: the measured kernels indexed by
// (level, knob), plus the axis values used for interpolation. Built off the audio
// thread and handed to the engine read-only (CLAUDE.md §4 profile swap).
struct RuntimeProfile
{
    capture::CaptureProfile profile; // owns the kernel data

    int    numLevels    = 0;
    int    numKnobs     = 0;
    int    kernelLength = 0;
    double sampleRate   = 48000.0;
    int    latencyReferenceSamples = 0;

    bool load (const std::string& path, std::string* error = nullptr)
    {
        if (! capture::readDcPack (path, profile, error))
            return false;

        numLevels    = (int) profile.levelAxis.values.size();
        numKnobs     = (int) profile.knobAxis.values.size();
        kernelLength = profile.kernelLengthSamples;
        sampleRate   = profile.sampleRate;
        latencyReferenceSamples = profile.latencyReferenceSamples;
        return numLevels > 0 && numKnobs > 0;
    }

    // Linear IR (h1) for a grid cell, or nullptr if that cell is absent.
    const std::vector<float>* linearKernel (int levelIdx, int knobIdx, int channel = 0) const
    {
        for (const auto& c : profile.cells)
            if (c.levelIndex == levelIdx && c.knobIndex == knobIdx && c.channel == channel)
                return &c.linear;
        return nullptr;
    }

    float levelValue (int i) const { return profile.levelAxis.values[(size_t) i]; }
    float knobValue  (int i) const { return profile.knobAxis.values[(size_t) i]; }

    // Normalize each kernel so its peak frequency-response magnitude is unity (0 dB)
    // for playback. The .dcpack stores *peak*-normalized (time-domain) kernels, which
    // inflates the gain of spread kernels (e.g. a closed low-pass) — without this a
    // closing filter gets louder instead of darker. Normalizing the max |H(f)| is
    // robust to the interface's DC blocking (a DC-gain normalization is not) and gives
    // correct relative levels: passband stays ~unity, the filter only ever attenuates.
    // Opt-in, so tests that convolve with raw kernels are unaffected.
    // [Absolute, calibrated gain — dBFS-at-unit — is future work, CLAUDE.md §6.]
    void normalizeMaxGain();
};

} // namespace statebox
