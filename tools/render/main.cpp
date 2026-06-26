// Statebox offline renderer — Milestone 0 (CLAUDE.md §10).
//
//   render --profile UNIT.dcpack --in input.wav --out output.wav
//          [--knob V | --knob-sweep --lfo-hz H] [--level V | --level-track]
//          [--drive D] [--block N]
//
// Loads a measured .dcpack, runs the dynamic convolution engine (warm pool +
// bilinear interpolation), and writes the result. By default it sweeps the knob
// (cutoff) with an LFO so the filter's captured motion is audible — proving the
// engine reconstructs the unit before any plugin plumbing.

#include <cmath>
#include <iostream>
#include <vector>

#include <juce_core/juce_core.h>

#include "DcPack.h"
#include "capture/RuntimeProfile.h"
#include "dsp/DynamicConvolutionEngine.h"
#include "dsp/StateEstimator.h"

using namespace statebox;

namespace
{
juce::String optValue (const juce::ArgumentList& args, const char* opt)
{
    const auto eq = args.getValueForOption (opt);
    if (eq.isNotEmpty()) return eq;
    const int i = args.indexOfOption (opt);
    if (i >= 0 && i + 1 < args.size() && ! args[i + 1].isOption())
        return args[i + 1].text;
    return {};
}

float triangle (double phase) // phase in [0,1) -> 0..1..0
{
    return 1.0f - std::abs (2.0f * (float) phase - 1.0f);
}
} // namespace

int main (int argc, char* argv[])
{
    const juce::ArgumentList args (argv[0], juce::StringArray (argv + 1, argc - 1));

    const auto profilePath = optValue (args, "--profile");
    const auto inPath      = optValue (args, "--in");
    const auto outPath     = optValue (args, "--out");
    if (profilePath.isEmpty() || inPath.isEmpty() || outPath.isEmpty())
    {
        std::cerr << "usage: render --profile UNIT.dcpack --in input.wav --out output.wav "
                     "[--knob V | --knob-sweep --lfo-hz H] [--level V | --level-track] [--drive D] [--block N]\n";
        return 1;
    }

    RuntimeProfile profile;
    std::string    err;
    if (! profile.load (profilePath.toStdString(), &err))
    {
        std::cerr << "load profile failed: " << err << "\n";
        return 1;
    }
    profile.normalizeMaxGain(); // peak-normalized .dcpack -> 0 dB max gain for playback

    std::vector<float> input;
    double             inSr = 0.0;
    if (! capture::readWavFile (inPath.toStdString(), input, &inSr, &err))
    {
        std::cerr << "read input failed: " << err << "\n";
        return 1;
    }
    if (std::abs (inSr - profile.sampleRate) > 1.0)
        std::cerr << "WARNING: input is " << inSr << " Hz but profile is " << profile.sampleRate
                  << " Hz — Milestone 0 has no resampling; results will be pitch/time-shifted.\n";

    const int block = optValue (args, "--block").isNotEmpty() ? optValue (args, "--block").getIntValue() : 512;

    DynamicConvolutionEngine engine;
    engine.prepare (profile, block);

    StateEstimator state;
    state.prepare (inSr, 5.0f, 80.0f,
                   profile.levelValue (0), profile.levelValue (profile.numLevels - 1),
                   profile.numLevels);

    const int   K = engine.numKnobs();
    const int   L = engine.numLevels();
    const bool  knobSweep   = args.containsOption ("--knob-sweep") || optValue (args, "--knob").isEmpty();
    const float knobFixed   = optValue (args, "--knob").isNotEmpty()
                                  ? juce::jlimit (0.0f, 1.0f, (float) optValue (args, "--knob").getDoubleValue())
                                  : 0.0f;
    const float lfoHz       = optValue (args, "--lfo-hz").isNotEmpty() ? (float) optValue (args, "--lfo-hz").getDoubleValue() : 0.2f;
    const bool  levelTrack  = args.containsOption ("--level-track");
    const float levelFixed  = optValue (args, "--level").isNotEmpty()
                                  ? juce::jlimit (0.0f, 1.0f, (float) optValue (args, "--level").getDoubleValue())
                                  : 0.5f;
    const float drive       = optValue (args, "--drive").isNotEmpty() ? (float) optValue (args, "--drive").getDoubleValue() : 0.0f;

    std::vector<float> output (input.size(), 0.0f);

    for (int start = 0; start < (int) input.size(); start += block)
    {
        const int n = std::min (block, (int) input.size() - start);

        // Knob (cutoff) position: LFO sweep across the axis, or fixed.
        float knobPos;
        if (knobSweep)
        {
            const double t = (double) start / inSr;
            knobPos = triangle (std::fmod (t * lfoHz, 1.0)) * (float) (K - 1);
        }
        else
        {
            knobPos = knobFixed * (float) (K - 1);
        }

        // Level position: track the input envelope, or fixed.
        const float levelPos = levelTrack ? state.process (input.data() + start, n)
                                          : levelFixed * (float) (L - 1);

        engine.setState (levelPos, knobPos);
        engine.process (input.data() + start, output.data() + start, n);
    }

    // Optional small drive (the only harmonic source; off by default).
    if (drive > 0.0f)
    {
        const float norm = std::tanh (drive);
        for (auto& s : output)
            s = std::tanh (drive * s) / norm;
    }

    // Output safety: scale below full scale if the convolution pushed past it.
    float peak = 0.0f;
    for (const float s : output) peak = std::max (peak, std::abs (s));
    if (peak > 0.99f)
    {
        const float g = 0.99f / peak;
        for (auto& s : output) s *= g;
        std::cout << "(output scaled by " << juce::String (20.0f * std::log10 (g), 1) << " dB to avoid clipping)\n";
    }

    if (! capture::writeWavFile (outPath.toStdString(), output, inSr, &err))
    {
        std::cerr << "write output failed: " << err << "\n";
        return 1;
    }

    std::cout << "Rendered " << output.size() << " samples @ " << inSr << " Hz -> " << outPath << "\n"
              << "  profile: " << profile.numLevels << " levels x " << profile.numKnobs << " knobs, "
              << "peak " << juce::String (20.0f * std::log10 (std::max (peak, 1.0e-9f)), 1) << " dBFS\n"
              << "  mode: " << (knobSweep ? "knob-sweep" : "knob-fixed")
              << (levelTrack ? ", level-track" : ", level-fixed") << "\n";
    return 0;
}
