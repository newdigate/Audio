/* sai1176.c - shared SAI (I2S) register/clock core for the NXP MIMXRT1176.
 *
 * HW-verified-sequence distillation (see sai1176.h header comment for the
 * source provenance and the consumer NVIC contract). No NVIC calls in here.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */

#include "sai1176.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static inline sai1176_regs_t *sai_regs(const sai1176_hw_t *hw)
{
	return (sai1176_regs_t *)(uintptr_t)hw->sai_base;
}

/* ---- ANATOP Audio PLL (AI indirect) ------------------------------------
 * Addresses from imxrt1176.h; identical from the CM7 and the CM4 (the CM4
 * driving these is Plan-2 new territory -- the capstone HW run is the probe).
 */
#define ANADIG_PLL_AUDIO_CTRL_A       0x40C84300u
#define ANADIG_MISC_AI_CTRL_AUDIO_A   0x40C84880u
#define ANADIG_MISC_AI_WDATA_AUDIO_A  0x40C84890u
#define PLL_AUDIO_CTRL_ENABLE_CLK     (1u << 13)
#define PLL_AUDIO_CTRL_GATE           (1u << 14)
#define PLL_AUDIO_CTRL_STABLE         (1u << 29)
#define AI_TOGGLE                     (1u << 8)
#define AI_TOGGLE_DONE                (1u << 9)
#define AI_RWB                        (1u << 16)

/* One ANATOP AI indirect write to the Audio-PLL register file. Matches the
 * SDK ANATOP_AI_Write / the core's HW-verified I2S.cpp ai_write (toggle
 * bit8 -> done bit9). */
static void sai1176_ai_write(uint8_t aiAddr, uint32_t wdata)
{
	uint32_t pre = REG32(ANADIG_MISC_AI_CTRL_AUDIO_A) & AI_TOGGLE_DONE;
	REG32(ANADIG_MISC_AI_CTRL_AUDIO_A) &= ~AI_RWB;               /* write mode */
	REG32(ANADIG_MISC_AI_CTRL_AUDIO_A) =
	    (REG32(ANADIG_MISC_AI_CTRL_AUDIO_A) & ~0xFFu) | aiAddr;
	REG32(ANADIG_MISC_AI_WDATA_AUDIO_A) = wdata;
	REG32(ANADIG_MISC_AI_CTRL_AUDIO_A) ^= AI_TOGGLE;             /* request */
	while ((REG32(ANADIG_MISC_AI_CTRL_AUDIO_A) & AI_TOGGLE_DONE) == pre) { }
}

static void sai1176_udelay_loops(volatile uint32_t n)
{
	while (n--) { __asm__ volatile("nop"); }
}

void sai1176_pll_init_44k(void)
{
	/* AI sub-addresses (fsl_anatop_ai.h kAI_PLLAUDIO_*): CTRL0=0x00,
	 * CTRL0_SET=0x04, CTRL0_CLR=0x08, CTRL2(num)=0x20, CTRL3(denom)=0x30.
	 * CTRL0 bits: loopDiv[6:0], HOLD_RING_OFF b13, POWER_UP b14, ENABLE b15,
	 * BYPASS b16, PLL_REG_EN b22, POST_DIV[27:25].
	 * 44.1 kHz derivation (Audio/output_i2s.cpp, HW-verified): VCO =
	 * 24 MHz * (30 + 1056/10000) = 722.5344 MHz; postDiv=1 -> /2 =
	 * 361.2672 MHz; clock root /16 -> 22.5792 MHz = 44100 * 512 MCLK. */
	sai1176_ai_write(0x04, (1u << 16));                /* CTRL0_SET: BYPASS */
	REG32(ANADIG_PLL_AUDIO_CTRL_A) |= PLL_AUDIO_CTRL_ENABLE_CLK;
	sai1176_ai_write(0x30, 10000);                     /* denominator */
	sai1176_ai_write(0x20, 1056);                      /* numerator */
	sai1176_ai_write(0x08, 0x7Fu);                     /* CTRL0_CLR: loopDiv */
	sai1176_ai_write(0x04, 30u & 0x7Fu);               /* CTRL0_SET: loopDiv=30 */
	sai1176_ai_write(0x08, 0x7u << 25);                /* CTRL0_CLR: postDiv */
	sai1176_ai_write(0x04, (1u << 25));                /* CTRL0_SET: postDiv=1 */
	sai1176_ai_write(0x04, (1u << 22));                /* CTRL0_SET: PLL_REG_EN */
	sai1176_udelay_loops(20000);                       /* >=100us settle */
	sai1176_ai_write(0x04, (1u << 14) | (1u << 13));   /* POWER_UP|HOLD_RING_OFF */
	sai1176_udelay_loops(45000);                       /* >=225us */
	sai1176_ai_write(0x08, (1u << 13));                /* CTRL0_CLR: HOLD_RING_OFF */
	{
		uint32_t guard = 2000000;
		while (!(REG32(ANADIG_PLL_AUDIO_CTRL_A) & PLL_AUDIO_CTRL_STABLE) &&
		       guard--) { }
	}
	sai1176_ai_write(0x04, (1u << 15));                /* CTRL0_SET: ENABLE */
	REG32(ANADIG_PLL_AUDIO_CTRL_A) &= ~PLL_AUDIO_CTRL_GATE;  /* ungate */
	sai1176_ai_write(0x08, (1u << 16));                /* CTRL0_CLR: BYPASS */
}

