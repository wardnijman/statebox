#pragma once

#include <functional>
#include <string>
#include <vector>

#include "SweepSynth.h"
#include "DcPack.h"

namespace statebox::capture
{

// A per-unit capture plan: the operating-point grid + measurement protocol. The
// level axis is software-driven (sweep amplitude, auto-stepped per cell); the knob
// axis is human-set (one prompt per position). Loaded from / saved to a small JSON
// file so a library of units is reproducible (CLAUDE.md §6, §10).
struct GridPlan
{
    std::string name   = "Captured Unit";
    std::string vendor = "User Capture";

    SweepSpec sweep;                 // sr / f1 / f2 / duration

    int    repetitions  = 5;
    double conditioning = 2.0;       // seconds of conditioning tone before each sweep
    double settle       = 0.5;       // seconds of silence before the sweep, and tail after
    int    maxHarmonic  = 3;
    int    kernelLength = 4096;
    int    latencyReferenceSamples = 0; // interface loopback latency (measure-latency); 0 = auto

    std::vector<float> levelsDb;     // dBFS-at-output (<= 0); auto-stepped, no human needed
    std::string        knobName  = "knob";
    std::string        knobUnit  = "normalized";
    std::vector<float> knobValues; // human-set positions, one prompt each

    int numLevels() const { return (int) levelsDb.size(); }
    int numKnobs()  const { return (int) knobValues.size(); }
    int cellCount() const { return numLevels() * numKnobs(); }
};

// JSON load/save for a capture plan. Returns false and sets *error on failure.
bool loadGridPlan (const std::string& path, GridPlan& out, std::string* error = nullptr);
bool saveGridPlan (const GridPlan& plan, const std::string& path, std::string* error = nullptr);

// dBFS (<= 0) -> linear sweep amplitude, clamped to [0, 1].
float dbToLinear (float db);

// One attempted cell capture. ok=false -> skip this cell (omitted from the profile);
// abort=true -> stop the whole grid (cells captured so far are kept).
struct CellCapture
{
    bool               ok    = false;
    bool               abort = false;
    std::vector<float> meanRecording;
};

// Capture one cell. Args identify the operating point and supply the sweep level.
using CaptureCellFn = std::function<CellCapture (int levelIndex, int knobIndex,
                                                 float sweepLevel, float levelDb, float knobValue)>;
// Invoked before each knob column (the human turns the knob here). Return false to abort.
using KnobChangeFn  = std::function<bool (int knobIndex, float knobValue)>;
// Invoked after each newly captured cell is added (for incremental persistence).
using CellDoneFn    = std::function<void (const CaptureProfile&)>;

// Walk the grid knob-outer / level-inner, building a multi-cell profile. Cells already
// present in `existing` (matched by level+knob index) are kept and not re-captured, and
// a fully-captured knob column skips its prompt (resume). Each new cell is processed +
// normalized exactly once. Pure orchestration: all hardware / human interaction is
// injected via the callbacks, so it is unit-testable with a synthetic captureCell.
CaptureProfile assembleProfile (const GridPlan& plan,
                                const CaptureCellFn&  captureCell,
                                const KnobChangeFn&   onKnobChange = {},
                                const CellDoneFn&     onCellDone   = {},
                                const CaptureProfile* existing     = nullptr);

} // namespace statebox::capture
