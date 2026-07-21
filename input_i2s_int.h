/* input_i2s_int.h - interrupt-driven SAI1 RX audio input for the RT1176.
 *
 * AudioInputI2SInt: the DMA-free twin of AudioInputI2S (block-fill
 * discipline distilled from this fork's HW-verified input_i2s.cpp), drained
 * from the SAME shared SAI1 interrupt as AudioOutputI2SInt via the sai1176
 * C core -- ONE ISR services TX fill and RX drain (sai1176_isr_dispatch
 * calls the rx hook first, the tx hook last).
 *
 * RX is synchronous to TX (RCR2.SYNC=1, shared BCLK/FS): begin() enables the
 * transmitter's clock (TE|BCE; FCONT keeps it running through the perpetual
 * TX underrun when no output node feeds it) exactly as the HW-verified
 * input_i2s.cpp does. On the EVKB the onboard mics reach the WM8962 as the
 * RIGHT channel (Input3) -- the LEFT channel is unconnected (HW-verified,
 * sai_rx_test).
 *
 * This node requires output_i2s_int.cpp in the link (it owns the shared
 * hardware table, config-once and per-world NVIC hookup helpers) -- a
 * deliberate pairing: RX cannot exist without the TX clock anyway.
 *
 * Constructor does NOT auto-call begin() (same rationale as
 * AudioOutputI2SInt). Per-world interrupt hookup: see output_i2s_int.h.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 * Block-fill discipline copied from this fork's input_i2s.{h,cpp}
 * (c) 2014 Paul Stoffregen, MIT -- see those files' headers.
 */

#ifndef input_i2s_int_h_
#define input_i2s_int_h_

#if defined(__IMXRT1176__) || defined(EVKB_CM4_WORLD)

#include <Arduino.h>
#include <AudioStream.h>
#include "sai1176.h"

class AudioInputI2SInt : public AudioStream
{
public:
	AudioInputI2SInt(void) : AudioStream(0, NULL) {}
	virtual void update(void);
	void begin(void);
	static uint32_t frameCount(void) { return frame_count; }
	/* RX FIFO overflows detected (each one FEF-cleared + FIFO-reset back to
	 * a pair-aligned state by sai1176_rx_check_overflow -- an overflow can
	 * drop an odd word count and silently swap L/R forever). Gates assert 0. */
	static uint32_t overflows(void) { return rx_overflows; }
	static void isr(void);           /* rx service hook */
protected:
	static audio_block_t *block_left;
	static audio_block_t *block_right;
	static uint16_t block_offset;    /* frames captured into the pair */
	static bool update_responsibility;
	static volatile uint32_t frame_count;
	static volatile uint32_t rx_overflows;
};

#endif /* __IMXRT1176__ || EVKB_CM4_WORLD */
#endif /* input_i2s_int_h_ */
