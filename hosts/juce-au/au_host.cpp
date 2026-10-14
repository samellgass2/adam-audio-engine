// adam-au-host: load an INSTALLED Audio Unit through the real plugin API (the
// way Logic does), play a pattern into it, render a WAV. Used to prove the AU
// sounds the same as adam-render / the workshop.
//
//   adam-au-host --au aumu,AdCs,Adam --pattern P --preset F --seconds 8
//                --tail 0 --block 128 --sr 48000 --out out/au.wav
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "adam/seq/sequencer.hpp"
#include "params.hpp"

namespace cs = adam::core_synth;

static std::string readFile(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::string au = "aumu,AdCs,Adam", pattern, preset, out;
    double seconds = 8, tail = 0, sr = 48000;
    int block = 128;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--au") au = v;
        else if (k == "--pattern") pattern = v;
        else if (k == "--preset") preset = v;
        else if (k == "--seconds") seconds = std::atof(v.c_str());
        else if (k == "--tail") tail = std::atof(v.c_str());
        else if (k == "--sr") sr = std::atof(v.c_str());
        else if (k == "--block") block = std::atoi(v.c_str());
        else if (k == "--out") out = v;
    }
    if (pattern.empty() || out.empty()) {
        std::fprintf(stderr, "usage: adam-au-host --pattern P [--preset F] --out W [--au type,sub,manu]\n");
        return 1;
    }

    juce::AudioUnitPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    // "type,subtype,manufacturer" -> AudioUnit identifier string
    juce::StringArray codes = juce::StringArray::fromTokens(au, ",", "");
    format.findAllTypesForFile(found, "AudioUnit:Synths/" + codes.joinIntoString(","));
    if (found.isEmpty()) {
        std::fprintf(stderr, "adam-au-host: AU %s not found (installed + registered?)\n", au.c_str());
        return 2;
    }
    juce::String error;
    auto plugin = format.createInstanceFromDescription(*found[0], sr, block, error);
    if (!plugin) {
        std::fprintf(stderr, "adam-au-host: cannot instantiate: %s\n", error.toRawUTF8());
        return 2;
    }
    std::printf("loaded %s (%s)\n", found[0]->name.toRawUTF8(), found[0]->fileOrIdentifier.toRawUTF8());

    // Preset -> host parameters by display name, normalized the way the
    // plugin's own parameters are (engine mapping for floats, index/(n-1) for
    // choices, linear for ints).
    std::map<std::string, juce::AudioProcessorParameter*> byName;
    for (auto* p : plugin->getParameters()) byName[p->getName(128).toStdString()] = p;
    int applied = 0;
    if (!preset.empty()) {
        std::istringstream lines(readFile(preset));
        std::string line;
        while (std::getline(lines, line)) {
            if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
            char id[96];
            float v;
            if (std::sscanf(line.c_str(), "%95s %f", id, &v) != 2) continue;
            const int idx = cs::paramIndexForId(id);
            if (idx < 0) continue;
            const auto pid = static_cast<cs::ParamId>(idx);
            const auto& spec = cs::paramSpec(pid);
            auto it = byName.find(spec.name);
            if (it == byName.end()) {
                std::fprintf(stderr, "adam-au-host: AU has no parameter named %s\n", spec.name);
                return 3;
            }
            const float phys = cs::clampPhysical(pid, v);
            float n = cs::toNormalized(pid, phys);
            if (spec.mapping == cs::Mapping::Choice) n = (phys - spec.min) / (spec.max - spec.min);
            it->second->setValue(n);
            ++applied;
        }
    }
    std::printf("applied %d preset params; plugin has %d parameters, latency %d samples\n", applied,
                (int)plugin->getParameters().size(), plugin->getLatencySamples());

    plugin->setPlayConfigDetails(0, 2, sr, block);
    plugin->prepareToPlay(sr, block);

    adam::seq::Sequencer seq;
    if (seq.load(readFile(pattern).c_str()) <= 0) return 4;
    seq.setBpm(seq.patternBpm());
    seq.setLoop(true);
    seq.setPlaying(true);

    const long playFrames = static_cast<long>(seconds * sr);
    const long total = playFrames + static_cast<long>(tail * sr);
    juce::AudioBuffer<float> all(2, static_cast<int>(total));
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    std::set<int> held;
    for (long done = 0; done < total; done += block) {
        const int n = static_cast<int>(std::min<long>(block, total - done));
        midi.clear();
        if (done >= playFrames && seq.playing()) {
            seq.setPlaying(false);  // the tail: release what is held
            for (int note : held) midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
            held.clear();
        }
        seq.advance(n, sr, [&](int off, int note, int vel) {
            if (vel > 0) {
                midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(vel)), off);
                held.insert(note);
            } else {
                midi.addEvent(juce::MidiMessage::noteOff(1, note), off);
                held.erase(note);
            }
        });
        buf.setSize(2, n, false, false, true);
        buf.clear();
        plugin->processBlock(buf, midi);
        for (int c = 0; c < 2; ++c) all.copyFrom(c, static_cast<int>(done), buf, c, 0, n);
    }
    plugin->releaseResources();

    juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(out);
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    juce::AudioFormatWriterOptions opts;
    auto writer = wav.createWriterFor(stream, opts.withSampleRate(sr).withNumChannels(2).withBitsPerSample(32)
                                                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (!writer) {
        std::fprintf(stderr, "adam-au-host: cannot write %s\n", out.c_str());
        return 5;
    }
    writer->writeFromAudioSampleBuffer(all, 0, all.getNumSamples());
    writer.reset();
    std::printf("wrote %s (%.2f s)\n", file.getFullPathName().toRawUTF8(), total / sr);
    return 0;
}
