/* sai1176.h - shared SAI (I2S) register/clock core for the NXP MIMXRT1176.
 *
 * This project's HW-verified RT1176 SAI1 bring-up, re-expressed as the single
 * shared C core (the lpi2c1176 pattern, Wire/lpi2c1176.h). Distilled from:
 *   - cores/imxrt1176/I2S.cpp (sai1_audio_pll_init + configureSAI + the
 *     16-word prefill / FCONT discipline, HW-verified 48 kHz),
 *   - Audio/output_i2s.cpp __IMXRT1176__ config_i2s (the 44.1 kHz Audio-PLL
 *     fractional divider: loopDiv=30, NUM=1056, DENOM=10000; HW-verified,
 *     audible on J101),
 *   - Audio/input_i2s.cpp __IMXRT1176__ begin (RX synchronous to TX,
 *     RDR0/TDR0 read/write at offset +0 low half-word per FBT=15 -- the
 *     silicon-proven packing).
 * Consumed by BOTH the CM7 audio nodes (AudioOutputI2SInt/AudioInputI2SInt)
 * and bare-metal CM4 images -- ending CM7/CM4 keep-in-sync duplication.
 * Freestanding C11: compiles under the CM4 image flags (-ffreestanding, no
 * core headers) and inside the C++ library.
 *
 * RULES (same as lpi2c1176): NO NVIC calls in here. Consumers sequence the
 * interrupt-controller hookup themselves so each world's HW-verified order is
 * preserved:
 *   CM7:  attachInterruptVector(IRQ_SAI1 /=76/, sai1176_isr_dispatch);
 *         NVIC_SET_PRIORITY(76, p); NVIC_ENABLE_IRQ(76);
 *   CM4:  place an extern-C SAI1_IRQHandler wrapper (calling
 *         sai1176_isr_dispatch) in the static vector table at index 92
 *         (16 + IRQ 76), write NVIC_ISER2 |= (1u << 12) /(76-64)/, `cpsie i`.
 *
 * Register offsets triangulated 2026-07-21 against cores/imxrt1176/imxrt1176.h
 * AND the RT1170 RM rev.5 58.5.1.1 (SAI memory map): TCSR +0x08 (NOT +0x00),
 * TDR0 +0x20, TMR +0x60, RCSR +0x88, RCR1..5 +0x8C..0x9C, RDR0 +0xA0,
 * RFR0 +0xC0, RMR +0xE0. The TX block was probed on the EVKB (Plan 1); the RX
 * offsets agree across both references (static-asserted below).
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef SAI1176_H
#define SAI1176_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#define SAI1176_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
#define SAI1176_ASSERT(c, m) _Static_assert(c, m)
#endif

/* SAI register-block overlay (offsets per RT1170 RM ch.58 + imxrt1176.h). */
typedef struct {
	volatile uint32_t VERID;    /* 0x00 */
	volatile uint32_t PARAM;    /* 0x04 */
	volatile uint32_t TCSR;     /* 0x08  (transmit control IS at +0x08) */
	volatile uint32_t TCR1;     /* 0x0C */
	volatile uint32_t TCR2;     /* 0x10 */
	volatile uint32_t TCR3;     /* 0x14 */
	volatile uint32_t TCR4;     /* 0x18 */
	volatile uint32_t TCR5;     /* 0x1C */
	volatile uint32_t TDR[4];   /* 0x20..0x2C */
	volatile uint32_t r30[4];   /* 0x30..0x3C */
	volatile uint32_t TFR[4];   /* 0x40..0x4C */
	volatile uint32_t r50[4];   /* 0x50..0x5C */
	volatile uint32_t TMR;      /* 0x60 */
	volatile uint32_t r64[9];   /* 0x64..0x84 */
	volatile uint32_t RCSR;     /* 0x88 */
	volatile uint32_t RCR1;     /* 0x8C */
	volatile uint32_t RCR2;     /* 0x90 */
	volatile uint32_t RCR3;     /* 0x94 */
	volatile uint32_t RCR4;     /* 0x98 */
	volatile uint32_t RCR5;     /* 0x9C */
	volatile uint32_t RDR[4];   /* 0xA0..0xAC */
	volatile uint32_t rB0[4];   /* 0xB0..0xBC */
	volatile uint32_t RFR[4];   /* 0xC0..0xCC */
	volatile uint32_t rD0[4];   /* 0xD0..0xDC */
	volatile uint32_t RMR;      /* 0xE0 */
} sai1176_regs_t;

