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

// Audio graph SOURCE fed by a USB Audio Class device's capture (IN) stream,
// via USBHost_t36's USBAudioOut driver.
//
// Mirror of AudioOutputUSBHost, with one deliberate asymmetry: this node does
// NOT own the graph's clock and cannot. The sink node can pace the graph off
// FIFO occupancy because it decides when to produce; a source has no such
// lever -- the device sends what its converter produces, whenever it produces
// it, and the graph must be running to collect it. So some other node owns the
// clock (AudioOutputI2S, in the capstone) and this one takes whatever the
// capture FIFO holds when its update() is called.
//
// That means the two clocks are genuinely independent and there is no
// resampler between them: the adapter's converter runs on the adapter's
// crystal, the SAI on the board's. This project has measured that offset five
// times on the bench device -- about -86 ppm -- which at 44.1 kHz is roughly 4
// frames per second of drift. The FIFO absorbs it until it does not, and then
// one update() finds fewer than a block's worth of samples: underruns()
// counts that, and the short read is zero-filled so the shortfall is silence
// rather than stale audio. About one such block every quarter of a second at
// -86 ppm is not audible as pitch error; it is a very occasional tick.
//
// If a future design needs that tick gone, the fix is the fork's Resampler
// between this node and the sink, not a change here.

#ifndef input_usbhost_h_
#define input_usbhost_h_

#include "Arduino.h"
#include "AudioStream.h"
#include <USBHost_t36.h>

class AudioInputUSBHost : public AudioStream
{
public:
	AudioInputUSBHost(USBAudioOut &usb);
	virtual void update(void);

	// update()s that found less than a full block in the capture FIFO. The
	// tail of such a block is zero-filled. Non-zero and slowly rising is
	// normal -- it is the device's crystal drifting against the graph's
	// clock. Rising at the update rate (~344/s at 44.1 kHz) means no capture
	// data is arriving AT ALL, which is what a device with no input
	// interface looks like, and is the expected state in QEMU.
	uint32_t underruns(void) const { return short_reads; }

	// Blocks dropped because the audio memory pool was empty when update()
	// ran. Non-zero means AudioMemory() is undersized for this graph.
	uint32_t dropped(void) const { return blocks_dropped; }

private:
	USBAudioOut &audio;
	uint32_t short_reads;
	uint32_t blocks_dropped;
};

#endif
