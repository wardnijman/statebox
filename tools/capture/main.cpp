// Statebox Capture Tool — CLI.
//
//   gen-sweep --out sweep.wav [sweep options]
//       Generate an excitation sweep to play through the hardware.
//
//   process --in recording.wav --out unit.dcpack [sweep + processing options]
//       Turn a recorded sweep into a .dcpack: deconvolve -> separate h1/h2/h3 ->
//       latency + sub-sample align -> normalize -> write.
//
//   capture-live --out unit.dcpack [...]
//       Capture one operating point from the default interface (single cell).
//
//   capture-grid --out unit.dcpack (--plan p.json | --levels .. --knob ..) [--resume ...]
//       Capture a level x knob grid into one .dcpack: auto-steps the level axis,
//       prompts for each knob position, writes incrementally so it can resume.
//
//   inspect --in unit.dcpack
//       Print a .dcpack's metadata + per-cell kernel stats (peak, impulse sharpness,
//       harmonic levels) — e.g. a wire loopback should show a near-delta linear IR.
//
// See CLAUDE.md §6 and §10. A GUI comes in a later increment.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "core/SweepSynth.h"
#include "core/DcPack.h"
#include "core/CapturePipeline.h"
#include "core/LiveCaptureEngine.h"
#include "core/GridCapture.h"

using namespace statebox::capture;

