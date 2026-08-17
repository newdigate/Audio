/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef analyze_peakdetect_h_
#define analyze_peakdetect_h_

#include <Arduino.h>     // github.com/PaulStoffregen/cores/blob/master/teensy4/Arduino.h
#include <AudioStream.h> // github.com/PaulStoffregen/cores/blob/master/teensy4/AudioStream.h

class AudioAnalyzePeak : public AudioStream
{
public:
	AudioAnalyzePeak(void) : AudioStream(1, inputQueueArray) {
		new_output = false;
		min_sample = 32767;
		max_sample = -32768;
	}
	bool available(void) {
		__disable_irq();
		bool flag = new_output;
		if (flag) new_output = false;
		__enable_irq();
		return flag;
	}
	// read()/readPeakToPeak() CONSUME the accumulated min/max, so they must
	// also clear new_output -- in the same critical section, or the clear is
	// itself racy against update().
	//
	// ★ They did not, and the bug that exposed it is worth keeping. update()
	// runs from the audio ISR, so it can land between a caller's available()
	// and its read():
	//
	//     update()      -> data,     new_output = true
	//     available()   -> true,     new_output = false
	//     update()  [ISR] -> data,   new_output = true      <-- lands here
	//     read()        -> returns the data, resets min/max, flag STAYS true
	//     available()   -> true, with NO data behind it
	//     read()        -> abs(-32768)/32767 == 1.0000305   <-- garbage
	//
	// The sentinel reads as a full-scale peak, so the failure looks like a
	// signal-level problem rather than a flag problem, and it is timing
	// dependent: it needs the ISR to fall inside that window. It surfaced as
	// an intermittent red on rt1062:audio/audiooutput_i2s_test, whose sketch
	// takes a running MAXIMUM over 500 ms -- one poisoned read outlives every
	// correct one. Its steady-state readings in the same run were 0.5000: the
	// audio path was fine throughout, only the sampling of it was not.
	float read(void) {
		__disable_irq();
		int min = min_sample;
		int max = max_sample;
		min_sample = 32767;
		max_sample = -32768;
		new_output = false;
		__enable_irq();
		min = abs(min);
		max = abs(max);
		if (min > max) max = min;
		return (float)max / 32767.0f;
	}
	float readPeakToPeak(void) {
		__disable_irq();
		int min = min_sample;
		int max = max_sample;
		min_sample = 32767;
		max_sample = -32768;
		new_output = false;
		__enable_irq();
		return (float)(max - min) / 32767.0f;
	}

	virtual void update(void);
private:
	audio_block_t *inputQueueArray[1];
	volatile bool new_output;
	int16_t min_sample;
	int16_t max_sample;
};

#endif
