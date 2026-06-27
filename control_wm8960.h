/* Audio Library for Teensy 3.X / 4.X
 * WM8960 codec control (e.g. the on-board codec of the NXP MIMXRT1060-EVKB).
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#ifndef control_wm8960_h_
#define control_wm8960_h_

#include "AudioControl.h"

class AudioControlWM8960 : public AudioControl
{
public:
	bool enable(void);
	bool disable(void);
	bool volume(float n) { return volumeInteger(n * 127.0f + 0.499f); }
	bool inputLevel(float n);   // 0.0 .. 1.0
	bool inputSelect(int n);    // AUDIO_INPUT_LINEIN / AUDIO_INPUT_MIC
	bool headphoneVolume(float n) { return volumeInteger(n * 127.0f + 0.499f); }
	bool speakerVolume(float n);
protected:
	bool write(unsigned int reg, unsigned int val);
	bool volumeInteger(unsigned int n); // 0 .. 127 (headphone OUT1)
};

#endif