namespace
{
// A token that JUCE would treat as an option ("-18,-12,-6") but is really a negative
// numeric value: leading '-' followed by a digit or '.'.
bool looksLikeNegativeNumber (const juce::String& s)
{
    if (! s.startsWithChar ('-') || s.length() < 2)
        return false;
    const auto c = s[1];
    return juce::CharacterFunctions::isDigit (c) || c == '.';
}

// Returns an option's value, accepting both "--opt=value" and "--opt value".
// (JUCE's getValueForOption only handles the '=' form.) A following token that looks
// like a negative number is taken as the value, so "--levels -18,-12,-6" works.
juce::String optValue (const juce::ArgumentList& args, const char* opt)
{
    const auto eqForm = args.getValueForOption (opt);
    if (eqForm.isNotEmpty())
        return eqForm;

    const int i = args.indexOfOption (opt);
    if (i >= 0 && i + 1 < args.size())
    {
        const auto& next = args[i + 1].text;
        if (! args[i + 1].isOption() || looksLikeNegativeNumber (next))
            return next;
    }

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

// Linear amplitude -> dBFS string (guards log(0)).
juce::String dbfs (float linear)
{
    if (linear <= 1.0e-9f)
        return "-inf";
    return juce::String (20.0f * std::log10 (linear), 1);
}

// Print peak / sweep RMS / noise floor / latency so a capture is interpretable (got
// signal? clipping? silent?) rather than just "Wrote .dcpack".
void printStats (const CaptureStats& st, double sampleRate)
{
    std::cout << "  peak       : " << dbfs (st.peak) << " dBFS"
              << (st.clipping ? "   *** CLIPPING — reduce --sweep-level or input trim ***" : "") << "\n";
    std::cout << "  sweep RMS  : " << dbfs (st.sweepRms) << " dBFS\n";

    if (st.noiseFloorValid)
    {
        std::cout << "  noise floor: " << dbfs (st.noiseFloor) << " dBFS";
        if (st.snrValid)
            std::cout << "   (SNR " << juce::String (st.snrDb, 1) << " dB)";
        std::cout << "\n";
    }
    else
    {
        std::cout << "  noise floor: n/a (tail too short after the return — increase --settle)\n";
    }

    std::cout << "  latency    : " << st.latencySamples << " samples";
    if (sampleRate > 0.0)
        std::cout << " (" << juce::String (1000.0 * st.latencySamples / sampleRate, 1) << " ms)";
    std::cout << "\n";

    if (st.silent)
        std::cout << "  WARNING: input is essentially silent — check cabling, channel routing, "
                     "and macOS mic permission for the terminal.\n";
}

// Read one line from stdin; return the first non-space char lowercased, '\n' for an
// empty line (just Enter), or 'a' on EOF (treat a closed pipe as abort).
char prompt (const char* msg)
{
    std::cout << msg << std::flush;
    std::string line;
    if (! std::getline (std::cin, line))
        return 'a';
    for (const char ch : line)
        if (! std::isspace ((unsigned char) ch))
            return (char) std::tolower ((unsigned char) ch);
    return '\n';
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

// Open the default device as 1-in/1-out at the requested rate, printing what opened.
bool openCaptureDevice (juce::AudioDeviceManager& dm, double sampleRate, juce::String& err)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.sampleRate = sampleRate;

    err = dm.initialise (1, 1, nullptr, true, {}, &setup);
    if (err.isNotEmpty())
        return false;

    if (auto* dev = dm.getCurrentAudioDevice())
        std::cout << "Device: " << dev->getName()
                  << " @ " << dev->getCurrentSampleRate() << " Hz, in="
                  << dev->getActiveInputChannels().countNumberOfSetBits()
                  << " out=" << dev->getActiveOutputChannels().countNumberOfSetBits() << "\n";
    return true;
}

struct OneCapture
{
    bool               finished = false;
    std::vector<float> mean;
    int                tailLen = 0;
};

// Run a single capture sequence on an already-open device. Adds/removes its callback
// (device stays open across cells), waits with a generous timeout guard.
OneCapture runCapture (juce::AudioDeviceManager& dm, const LiveCaptureConfig& cfg)
{
    LiveCaptureEngine engine (cfg);
    LoopbackCallback  cb (engine);
    dm.addAudioCallback (&cb);

    const double perRep    = cfg.conditioningSeconds + 2.0 * cfg.settleSeconds + cfg.sweep.durationSeconds;
    const int    maxWaitMs = (int) ((perRep * cfg.repetitions + 5.0) * 2.0 * 1000.0);
    int          waited    = 0;
    while (! engine.isFinished() && waited < maxWaitMs)
    {
        juce::Thread::sleep (50);
        waited += 50;
    }
    dm.removeAudioCallback (&cb);

    OneCapture r;
    r.finished = engine.isFinished();
    if (r.finished)
    {
        engine.finalize();
        r.mean    = engine.meanRecording();
        r.tailLen = engine.tailLengthSamples();
    }
    return r;
}

// Build a GridPlan from inline flags (the alternative to --plan).
//   --levels -24,-18,-12,-6      --knob cutoff:0,0.25,0.5,0.75,1.0
GridPlan planFromArgs (const juce::ArgumentList& args)
{
    GridPlan plan;
    plan.sweep = sweepFromArgs (args);

    if (const auto v = optValue (args, "--reps");          v.isNotEmpty()) plan.repetitions  = v.getIntValue();
    if (const auto v = optValue (args, "--conditioning");  v.isNotEmpty()) plan.conditioning = v.getDoubleValue();
    if (const auto v = optValue (args, "--settle");        v.isNotEmpty()) plan.settle       = v.getDoubleValue();
    if (const auto v = optValue (args, "--harmonics");     v.isNotEmpty()) plan.maxHarmonic  = v.getIntValue();
    if (const auto v = optValue (args, "--kernel-length"); v.isNotEmpty()) plan.kernelLength = v.getIntValue();

    if (const auto v = optValue (args, "--levels"); v.isNotEmpty())
    {
        plan.levelsDb.clear();
        for (const auto& tok : juce::StringArray::fromTokens (v, ",", ""))
            if (tok.trim().isNotEmpty())
                plan.levelsDb.push_back ((float) tok.trim().getDoubleValue());
    }

    if (const auto v = optValue (args, "--knob"); v.isNotEmpty())
    {
        juce::String spec = v;
        const int colon = spec.indexOfChar (':');
        if (colon >= 0)
        {
            plan.knobName = spec.substring (0, colon).trim().toStdString();
            spec = spec.substring (colon + 1);
        }
        plan.knobValues.clear();
        for (const auto& tok : juce::StringArray::fromTokens (spec, ",", ""))
            if (tok.trim().isNotEmpty())
                plan.knobValues.push_back ((float) tok.trim().getDoubleValue());
    }

    return plan;
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

    app.addCommand ({ "capture-live",
                      "capture-live --out UNIT.dcpack [--sr R --f1 F --f2 F --duration S "
                      "--reps N --conditioning S --settle S --sweep-level G --harmonics N --kernel-length L --name NAME]",
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
                          if (const auto v = optValue (args, "--sweep-level");  v.isNotEmpty()) cfg.sweepLevel          = (float) v.getDoubleValue();

                          cfg.sweepLevel = juce::jlimit (0.0f, 1.0f, cfg.sweepLevel); // linear 0..1; 1.0 = full scale

                          const auto harmonicsArg = optValue (args, "--harmonics");
                          const auto kernelArg    = optValue (args, "--kernel-length");
                          const auto nameArg      = optValue (args, "--name");
                          const int  maxHarmonic  = harmonicsArg.isNotEmpty() ? harmonicsArg.getIntValue() : 3;
                          const int  kernelLength = kernelArg.isNotEmpty()    ? kernelArg.getIntValue()    : 4096;
                          const auto name         = nameArg.isNotEmpty()      ? nameArg : juce::String ("Captured Unit");

                          // The audio callback runs on the device thread; no message loop needed.
                          juce::MessageManager::getInstance();
                          juce::AudioDeviceManager dm;
                          juce::String derr;
                          if (! openCaptureDevice (dm, cfg.sweep.sampleRate, derr))
                              juce::ConsoleApplication::fail ("audio device init failed: " + derr);

                          std::cout << "Capturing " << cfg.repetitions << " repetition(s) at sweep level "
                                    << dbfs (cfg.sweepLevel) << " dBFS...\n";
                          const auto cap = runCapture (dm, cfg);
                          dm.closeAudioDevice();

                          if (! cap.finished)
                              juce::ConsoleApplication::fail ("capture timed out (no audio callbacks?)");

                          std::cout << "Captured return:\n";
                          printStats (analyzeRecording (cap.mean, cap.tailLen), cfg.sweep.sampleRate);

                          SweepSynth synth (cfg.sweep);
                          auto kernels = processRecording (cap.mean, synth, maxHarmonic, kernelLength);
                          auto profile = buildSingleCellProfile (std::move (kernels), cfg.sweep, maxHarmonic,
                                                                 kernelLength, name.toStdString());
                          normalizeProfile (profile, 1.0f);

                          std::string err;
                          if (! writeDcPack (profile, outPath.toStdString(), &err))
                              juce::ConsoleApplication::fail ("write .dcpack failed: " + juce::String (err));

                          std::cout << "Wrote .dcpack -> " << outPath << "\n";
                      } });

    app.addCommand ({ "capture-grid",
                      "capture-grid --out UNIT.dcpack (--plan PLAN.json | --levels dB,dB,.. --knob NAME:v,v,..) "
                      "[--resume --dry-run --sr R --f1 F --f2 F --duration S --reps N --conditioning S "
                      "--settle S --harmonics N --kernel-length L --name NAME --vendor V]",
                      "Capture a level x knob grid into one .dcpack (auto-steps level, prompts per knob).",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          const auto outPath = optValue (args, "--out");
                          if (outPath.isEmpty())
                              juce::ConsoleApplication::fail ("--out is required");

                          GridPlan plan;
                          if (const auto planPath = optValue (args, "--plan"); planPath.isNotEmpty())
                          {
                              std::string e;
                              if (! loadGridPlan (planPath.toStdString(), plan, &e))
                                  juce::ConsoleApplication::fail ("plan: " + juce::String (e));
                          }
                          else
                          {
                              plan = planFromArgs (args);
                          }

                          // Name/vendor can override the plan from the command line.
                          if (const auto v = optValue (args, "--name");   v.isNotEmpty()) plan.name   = v.toStdString();
                          if (const auto v = optValue (args, "--vendor"); v.isNotEmpty()) plan.vendor = v.toStdString();

                          if (plan.levelsDb.empty() || plan.knobValues.empty())
                              juce::ConsoleApplication::fail ("need at least one level and one knob value "
                                                              "(use --plan, or --levels and --knob)");

                          // --dry-run: validate + print the grid, never touch the device.
                          if (args.containsOption ("--dry-run"))
                          {
                              std::cout << plan.name << " (" << plan.vendor << ")\n"
                                        << "  sweep: " << plan.sweep.startHz << "-" << plan.sweep.endHz
                                        << " Hz, " << plan.sweep.durationSeconds << " s @ "
                                        << plan.sweep.sampleRate << " Hz, " << plan.repetitions << " rep(s)\n"
                                        << "  grid: " << plan.numLevels() << " level(s) x " << plan.numKnobs()
                                        << " knob position(s) = " << plan.cellCount() << " cells\n  levels (dBFS):";
                              for (const float d : plan.levelsDb) std::cout << " " << d;
                              std::cout << "\n  " << plan.knobName << ":";
                              for (const float v : plan.knobValues) std::cout << " " << v;
                              std::cout << "\n";
                              return;
                          }

                          // Resume: load any cells already captured into this pack.
                          CaptureProfile existing;
                          bool haveExisting = false;
                          if (args.containsOption ("--resume"))
                          {
                              std::string e;
                              if (readDcPack (outPath.toStdString(), existing, &e))
                              {
                                  haveExisting = true;
                                  std::cout << "Resuming: " << existing.cells.size() << " cell(s) already in "
                                            << outPath << "\n";
                              }
                              else
                              {
                                  std::cout << "Resume requested but no readable pack at " << outPath
                                            << " — starting fresh.\n";
                              }
                          }

                          juce::MessageManager::getInstance();
                          juce::AudioDeviceManager dm;
                          juce::String derr;
                          if (! openCaptureDevice (dm, plan.sweep.sampleRate, derr))
                              juce::ConsoleApplication::fail ("audio device init failed: " + derr);

                          const int total = plan.cellCount();
                          std::cout << "Grid: " << plan.numLevels() << " level(s) x " << plan.numKnobs()
                                    << " knob position(s) = " << total << " cells.\n";

                          // Capture one cell, with a retry gate on clipping / silence / timeout.
                          auto captureCell = [&dm, &plan] (int li, int /*ki*/, float sweepLevel,
                                                           float levelDb, float /*knobVal*/) -> CellCapture
                          {
                              LiveCaptureConfig cfg;
                              cfg.sweep               = plan.sweep;
                              cfg.repetitions         = plan.repetitions;
                              cfg.conditioningSeconds = plan.conditioning;
                              cfg.settleSeconds       = plan.settle;
                              cfg.sweepLevel          = sweepLevel;

                              for (;;)
                              {
                                  std::cout << "  [L" << li << " @ " << juce::String (levelDb, 1) << " dBFS] capturing "
                                            << plan.repetitions << " rep(s)...\n";
                                  const auto cap = runCapture (dm, cfg);

                                  if (! cap.finished)
                                  {
                                      std::cout << "  capture timed out (no audio callbacks).\n";
                                      const char c = prompt ("  [s]kip / [a]bort / Enter=retry: ");
                                      if (c == 'a') return { false, true, {} };
                                      if (c == 's') return { false, false, {} };
                                      continue;
                                  }

                                  const auto st = analyzeRecording (cap.mean, cap.tailLen);
                                  printStats (st, plan.sweep.sampleRate);

                                  if (st.clipping || st.silent)
                                  {
                                      const char c = prompt ("  problem — [c]ontinue anyway / [s]kip / [a]bort / Enter=retry: ");
                                      if (c == 'a') return { false, true, {} };
                                      if (c == 's') return { false, false, {} };
                                      if (c == 'c') return { true, false, cap.mean };
                                      continue; // retry
                                  }
                                  return { true, false, cap.mean };
                              }
                          };

                          auto onKnobChange = [&plan] (int ki, float knobVal) -> bool
                          {
                              std::cout << "\n=== Set " << plan.knobName << " to " << knobVal
                                        << " (position " << (ki + 1) << "/" << plan.numKnobs() << ") ===\n";
                              return prompt ("Press Enter to capture this column, or 'a'+Enter to abort: ") != 'a';
                          };

                          int doneCount = haveExisting ? (int) existing.cells.size() : 0;
                          auto onCellDone = [&outPath, &doneCount, total] (const CaptureProfile& p)
                          {
                              ++doneCount;
                              std::string e;
                              if (! writeDcPack (p, outPath.toStdString(), &e))
                                  std::cout << "  WARNING: incremental write failed: " << e << "\n";
                              else
                                  std::cout << "  saved (" << doneCount << "/" << total << ")\n";
                          };

                          auto profile = assembleProfile (plan, captureCell, onKnobChange, onCellDone,
                                                          haveExisting ? &existing : nullptr);
                          dm.closeAudioDevice();

                          if (profile.cells.empty())
                          {
                              std::cout << "No cells captured.\n";
                              return;
                          }

                          std::string werr;
                          if (! writeDcPack (profile, outPath.toStdString(), &werr))
                              juce::ConsoleApplication::fail ("write .dcpack failed: " + juce::String (werr));

                          std::cout << "\nWrote " << profile.cells.size() << "/" << total
                                    << " cell(s) -> " << outPath << "\n";
                      } });

