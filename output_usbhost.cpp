/* Audio Library for Teensy 3.X / i.MX RT
 * Copyright (c) 2026 Nicholas Newdigate
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice, development funding notice, and this
 * permission notice shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <Arduino.h>
#include "output_usbhost.h"

AudioOutputUSBHost *AudioOutputUSBHost::instance = nullptr;
bool AudioOutputUSBHost::update_responsibility = false;

AudioOutputUSBHost::AudioOutputUSBHost(USBAudioOut &usb)
	: AudioStream(2, inputQueueArray), audio(usb), blocks_dropped(0)
{
	instance = this;

	// Claim the graph's clock ownership, exactly as AudioOutputI2S does from
	// its DMA path. Only one node may own it; USB is the master here.
	update_responsibility = update_setup();

	audio.onFrameConsumed(frame_consumed);
}

// Called by USBAudioOut once per 1 ms USB frame it consumes. Runs from
// USBHost::Task(), not an interrupt, so this must stay short -- update_all()
// only pends IRQ_SOFTWARE, it does not run the graph inline.
//
// The FIFO check is what actually paces the graph: run it when there is room
// for another block, and not otherwise. A 128-sample stereo block is 256
// int16 samples, about 2.9 ms of audio, so this fires roughly every third
// frame rather than every one.
void AudioOutputUSBHost::frame_consumed(void)
{
	if (!update_responsibility || instance == nullptr) return;
	if (instance->audio.available() < AUDIO_BLOCK_SAMPLES * 2) return;
	AudioStream::update_all();
}

void AudioOutputUSBHost::update(void)
{
	audio_block_t *left  = receiveReadOnly(0);
	audio_block_t *right = receiveReadOnly(1);

	// A missing channel plays as silence rather than dropping the block, so
	// a mono source stays in sync instead of running at half rate.
	int16_t interleaved[AUDIO_BLOCK_SAMPLES * 2];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		interleaved[i * 2]     = left  ? left->data[i]  : 0;
		interleaved[i * 2 + 1] = right ? right->data[i] : 0;
	}

	if (left)  release(left);
	if (right) release(right);

	// Partial writes are counted, not retried: there is nowhere to hold the
	// remainder, and silently dropping part of a block would desynchronise
	// the channels. A non-zero count means something other than USB is
	// pending the audio interrupt.
	uint32_t taken = audio.write(interleaved, AUDIO_BLOCK_SAMPLES * 2);
	if (taken < (uint32_t)(AUDIO_BLOCK_SAMPLES * 2)) blocks_dropped++;
}
