/* output_i2s_int.cpp - interrupt-driven SAI1 TX audio output for the RT1176.
 *
 * See output_i2s_int.h for the design contract (worlds, ISR-as-graph-clock,
 * pause discipline, per-world NVIC hookup). The block-queue discipline is
 * copied from this fork's HW-verified output_i2s.cpp (MIT, (c) 2014 Paul
 * Stoffregen); the register sequences live in sai1176.c.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */

#include "output_i2s_int.h"

#if defined(__IMXRT1176__) || defined(EVKB_CM4_WORLD)

/* EVKB SAI1 register addresses (values cross-checked against imxrt1176.h;
 * identical from the CM4 -- one address map for both worlds). */
const sai1176_hw_t sai1176_evkb_sai1 = {
	0x40404000u,   /* sai_base:  SAI1 */
	0x40CC2000u,   /* ccm_root:  CCM_CLOCK_ROOT64_CONTROL (SAI1) */
	0x40CC6F60u,   /* lpcg:      CCM_LPCG123_DIRECT (SAI1) */
	0x400E8150u,   /* pad_mclk:  SW_MUX_CTL GPIO_AD_17 (SAI1_MCLK) */
	0x400E8164u,   /* pad_bclk:  SW_MUX_CTL GPIO_AD_22 (SAI1_TX_BCLK) */
	0x400E8168u,   /* pad_sync:  SW_MUX_CTL GPIO_AD_23 (SAI1_TX_SYNC) */
	0x400E8160u,   /* pad_txd:   SW_MUX_CTL GPIO_AD_21 (SAI1_TX_DATA00) */
	0x400E815Cu,   /* pad_rxd:   SW_MUX_CTL GPIO_AD_20 (SAI1_RX_DATA00) */
	0x400E8674u,   /* rxd_daisy: IOMUXC_SAI1_RX_DATA0_SELECT_INPUT (0=AD_20) */
	0x400E4000u,   /* gpr0:      IOMUXC_GPR_GPR0 (SAI1_MCLK_DIR bit 8) */
};

void sai1176_evkb_config_once(void)
{
	static bool configured = false;
	if (configured) {
		return;
	}
	configured = true;
	sai1176_pll_init_44k();
	sai1176_config(&sai1176_evkb_sai1);
}

audio_block_t *AudioOutputI2SInt::block_left_1st = NULL;
audio_block_t *AudioOutputI2SInt::block_right_1st = NULL;
audio_block_t *AudioOutputI2SInt::block_left_2nd = NULL;
audio_block_t *AudioOutputI2SInt::block_right_2nd = NULL;
uint16_t AudioOutputI2SInt::block_offset = 0;
bool AudioOutputI2SInt::update_responsibility = false;
volatile uint32_t AudioOutputI2SInt::dispatch_count = 0;
volatile uint32_t AudioOutputI2SInt::underrun_count = 0;
volatile uint32_t AudioOutputI2SInt::pause_after = 0;
volatile bool AudioOutputI2SInt::first_block_seen = false;

/* Shared per-world SAI1 vector hookup (also used by AudioInputI2SInt). */
void sai1176_evkb_nvic_hookup(void)
{
#if defined(EVKB_CM4_WORLD)
	/* Consumer contract (no NVIC access from library code on the CM4): the
	 * image places an extern-C SAI1_IRQHandler wrapper calling
	 * sai1176_isr_dispatch() in the static vector table at index 92
	 * (16 + IRQ 76), writes NVIC_ISER2 |= (1u << 12), and runs `cpsie i`
	 * after all begin() calls. Nothing to do here. */
#else
	/* CM7: priority 224 -- numerically ABOVE software_isr's 208 so a pended
	 * update_all wins NVIC arbitration against the level-held SAI line (in
	 * the QEMU model TX FRF is a constant level without an audio backend;
	 * on silicon this simply defines the nesting order). */
	attachInterruptVector(IRQ_SAI1, sai1176_isr_dispatch);
	NVIC_SET_PRIORITY(IRQ_SAI1, 224);
	NVIC_ENABLE_IRQ(IRQ_SAI1);
#endif
}

