/* output_i2s_int.h - interrupt-driven SAI1 TX audio output for the RT1176.
 *
 * AudioOutputI2SInt: the DMA-free twin of AudioOutputI2S (same block-queue
 * discipline, distilled from this fork's HW-verified output_i2s.cpp), fed
 * from the shared SAI1 FIFO-request interrupt via the sai1176 C core. Built
 * for BOTH worlds: the CM7 (Arduino core) and bare-metal CM4 images (the
 * main eDMA cannot interrupt the CM4, so interrupt-driven I/O is the only
 * CM4-ownable audio path).
 *
 * THE ISR IS THE GRAPH CLOCK: every 128 completed frames (AUDIO_BLOCK_SAMPLES)
 * the shared ISR advances the block queue and pends IRQ_SOFTWARE, mirroring
 * the DMA node's half-buffer cadence (~344 dispatches/s at 44.1 kHz).
 *
 * Differences from the DMA node (all deliberate):
 *  - The constructor does NOT auto-call begin(): the SAI interrupt must not
 *    start before the sketch has sequenced the codec/measurement phases
 *    (and, in QEMU, before the main thread has printed its banner -- see
 *    setPauseAfter below).
 *  - setPauseAfter(n): if n > 0, the ISR clears TCSR.FRIE after the n-th
 *    graph dispatch (resume() re-arms it). This is the QEMU world-split
 *    escape hatch: the QEMU SAI model's TX FIFO-request level never
 *    deasserts without an audio backend (qemu2 imxrt_sai.c fidelity note),
 *    so a free-running FRIE would starve the main thread forever. On
 *    hardware it simply pauses the output. 0 (default) = run forever.
 *
 * Per-world interrupt hookup performed by begin():
 *  - CM7 (__IMXRT1176__): attachInterruptVector(IRQ_SAI1=76,
 *    sai1176_isr_dispatch); priority 224 -- numerically ABOVE the audio
 *    library's software_isr (208) so a pended update_all wins NVIC
 *    arbitration against the level-held SAI line; NVIC_ENABLE_IRQ(76).
 *  - CM4 (EVKB_CM4_WORLD): begin() does NOT touch the NVIC (the lpi2c1176
 *    consumer-sequences rule). The image must: place an extern-C
 *    SAI1_IRQHandler wrapper calling sai1176_isr_dispatch() in the static
 *    vector table at index 92 (16 + IRQ 76), write NVIC_ISER2 |= (1u << 12)
 *    (IRQ 76 - 64), and execute `cpsie i` after all begin() calls.
 *
 * AudioInputI2SInt (input_i2s_int.h) shares this file's SAI1 hardware table
 * and config-once helper, and rides the same single SAI1 ISR.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 * Block-queue discipline copied from this fork's output_i2s.{h,cpp}
 * (c) 2014 Paul Stoffregen, MIT -- see those files' headers.
 */

#ifndef output_i2s_int_h_
#define output_i2s_int_h_

#if defined(__IMXRT1176__) || defined(EVKB_CM4_WORLD)

#include <Arduino.h>
#include <AudioStream.h>
#include "sai1176.h"

/* The EVKB SAI1 hardware table (imxrt1176.h addresses; identical from the
 * CM4 -- both worlds share one address map). Defined in output_i2s_int.cpp. */
extern const sai1176_hw_t sai1176_evkb_sai1;

/* PLL + clock-root + pad + SAI register program, exactly once per boot
 * (both nodes call it; whoever begin()s first does the work). */
void sai1176_evkb_config_once(void);

/* Per-world SAI1 vector hookup (CM7: attach+priority+enable; CM4: no-op --
 * the image owns the static vector, see the header comment). Idempotent. */
void sai1176_evkb_nvic_hookup(void);

class AudioOutputI2SInt : public AudioStream
{
public:
	AudioOutputI2SInt(void) : AudioStream(2, inputQueueArray) {}
	virtual void update(void);
	void begin(void);
	/* Observability (the gates' measurable assertions). */
	static uint32_t dispatchCount(void) { return dispatch_count; }
	static uint32_t underrunCount(void) { return underrun_count; }
	static bool fef(void);                    /* TCSR.FEF (sticky, W1C) */
	/* QEMU world-split pause discipline (see the header comment). */
	static void setPauseAfter(uint32_t n) { pause_after = n; }
	static void resume(void);                 /* re-arm TCSR.FRIE */
	static void isr(void);                    /* tx service hook */
	friend class AudioInputI2SInt;
protected:
	static audio_block_t *block_left_1st;
	static audio_block_t *block_right_1st;
	static audio_block_t *block_left_2nd;
	static audio_block_t *block_right_2nd;
	static uint16_t block_offset;             /* frames of 1st pair consumed */
	static bool update_responsibility;
	static volatile uint32_t dispatch_count;
	static volatile uint32_t underrun_count;
	static volatile uint32_t pause_after;
	static volatile bool first_block_seen;
private:
	audio_block_t *inputQueueArray[2];
};

#endif /* __IMXRT1176__ || EVKB_CM4_WORLD */
#endif /* output_i2s_int_h_ */
