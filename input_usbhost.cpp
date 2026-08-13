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
#include "input_usbhost.h"

AudioInputUSBHost::AudioInputUSBHost(USBAudioOut &usb)
	: AudioStream(0, NULL), audio(usb), short_reads(0), blocks_dropped(0)
{
	// Normalise the wire geometry to at most stereo by construction, the way
	// AudioOutputUSBHost pins the rate in its constructor. The driver unpacks
	// whatever the device's alt actually carries (the bench dongle captures
	// 1ch/16; uac_pack16 handles the wire format underneath), and this caps
	// what read() hands back so update()'s stack buffer cannot be overrun by
	// a device that captures more channels than a stereo graph can use.
	//
	// This is a REQUEST. captureChannels() reads back what was negotiated,
	// which for a mono device is 1 -- update() fans that to both outputs.
	usb.captureChannels(2);
}

void AudioInputUSBHost::update(void)
{
	audio_block_t *left = allocate();
	if (left == NULL) { blocks_dropped++; return; }
	audio_block_t *right = allocate();
	if (right == NULL) { release(left); blocks_dropped++; return; }

	// The zero case is not merely defensive, it is the common one: capture_ch
	// stays 0 from reset until beginRecording() assigns it, so every update()
	// before the input interface is selected lands here -- and in QEMU, whose
	// usb-audio model cannot capture, recording never starts and this is the
	// ONLY path taken. Without the guard `want` would be 0, the zero-fill loop
	// below would not run, and the fan loop would splat uninitialised stack
	// into both outputs 128 times per update: noise, not silence.
	//
	// The upper clamp is the defensive half: a device reporting more channels
	// than a stereo graph can use must not overrun `buf`.
	uint8_t ch = audio.captureChannels();
	if (ch == 0) ch = 1;
	if (ch > 2) ch = 2;

	const uint32_t want = (uint32_t)AUDIO_BLOCK_SAMPLES * ch;
	int16_t buf[AUDIO_BLOCK_SAMPLES * 2];
	const uint32_t got = audio.read(buf, want);

	// A short read is the drift (or, in QEMU, a device with no input at all).
	// Zero-fill the tail rather than leaving the previous pass's samples in
	// place: silence is an honest gap, stale audio is a click that also lies
	// about what arrived.
	if (got < want) {
		short_reads++;
		for (uint32_t i = got; i < want; i++) buf[i] = 0;
	}

	// Capture -> stereo. One channel is fanned to both (the dongle's case);
	// two are taken as L,R.
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		const int16_t l = buf[i * ch];
		left->data[i]  = l;
		right->data[i] = (ch >= 2) ? buf[i * ch + 1] : l;
	}

	transmit(left, 0);
	transmit(right, 1);
	release(left);
	release(right);
}