void sai1176_config(const sai1176_hw_t *hw)
{
	sai1176_regs_t *r = sai_regs(hw);

	/* SAI1 clock root: mux 4 (Audio PLL) / 16 -> 22.5792 MHz MCLK; ungate. */
	REG32(hw->ccm_root) = (4u << 8) | (15u << 0);
	REG32(hw->lpcg) = 1u;

	/* Pin mux ALT0 + pad ctl 0x02; SION (0x10) on the clock pins per the SDK
	 * pin_mux; data pins plain ALT0; RXD input needs the daisy select (0 =
	 * GPIO_AD_20); MCLK is an output via GPR0 bit 8 (SAI1_MCLK_DIR). */
	REG32(hw->pad_mclk) = 0x10u;
	REG32(hw->pad_bclk) = 0x10u;
	REG32(hw->pad_sync) = 0x10u;
	REG32(hw->pad_txd)  = 0u;
	REG32(hw->pad_rxd)  = 0u;
	REG32(hw->pad_mclk + SAI1176_MUX_TO_PADCTL) = 0x02u;
	REG32(hw->pad_bclk + SAI1176_MUX_TO_PADCTL) = 0x02u;
	REG32(hw->pad_sync + SAI1176_MUX_TO_PADCTL) = 0x02u;
	REG32(hw->pad_txd  + SAI1176_MUX_TO_PADCTL) = 0x02u;
	REG32(hw->pad_rxd  + SAI1176_MUX_TO_PADCTL) = 0x02u;
	REG32(hw->rxd_daisy) = 0u;
	REG32(hw->gpr0) |= (1u << 8);

	/* TX: reset, then I2S 16-bit stereo master (verbatim from the core's
	 * HW-verified configureSAI; only the feeding clock rate is 44.1 kHz). */
	r->TCSR = SAI1176_CSR_SR; r->TCSR = 0u;
	r->TCSR = SAI1176_CSR_FR; r->TCSR = 0u;
	r->TMR  = 0u;
	r->TCR1 = SAI1176_TX_WATERMARK;                 /* TFW = 16 */
	r->TCR2 = (1u << 26)                            /* MSEL=1 (Audio PLL root) */
	        | (1u << 24)                            /* BCD: bit clock master */
	        | (1u << 25)                            /* BCP */
	        | 7u;                                   /* DIV=7 -> MCLK/16 BCLK */
	r->TCR3 = (1u << 16);                           /* TCE: channel 0 */
	r->TCR4 = (1u << 16)                            /* FRSZ=1 (2 words/frame) */
	        | (15u << 8)                            /* SYWD=15 (16-bit sync) */
	        | (1u << 4)                             /* MF */
	        | (1u << 0) | (1u << 3) | (1u << 1)     /* FSD | FSE | FSP */
	        | (1u << 28);  /* FCONT: keep the bit clock running through a FIFO
	                        * underrun (silicon halts and never recovers
	                        * without it -- HW-verified, cores I2S.cpp). */
	r->TCR5 = (15u << 24) | (15u << 16) | (15u << 8);  /* WNW|W0W|FBT = 15 */

	/* RX: reset, synchronous to TX (shares BCLK/FS). Verbatim from the
	 * HW-verified input_i2s.cpp begin() EXCEPT RCR1: the polled/DMA paths
	 * used RFW=0 (FRF at >=1 sample); the interrupt path uses RFW=16 so (a)
	 * FRF guarantees a full L/R pair per check (count > 16 >= 2 -- channel
	 * pairing needs no per-sample phase state) and (b) the IRQ cadence is
	 * ~8 frames, matching TX, instead of per-sample. RFW semantics straight
	 * from RM 58.5.1.13 (FRF when count > RFW). Deviation documented. */
	r->RCSR = SAI1176_CSR_SR; r->RCSR = 0u;
	r->RCSR = SAI1176_CSR_FR; r->RCSR = 0u;
	r->RMR  = 0u;
	r->RCR1 = SAI1176_RX_WATERMARK;                 /* RFW = 16 (see above) */
	r->RCR2 = (1u << 30);                           /* SYNC=1: sync to TX */
	r->RCR3 = (1u << 16);                           /* RCE: channel 0 */
	r->RCR4 = (1u << 16)                            /* FRSZ=1 */
	        | (15u << 8)                            /* SYWD=15 */
	        | (1u << 4)                             /* MF */
	        | (1u << 3) | (1u << 1);                /* FSE | FSP (no FSD: TX
	                                                 * owns the frame sync) */
	r->RCR5 = (15u << 24) | (15u << 16) | (15u << 8);  /* WNW|W0W|FBT = 15 */
}