SAI1176_ASSERT(offsetof(sai1176_regs_t, TCSR) == 0x08, "SAI TCSR");
SAI1176_ASSERT(offsetof(sai1176_regs_t, TCR1) == 0x0C, "SAI TCR1");
SAI1176_ASSERT(offsetof(sai1176_regs_t, TDR)  == 0x20, "SAI TDR0");
SAI1176_ASSERT(offsetof(sai1176_regs_t, TFR)  == 0x40, "SAI TFR0");
SAI1176_ASSERT(offsetof(sai1176_regs_t, TMR)  == 0x60, "SAI TMR");
SAI1176_ASSERT(offsetof(sai1176_regs_t, RCSR) == 0x88, "SAI RCSR");
SAI1176_ASSERT(offsetof(sai1176_regs_t, RCR1) == 0x8C, "SAI RCR1");
SAI1176_ASSERT(offsetof(sai1176_regs_t, RDR)  == 0xA0, "SAI RDR0");
SAI1176_ASSERT(offsetof(sai1176_regs_t, RFR)  == 0xC0, "SAI RFR0");
SAI1176_ASSERT(offsetof(sai1176_regs_t, RMR)  == 0xE0, "SAI RMR");

/* Hardware description: register ADDRESSES, supplied by the consumer (the
 * CM7 nodes pass imxrt1176.h values; CM4 images pass the same literals). The
 * pad_* fields are IOMUXC SW_MUX_CTL addresses; the matching SW_PAD_CTL is at
 * a fixed +0x244 for the whole GPIO_AD group (verified from imxrt1176.h:
 * AD_17 0x400E8150/0x400E8394, AD_20 0x400E815C/0x400E83A0, AD_21
 * 0x400E8160/0x400E83A4, AD_22 0x400E8164/0x400E83A8, AD_23
 * 0x400E8168/0x400E83AC -- all deltas 0x244). */
typedef struct {           /* register addresses, supplied by the consumer */
    uint32_t sai_base;     /* 0x40404000 for SAI1 */
    uint32_t ccm_root;     /* clock root reg addr */
    uint32_t lpcg;         /* LPCG reg addr */
    /* pad/mux/daisy addrs for mclk/bclk/sync/txd/rxd + gpr0 */
    uint32_t pad_mclk, pad_bclk, pad_sync, pad_txd, pad_rxd, rxd_daisy, gpr0;
} sai1176_hw_t;

#define SAI1176_MUX_TO_PADCTL 0x244u  /* SW_MUX_CTL -> SW_PAD_CTL (GPIO_AD group) */

/* TCSR/RCSR bits (identical positions; bit 31 = TE for TX, RE for RX). */
#define SAI1176_CSR_EN    (1u << 31)  /* TE / RE */
#define SAI1176_CSR_BCE   (1u << 28)  /* bit clock enable */
#define SAI1176_CSR_FR    (1u << 25)  /* FIFO reset (momentary) */
#define SAI1176_CSR_SR    (1u << 24)  /* software reset (momentary) */
#define SAI1176_CSR_FEF   (1u << 18)  /* FIFO error flag (W1C) */
#define SAI1176_CSR_FWF   (1u << 17)  /* FIFO warning flag */
#define SAI1176_CSR_FRF   (1u << 16)  /* FIFO request flag */
#define SAI1176_CSR_FWIE  (1u << 9)   /* FIFO warning interrupt enable */
#define SAI1176_CSR_FRIE  (1u << 8)   /* FIFO request interrupt enable */
#define SAI1176_CSR_FRDE  (1u << 0)   /* FIFO request DMA enable */

