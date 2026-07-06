/* Audio Library for Teensy 3.X / 4.X
 * WM8962 codec control (e.g. the on-board codec of the NXP MIMXRT1176-EVKB).
 *
 * The WM8962 hangs off I2C (LPI2C5 / Wire2 on the EVKB) at address 0x1A.
 * enable() reuses the core's HW-verified WM8962Codec::begin() (see
 * cores/imxrt1176/wm8962.{h,cpp}) for the full mic->ADC record-init register
 * sequence (reset, POWER1/POWER2/POWER3 power-up, the AnalogueInputPowerUp
 * StartSequence ID, ROUTE with leftInputPGASource=Input1 /
 * rightInputPGASource=Input3 -- the board's mic is on the RIGHT channel --
 * VOLUME, and clocking). Mic bias comes from those POWER/StartSequence writes;
 * begin() has no separate standalone MICBIAS register write.
 *
 * WM8962Codec::begin() was written for 48 kHz and writes ADDCTL3 = 0x0010 (the
 * SDK's kWM8962_AudioSampleRate48KHz case in fsl_wm8962.c
 * WM8962_ConfigDataFormat). This board runs I2S at 44.1 kHz (config_i2s /
 * AudioInputI2S set SAI1 + the audio PLL for a 22.5792 MHz MCLK). The
 * sysclk/Fs ratio is unchanged -- 22579200 / 44100 = 512, same as
 * 24576000 / 48000 = 512 -- so CLK4 (register 0x38 = 0x0A, the ratio==512
 * case) stays correct. ADDCTL3's SAMPLE_RATE field, however, is keyed on the
 * absolute rate, not the ratio: fsl_wm8962.c's WM8962_ConfigDataFormat maps
 * kWM8962_AudioSampleRate44100Hz to 0x0000 (vs. 0x0010 for 48 kHz). So after
 * begin() completes we re-write ADDCTL3 with the 44.1 kHz code.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#include <Arduino.h>
#include "control_wm8962.h"
#include "wm8962.h"
#include "Wire.h"

#define WM8962_I2C_ADDR 0x1A
#define WM8962_ADDCTL3  0x1B
// fsl_wm8962.c WM8962_ConfigDataFormat, kWM8962_AudioSampleRate44100Hz case.
#define WM8962_ADDCTL3_44100HZ 0x0000

bool AudioControlWM8962::enable(void)
{
	Wire2.begin();
	// Runs the full HW-verified record-init sequence (reset, power-up,
	// StartSequence, ROUTE, VOLUME, CLK4=0x0A) at address 0x1A.
	if (!Codec.begin(Wire2, WM8962_I2C_ADDR)) {
		return false; // no WM8962 responding at 0x1A
	}

	// Override the 48 kHz ADDCTL3 that begin() wrote with the 44.1 kHz code.
	// (WM8962Codec has no public register-write accessor, so this issues the
	// same 4-byte control-interface write WM8962Codec::writeReg uses:
	// 0x00 [subaddress hi] / reg / val_hi / val_lo.)
	Wire2.beginTransmission(WM8962_I2C_ADDR);
	Wire2.write((uint8_t)0x00);
	Wire2.write((uint8_t)WM8962_ADDCTL3);
	Wire2.write((uint8_t)(WM8962_ADDCTL3_44100HZ >> 8));
	Wire2.write((uint8_t)(WM8962_ADDCTL3_44100HZ & 0xFF));
	if (Wire2.endTransmission() != 0) {
		return false;
	}

	return true;
}