void AudioOutputI2SInt::begin(void)
{
	block_left_1st = NULL;
	block_right_1st = NULL;
	sai1176_evkb_config_once();
	update_responsibility = update_setup();
	sai1176_tx_isr_hook = isr;
	sai1176_evkb_nvic_hookup();
	sai1176_tx_start_int(&sai1176_evkb_sai1);   /* prefill + TE|BCE|FRIE */
}

bool AudioOutputI2SInt::fef(void)
{
	sai1176_regs_t *r = (sai1176_regs_t *)(uintptr_t)sai1176_evkb_sai1.sai_base;
	return (r->TCSR & SAI1176_CSR_FEF) != 0;
}

void AudioOutputI2SInt::resume(void)
{
	sai1176_regs_t *r = (sai1176_regs_t *)(uintptr_t)sai1176_evkb_sai1.sai_base;
	r->TCSR |= SAI1176_CSR_FRIE;
}

/* TX service hook, called from sai1176_isr_dispatch (LAST, so the software-
 * interrupt pend below is the final action of the whole SAI ISR). */
void AudioOutputI2SInt::isr(void)
{
	audio_block_t *blockL = block_left_1st;
	audio_block_t *blockR = block_right_1st;
	uint32_t off = block_offset;

	if (first_block_seen && !blockL && !blockR) {
		underrun_count++;      /* graph failed to supply data (silence fed) */
	}

	uint32_t n = sai1176_tx_service(&sai1176_evkb_sai1,
	                                blockL ? blockL->data + off : NULL,
	                                blockR ? blockR->data + off : NULL,
	                                AUDIO_BLOCK_SAMPLES - off);
	off += n;

	if (off < AUDIO_BLOCK_SAMPLES) {
		block_offset = off;
		return;
	}

	/* 128 frames complete: advance the queue (output_i2s.cpp discipline),
	 * then clock the graph. */
	block_offset = 0;
	if (blockL) {
		AudioStream::release(blockL);
		block_left_1st = block_left_2nd;
		block_left_2nd = NULL;
	}
	if (blockR) {
		AudioStream::release(blockR);
		block_right_1st = block_right_2nd;
		block_right_2nd = NULL;
	}
	dispatch_count++;
	if (pause_after && dispatch_count >= pause_after) {
		/* Measurement window over: stop interrupting (QEMU's TX FRF level
		 * never deasserts without an audio backend). resume() re-arms. */
		sai1176_regs_t *r =
		    (sai1176_regs_t *)(uintptr_t)sai1176_evkb_sai1.sai_base;
		r->TCSR &= ~SAI1176_CSR_FRIE;
	}
	if (update_responsibility) {
		AudioStream::update_all();   /* pend IRQ_SOFTWARE: the graph clock */
	}
}

void AudioOutputI2SInt::update(void)
{
	/* Verbatim queue discipline from output_i2s.cpp::update(). */
	audio_block_t *block;
	block = receiveReadOnly(0);  /* input 0 = left channel */
	if (block) {
		first_block_seen = true;
		__disable_irq();
		if (block_left_1st == NULL) {
			block_left_1st = block;
			block_offset = 0;
			__enable_irq();
		} else if (block_left_2nd == NULL) {
			block_left_2nd = block;
			__enable_irq();
		} else {
			audio_block_t *tmp = block_left_1st;
			block_left_1st = block_left_2nd;
			block_left_2nd = block;
			block_offset = 0;
			__enable_irq();
			release(tmp);
		}
	}
	block = receiveReadOnly(1);  /* input 1 = right channel */
	if (block) {
		first_block_seen = true;
		__disable_irq();
		if (block_right_1st == NULL) {
			block_right_1st = block;
			__enable_irq();
		} else if (block_right_2nd == NULL) {
			block_right_2nd = block;
			__enable_irq();
		} else {
			audio_block_t *tmp = block_right_1st;
			block_right_1st = block_right_2nd;
			block_right_2nd = block;
			__enable_irq();
			release(tmp);
		}
	}
}

#endif /* __IMXRT1176__ || EVKB_CM4_WORLD */
