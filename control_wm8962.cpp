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
#include "Wire.h"

#define WM8962_I2C_ADDR 0x1A
#define WM8962_ADDCTL3  0x1B
// fsl_wm8962.c WM8962_ConfigDataFormat, kWM8962_AudioSampleRate44100Hz case.
#define WM8962_ADDCTL3_44100HZ 0x0000

bool WM8962Codec::writeReg(uint16_t reg, uint16_t val) {
    bus->beginTransmission(addr);
    bus->write((uint8_t)0x00);            // subaddress hi (reg <= 0xFF)
    bus->write((uint8_t)reg);             // subaddress lo
    bus->write((uint8_t)(val >> 8));      // data hi (16-bit, big-endian)
    bus->write((uint8_t)(val & 0xFF));    // data lo
    return bus->endTransmission() == 0;
}
bool WM8962Codec::readReg(uint16_t reg, uint16_t *val) {
    bus->beginTransmission(addr);
    bus->write((uint8_t)0x00); bus->write((uint8_t)reg);
    if (bus->endTransmission(false) != 0) return false;   // repeated start
    if (bus->requestFrom(addr, (uint8_t)2) != 2) return false;
    uint8_t hi = bus->read(), lo = bus->read();
    *val = ((uint16_t)hi << 8) | lo;
    return true;
}
// Read-modify-write, mirroring fsl_wm8962.c WM8962_ModifyReg: read the
// register, clear `mask` bits, OR in `val`, write back.
bool WM8962Codec::modifyReg(uint16_t reg, uint16_t mask, uint16_t val) {
    uint16_t regVal;
    if (!readReg(reg, &regVal)) return false;
    regVal &= (uint16_t)~mask;
    regVal |= val;
    return writeReg(reg, regVal);
}
bool WM8962Codec::pollSeqDone() {
    for (int i = 0; i < 100000; i++) {
        uint16_t s; if (!readReg(0x5D, &s)) return false;
        if ((s & 0x1) == 0) return true;
    }
    return false;
}

// Faithful transcription of fsl_wm8962.c WM8962_Init for this board's fixed
// configuration (see wm8962_config_t in the EVKB SAI example's hardware_init.c,
// with sampleRate overridden to 48 kHz to match this core's fixed I2S rate):
//   bus            = kWM8962_BusI2S
//   format         = { mclk_HZ = 24576000, sampleRate = 48000, bitWidth = 16 }
//   masterSlave    = false (slave)                sysclkSource = MCLK (not FLL)
//   route: leftInputPGASource=Input1, rightInputPGASource=Input3,
//          left/rightInputMixerSource=InputPGA,
//          left/rightHeadphoneMixerSource=OutputMixerDisabled,
//          left/rightHeadphonePGASource=OutputPGASourceDAC (DAC->headphone,
//          speaker path unused/disabled)
//   slaveAddress   = 0x1A
//
// Since masterSlave=false and sysclkSource=MCLK, WM8962_Init's FLL branch
// (kWM8962_SysClkSourceFLL) and its SetMasterClock/IFACE0-MS-bit branch are
// both dead code for this config and are omitted below.
//
// Concrete values resolved against fsl_wm8962.c/.h (WM8962_MAX_DSP_CLOCK =
// 24576000U):
//   WM8962_GetClockDivider(mclk=24576000, WM8962_MAX_DSP_CLOCK=24576000, &clockDiv)
//     -> inputClock(24576000) is NOT > maxClock(24576000) (equal) -> clockDiv = 0
//   CLOCK1 (0x04) ModifyReg(mask=3<<9=0x600, val=clockDiv<<9=0x000)
//   sysClk = mclk / (1<<clockDiv) = 24576000 / 1 = 24576000
//   ConfigDataFormat ratio = sysClk / sampleRate = 24576000 / 48000 = 512
//     -> CLK4 (0x38) = 0x0A (ratio==512 case), ADDCTL3 (0x1B) = 0x10 (48kHz case)
//   IFACE0 (0x07) WL_MASK=0x0C, WL(16BITS=0) = 0x00 (16-bit case)
//   IFACE0 (0x07) FORMAT_MASK=0x13, val=kWM8962_BusI2S=2 (SetProtocol "else" branch)

