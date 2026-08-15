/* Audio Library for Teensy - TB-303 style monophonic acid bass voice
 * Copyright (c) 2026, Nic Newdigate
 *
 * Ported from the mulch project's AcidVoice (same author, MIT). One deliberate
 * divergence: noteOff snaps pitch/glide state only when the SOUNDING note is
 * released; mulch snapped unconditionally, which killed an in-progress glide
 * when a sequencer released the old note right after a legato press.
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

// Monophonic 303-style voice: saw/square VCO + square sub-osc (-1 oct)
// -> Stilson/Smith 4-pole ladder (env/accent/keytrack/filterFM modulated)
// -> VCA -> bounded tanh distortion. Last-note priority; legato slide does
// not retrigger the filter envelope; accent drives both filter and VCA.
class AudioSynthAcidBass : public AudioStream
{
public:
	AudioSynthAcidBass() : AudioStream(0, NULL) { updateCoefs(); }

	void noteOn(uint8_t note, uint8_t velocity, bool slide = false);
	void noteOff(uint8_t note);

	void waveform(short type) { waveform_ = (type == WAVEFORM_SQUARE) ? 1 : 0; }
	void cutoff(float hz)     { cutoff_ = hz < 20.0f ? 20.0f : (hz > 12000.0f ? 12000.0f : hz); }
	void resonance(float r)   { resonance_ = clamp01(r); }
	void envMod(float a)      { envMod_ = clamp01(a); }
	void decay(float s)       { decay_ = s < 1e-3f ? 1e-3f : s; updateCoefs(); }
	void accent(float a)      { accent_ = clamp01(a); }
	void subLevel(float a)    { subLevel_ = clamp01(a); }
	void slideTime(float s)   { slideTime_ = s < 1e-3f ? 1e-3f : s; updateCoefs(); }
	void filterFM(float a)    { filterFM_ = clamp01(a); }
	void keyTrack(float a)    { keyTrack_ = clamp01(a); }
	void distortion(float a)  { distortion_ = clamp01(a); }
	void level(float a)       { level_ = clamp01(a); }

	float currentFreq() const { return curFreq_; }   // test hook: glide
	float filtEnv() const     { return filtEnv_; }   // test hook: envelope

	virtual void update(void);

private:
	// Compact 4-pole (24 dB/oct) resonant low-pass -- the classic "simplified
	// Moog" ladder (Stilson/Smith). tanh on the feedback tap saturates
	// self-oscillation: BIBO-stable even at res = 1 under hard drive, with a
	// gentle "transistor" character; near-linear for small signals.
	struct Ladder {
		float s1 = 0, s2 = 0, s3 = 0, s4 = 0;   // stage outputs
		float d1 = 0, d2 = 0, d3 = 0, d4 = 0;   // one-sample delays
		float process(float in, float cutoffHz, float res) {
			float fc = cutoffHz / (0.5f * AUDIO_SAMPLE_RATE_EXACT);
			if (fc < 0.0f) fc = 0.0f;
			if (fc > 0.99f) fc = 0.99f;
			float f  = fc * 1.16f;
			float fb = res * 4.0f * (1.0f - 0.15f * f * f);
			float x  = in - tanhf(s4) * fb;
			x *= 0.35013f * (f * f) * (f * f);
			s1 = x  + 0.3f * d1 + (1.0f - f) * s1;  d1 = x;
			s2 = s1 + 0.3f * d2 + (1.0f - f) * s2;  d2 = s1;
			s3 = s2 + 0.3f * d3 + (1.0f - f) * s3;  d3 = s2;
			s4 = s3 + 0.3f * d4 + (1.0f - f) * s4;  d4 = s3;
			return s4;
		}
	};

	void updateCoefs();
	static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
	static float midiToFreq(uint8_t n) { return 440.0f * powf(2.0f, ((int)n - 69) / 12.0f); }

	// parameters -- bare 32-bit aligned stores are atomic on Cortex-M7, so the
	// float setters need no lock against the audio ISR
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
	Ladder  filter_;
};

#endif
