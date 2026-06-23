// Statebox Capture Tool — scaffold.
//
// This entry point currently just verifies the JUCE toolchain links and that the
// modules the capture pipeline depends on (dsp/FFT, audio formats, audio buffers)
// are available. The real pipeline (ESS generation -> deconvolution -> h1 + h2/h3
// separation -> alignment -> normalization -> .dcpack) lands next, module by
// module, each with its own self-test. See CLAUDE.md §6 and §10.

#include <iostream>

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

int main()
{
    std::cout << "Statebox Capture Tool — scaffold OK\n";

    // juce_dsp: the FFT we will use for ESS deconvolution.
    juce::dsp::FFT fft (10); // order 10 -> 1024-point
    std::cout << "  juce_dsp     : FFT size " << fft.getSize() << '\n';

    // juce_audio_formats: WAV read/write for kernels and recordings.
    juce::WavAudioFormat wav;
    std::cout << "  audio_formats: " << wav.getFormatName() << '\n';

    // juce_audio_basics: the buffer type the whole pipeline passes around.
    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    std::cout << "  audio_basics : AudioBuffer " << buffer.getNumChannels()
              << " ch x " << buffer.getNumSamples() << " smp\n";

    return 0;
}
