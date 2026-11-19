/* play_buffer.cpp - AudioPlayer, the AudioStream wrapper around
 * AudioPlayerCore.  Copyright (c) 2026 Nicholas Newdigate.
 * SPDX-License-Identifier: MIT */
#include <Arduino.h>
#include "play_buffer.h"

// Every call into the core from thread context runs with interrupts off:
// update() runs from the audio interrupt and the core's 64-bit position and
// step must not be seen half-written.  Each is a few dozen instructions.
#define AP_LOCKED(stmt) do { __disable_irq(); stmt; __enable_irq(); } while (0)

AudioPlayer::AudioPlayer(void) : AudioStream(0, NULL), lastScratchMicros(0)
{
	core.setOutputRate(AUDIO_SAMPLE_RATE_EXACT);
}

void AudioPlayer::setSource(const int16_t *pcm, uint32_t frames, uint8_t channels, float sampleRate)
{
	AP_LOCKED(core.setSource(pcm, frames, channels, sampleRate));
}
void AudioPlayer::setAvailableFrames(uint32_t n) { AP_LOCKED(core.setAvailableFrames(n)); }
void AudioPlayer::setRate(float rate) { AP_LOCKED(core.setRate(rate)); }
void AudioPlayer::play(void) { AP_LOCKED(core.play()); }
void AudioPlayer::pause(void) { AP_LOCKED(core.pause()); }
bool AudioPlayer::isPlaying(void) { bool b; AP_LOCKED(b = core.isPlaying()); return b; }
void AudioPlayer::seek(double frame) { AP_LOCKED(core.seek(frame)); }
double AudioPlayer::positionFrames(void) { double p; AP_LOCKED(p = core.position()); return p; }
uint32_t AudioPlayer::lengthFrames(void) { uint32_t n; AP_LOCKED(n = core.lengthFrames()); return n; }
bool AudioPlayer::isScratching(void) { bool b; AP_LOCKED(b = core.isScratching()); return b; }

void AudioPlayer::scratchBegin(void)
{
	lastScratchMicros = micros();
	AP_LOCKED(core.scratchBegin());
}

void AudioPlayer::scratchTo(double frame)
{
	// Time since the previous report, in output samples, from micros():
	// finer than the core's own block counter (2.9 ms).
	const uint32_t now = micros();
	const float dt = (float)(now - lastScratchMicros) * (AUDIO_SAMPLE_RATE_EXACT / 1000000.0f);
	lastScratchMicros = now;
	AP_LOCKED(core.scratchTo(frame, dt));
}

void AudioPlayer::scratchEnd(void) { AP_LOCKED(core.scratchEnd()); }

void AudioPlayer::update(void)
{
	if (core.isIdle()) return;
	audio_block_t *left = allocate();
	audio_block_t *right = allocate();
	if (!left || !right) {
		// No blocks: still advance, so the position keeps time.
		static int16_t scrapL[AUDIO_BLOCK_SAMPLES], scrapR[AUDIO_BLOCK_SAMPLES];
		core.render(scrapL, scrapR, AUDIO_BLOCK_SAMPLES);
		if (left) release(left);
		if (right) release(right);
		return;
	}
	core.render(left->data, right->data, AUDIO_BLOCK_SAMPLES);
	transmit(left, 0);
	transmit(right, 1);
	release(left);
	release(right);
}
