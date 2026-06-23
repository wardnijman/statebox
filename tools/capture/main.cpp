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
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "core/SweepSynth.h"
#include "core/DcPack.h"
#include "core/CapturePipeline.h"
#include "core/LiveCaptureEngine.h"

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

// Bridges the audio device to the (hardware-agnostic) LiveCaptureEngine.
class LoopbackCallback : public juce::AudioIODeviceCallback
{
public:
    explicit LoopbackCallback (LiveCaptureEngine& e) : engine (e) {}

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        const float* input = (numIn > 0) ? in[0] : nullptr;
        if (numOut > 0 && out[0] != nullptr)
        {
            engine.processBlock (input, out[0], numSamples);
            for (int ch = 1; ch < numOut; ++ch)
                if (out[ch] != nullptr)
                    juce::FloatVectorOperations::copy (out[ch], out[0], numSamples);
        }
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    LiveCaptureEngine& engine;
};
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

    app.addCommand ({ "capture-live",
                      "capture-live --out UNIT.dcpack [--sr R --f1 F --f2 F --duration S "
                      "--reps N --conditioning S --settle S --harmonics N --kernel-length L --name NAME]",
                      "Capture from the default audio interface (plays the sweep, records the return).",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          const auto outPath = optValue (args, "--out");
                          if (outPath.isEmpty())
                              juce::ConsoleApplication::fail ("--out is required");

                          LiveCaptureConfig cfg;
                          cfg.sweep = sweepFromArgs (args);
                          if (const auto v = optValue (args, "--reps");         v.isNotEmpty()) cfg.repetitions         = v.getIntValue();
                          if (const auto v = optValue (args, "--conditioning"); v.isNotEmpty()) cfg.conditioningSeconds = v.getDoubleValue();
                          if (const auto v = optValue (args, "--settle");       v.isNotEmpty()) cfg.settleSeconds       = v.getDoubleValue();

                          const auto harmonicsArg = optValue (args, "--harmonics");
                          const auto kernelArg    = optValue (args, "--kernel-length");
                          const auto nameArg      = optValue (args, "--name");
                          const int  maxHarmonic  = harmonicsArg.isNotEmpty() ? harmonicsArg.getIntValue() : 3;
                          const int  kernelLength = kernelArg.isNotEmpty()    ? kernelArg.getIntValue()    : 4096;
                          const auto name         = nameArg.isNotEmpty()      ? nameArg : juce::String ("Captured Unit");

                          LiveCaptureEngine engine (cfg);

                          // A minimal message system for the device manager. No GUI / event loop is
                          // needed for a one-shot capture: the audio callback runs on the device thread.
                          juce::MessageManager::getInstance();
                          {
                              juce::AudioDeviceManager dm;
                              juce::AudioDeviceManager::AudioDeviceSetup setup;
                              setup.sampleRate = cfg.sweep.sampleRate;

                              const auto initErr = dm.initialise (1, 1, nullptr, true, {}, &setup);
                              if (initErr.isNotEmpty())
                                  juce::ConsoleApplication::fail ("audio device init failed: " + initErr);

                              if (auto* dev = dm.getCurrentAudioDevice())
                                  std::cout << "Device: " << dev->getName()
                                            << " @ " << dev->getCurrentSampleRate() << " Hz, in="
                                            << dev->getActiveInputChannels().countNumberOfSetBits()
                                            << " out=" << dev->getActiveOutputChannels().countNumberOfSetBits() << "\n";

                              LoopbackCallback cb (engine);
                              dm.addAudioCallback (&cb);
                              std::cout << "Capturing " << cfg.repetitions << " repetition(s)...\n";

                              const double perRep    = cfg.conditioningSeconds + 2.0 * cfg.settleSeconds + cfg.sweep.durationSeconds;
                              const int    maxWaitMs  = (int) ((perRep * cfg.repetitions + 5.0) * 2.0 * 1000.0);
                              int          waited     = 0;
                              while (! engine.isFinished() && waited < maxWaitMs)
                              {
                                  juce::Thread::sleep (50);
                                  waited += 50;
                              }
                              dm.removeAudioCallback (&cb);
                              dm.closeAudioDevice();

                              if (! engine.isFinished())
                                  juce::ConsoleApplication::fail ("capture timed out (no audio callbacks?)");
                          }

                          engine.finalize();
                          SweepSynth synth (cfg.sweep);
                          auto kernels = processRecording (engine.meanRecording(), synth, maxHarmonic, kernelLength);
                          auto profile = buildSingleCellProfile (std::move (kernels), cfg.sweep, maxHarmonic,
                                                                 kernelLength, name.toStdString());
                          normalizeProfile (profile, 1.0f);

                          std::string err;
                          if (! writeDcPack (profile, outPath.toStdString(), &err))
                              juce::ConsoleApplication::fail ("write .dcpack failed: " + juce::String (err));

                          std::cout << "Wrote .dcpack -> " << outPath << "\n";
                      } });

    return app.findAndRunCommand (argc, argv);
}