    app.addCommand ({ "inspect",
                      "inspect --in UNIT.dcpack",
                      "Print a .dcpack's metadata and per-cell kernel stats (peak, sharpness, harmonics).",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          const auto inPath = optValue (args, "--in");
                          if (inPath.isEmpty())
                              juce::ConsoleApplication::fail ("--in is required");

                          CaptureProfile p;
                          std::string    e;
                          if (! readDcPack (inPath.toStdString(), p, &e))
                              juce::ConsoleApplication::fail ("read .dcpack failed: " + juce::String (e));

                          std::cout << p.name << " (" << p.vendor << ")\n"
                                    << "  sr=" << p.sampleRate << " Hz, kernelLength=" << p.kernelLengthSamples
                                    << ", channels=" << p.channels << ", cells=" << p.cells.size()
                                    << ", latencyRef=" << p.latencyReferenceSamples << "\n  harmonics:";
                          for (const int h : p.harmonicOrders) std::cout << " h" << h;
                          std::cout << "\n  " << p.levelAxis.name << " (" << p.levelAxis.unit << "):";
                          for (const float v : p.levelAxis.values) std::cout << " " << v;
                          std::cout << "\n  " << p.knobAxis.name << " (" << p.knobAxis.unit << "):";
                          for (const float v : p.knobAxis.values) std::cout << " " << v;
                          std::cout << "\n";

                          for (const auto& c : p.cells)
                          {
                              const auto ks = analyzeKernel (c.linear);
                              std::cout << "  [L" << c.levelIndex << " K" << c.knobIndex << " ch" << c.channel << "]"
                                        << " peak " << dbfs (std::abs (ks.peakValue)) << " dBFS @ " << ks.peakIndex
                                        << ", sharpness " << juce::String (ks.sharpnessDb, 1) << " dB"
                                        << ", gain " << juce::String (c.gain, 3);

                              const float h1 = std::abs (ks.peakValue);
                              for (size_t h = 0; h < c.harmonics.size(); ++h)
                              {
                                  float hm = 0.0f;
                                  for (const float s : c.harmonics[h]) hm = std::max (hm, std::abs (s));
                                  std::cout << ", h" << (h + 2) << "/h1 " << dbfs (h1 > 0.0f ? hm / h1 : 0.0f) << " dB";
                              }
                              std::cout << "\n";
                          }
                      } });

    // JUCE's ConsoleApplication treats every '-'-leading token as an option, so a bare
    // negative value ("--levels -18,-12,-6") breaks command dispatch (it silently falls
    // back to the help screen). Merge "--opt <neg-number>" into "--opt=<neg-number>" up
    // front — except for boolean flags — so both spelling styles work.
    static const juce::StringArray booleanFlags { "--resume", "--dry-run" };
    juce::StringArray raw;
    for (int i = 1; i < argc; ++i)
        raw.add (juce::String (argv[i]));

    juce::StringArray merged;
    for (int i = 0; i < raw.size(); ++i)
    {
        const auto& t = raw[i];
        if (t.startsWith ("--") && ! t.containsChar ('=') && ! booleanFlags.contains (t)
            && i + 1 < raw.size() && looksLikeNegativeNumber (raw[i + 1]))
        {
            merged.add (t + "=" + raw[i + 1]);
            ++i;
        }
        else
        {
            merged.add (t);
        }
    }

    return app.findAndRunCommand (juce::ArgumentList (juce::String (argv[0]), merged));
}
