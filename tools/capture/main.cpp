// Statebox Capture Tool — CLI entry point.
//
// Pipeline so far: exponential sweep synthesis + FFT deconvolution (core/). Next
// increments add harmonic-order separation (h2/h3), alignment, normalization, and
// the .dcpack writer. See CLAUDE.md §6 and §10.

#include <iostream>

#include "core/SweepSynth.h"

int main()
{
    using namespace statebox::capture;

    SweepSpec  spec;
    SweepSynth synth (spec);

    std::cout << "Statebox Capture Tool\n"
              << "  sweep : " << synth.length() << " samples @ " << spec.sampleRate
              << " Hz (" << spec.startHz << "-" << spec.endHz << " Hz, "
              << spec.durationSeconds << " s)\n"
              << "  h2 offset: " << synth.harmonicOffsetSamples (2) << " samples before linear IR\n"
              << "  h3 offset: " << synth.harmonicOffsetSamples (3) << " samples before linear IR\n";

    return 0;
}
