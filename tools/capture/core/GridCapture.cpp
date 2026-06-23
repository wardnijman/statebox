#include "GridCapture.h"

#include <cmath>

#include <juce_core/juce_core.h>

#include "CapturePipeline.h"

namespace statebox::capture
{

float dbToLinear (float db)
{
    return juce::jlimit (0.0f, 1.0f, std::pow (10.0f, db / 20.0f));
}

namespace
{
CaptureProfile makeProfileShell (const GridPlan& plan)
{
    CaptureProfile p;
    p.name                = plan.name;
    p.vendor              = plan.vendor;
    p.sampleRate          = plan.sweep.sampleRate;
    p.bitDepth            = 24;
    p.kernelLengthSamples = plan.kernelLength;
    p.channels            = 1; // mono capture (CLAUDE.md: crosstalk negligible for in-scope gear)
    p.levelAxis           = ProfileAxis { "level", "dBFS-at-output", plan.levelsDb };
    p.knobAxis            = ProfileAxis { plan.knobName, plan.knobUnit, plan.knobValues };

    for (int m = 2; m <= plan.maxHarmonic; ++m)
        p.harmonicOrders.push_back (m);

    return p;
}
} // namespace

CaptureProfile assembleProfile (const GridPlan& plan,
                                const CaptureCellFn&  captureCell,
                                const KnobChangeFn&   onKnobChange,
                                const CellDoneFn&     onCellDone,
                                const CaptureProfile* existing)
{
    CaptureProfile profile = makeProfileShell (plan);

    const int L = plan.numLevels();
    const int K = plan.numKnobs();
    const auto idx = [L] (int li, int ki) { return (size_t) (ki * L + li); };

    // Seed already-captured cells (resume) and remember which (level,knob) are done.
    std::vector<char> done ((size_t) std::max (0, L * K), 0);
    if (existing != nullptr)
    {
        for (const auto& c : existing->cells)
        {
            if (c.levelIndex >= 0 && c.levelIndex < L && c.knobIndex >= 0 && c.knobIndex < K)
            {
                profile.cells.push_back (c);
                done[idx (c.levelIndex, c.knobIndex)] = 1;
            }
        }
    }

    SweepSynth synth (plan.sweep);

    for (int ki = 0; ki < K; ++ki)
    {
        // Skip the human prompt entirely if this whole column is already captured.
        bool columnComplete = true;
        for (int li = 0; li < L; ++li)
            if (! done[idx (li, ki)]) { columnComplete = false; break; }
        if (columnComplete)
            continue;

        if (onKnobChange && ! onKnobChange (ki, plan.knobValues[(size_t) ki]))
            break; // user aborted at the knob prompt

        for (int li = 0; li < L; ++li)
        {
            if (done[idx (li, ki)])
                continue;

            const float levelDb    = plan.levelsDb[(size_t) li];
            const float sweepLevel = dbToLinear (levelDb);

            const CellCapture cap = captureCell (li, ki, sweepLevel, levelDb,
                                                 plan.knobValues[(size_t) ki]);
            if (cap.abort)
                return profile;   // stop the grid; cells so far are persisted via onCellDone
            if (! cap.ok)
                continue;         // user skipped this cell

            CaptureKernels cell = processRecording (cap.meanRecording, synth, plan.maxHarmonic,
                                                    plan.kernelLength, li, ki, 0);
            normalizeKernels (cell, 1.0f);
            profile.cells.push_back (std::move (cell));
            done[idx (li, ki)] = 1;

            if (onCellDone)
                onCellDone (profile);
        }
    }

    return profile;
}

bool loadGridPlan (const std::string& path, GridPlan& out, std::string* error)
{
    const juce::File f { juce::String (path) };
    if (! f.existsAsFile())
    {
        if (error) *error = "plan file not found: " + path;
        return false;
    }

    const auto root = juce::JSON::parse (f.loadFileAsString());
    if (! root.isObject())
    {
        if (error) *error = "plan is not a JSON object: " + path;
        return false;
    }

    const auto get = [&root] (const char* k) { return root.getProperty (k, juce::var()); };

    if (const auto v = get ("name");   v.isString()) out.name   = v.toString().toStdString();
    if (const auto v = get ("vendor"); v.isString()) out.vendor = v.toString().toStdString();

    if (const auto sweep = get ("sweep"); sweep.isObject())
    {
        if (const auto v = sweep.getProperty ("sr",       juce::var()); ! v.isVoid()) out.sweep.sampleRate      = (double) v;
        if (const auto v = sweep.getProperty ("f1",       juce::var()); ! v.isVoid()) out.sweep.startHz         = (double) v;
        if (const auto v = sweep.getProperty ("f2",       juce::var()); ! v.isVoid()) out.sweep.endHz           = (double) v;
        if (const auto v = sweep.getProperty ("duration", juce::var()); ! v.isVoid()) out.sweep.durationSeconds = (double) v;
    }

    if (const auto v = get ("reps");         ! v.isVoid()) out.repetitions  = (int) v;
    if (const auto v = get ("conditioning"); ! v.isVoid()) out.conditioning = (double) v;
    if (const auto v = get ("settle");       ! v.isVoid()) out.settle       = (double) v;
    if (const auto v = get ("harmonics");    ! v.isVoid()) out.maxHarmonic  = (int) v;
    if (const auto v = get ("kernelLength"); ! v.isVoid()) out.kernelLength = (int) v;

    out.levelsDb.clear();
    if (auto* arr = get ("levels").getArray())
        for (const auto& e : *arr) out.levelsDb.push_back ((float) e);

    if (const auto knob = get ("knob"); knob.isObject())
    {
        if (const auto v = knob.getProperty ("name", juce::var()); v.isString()) out.knobName = v.toString().toStdString();
        if (const auto v = knob.getProperty ("unit", juce::var()); v.isString()) out.knobUnit = v.toString().toStdString();
        out.knobValues.clear();
        if (auto* arr = knob.getProperty ("values", juce::var()).getArray())
            for (const auto& e : *arr) out.knobValues.push_back ((float) e);
    }

    if (out.levelsDb.empty() || out.knobValues.empty())
    {
        if (error) *error = "plan must define non-empty 'levels' and 'knob.values'";
        return false;
    }
    return true;
}

bool saveGridPlan (const GridPlan& plan, const std::string& path, std::string* error)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("name",   juce::String (plan.name));
    root->setProperty ("vendor", juce::String (plan.vendor));