void sai1176_tx_start_int(const sai1176_hw_t *hw)
{
	sai1176_regs_t *r = sai_regs(hw);
	/* 16-word silence prefill before TE so the first frames cannot underrun
	 * (HW-verified discipline: prefill + FCONT, cores I2S.cpp begin()). */
	for (int i = 0; i < 16; i++) {
		r->TDR[0] = 0u;
	}
	r->TCSR = SAI1176_CSR_EN | SAI1176_CSR_BCE | SAI1176_CSR_FRIE;
}

void sai1176_rx_start_int(const sai1176_hw_t *hw)
{
	sai1176_regs_t *r = sai_regs(hw);
	/* TX must already be running (RE alone produces no BCLK/FS: RX is
	 * synchronous to TX -- input_i2s.cpp:141-149, HW-verified). */
	r->RCSR = SAI1176_CSR_EN | SAI1176_CSR_BCE | SAI1176_CSR_FRIE |
	          SAI1176_CSR_FR;
}

uint32_t sai1176_tx_service(const sai1176_hw_t *hw, const int16_t *l,
                            const int16_t *r_ch, uint32_t frames)
{
	sai1176_regs_t *r = sai_regs(hw);
	uint32_t done = 0;

	/* Burst-fill: FRF asserts while count <= TFW(16), so 8 frames (16 words)
	 * per FRF check can never overflow the 32-deep FIFO; re-checking FRF per
	 * word would stop after a single frame (FRF clears the moment count
	 * exceeds the watermark). Right-packed low half-word (FBT=15). */
	while (done < frames && (r->TCSR & SAI1176_CSR_FRF)) {
		uint32_t burst = frames - done;
		if (burst > (SAI1176_FIFO_WORDS - SAI1176_TX_WATERMARK) / 2u) {
			burst = (SAI1176_FIFO_WORDS - SAI1176_TX_WATERMARK) / 2u;
		}
		for (uint32_t i = 0; i < burst; i++) {
			r->TDR[0] = l ? (uint16_t)l[done + i] : 0u;
			r->TDR[0] = r_ch ? (uint16_t)r_ch[done + i] : 0u;
		}
		done += burst;
	}
	return done;
}

uint32_t sai1176_rx_service(const sai1176_hw_t *hw, int16_t *l,
                            int16_t *r_ch, uint32_t frames)
{
	sai1176_regs_t *r = sai_regs(hw);
	uint32_t done = 0;

	/* FRF asserts while count > RFW(16): every check guarantees at least one
	 * full L/R pair, preserving channel pairing with no phase state. Low
	 * half-word reads (FBT=15) -- the silicon-proven RDR0 packing. */
	while (done < frames && (r->RCSR & SAI1176_CSR_FRF)) {
		l[done]    = (int16_t)r->RDR[0];
		r_ch[done] = (int16_t)r->RDR[0];
		done++;
	}
	return done;
}

uint32_t sai1176_rx_check_overflow(const sai1176_hw_t *hw)
{
	sai1176_regs_t *r = sai_regs(hw);
	uint32_t rcsr = r->RCSR;

	if (!(rcsr & SAI1176_CSR_FEF)) {
		return 0;
	}
	/* Overflow: the FIFO may now hold an odd word count -> every later
	 * L/R pair silently swapped. W1C the error, then FIFO-reset back to a
	 * pair-aligned (empty) state. Preserve only the enable/interrupt/DMA
	 * control bits on the writes (all other flag bits are W1C or RO --
	 * writing them as 0 is a no-op; writing readback values would W1C
	 * flags we did not mean to touch). */
	uint32_t ctrl = rcsr & (SAI1176_CSR_EN | SAI1176_CSR_BCE |
	                        SAI1176_CSR_FRIE | SAI1176_CSR_FWIE |
	                        SAI1176_CSR_FRDE);
	r->RCSR = ctrl | SAI1176_CSR_FEF;   /* clear the error flag */
	r->RCSR = ctrl | SAI1176_CSR_FR;    /* FIFO reset (momentary) */
	return 1;
}

/* ---- Shared ISR dispatch (no NVIC access; see sai1176.h) ---------------- */

void (*sai1176_tx_isr_hook)(void) = 0;
void (*sai1176_rx_isr_hook)(void) = 0;
volatile uint32_t sai1176_isr_count = 0;

void sai1176_isr_dispatch(void)
{
	sai1176_isr_count++;
	/* rx first; tx LAST -- the tx hook is the graph clock and may pend the
	 * software interrupt as its final action (a higher-priority software_isr
	 * preempts right there; everything it can touch is consistent by then). */
	if (sai1176_rx_isr_hook) {
		sai1176_rx_isr_hook();
	}
	if (sai1176_tx_isr_hook) {
		sai1176_tx_isr_hook();
	}
}
