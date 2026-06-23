#include "DcPack.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>

namespace statebox::capture
{

namespace
{
juce::String cellTag (const CaptureKernels& c)
{
    return "L" + juce::String (c.levelIndex)
         + "_K" + juce::String (c.knobIndex)
         + "_ch" + juce::String (c.channel);
}

juce::String linearRel (const CaptureKernels& c)
{
    return "kernels/" + cellTag (c) + ".wav";
}

juce::String harmonicRel (const CaptureKernels& c, int order)
{
    return "harmonics/h" + juce::String (order) + "_" + cellTag (c) + ".wav";
}

int orderForIndex (const CaptureProfile& p, size_t h)
{
    return h < p.harmonicOrders.size() ? p.harmonicOrders[h] : (int) (h + 2);
}

// Kernels are stored as 32-bit float WAV so values outside [-1, 1] survive exactly.
bool writeWav (const juce::File& f, const std::vector<float>& data, double sr, std::string* err)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();

    auto stream = f.createOutputStream();
    if (stream == nullptr)
    {
        if (err) *err = "cannot open " + f.getFullPathName().toStdString();
        return false;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), sr, 1u, 32, {}, 0));
    if (writer == nullptr)
    {
        if (err) *err = "cannot create WAV writer";
        return false;
    }
    stream.release(); // writer owns the stream now

    juce::AudioBuffer<float> buf (1, (int) data.size());
    for (int i = 0; i < (int) data.size(); ++i)
        buf.setSample (0, i, data[(size_t) i]);

    const bool ok = writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    if (! ok && err) *err = "WAV write failed";
    return ok;
}

bool readWav (const juce::File& f, std::vector<float>& out, double* sampleRateOut, std::string* err)
{
    if (! f.existsAsFile())
    {
        if (err) *err = "missing " + f.getFullPathName().toStdString();
        return false;
    }

    juce::WavAudioFormat wav;
    auto inStream = f.createInputStream();
    if (inStream == nullptr)
    {
        if (err) *err = "cannot open " + f.getFullPathName().toStdString();
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (inStream.release(), true));
    if (reader == nullptr)
    {
        if (err) *err = "cannot read WAV " + f.getFullPathName().toStdString();
        return false;
    }

    if (sampleRateOut != nullptr)
        *sampleRateOut = reader->sampleRate;

    const int n = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buf ((int) juce::jmax (1u, reader->numChannels), juce::jmax (1, n));
    reader->read (&buf, 0, n, 0, true, false);

    out.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        out[(size_t) i] = buf.getSample (0, i);
    return true;
}

juce::var axisToVar (const ProfileAxis& a)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", juce::String (a.name));
    o->setProperty ("unit", juce::String (a.unit));
    juce::Array<juce::var> vals;
    for (const float v : a.values) vals.add (v);
    o->setProperty ("values", vals);
    return juce::var (o);
}

void axisFromVar (const juce::var& v, ProfileAxis& a)
{
    a.name = v.getProperty ("name", "").toString().toStdString();
    a.unit = v.getProperty ("unit", "").toString().toStdString();
    a.values.clear();
    if (auto* arr = v.getProperty ("values", juce::var()).getArray())
        for (auto& e : *arr) a.values.push_back ((float) e);
}
} // namespace

bool writeWavFile (const std::string& path, const std::vector<float>& data, double sampleRate, std::string* error)
{
    return writeWav (juce::File { juce::String (path) }, data, sampleRate, error);
}

bool readWavFile (const std::string& path, std::vector<float>& out, double* sampleRateOut, std::string* error)
{
    return readWav (juce::File { juce::String (path) }, out, sampleRateOut, error);
}

void normalizeKernels (CaptureKernels& c, float targetPeak)
{
    float peak = 0.0f;
    for (const float s : c.linear) peak = juce::jmax (peak, std::abs (s));

    if (peak <= 0.0f) { c.gain = 1.0f; return; }

    const float g = targetPeak / peak;
    for (auto& s : c.linear) s *= g;
    for (auto& h : c.harmonics)
        for (auto& s : h) s *= g;
    c.gain = g;
}

void normalizeProfile (CaptureProfile& profile, float targetPeak)
{
    for (auto& c : profile.cells)
        normalizeKernels (c, targetPeak);
}

