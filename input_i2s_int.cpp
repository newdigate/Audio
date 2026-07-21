/* input_i2s_int.cpp - interrupt-driven SAI1 RX audio input for the RT1176.
 *
 * See input_i2s_int.h for the design contract. Block-fill discipline copied
 * from this fork's HW-verified input_i2s.cpp (MIT, (c) 2014 Paul
 * Stoffregen); register sequences live in sai1176.c; the shared hardware
 * table / config-once / NVIC hookup live in output_i2s_int.cpp.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */

#include "input_i2s_int.h"
#include "output_i2s_int.h"

#if defined(__IMXRT1176__) || defined(EVKB_CM4_WORLD)

audio_block_t *AudioInputI2SInt::block_left = NULL;
audio_block_t *AudioInputI2SInt::block_right = NULL;
uint16_t AudioInputI2SInt::block_offset = 0;
bool AudioInputI2SInt::update_responsibility = false;
volatile uint32_t AudioInputI2SInt::frame_count = 0;

void AudioInputI2SInt::begin(void)
{
	sai1176_evkb_config_once();
	update_responsibility = update_setup();
	sai1176_rx_isr_hook = isr;
	sai1176_evkb_nvic_hookup();
	/* Ensure the transmitter's clock generator runs BEFORE enabling RX: RX
	 * is synchronous to TX, so with TX disabled there is no BCLK/FS at all
	 * (HW-verified, input_i2s.cpp:141-149). OR-in so an already-armed
	 * output node's FRIE is preserved; FCONT (set by config) keeps the
	 * clock through the perpetual TX underrun when nothing feeds TX. */
	sai1176_regs_t *r =
	    (sai1176_regs_t *)(uintptr_t)sai1176_evkb_sai1.sai_base;
	r->TCSR |= SAI1176_CSR_EN | SAI1176_CSR_BCE;
	sai1176_rx_start_int(&sai1176_evkb_sai1);
}

/* RX service hook, called from sai1176_isr_dispatch (before the tx hook). */
void AudioInputI2SInt::isr(void)
{
	audio_block_t *left = block_left;
	audio_block_t *right = block_right;
	uint32_t off = block_offset;

	if (left && right && off < AUDIO_BLOCK_SAMPLES) {
		uint32_t n = sai1176_rx_service(&sai1176_evkb_sai1,
		                                left->data + off,
		                                right->data + off,
		                                AUDIO_BLOCK_SAMPLES - off);
		off += n;
		frame_count += n;
		block_offset = off;
		if (off >= AUDIO_BLOCK_SAMPLES && update_responsibility) {
			/* Only the graph-clock owner pends; when an output node is
			 * present it owns the clock and this stays false. */
			AudioStream::update_all();
		}
	} else {
		/* No blocks (graph not started / update pending): the FIFO must
		 * still drain or the level interrupt storms. Discard. */
		int16_t dl[16], dr[16];
		uint32_t n = sai1176_rx_service(&sai1176_evkb_sai1, dl, dr, 16);
		frame_count += n;
	}
}

void AudioInputI2SInt::update(void)
{
	/* Verbatim discipline from input_i2s.cpp::update(). */
	audio_block_t *new_left = NULL, *new_right = NULL;
	audio_block_t *out_left = NULL, *out_right = NULL;

	new_left = allocate();
	if (new_left != NULL) {
		new_right = allocate();
		if (new_right == NULL) {
			release(new_left);
			new_left = NULL;
		}
	}
	__disable_irq();
	if (block_offset >= AUDIO_BLOCK_SAMPLES) {
		/* The ISR filled both blocks: swap in the new pair fast. */
		out_left = block_left;
		block_left = new_left;
		out_right = block_right;
		block_right = new_right;
		block_offset = 0;
		__enable_irq();
		transmit(out_left, 0);
		release(out_left);
		transmit(out_right, 1);
		release(out_right);
	} else if (new_left != NULL) {
		if (block_left == NULL) {
			/* The ISR has nothing to fill: hand it the new pair. */
			block_left = new_left;
			block_right = new_right;
			block_offset = 0;
			__enable_irq();
		} else {
			__enable_irq();
			release(new_left);
			release(new_right);
		}
	} else {
		__enable_irq();
	}
}

#endif /* __IMXRT1176__ || EVKB_CM4_WORLD */
