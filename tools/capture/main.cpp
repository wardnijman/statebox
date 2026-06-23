// Statebox Capture Tool — CLI.
//
//   gen-sweep --out sweep.wav [sweep options]
//       Generate an excitation sweep to play through the hardware.
//
//   process --in recording.wav --out unit.dcpack [sweep + processing options]
//       Turn a recorded sweep into a .dcpack: deconvolve -> separate h1/h2/h3 ->
//       latency + sub-sample align -> normalize -> write.
//
// See CLAUDE.md §6 and §10. Live audio I/O and a GUI come in later increments.

#include <iostream>

#include <juce_core/juce_core.h>

#include "core/SweepSynth.h"
#include "core/DcPack.h"
#include "core/CapturePipeline.h"

using namespace statebox::capture;

namespace
{
// Returns an option's value, accepting both "--opt=value" and "--opt value".
// (JUCE's getValueForOption only handles the '=' form.)
juce::String optValue (const juce::ArgumentList& args, const char* opt)
{
    const auto eqForm = args.getValueForOption (opt);
    if (eqForm.isNotEmpty())
        return eqForm;

    const int i = args.indexOfOption (opt);
    if (i >= 0 && i + 1 < args.size() && ! args[i + 1].isOption())
        return args[i + 1].text;

    return {};
}

SweepSpec sweepFromArgs (const juce::ArgumentList& args)
{
    SweepSpec s;
    if (const auto v = optValue (args, "--sr");       v.isNotEmpty()) s.sampleRate      = v.getDoubleValue();
    if (const auto v = optValue (args, "--f1");       v.isNotEmpty()) s.startHz         = v.getDoubleValue();
    if (const auto v = optValue (args, "--f2");       v.isNotEmpty()) s.endHz           = v.getDoubleValue();
    if (const auto v = optValue (args, "--duration"); v.isNotEmpty()) s.durationSeconds = v.getDoubleValue();
    return s;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ConsoleApplication app;
    app.addHelpCommand ("--help|-h", "Statebox Capture Tool. Commands:", true);

    app.addCommand ({ "gen-sweep",
                      "gen-sweep --out FILE [--sr R --f1 F --f2 F --duration S]",
                      "Generate an excitation sweep WAV to play through the unit.",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          const auto out = optValue (args, "--out");
                          if (out.isEmpty())
                              juce::ConsoleApplication::fail ("--out is required");

                          const auto spec = sweepFromArgs (args);
                          SweepSynth synth (spec);

                          std::string err;
                          if (! writeWavFile (out.toStdString(), synth.sweep(), spec.sampleRate, &err))
                              juce::ConsoleApplication::fail ("write failed: " + juce::String (err));

                          std::cout << "Wrote sweep: " << synth.length() << " samples @ "
                                    << spec.sampleRate << " Hz -> " << out << "\n";
                      } });

    app.addCommand ({ "process",
                      "process --in REC.wav --out UNIT.dcpack [--sr R --f1 F --f2 F --duration S "
                      "--harmonics N --kernel-length L --name NAME]",
                      "Process a recorded sweep into a .dcpack (deconvolve, separate, align, normalize).",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          const auto inPath  = optValue (args, "--in");
                          const auto outPath = optValue (args, "--out");
                          if (inPath.isEmpty() || outPath.isEmpty())
                              juce::ConsoleApplication::fail ("--in and --out are required");

                          std::vector<float> recording;
                          double             recSr = 0.0;
                          std::string        err;
                          if (! readWavFile (inPath.toStdString(), recording, &recSr, &err))
                              juce::ConsoleApplication::fail ("read failed: " + juce::String (err));

                          auto spec = sweepFromArgs (args);
                          if (recSr > 0.0)
                              spec.sampleRate = recSr; // the recording's rate is authoritative

                          const auto harmonicsArg = optValue (args, "--harmonics");
                          const auto kernelArg    = optValue (args, "--kernel-length");
                          const auto nameArg      = optValue (args, "--name");

                          const int  maxHarmonic  = harmonicsArg.isNotEmpty() ? harmonicsArg.getIntValue() : 3;
                          const int  kernelLength = kernelArg.isNotEmpty()    ? kernelArg.getIntValue()    : 4096;
                          const auto name         = nameArg.isNotEmpty()      ? nameArg : juce::String ("Captured Unit");

                          SweepSynth synth (spec);
                          auto kernels = processRecording (recording, synth, maxHarmonic, kernelLength);
                          auto profile = buildSingleCellProfile (std::move (kernels), spec, maxHarmonic,
                                                                 kernelLength, name.toStdString());
                          normalizeProfile (profile, 1.0f);

                          if (! writeDcPack (profile, outPath.toStdString(), &err))
                              juce::ConsoleApplication::fail ("write .dcpack failed: " + juce::String (err));

                          std::cout << "Wrote .dcpack -> " << outPath
                                    << " (sr=" << spec.sampleRate
                                    << ", kernelLength=" << kernelLength
                                    << ", harmonics 2.." << maxHarmonic << ")\n";
                      } });

    return app.findAndRunCommand (argc, argv);
}