bool writeDcPack (const CaptureProfile& profile, const std::string& dirPath, std::string* error)
{
    juce::File dir { juce::String (dirPath) };
    if (! dir.createDirectory())
    {
        if (error) *error = "cannot create directory " + dirPath;
        return false;
    }

    auto*     root = new juce::DynamicObject();
    juce::var rootVar (root);

    root->setProperty ("formatVersion", profile.formatVersion);
    root->setProperty ("name", juce::String (profile.name));
    root->setProperty ("vendor", juce::String (profile.vendor));
    root->setProperty ("sampleRate", profile.sampleRate);
    root->setProperty ("bitDepth", profile.bitDepth);
    root->setProperty ("kernelLengthSamples", profile.kernelLengthSamples);
    root->setProperty ("channels", profile.channels);
    root->setProperty ("levelAxis", axisToVar (profile.levelAxis));
    root->setProperty ("knobAxis", axisToVar (profile.knobAxis));

    auto* norm = new juce::DynamicObject();
    norm->setProperty ("referenceDbfs", profile.referenceDbfs);
    norm->setProperty ("trimMode", juce::String (profile.trimMode));
    root->setProperty ("normalization", juce::var (norm));

    root->setProperty ("latencyReferenceSamples", profile.latencyReferenceSamples);

    juce::Array<juce::var> orders;
    for (const int o : profile.harmonicOrders) orders.add (o);
    root->setProperty ("harmonicOrders", orders);
    root->setProperty ("createdWith", juce::String (profile.createdWith));

    juce::Array<juce::var> cells;
    for (const auto& c : profile.cells)
    {
        if (! writeWav (dir.getChildFile (linearRel (c)), c.linear, profile.sampleRate, error))
            return false;

        juce::Array<juce::var> harmonicFiles;
        for (size_t h = 0; h < c.harmonics.size(); ++h)
        {
            const int order = orderForIndex (profile, h);
            if (! writeWav (dir.getChildFile (harmonicRel (c, order)), c.harmonics[h], profile.sampleRate, error))
                return false;
            harmonicFiles.add (harmonicRel (c, order));
        }

        auto* co = new juce::DynamicObject();
        co->setProperty ("level", c.levelIndex);
        co->setProperty ("knob", c.knobIndex);
        co->setProperty ("channel", c.channel);
        co->setProperty ("gain", c.gain);
        co->setProperty ("linearFile", linearRel (c));
        co->setProperty ("harmonicFiles", harmonicFiles);
        cells.add (juce::var (co));
    }
    root->setProperty ("cells", cells);

    if (! dir.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (rootVar)))
    {
        if (error) *error = "cannot write manifest.json";
        return false;
    }
    return true;
}

bool readDcPack (const std::string& dirPath, CaptureProfile& out, std::string* error)
{
    juce::File dir { juce::String (dirPath) };
    juce::File manifest = dir.getChildFile ("manifest.json");
    if (! manifest.existsAsFile())
    {
        if (error) *error = "no manifest.json in " + dirPath;
        return false;
    }

    const juce::var root = juce::JSON::parse (manifest.loadFileAsString());
    if (! root.isObject())
    {
        if (error) *error = "invalid manifest JSON";
        return false;
    }

    out = CaptureProfile{};
    out.formatVersion       = (int) root.getProperty ("formatVersion", 1);
    out.name                = root.getProperty ("name", "").toString().toStdString();
    out.vendor              = root.getProperty ("vendor", "").toString().toStdString();
    out.sampleRate          = (double) root.getProperty ("sampleRate", 48000.0);
    out.bitDepth            = (int) root.getProperty ("bitDepth", 24);
    out.kernelLengthSamples = (int) root.getProperty ("kernelLengthSamples", 0);
    out.channels            = (int) root.getProperty ("channels", 1);
    axisFromVar (root.getProperty ("levelAxis", juce::var()), out.levelAxis);
    axisFromVar (root.getProperty ("knobAxis", juce::var()), out.knobAxis);

    const juce::var norm = root.getProperty ("normalization", juce::var());
    out.referenceDbfs           = (float) norm.getProperty ("referenceDbfs", -18.0f);
    out.trimMode                = norm.getProperty ("trimMode", "").toString().toStdString();
    out.latencyReferenceSamples = (int) root.getProperty ("latencyReferenceSamples", 0);

    if (auto* arr = root.getProperty ("harmonicOrders", juce::var()).getArray())
        for (auto& e : *arr) out.harmonicOrders.push_back ((int) e);
    out.createdWith = root.getProperty ("createdWith", "").toString().toStdString();

    if (auto* arr = root.getProperty ("cells", juce::var()).getArray())
    {
        for (auto& cv : *arr)
        {
            CaptureKernels c;
            c.levelIndex = (int) cv.getProperty ("level", 0);
            c.knobIndex  = (int) cv.getProperty ("knob", 0);
            c.channel    = (int) cv.getProperty ("channel", 0);
            c.gain       = (float) cv.getProperty ("gain", 1.0f);

            if (! readWav (dir.getChildFile (cv.getProperty ("linearFile", "").toString()), c.linear, nullptr, error))
                return false;

            if (auto* hfs = cv.getProperty ("harmonicFiles", juce::var()).getArray())
            {
                for (auto& hv : *hfs)
                {
                    std::vector<float> h;
                    if (! readWav (dir.getChildFile (hv.toString()), h, nullptr, error))
                        return false;
                    c.harmonics.push_back (std::move (h));
                }
            }
            out.cells.push_back (std::move (c));
        }
    }
    return true;
}

} // namespace statebox::capture
