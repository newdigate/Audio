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

#include <stdint.h>
#include "Wire.h"
#include "AudioControl.h"

// Low-level WM8962 I2C driver (moved from cores/imxrt1176/wm8962.h). 48k/16-bit
// I2S slave, HP out. writeReg is public so AudioControlWM8962 (and future nodes)
// can issue single register writes (e.g. the 44.1 kHz ADDCTL3 override).
class WM8962Codec {
public:
    bool begin(TwoWire &bus, uint8_t addr = 0x1A);
    bool writeReg(uint16_t reg, uint16_t val);   // now public
private:
    TwoWire *bus; uint8_t addr;
    bool readReg(uint16_t reg, uint16_t *val);
    bool modifyReg(uint16_t reg, uint16_t mask, uint16_t val);
    bool pollSeqDone();
};

// AudioControl wrapper around WM8962Codec for the audio graph. enable() runs the
// full record+playback init at 44.1 kHz (mic on Input3 / right channel).
class AudioControlWM8962 : public AudioControl
{
public:
    bool enable(void);
    bool disable(void) { return true; }
    bool volume(float n) { return true; }
    bool inputLevel(float n) { return true; }
    bool inputSelect(int n) { return true; }
private:
    WM8962Codec codec;
};

#endif
