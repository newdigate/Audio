/* play_buffer.h - AudioPlayer: variable-rate playback of 16-bit PCM held in
 * memory, with reverse and a scratch mode.  For samples and whole tracks
 * decoded into RAM (see play_buffer_core.h for the engine and its tests).
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Interpolation from teensy-variable-playback (MIT, same author).
 */
#ifndef play_buffer_h_
#define play_buffer_h_

#include <Arduino.h>
#include <AudioStream.h>
#include "play_buffer_core.h"

class AudioPlayer : public AudioStream
{
public:
	AudioPlayer(void);
	// Interleaved 16-bit PCM, 1 or 2 channels (mono plays on both outputs).
	// The memory is NOT owned or copied: it must stay valid until the next
	// setSource().  Resets to frame 0, paused.  Pass (nullptr, 0, ...) to
	// detach.
	void setSource(const int16_t *pcm, uint32_t frames, uint8_t channels, float sampleRate);
	// How much of the buffer is valid so far, for playing during a load.
	// The player never reads at or beyond this frame.
	void setAvailableFrames(uint32_t n);
	void setRate(float rate);        // 1.0 = normal, negative = reverse, 0 = hold
	void play(void);
	void pause(void);
	bool isPlaying(void);
	void seek(double frame);
	double positionFrames(void);     // fractional; safe to call from the main loop
	uint32_t lengthFrames(void);
	// Scratch: the player chases the frame given to scratchTo() (call it at
	// the touch-report rate) and ignores the set rate until scratchEnd().
	void scratchBegin(void);
	void scratchTo(double frame);
	void scratchEnd(void);
	bool isScratching(void);
	virtual void update(void);
private:
	AudioPlayerCore core;
	uint32_t lastScratchMicros;
};

#endif
