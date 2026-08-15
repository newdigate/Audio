/* Audio Library for Teensy - TB-303 style monophonic acid bass voice
 * Copyright (c) 2026, Nic Newdigate
 * MIT license -- see synth_acidbass.h for the full text.
 */

#include "synth_acidbass.h"

void AudioSynthAcidBass::updateCoefs() {
	const float sr = AUDIO_SAMPLE_RATE_EXACT;
	decayCoef_   = expf(-1.0f / (decay_ * sr));
	glideCoef_   = 1.0f - expf(-1.0f / (slideTime_ * sr));
	attackCoef_  = 1.0f - expf(-1.0f / (0.003f * sr));   // ~3 ms
	releaseCoef_ = 1.0f - expf(-1.0f / (0.008f * sr));   // ~8 ms
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
	__disable_irq();
	if (heldCount_ == kMaxHeld) {        // full: drop the oldest held note
		for (int i = 1; i < kMaxHeld; i++) held_[i - 1] = held_[i];
		heldCount_--;
	}
	held_[heldCount_++] = note;
	curNote_ = note;
	curVel_  = velocity;
	targetFreq_ = midiToFreq(note);
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
	// in-progress glide toward it -- untouched (divergence from mulch, see .h)
	__enable_irq();
}

void AudioSynthAcidBass::update(void) {
	audio_block_t *block = allocate();
	if (!block) return;                  // memory pressure: silence, no state advance

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
		float main = (waveform_ == 0) ? (float)(2.0 * phase_ - 1.0)
		                              : (phase_ < 0.5 ? 1.0f : -1.0f);
		float sub  = (subPhase_ < 0.5 ? 1.0f : -1.0f) * subLevel_;
		float osc  = main + sub;

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