// Reset / disable-osc / CLOCK2 / POWER1 / POWER2 -- fixed WriteReg sequence,
// identical regardless of route/format (WM8962_Init lines 358-364). Split
// into PRE/POST around the FLL_CTRL_1 ModifyReg to preserve the SDK's exact
// write order (WriteReg(0x81,0) -> ModifyReg(FLL_CTRL_1) -> WriteReg(CLOCK2)...).
static const uint16_t INIT_PRE[][2] = {
    {0x0F, 0x6243},   // WM8962_RESET: reset the codec
    {0x81, 0x0000},   // disable internal osc/FLL2/FLL3/FLL
};
static const uint16_t INIT_POST[][2] = {
    {0x08, 0x09E4},   // WM8962_CLOCK2: initial clocking config (SYSCLK off)
    {0x19, 0x01FE},   // WM8962_POWER1
    {0x1A, 0x01E0},   // WM8962_POWER2
};
// StartSequence IDs, in WM8962_Init order: DACToHeadphonePowerUp,
// AnalogueInputPowerUp, SpeakerWake (WM8962_Init lines 366-368).
static const uint16_t SEQ[] = {0x80, 0x92, 0xE8};
// Fixed volume writes, identical regardless of route/format
// (WM8962_Init lines 409-424).
static const uint16_t VOLUME[][2] = {
    {0x15, 0x01C0},   // WM8962_LADC: ADC volume 0dB
    {0x16, 0x01C0},   // WM8962_RADC
    {0x0A, 0x01C0},   // WM8962_LDAC: digital DAC volume -15.5dB
    {0x0B, 0x01C0},   // WM8962_RDAC
    {0x28, 0x01FF},   // WM8962_LOUT2: speaker volume 6dB
    {0x29, 0x01FF},   // WM8962_ROUT2
    {0x00, 0x013F},   // WM8962_LINVOL: input PGA volume
    {0x01, 0x013F},   // WM8962_RINVOL
    {0x02, 0x016B},   // WM8962_LOUT1: headphone volume
    {0x03, 0x016B},   // WM8962_ROUT1
};
// SetDataRoute writes for our fixed route (WM8962_SetDataRoute, resolved for
// leftInputPGASource=Input1(8), rightInputPGASource=Input3(2),
// left/rightInputMixerSource=InputPGA(1), left/rightHeadphonePGASource=DAC(1)
// so headphone mixers write 0 (the "else" branch), and left/rightSpeakerPGASource
// default to Mixer(0) with leftSpeakerMixerSource/rightSpeakerMixerSource
// defaulting to OutputMixerDisabled(0), so the speaker mixer writes are 0
// either way):
static const uint16_t ROUTE[][2] = {
    {0x25, 0x0018},   // WM8962_LEFT_INPUT_PGA: 0x10 | leftInputPGASource(8)
    {0x26, 0x0012},   // WM8962_RIGHT_INPUT_PGA: 0x10 | rightInputPGASource(2)
    {0x22, 0x0009},   // WM8962_INPUTMIX: (leftInputMixerSource(1)<<3) | rightInputMixerSource(1)
    {0x69, 0x0000},   // WM8962_LEFT_SPEAKER_MIXER: disabled (speaker path unused)
    {0x6A, 0x0000},   // WM8962_RIGHT_SPEAKER_MIXER: disabled
    {0x64, 0x0000},   // WM8962_LEFT_HEADPHONE_MIXER: 0 (PGA source is DAC, not Mixer)
    {0x65, 0x0000},   // WM8962_RIGHT_HEADPHONE_MIXER: 0 (PGA source is DAC, not Mixer)
    {0x1F, 0x0003},   // WM8962_INPUT_MIXER_1
};

bool WM8962Codec::begin(TwoWire &b, uint8_t a) {
    bus = &b; addr = a;

    for (auto &e : INIT_PRE) if (!writeReg(e[0], e[1])) return false;
    /* disable internal osc/FLL2/FLL3/FLL: FLL_CTRL_1 (0x9B), bit0 */
    if (!modifyReg(0x9B, 0x0001, 0x0000)) return false;
    for (auto &e : INIT_POST) if (!writeReg(e[0], e[1])) return false;

    for (uint16_t id : SEQ) {
        if (!writeReg(0x57, 0x0020)) return false;
        if (!writeReg(0x5A, id))     return false;
        if (!pollSeqDone())          return false;
    }

    /* masterSlave==false && sysclkSource==MCLK: internal FLL config block
     * (WM8962_Init lines 371-382) is skipped entirely for this fixed config. */

    /* DSP clock divider, maximum WM8962_MAX_DSP_CLOCK=24576000: mclk(24576000)
     * is not > maxClock(24576000) -> clockDiv=0. CLOCK1 (0x04) bits[10:9]. */
    if (!modifyReg(0x04, 0x0600, 0x0000)) return false;
    /* masterSlave==false: SetMasterClock + IFACE0 MS-bit block
     * (WM8962_Init lines 387-393) is skipped. */

    /* enable system clock: CLOCK2 (0x08) bit5 */
    if (!modifyReg(0x08, 0x0020, 0x0020)) return false;

    /* sysClk = mclk / (1<<clockDiv) = 24576000 / 1 = 24576000 (clockDiv read
     * back from CLOCK1 in the SDK; constant-folded here since clockDiv=0). */

    for (auto &e : ROUTE) if (!writeReg(e[0], e[1])) return false;

    /* SetProtocol(kWM8962_BusI2S=2): IFACE0 (0x07) FORMAT_MASK=0x13 */
    if (!modifyReg(0x07, 0x0013, 0x0002)) return false;

    for (auto &e : VOLUME) if (!writeReg(e[0], e[1])) return false;

    /* ConfigDataFormat(sysClk=24576000, sampleRate=48000, bits=16):
     * IFACE0 (0x07) WL_MASK=0x0C, WL(16BITS=0)=0x00 */
    if (!modifyReg(0x07, 0x000C, 0x0000)) return false;
    /* ADDCTL3 (0x1B): sampleRate==48kHz case */
    if (!writeReg(0x1B, 0x0010)) return false;
    /* CLK4 (0x38): ratio = sysClk/sampleRate = 24576000/48000 = 512 case */
    if (!writeReg(0x38, 0x000A)) return false;

    return true;
}

bool AudioControlWM8962::enable(void)
{
	Wire2.begin();
	if (!codec.begin(Wire2, WM8962_I2C_ADDR)) {
		return false; // no WM8962 responding at 0x1A
	}
	// Override the 48 kHz ADDCTL3 that begin() wrote with the 44.1 kHz code,
	// via the now-public register-write accessor (no hand-rolled I2C duplication).
	if (!codec.writeReg(WM8962_ADDCTL3, WM8962_ADDCTL3_44100HZ)) {
		return false;
	}
	return true;
}
