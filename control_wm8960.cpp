/* Audio Library for Teensy 3.X / 4.X
 * WM8960 codec control (e.g. the on-board codec of the NXP MIMXRT1060-EVKB).
 *
 * The WM8960 hangs off I2C (LPI2C1 on the EVKB) at address 0x1A.  Its 2-wire
 * control interface is write-only: each write is two bytes carrying a 7-bit
 * register address (bits [7:1] of byte 0) and 9 bits of data (bit 8 in byte 0
 * bit [0], bits [7:0] in byte 1).  The register values below follow the NXP
 * MCUXpresso SDK WM8960 driver defaults (codec as I2S slave; the SAI is the
 * clock master), routing the DACs to the headphone (OUT1) and speaker (Class-D)
 * outputs.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#include <Arduino.h>
#include "control_wm8960.h"
#include "Wire.h"

#define WM8960_I2C_ADDR 0x1A

// Register addresses (7-bit).
#define WM8960_LINVOL   0x00
#define WM8960_RINVOL   0x01
#define WM8960_LOUT1    0x02
#define WM8960_ROUT1    0x03
#define WM8960_CLOCK1   0x04
#define WM8960_DACCTL1  0x05
#define WM8960_IFACE1   0x07
#define WM8960_IFACE2   0x09
#define WM8960_LDAC     0x0A
#define WM8960_RDAC     0x0B
#define WM8960_RESET    0x0F
#define WM8960_LADC     0x15
#define WM8960_RADC     0x16
#define WM8960_ADDCTL1  0x17
#define WM8960_POWER1   0x19
#define WM8960_POWER2   0x1A
#define WM8960_LINPATH  0x20
#define WM8960_RINPATH  0x21
#define WM8960_LOUTMIX  0x22
#define WM8960_ROUTMIX  0x25
#define WM8960_LOUT2    0x28
#define WM8960_ROUT2    0x29
#define WM8960_BYPASS1  0x2D
#define WM8960_BYPASS2  0x2E
#define WM8960_POWER3   0x2F
#define WM8960_ADDCTL4  0x30
#define WM8960_CLASSD1  0x31

bool AudioControlWM8960::enable(void)
{
	Wire.begin();
	delay(5);
	if (!write(WM8960_RESET, 0x000)) {
		return false; // no WM8960 responding at 0x1A
	}

	// Power: VREF + VMID(50k), analogue in, ADC, mic bias, DACs, OUT1/2, SPK.
	write(WM8960_POWER1, 0x0FE);
	write(WM8960_POWER2, 0x1F8);
	write(WM8960_POWER3, 0x03C); // L/R output mixers + L/R mic

	// Clocking: SYSCLK = MCLK direct, DAC/ADC = SYSCLK/256 (256*Fs MCLK).
	write(WM8960_CLOCK1, 0x000);

	// Audio interface: I2S, 16-bit word length, codec is SLAVE (SAI master).
	write(WM8960_IFACE1, 0x002);
	write(WM8960_IFACE2, 0x040); // ADCLRC pin as GPIO (shared LRCLK)

	// Route each DAC into its output mixer.
	write(WM8960_LOUTMIX, 0x100); // LD2LO
	write(WM8960_ROUTMIX, 0x100); // RD2RO
	write(WM8960_BYPASS1, 0x000);
	write(WM8960_BYPASS2, 0x000);

	// DAC digital volume 0 dB (+ update), DAC unmuted.
	write(WM8960_LDAC, 0x1FF);
	write(WM8960_RDAC, 0x1FF);
	write(WM8960_DACCTL1, 0x000);

	// Headphone (OUT1) and speaker (OUT2 + Class-D) volumes (+ volume update).
	write(WM8960_LOUT1, 0x16F);
	write(WM8960_ROUT1, 0x16F);
	write(WM8960_LOUT2, 0x1FF);
	write(WM8960_ROUT2, 0x1FF);
	write(WM8960_CLASSD1, 0x0F7);

	// ADC path defaults (line input).
	write(WM8960_LADC, 0x1C3);
	write(WM8960_RADC, 0x1C3);
	write(WM8960_LINVOL, 0x117);
	write(WM8960_RINVOL, 0x117);
	write(WM8960_ADDCTL1, 0x0C0);
	write(WM8960_ADDCTL4, 0x040);

	return true;
}

bool AudioControlWM8960::disable(void)
{
	// Mute the DAC then drop the analogue/output power rails.
	write(WM8960_DACCTL1, 0x008); // DACMU
	write(WM8960_POWER2, 0x000);
	write(WM8960_POWER1, 0x000);
	return true;
}

// Headphone (OUT1) volume: 0 .. 127 maps to the codec's 7-bit OUT1 field.
bool AudioControlWM8960::volumeInteger(unsigned int n)
{
	if (n > 127) n = 127;
	write(WM8960_LOUT1, n | 0x100); // OUT1VU updates both channels
	write(WM8960_ROUT1, n | 0x100);
	return true;
}

bool AudioControlWM8960::speakerVolume(float vol)
{
	int n = vol * 127.0f + 0.499f;
	if (n > 127) n = 127;
	if (n < 0) n = 0;
	write(WM8960_LOUT2, n | 0x100); // SPKVU
	write(WM8960_ROUT2, n | 0x100);
	return true;
}

bool AudioControlWM8960::inputLevel(float n)
{
	int level = n * 63.0f + 0.499f; // 6-bit input PGA
	if (level > 63) level = 63;
	if (level < 0) level = 0;
	write(WM8960_LINVOL, level | 0x100); // IPVU updates both channels
	write(WM8960_RINVOL, level | 0x100);
	return true;
}

bool AudioControlWM8960::inputSelect(int n)
{
	if (n == AUDIO_INPUT_LINEIN) {
		// LINPUT1 -> input PGA, PGA -> boost mixer.
		write(WM8960_LINPATH, 0x108);
		write(WM8960_RINPATH, 0x108);
	} else if (n == AUDIO_INPUT_MIC) {
		// LINPUT1 -> PGA with +20 dB boost.
		write(WM8960_LINPATH, 0x138);
		write(WM8960_RINPATH, 0x138);
	} else {
		return false;
	}
	return true;
}

bool AudioControlWM8960::write(unsigned int reg, unsigned int val)
{
	Wire.beginTransmission(WM8960_I2C_ADDR);
	Wire.write((reg << 1) | ((val >> 8) & 1));
	Wire.write(val & 0xFF);
	return Wire.endTransmission() == 0;
}
