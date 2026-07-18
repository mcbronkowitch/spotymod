#include "workload.h"
#include "daisysp.h"
#include "synth/morph_osc.h"

namespace bench {
namespace {

using namespace daisysp;

// Every candidate runs as ONE sounding voice at one pitch, retriggered often
// enough that it never falls silent inside the measured window. Answers the
// engine-expansion research's open question 1 with cycles instead of analogies.
constexpr float kFreq = 220.f;
constexpr int   kTrigEvery = 12000;   // 0.25 s

int g_ctr = 0;

// --- component reference: ONE BARE MorphOsc ---------------------------------
// NOT the anchor for "our voice" -- read the note below. This row exists to
// price a single oscillator kernel, so the six Plaits oscillator candidates
// (which are also bare kernels) have a like-for-like comparison.
//
// A real spotymod Voice is ~7.3x this: engine/synth/voice.cpp drives TWO
// MorphOsc instances in unison plus a sub-oscillator, an SVF and an envelope.
// The decision-relevant anchor is family 1's `synth_1_voice` row, which
// measures exactly that full pipeline. Ratios that steer engine selection are
// computed against THAT, not against this row.
spky::MorphOsc g_morph;

void setup_morph()
{
    g_morph.init(kSampleRate);
    g_morph.set_freq(kFreq);
    g_morph.set_morph(0.5f);
}
float proc_morph()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_morph.process();
    return acc;
}

// --- physical modelling -----------------------------------------------------
ModalVoice  g_modal;
StringVoice g_string;
Resonator   g_reso;

void setup_modal()
{
    g_modal.Init(kSampleRate);
    g_modal.SetFreq(kFreq);
    g_modal.SetStructure(0.6f);
    g_modal.SetBrightness(0.6f);
    g_modal.SetDamping(0.5f);
    g_ctr = 0;
}
float proc_modal()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        if (++g_ctr >= kTrigEvery) { g_ctr = 0; g_modal.Trig(); }
        acc += g_modal.Process();
    }
    return acc;
}

void setup_string()
{
    g_string.Init(kSampleRate);
    g_string.SetFreq(kFreq);
    g_string.SetBrightness(0.6f);
    g_string.SetDamping(0.6f);
    g_ctr = 0;
}
float proc_string()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        if (++g_ctr >= kTrigEvery) { g_ctr = 0; g_string.Trig(); }
        acc += g_string.Process();
    }
    return acc;
}

void setup_reso()
{
    g_reso.Init(0.3f, 24, kSampleRate);
    g_reso.SetFreq(kFreq);
    g_reso.SetStructure(0.6f);
    g_reso.SetBrightness(0.6f);
    g_reso.SetDamping(0.5f);
    g_ctr = 0;
}
float proc_reso()
{
    // Resonator is excited, not triggered: feed it a short impulse train.
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float ex = 0.f;
        if (++g_ctr >= kTrigEvery) { g_ctr = 0; ex = 1.f; }
        acc += g_reso.Process(ex);
    }
    return acc;
}

// --- Plaits-derived oscillator kernels --------------------------------------
FormantOscillator       g_formant;
VosimOscillator         g_vosim;
HarmonicOscillator<16>  g_harm;
GrainletOscillator      g_grainlet;
ZOscillator             g_zosc;
VariableShapeOscillator g_varshape;

void setup_formant()
{
    g_formant.Init(kSampleRate);
    g_formant.SetFormantFreq(600.f);
    g_formant.SetCarrierFreq(kFreq);
    g_formant.SetPhaseShift(0.3f);
}
float proc_formant()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_formant.Process();
    return acc;
}

void setup_vosim()
{
    g_vosim.Init(kSampleRate);
    g_vosim.SetFreq(kFreq);
    g_vosim.SetForm1Freq(600.f);
    g_vosim.SetForm2Freq(900.f);
    g_vosim.SetShape(0.5f);
}
float proc_vosim()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_vosim.Process();
    return acc;
}

void setup_harm()
{
    g_harm.Init(kSampleRate);
    g_harm.SetFreq(kFreq);
    g_harm.SetFirstHarmIdx(1);
    for (int i = 0; i < 8; ++i) g_harm.SetSingleAmp(0.5f, i);
}
float proc_harm()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_harm.Process();
    return acc;
}

void setup_grainlet()
{
    g_grainlet.Init(kSampleRate);
    g_grainlet.SetFreq(kFreq);
    g_grainlet.SetFormantFreq(600.f);
    g_grainlet.SetShape(0.5f);
    g_grainlet.SetBleed(0.3f);
}
float proc_grainlet()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_grainlet.Process();
    return acc;
}

void setup_zosc()
{
    g_zosc.Init(kSampleRate);
    g_zosc.SetFreq(kFreq);
    g_zosc.SetFormantFreq(600.f);
    g_zosc.SetShape(0.5f);
    g_zosc.SetMode(0.5f);
}
float proc_zosc()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_zosc.Process();
    return acc;
}

void setup_varshape()
{
    g_varshape.Init(kSampleRate);
    g_varshape.SetFreq(kFreq);
    g_varshape.SetSync(false);
    g_varshape.SetPW(0.5f);
    g_varshape.SetWaveshape(0.5f);
}
float proc_varshape()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_varshape.Process();
    return acc;
}

} // namespace

const Workload kVoiceWorkloads[] = {
    { "voice", "morph_osc_bare",       setup_morph,    proc_morph    },
    { "voice", "modal_voice",          setup_modal,    proc_modal    },
    { "voice", "string_voice",         setup_string,   proc_string   },
    { "voice", "resonator",            setup_reso,     proc_reso     },
    { "voice", "formant_osc",          setup_formant,  proc_formant  },
    { "voice", "vosim_osc",            setup_vosim,    proc_vosim    },
    { "voice", "harmonic_osc",         setup_harm,     proc_harm     },
    { "voice", "grainlet_osc",         setup_grainlet, proc_grainlet },
    { "voice", "z_osc",                setup_zosc,     proc_zosc     },
    { "voice", "variable_shape_osc",   setup_varshape, proc_varshape },
};
const int kVoiceCount = sizeof(kVoiceWorkloads) / sizeof(kVoiceWorkloads[0]);

} // namespace bench
