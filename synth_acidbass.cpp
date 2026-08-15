/* Audio Library for Teensy - TB-303 style monophonic acid bass voice
 * Copyright (c) 2026, Nic Newdigate
 * MIT license -- see synth_acidbass.h for the full text.
 */

#include "synth_acidbass.h"

// Why a bespoke ladder rather than the library's AudioFilterLadder: that class
// is an AudioStream node, so its cutoff can only be re-aimed once per
// AUDIO_BLOCK_SAMPLES-sample block (or tracked from a second audio-rate input,
// which costs a whole extra graph connection and block of memory). The 303
// sound is the filter envelope sweeping the cutoff PER SAMPLE, and the
// filter-FM path feeds the VCA output back into the cutoff with one sample of
// delay -- neither survives block-rate control. So the recurrence is inlined
// into this voice, where the cutoff is a plain argument.
//
// tanh on the feedback tap saturates self-oscillation: BIBO-stable even at
// res = 1 under hard drive, with a gentle "transistor" character; near-linear
// for small signals.
float AcidLadder::process(float in, float cutoffHz, float res) {
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

// Constructor only -- writes all four coefficients. See the invariant comment
// in the header: the runtime setters each write exactly one.
void AudioSynthAcidBass::updateCoefs() {
	decayCoef_   = decayCoefOf(decay_);
	glideCoef_   = lagCoefOf(slideTime_);
	attackCoef_  = lagCoefOf(0.003f);   // ~3 ms
	releaseCoef_ = lagCoefOf(0.008f);   // ~8 ms
}

// The `> 127` clamps below are NOT dead code: uint8_t spans 0..255, so a caller
// can hand us 128..255. Nothing there is unsafe -- note 255 is a finite ~20 MHz,
// which merely aliases into noise -- but the clamp keeps the parameter domain
// equal to MIDI's, and, more importantly, noteOn and noteOff MUST clamp
// identically: if noteOn pushed 127 while noteOff searched for 200, the entry
// would never leave held_ and the voice would gate on forever.
void AudioSynthAcidBass::noteOn(uint8_t note, uint8_t velocity, bool slide) {
	if (note > 127) note = 127;
	if (velocity > 127) velocity = 127;   // accentAmt stays within [0, accent_]
	// computed BEFORE the critical section: powf is ~360 ns, and the tree's
	// USB/eDMA/MU ISRs would otherwise eat that latency on every note event
	const float freq = midiToFreq(note);
	__disable_irq();
	// Drop any entry this pitch already has, so a retrigger of a still-held
	// note cannot leave a duplicate that noteOff can never fully release
	// (divergence 2 from mulch, see the header). Re-pushing below also gives
	// the retriggered pitch last-note priority, which is what a player expects.
	// This scan runs forward while noteOff's runs backward; the two are
	// equivalent only because this dedup keeps held_ free of duplicates.
	// Ordering matters too: dedup must precede the kMaxHeld eviction below, or
	// retriggering a held pitch on a full stack would evict an unrelated note.
	for (int i = 0; i < heldCount_; i++) {
		if (held_[i] == note) {
			for (int j = i + 1; j < heldCount_; j++) held_[j - 1] = held_[j];
			heldCount_--;
			break;
		}
	}
	if (heldCount_ == kMaxHeld) {        // full: drop the oldest held note
		for (int i = 1; i < kMaxHeld; i++) held_[i - 1] = held_[i];
		heldCount_--;
	}
	held_[heldCount_++] = note;
	curNote_ = note;
	curVel_  = velocity;
	targetFreq_ = freq;
	if (slide && gateOn_) {
		gliding_ = true;                 // legato glide; do NOT retrigger
	} else {
		curFreq_ = targetFreq_;          // jump
		gliding_ = false;
		filtEnv_ = 1.0f;                 // retrigger the filter envelope
	}
	gateOn_ = true;
	__enable_irq();
}

void AudioSynthAcidBass::noteOff(uint8_t note) {
	if (note > 127) note = 127;          // must match the noteOn clamp
	__disable_irq();
	bool removedTop = false;
	for (int i = heldCount_ - 1; i >= 0; i--) {
		if (held_[i] == note) {          // most recent matching entry
			removedTop = (i == heldCount_ - 1);
			for (int j = i + 1; j < heldCount_; j++) held_[j - 1] = held_[j];
			heldCount_--;
			break;
		}
	}
	if (heldCount_ == 0) {
		gateOn_ = false;                 // amp env releases to silence
	} else if (removedTop) {
		curNote_ = held_[heldCount_ - 1];   // fall back to the still-held note
		targetFreq_ = midiToFreq(curNote_);
		curFreq_ = targetFreq_;
		gliding_ = false;
	}
	// releasing a note BELOW the top leaves the sounding note -- and any
	// in-progress glide toward it -- untouched (divergence 1 from mulch)
	__enable_irq();
}

void AudioSynthAcidBass::update(void) {
	audio_block_t *block = allocate();
	if (!block) {
		// Under memory pressure the audio is lost either way, but time must not
		// stop: freezing here would make this voice's time base "blocks
		// successfully allocated" rather than wall clock, so a glide or a decay
		// would pause and resume mid-flight. Advance one block in closed form
		// and drop the samples -- same convention as synth_waveform.cpp's
		// `phase_accumulator += inc * AUDIO_BLOCK_SAMPLES`.
		//
		// The envelope and glide forms are exact (they are plain one-poles).
		// Phase is the one approximation: mid-glide it advances at the
		// END-of-block frequency instead of integrating the ramp. The ramp
		// question does not arise in synth_waveform.cpp -- its fallback
		// frequency is constant, so only the block-advance convention carries
		// over, not a precedent for this. The error is inaudible regardless:
		// the block it would have coloured is the one being dropped.
		const float n = AUDIO_BLOCK_SAMPLES;
		if (gliding_) {
			curFreq_ = targetFreq_ + (curFreq_ - targetFreq_) * powf(1.0f - glideCoef_, n);
			if (fabsf(targetFreq_ - curFreq_) < 0.01f) { curFreq_ = targetFreq_; gliding_ = false; }
		}
		phase_    += (double)curFreq_ * n / AUDIO_SAMPLE_RATE_EXACT;
		subPhase_ += 0.5 * (double)curFreq_ * n / AUDIO_SAMPLE_RATE_EXACT;
		if (phase_ >= 1.0)    phase_    -= floor(phase_);
		if (subPhase_ >= 1.0) subPhase_ -= floor(subPhase_);
		filtEnv_ *= powf(decayCoef_, n);
		const float accentAmt = accent_ * (curVel_ / 127.0f);
		const float ampTarget = gateOn_ ? (1.0f + 0.5f * accentAmt) : 0.0f;
		// The direction cannot flip inside a block: ampEnv_ approaches
		// ampTarget asymptotically and never crosses it, so one coefficient
		// covers all n samples.
		const float c = (ampTarget > ampEnv_) ? attackCoef_ : releaseCoef_;
		ampEnv_ = ampTarget + (ampEnv_ - ampTarget) * powf(1.0f - c, n);
		return;
	}

	const float ENV_OCT = 4.0f, FM_OCT = 2.0f;
	const float sr  = AUDIO_SAMPLE_RATE_EXACT;
	const float nyq = 0.45f * sr;
	const float accentAmt = accent_ * (curVel_ / 127.0f);
	const float keyF = powf(2.0f, keyTrack_ * ((int)curNote_ - 60) / 12.0f);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		if (gliding_) {
			curFreq_ += (targetFreq_ - curFreq_) * glideCoef_;
			if (fabsf(targetFreq_ - curFreq_) < 0.01f) { curFreq_ = targetFreq_; gliding_ = false; }
		}
		phase_    += curFreq_ / sr;        if (phase_ >= 1.0)    phase_    -= floor(phase_);
		subPhase_ += 0.5 * curFreq_ / sr;  if (subPhase_ >= 1.0) subPhase_ -= floor(subPhase_);
		float oscMain = (waveform_ == 0) ? (float)(2.0 * phase_ - 1.0)
		                                 : (phase_ < 0.5 ? 1.0f : -1.0f);
		float sub  = (subPhase_ < 0.5 ? 1.0f : -1.0f) * subLevel_;
		float osc  = oscMain + sub;

		filtEnv_ *= decayCoef_;
		float ampTarget = gateOn_ ? (1.0f + 0.5f * accentAmt) : 0.0f;
		ampEnv_ += (ampTarget - ampEnv_) * (ampTarget > ampEnv_ ? attackCoef_ : releaseCoef_);

		float modOct = (envMod_ + accentAmt) * filtEnv_ * ENV_OCT + filterFM_ * lastOut_ * FM_OCT;
		float fcHz = cutoff_ * keyF * powf(2.0f, modOct);
		if (fcHz < 20.0f) fcHz = 20.0f;
		if (fcHz > nyq)   fcHz = nyq;

		float filtered = filter_.process(osc, fcHz, resonance_);
		float s = filtered * ampEnv_;
		lastOut_ = tanhf(s);   // bounded VCA tap -> stable filter-FM feedback
		float y = level_ * tanhf(s * (1.0f + distortion_ * 9.0f));
		int32_t v = (int32_t)(y * 32767.0f);
		if (v >  32767) v =  32767;      // belt-and-braces; tanh already bounds y
		if (v < -32768) v = -32768;
		block->data[i] = (int16_t)v;
	}
	transmit(block);
	release(block);
}
