/* Audio Library for Teensy 3.X / 4.X
 * WM8962 codec control (e.g. the on-board codec of the NXP MIMXRT1176-EVKB).
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

#ifndef control_wm8962_h_
#define control_wm8962_h_

#include "AudioControl.h"

// Thin AudioControl wrapper around the core's HW-verified WM8962Codec
// (cores/imxrt1176/wm8962.{h,cpp}), which runs the mic->ADC record-init
// register sequence over LPI2C5 (Wire2) at address 0x1A. enable() is the
// only method the record path actually needs; the rest are minimal stubs
// so this class satisfies the AudioControl interface.
class AudioControlWM8962 : public AudioControl
{
public:
	bool enable(void);
	bool disable(void) { return true; }
	bool volume(float n) { return true; }
	bool inputLevel(float n) { return true; }   // 0.0 .. 1.0 (not yet wired up)
	bool inputSelect(int n) { return true; }    // mic is hardwired to Input3 (right ch)
};

#endif
