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

// Audio graph sink that streams to a USB Audio Class 1.0 device attached to
// the RT1176's USB host port, via USBHost_t36's USBAudioOut driver.
//
// Clocking: the USB frame clock is the master. USBAudioOut calls this node
// back once per 1 ms frame it consumes, and the callback pends the audio
// software interrupt when the FIFO has room for another block. The graph
// therefore runs at the bus's rate rather than free-running, which is what
// removes clock-drift handling from the design -- there is only one clock.
//
// That also means this node must be the graph's clock owner: do not use it
// alongside AudioOutputI2S, which claims the same responsibility.
//
// Rate: USBAudioOut must be configured for AUDIO_SAMPLE_RATE (44100), so that
// a 128-sample block is exactly a 128-sample block on the wire. Feeding
// 44.1 kHz blocks into a 48 kHz stream plays 8.8% sharp and drifts into
// permanent underrun.

#ifndef output_usbhost_h_
#define output_usbhost_h_

#include "Arduino.h"
#include "AudioStream.h"
#include <USBHost_t36.h>

class AudioOutputUSBHost : public AudioStream
{
public:
	AudioOutputUSBHost(USBAudioOut &usb);
	virtual void update(void);

	// Blocks dropped because the FIFO was full when update() ran. Non-zero
	// means the graph is producing faster than USB is consuming, which
	// should not happen while USB owns the clock -- if it does, something
	// else is also pending the audio interrupt.
	uint32_t dropped(void) const { return blocks_dropped; }

private:
	static void frame_consumed(void);

	USBAudioOut &audio;
	audio_block_t *inputQueueArray[2];
	uint32_t blocks_dropped;

	static AudioOutputUSBHost *instance;
	static bool update_responsibility;
};

#endif
