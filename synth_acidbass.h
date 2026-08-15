/* Audio Library for Teensy - TB-303 style monophonic acid bass voice
 * Copyright (c) 2026, Nic Newdigate
 *
 * Ported from the mulch project's AcidVoice (same author, MIT). Two deliberate
 * divergences, both in note handling -- the per-sample DSP is bit-exact:
 *
 *  1. noteOff snaps pitch/glide state only when the SOUNDING note is released;
 *     mulch snapped unconditionally, which killed an in-progress glide when a
 *     sequencer released the old note right after a legato press.
 *  2. noteOn removes any existing entry for the same pitch before pushing it.
 *     mulch pushed unconditionally while noteOff erased a single match, so two
 *     noteOn(60) with no noteOff between them left an entry that could never be
 *     released -- the gate never cleared and the voice droned forever.
 *     Retriggering a still-held pitch is ordinary sequencer behaviour.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef synth_acidbass_h_
#define synth_acidbass_h_

#include <Arduino.h>     // github.com/PaulStoffregen/cores/blob/master/teensy4/Arduino.h
#include <AudioStream.h> // github.com/PaulStoffregen/cores/blob/master/teensy4/AudioStream.h
#include <math.h>
#include "synth_waveform.h"   // WAVEFORM_SAWTOOTH / WAVEFORM_SQUARE constants

// Compact 4-pole (24 dB/oct) resonant low-pass -- the classic "simplified Moog"
// ladder (Stilson/Smith). State only; the recurrence lives in
// synth_acidbass.cpp, which also carries the note on why this voice does not
// reuse the library's AudioFilterLadder.
//
// Declared here rather than in the .cpp only because AudioSynthAcidBass holds
// one BY VALUE, so the type must be complete in this header.
struct AcidLadder {
	float s1 = 0, s2 = 0, s3 = 0, s4 = 0;   // stage outputs
	float d1 = 0, d2 = 0, d3 = 0, d4 = 0;   // one-sample delays
	// res in [0,1]; self-oscillates near 1. Cutoff is per-sample, not per-block.
	float process(float in, float cutoffHz, float res);
};

// Monophonic 303-style voice: saw/square VCO + square sub-osc (-1 oct)
// -> Stilson/Smith 4-pole ladder (env/accent/keytrack/filterFM modulated)
// -> VCA -> bounded tanh distortion. Last-note priority; legato slide does
// not retrigger the filter envelope; accent drives both filter and VCA.
class AudioSynthAcidBass : public AudioStream
{
public:
	AudioSynthAcidBass() : AudioStream(0, NULL) { updateCoefs(); }

	// USER CONTEXT ONLY -- both take a critical section and end with an
	// unconditional __enable_irq(), so calling either from an ISR, or from
	// inside another critical section, would re-enable interrupts early.
	// (Same house rule as synth_simple_drum.cpp / effect_envelope.cpp.)
	void noteOn(uint8_t note, uint8_t velocity, bool slide = false);
	void noteOff(uint8_t note);

	// WAVEFORM_SAWTOOTH and WAVEFORM_SQUARE are the only two shapes this voice
	// has; every other WAVEFORM_* constant silently selects the sawtooth.
	void waveform(short type) { waveform_ = (type == WAVEFORM_SQUARE) ? 1 : 0; }
	void cutoff(float hz)     { cutoff_ = hz < 20.0f ? 20.0f : (hz > 12000.0f ? 12000.0f : hz); }
	void resonance(float r)   { resonance_ = clamp01(r); }
	void envMod(float a)      { envMod_ = clamp01(a); }
	void decay(float s)       { decay_ = s < 1e-3f ? 1e-3f : s; decayCoef_ = decayCoefOf(decay_); }
	void accent(float a)      { accent_ = clamp01(a); }
	void subLevel(float a)    { subLevel_ = clamp01(a); }
	void slideTime(float s)   { slideTime_ = s < 1e-3f ? 1e-3f : s; glideCoef_ = lagCoefOf(slideTime_); }
	void filterFM(float a)    { filterFM_ = clamp01(a); }
	void keyTrack(float a)    { keyTrack_ = clamp01(a); }
	void distortion(float a)  { distortion_ = clamp01(a); }
	void level(float a)       { level_ = clamp01(a); }

	float currentFreq() const { return curFreq_; }   // test hook: glide
	float filtEnv() const     { return filtEnv_; }   // test hook: envelope

	virtual void update(void);

private:
	void updateCoefs();          // ALL four coefficients -- constructor only
	static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
	static float midiToFreq(uint8_t n) { return 440.0f * powf(2.0f, ((int)n - 69) / 12.0f); }
	// one-pole coefficients from a time constant in seconds
	static float decayCoefOf(float tau) { return expf(-1.0f / (tau * AUDIO_SAMPLE_RATE_EXACT)); }
	static float lagCoefOf(float tau)   { return 1.0f - expf(-1.0f / (tau * AUDIO_SAMPLE_RATE_EXACT)); }

	// parameters -- bare 32-bit aligned stores are atomic on Cortex-M7, so the
	// float setters need no lock against the audio ISR.
	//
	// THE INVARIANT THAT MAKES THAT TRUE FOR DERIVED COEFFICIENTS: every setter
	// writes at most ONE coefficient, so a preempting update() sees each one
	// either wholly old or wholly new, and never a torn set. decay() and
	// slideTime() therefore recompute a single coefficient inline instead of
	// calling updateCoefs(), which writes all four and is CONSTRUCTOR-ONLY --
	// at construction nothing can preempt it. If you add an attack()/release()
	// setter, give it the same one-store discipline; do not reach for
	// updateCoefs().
	uint8_t waveform_  = 0;      // 0 = saw, 1 = square
	float cutoff_      = 800.0f;
	float resonance_   = 0.7f;
	float envMod_      = 0.6f;
	float decay_       = 0.3f;
	float accent_      = 0.4f;
	float subLevel_    = 0.0f;
	float slideTime_   = 0.08f;
	float filterFM_    = 0.0f;
	float keyTrack_    = 0.0f;
	float distortion_  = 0.0f;
	float level_       = 0.7f;   // output gain (post-distortion volume trim)
	// derived coefficients
	float decayCoef_   = 0.0f;
	float glideCoef_   = 0.0f;
	float attackCoef_  = 0.0f;
	float releaseCoef_ = 0.0f;
	// note state: multi-word, mutated together -> noteOn/noteOff run in user
	// context and wrap themselves in __disable_irq() against update()
	static const int kMaxHeld = 8;
	uint8_t held_[kMaxHeld];     // held notes, [heldCount_-1] = sounding
	uint8_t heldCount_ = 0;
	uint8_t curNote_ = 60;
	uint8_t curVel_  = 100;
	bool    gateOn_  = false;
	bool    gliding_ = false;
	double  phase_    = 0.0;
	double  subPhase_ = 0.0;
	float   curFreq_    = 0.0f;
	float   targetFreq_ = 0.0f;
	float   filtEnv_ = 0.0f;
	float   ampEnv_  = 0.0f;
	float   lastOut_ = 0.0f;
	AcidLadder filter_;
};

#endif