    auto* sweep = new juce::DynamicObject();
    sweep->setProperty ("sr",       plan.sweep.sampleRate);
    sweep->setProperty ("f1",       plan.sweep.startHz);
    sweep->setProperty ("f2",       plan.sweep.endHz);
    sweep->setProperty ("duration", plan.sweep.durationSeconds);
    root->setProperty ("sweep", juce::var (sweep));

    root->setProperty ("reps",         plan.repetitions);
    root->setProperty ("conditioning", plan.conditioning);
    root->setProperty ("settle",       plan.settle);
    root->setProperty ("harmonics",    plan.maxHarmonic);
    root->setProperty ("kernelLength", plan.kernelLength);

    juce::Array<juce::var> levels;
    for (const float d : plan.levelsDb) levels.add (d);
    root->setProperty ("levels", levels);

    auto* knob = new juce::DynamicObject();
    knob->setProperty ("name", juce::String (plan.knobName));
    knob->setProperty ("unit", juce::String (plan.knobUnit));
    juce::Array<juce::var> kvals;
    for (const float v : plan.knobValues) kvals.add (v);
    knob->setProperty ("values", kvals);
    root->setProperty ("knob", juce::var (knob));

    const juce::var rootVar (root);
    const juce::File f { juce::String (path) };
    if (! f.replaceWithText (juce::JSON::toString (rootVar)))
    {
        if (error) *error = "cannot write plan file: " + path;
        return false;
    }
    return true;
}

} // namespace statebox::capture