/* FIFO geometry (RM 58.3: 32 x 32-bit words per data line). */
#define SAI1176_FIFO_WORDS   32u
#define SAI1176_TX_WATERMARK 16u  /* TCR1.TFW: FRF while count <= 16 */
#define SAI1176_RX_WATERMARK 16u  /* RCR1.RFW: FRF while count >  16 */

/* Bring up the Audio PLL for 44.1 kHz via ANATOP AI indirect writes
 * (loopDiv=30, NUM=1056, DENOM=10000, postDiv=1 -> 722.5344/2 = 361.2672 MHz;
 * /16 at the clock root = 22.5792 MHz MCLK = 44100*512). Sequence distilled
 * verbatim from Audio/output_i2s.cpp config_i2s (HW-verified); ANADIG
 * addresses are hardcoded (identical from both cores). */
void sai1176_pll_init_44k(void);

/* Clock root + LPCG + pads + full TX and RX register program. TX: watermark
 * 16, TCR4.FCONT (keep BCLK through underrun), I2S 16-bit stereo master.
 * RX: synchronous to TX (RCR2.SYNC=1), watermark 16. Leaves TCSR/RCSR = 0
 * (nothing enabled). Idempotent-safe: full re-write of every register. */
void sai1176_config(const sai1176_hw_t *hw);

/* 16-word silence prefill, then TCSR = TE | BCE | FRIE (interrupt-driven).
 * The prefill + FCONT pair is the HW-verified anti-underrun discipline
 * (cores I2S.cpp begin()). */
void sai1176_tx_start_int(const sai1176_hw_t *hw);

/* RCSR = RE | BCE | FRIE | FR. TX must already be running (RX is synchronous
 * to TX: without TE|BCE there is no BCLK/FS at all -- input_i2s.cpp:141-149). */
void sai1176_rx_start_int(const sai1176_hw_t *hw);

/* Feed up to `frames` L/R pairs while the TX FIFO requests data; returns
 * frames consumed. l/r may be NULL (silence for that channel). Fills in
 * bursts of 8 frames per FRF check: FRF means count <= TFW(16), so a 16-word
 * burst can never overflow the 32-deep FIFO, and per-word FRF re-checks would
 * stall after a single frame (FRF clears as soon as count exceeds the
 * watermark). Samples are written to TDR0 right-packed (low half-word,
 * FBT=15) -- the silicon-proven packing. */
uint32_t sai1176_tx_service(const sai1176_hw_t *hw, const int16_t *l,
                            const int16_t *r, uint32_t frames);

/* Drain up to `frames` L/R pairs while the RX FIFO requests data; returns
 * frames read. FRF means count > RFW(16), so each FRF check guarantees a full
 * frame pair (channel pairing is preserved without per-sample phase state).
 * Samples are read from RDR0 low half-word (FBT=15). */
uint32_t sai1176_rx_service(const sai1176_hw_t *hw, int16_t *l,
                            int16_t *r, uint32_t frames);

/* --- Shared ISR dispatch -------------------------------------------------
 * ONE SAI interrupt line services both directions. Consumers install
 * sai1176_isr_dispatch as the SAI1 vector (per-world hookup: see the header
 * comment) and register their service hooks; dispatch calls rx first, tx
 * last (the tx hook is the graph clock and may pend a software interrupt as
 * its final action). No NVIC access happens in here. */
extern void (*sai1176_tx_isr_hook)(void);
extern void (*sai1176_rx_isr_hook)(void);
extern volatile uint32_t sai1176_isr_count;
void sai1176_isr_dispatch(void);

#if defined(__cplusplus)
}
#endif
#endif /* SAI1176_H */
