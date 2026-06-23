#pragma once

#include <string>
#include <vector>

namespace statebox::capture
{

// One axis of the operating-point grid (e.g. level or knob position).
struct ProfileAxis
{
    std::string        name;   // "level", "knob"
    std::string        unit;   // "dBFS-at-unit-input", "normalized"
    std::vector<float> values; // sample points along the axis
};

// Measured kernels for one operating point (level x knob x channel).
struct CaptureKernels
{
    int   levelIndex = 0;
    int   knobIndex  = 0;
    int   channel    = 0;     // 0 = L, 1 = R
    float gain       = 1.0f;  // normalization gain applied; original = stored / gain

    std::vector<float>              linear;    // h1 (linear IR)
    std::vector<std::vector<float>> harmonics; // h2, h3, ... aligned with harmonicOrders
};

// A complete measured-hardware profile, serialized to a .dcpack (see CLAUDE.md §6).
struct CaptureProfile
{
    int         formatVersion       = 1;
    std::string name;
    std::string vendor;
    double      sampleRate          = 48000.0;
    int         bitDepth            = 24;   // capture bit depth (kernels stored as float)
    int         kernelLengthSamples = 4096;
    int         channels            = 2;

    ProfileAxis levelAxis;
    ProfileAxis knobAxis;

    float       referenceDbfs            = -18.0f;
    std::string trimMode                 = "peak-normalized-with-gain-metadata";
    int         latencyReferenceSamples  = 0;

    std::vector<int> harmonicOrders; // e.g. {2, 3}
    std::string      createdWith = "StateboxCapture";

    std::vector<CaptureKernels> cells;
};

// Peak-normalize one cell's linear kernel to targetPeak, scaling its harmonics by the
// same gain so the Hammerstein relationship between orders is preserved. Records the
// applied gain (original = stored / gain). Sets gain, so call exactly once per cell
// (re-running would overwrite the recorded gain — see incremental grid capture).
void normalizeKernels (CaptureKernels& cell, float targetPeak = 1.0f);

// Peak-normalize every cell in the profile via normalizeKernels.
void normalizeProfile (CaptureProfile& profile, float targetPeak = 1.0f);

// Write/read a .dcpack directory (manifest.json + float-WAV kernels). Returns false
// and sets *error (if non-null) on failure.
bool writeDcPack (const CaptureProfile& profile, const std::string& dirPath, std::string* error = nullptr);
bool readDcPack  (const std::string& dirPath, CaptureProfile& out, std::string* error = nullptr);

// Mono 32-bit-float WAV read/write helpers (used by the capture pipeline + CLI).
bool writeWavFile (const std::string& path, const std::vector<float>& data, double sampleRate, std::string* error = nullptr);
bool readWavFile  (const std::string& path, std::vector<float>& out, double* sampleRateOut = nullptr, std::string* error = nullptr);

} // namespace statebox::capture
