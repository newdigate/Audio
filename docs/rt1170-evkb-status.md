# Audio library — RT1170-EVKB (i.MX RT1176, CM7) status & roadmap

Status of every component in this fork against the `imxrt1176` core on the
MIMXRT1170-EVKB. **Primarily CM7** — but as of **2026-07-22 the CM4 can also own
the whole audio pipeline**, interrupt-driven (not DMA): the DMA I/O nodes stay
CM7-only (main-eDMA completion IRQs are CM7-domain), while the new
interrupt-driven SAI nodes + CMSIS-DSP run on the M4. See the
"CM4 ownership (dual-core)" section below.

Audited 2026-07-20 by sweeping every source file's platform guards and hardware
register usage. Three guard families decide a file's fate:

> **Changelog 2026-07-21**: CMSIS-DSP landed in the evkb manifest
> (`import_evkb_cmsis_dsp()`) — the 🔵 tier is retired; `filter_fir` and
> `analyze_fft256` are now HW-verified via the evkb `filter_fir_test` gate.
> Guard sweep landed — six 🟡 components revived and HW-verified via the evkb
> `guard_sweep_test` gate; a stale SerialFlash include was discovered and
> stripped in synth_wavetable.
>
> **Changelog 2026-07-22**: the CM4 can now **own the audio pipeline**
> (dual-core, interrupt-driven — not DMA). New interrupt-driven SAI nodes
> `output_i2s_int`/`input_i2s_int` + the shared `sai1176.c` register/clock core,
> CMSIS-DSP running on the M4, and a capstone where the CM4 owns
> codec+SAI+graph+FFT with the CM7 idle (audible 1 kHz on J101). The old
> "the audio graph cannot run on the CM4" caveat is retired — see the new
> "CM4 ownership (dual-core)" section.
>
> **Changelog 2026-07-22 — Phase A COMPLETE**: the **full Audio library now
> builds for RT1176** — `#include <Audio.h>` compiles every portable node and
> links on the **CM7**, HW-verified via the evkb `audio_h_test` gate (waveform →
> AudioFilterFIR → AudioAnalyzeFFT256 known-answer chain, QEMU == HW). This
> closes **Phase A step 3**, so **Phase A is COMPLETE** (CMSIS-DSP + guard sweep
> + Audio.h all landed). The hardware I/O nodes still compile as empty objects
> (no RT1176 port — instantiating one is a link error; see that section);
> everything else in the 🟢 tier is now pinned by the aggregate build.

| Guard style | Effect on RT1176 |
|---|---|
| `#if defined(__ARM_ARCH_7EM__)` (architecture) | **Works as-is** — the CM7 defines it, full DSP path taken |
| `#if defined(KINETISK) \|\| defined(__IMXRT1062__)` (chip list) | **Silently degraded/dead** — RT1176 matches neither; `update()` compiles to nothing, or a smaller `#else` fallback is taken |
| whole file wrapped in `#if defined(__IMXRT1062__)` | **Compiles empty** — hardware I/O node, needs a real port |

Legend:
✅ verified in an evkb gate (QEMU + real-hardware) ·
🟢 expected-compatible, not yet exercised on 1176 ·
🟡 (retired 2026-07-22 — **Phase A complete**; no 🟡 rows remain. The last one, `Audio.h`, now builds the whole library — see the changelog) ·
🔵 (retired 2026-07-21 — CMSIS-DSP landed in the evkb manifest; formerly: blocked on arm_math) ·
🟠 needs an RT1062→RT1176 hardware port ·
🟣 needs external hardware (and a port where noted) ·
❌ N/A — Kinetis-only peripheral, no RT1176 equivalent of the same shape

## Verified today (compiled by evkb gates, HW-verified on the EVKB)

`input_i2s` (SAI1 RX, mic on WM8962 **right** channel), `output_i2s` (SAI1 TX
DMA), `control_wm8962`, `play_sd_wav`, `synth_sine`, `analyze_peak`,
`filter_fir`, `analyze_fft256`, `synth_karplusstrong`, `synth_simple_drum`,
`synth_wavetable`, `effect_delay`, `play_queue`/`record_queue`,
`memcpy_audio.S`, `spi_interrupt`, `data_waveforms.c`. `AudioStream` itself
lives in the core (dispatch on spare `IRQ_SOFTWARE=44`, 44.1 kHz).

## Synths

| Component | Status | Notes |
|---|---|---|
| synth_sine | ✅ | in gates (fork already stripped its unused `arm_math` include) |
| synth_waveform | 🟢 | arch-guarded + bandlimit tables |
| synth_dc, synth_pinknoise, synth_whitenoise, synth_pwm | 🟢 | arch/`KINETISL`-split — full path taken on CM7 |
| synth_tonesweep | 🟢 | unblocked — CMSIS-DSP (`arm_math`) now in the evkb manifest; needs its own gate (`arm_sin_q31`) |
| synth_karplusstrong | ✅ | guards -> `__ARM_ARCH_7EM__`; HW-verified via evkb `guard_sweep_test` |
| synth_simple_drum | ✅ | guards -> `__ARM_ARCH_7EM__`; HW-verified via evkb `guard_sweep_test` |
| synth_wavetable | ✅ | guards -> `__ARM_ARCH_7EM__`; HW-verified via evkb `guard_sweep_test` (synthetic single-cycle instrument) |

## Effects

| Component | Status | Notes |
|---|---|---|
| bitcrusher, chorus, combine, envelope, fade, granular, midside, multiply, rectifier, reverb, freeverb, wavefolder, waveshaper | 🟢 | pure DSP, arch-guarded or unguarded |
| effect_delay | ✅ | 1176 joins the 4-second tier; 200 ms tap HW-verified via `guard_sweep_test` |
| effect_flange | 🟢 | unblocked — CMSIS-DSP (`arm_math`) now in the evkb manifest; needs its own gate (`arm_sin_q15`) |
| effect_delay_ext | 🟣 | code is portable; needs an external SPI RAM (23LC1024-class) on the header SPI — untested |

## Filters

| Component | Status | Notes |
|---|---|---|
| filter_biquad, filter_variable | 🟢 | arch-guarded |
| filter_fir | ✅ | CMSIS-DSP manifest lib in evkb; HW-verified via evkb `filter_fir_test` (boxcar known-answer) |
| filter_ladder | 🟢 | unblocked — CMSIS-DSP (`arm_math`) now in the evkb manifest; needs its own gate (`arm_fir_{decimate,interpolate}_*_f32`, CM7 FPU is fine) |

## Analyze

| Component | Status | Notes |
|---|---|---|
| analyze_peak | ✅ | in gates |
| analyze_rms, analyze_tonedetect | 🟢 | need `utility/sqrt_integer.c` compiled alongside |
| analyze_print | 🟢 | |
| analyze_notefreq | 🟢 | |
| analyze_fft256 | ✅ | HW-verified via evkb `filter_fir_test` |
| analyze_fft1024 | 🟢 | unblocked — CMSIS-DSP (`arm_math`) now in the evkb manifest; needs its own gate (`arm_cfft_radix4_q15` + q15 twiddle tables) |

## Mixer / queues / players

| Component | Status | Notes |
|---|---|---|
| mixer | 🟢 | arch-guarded; used implicitly nowhere yet — should just work |
| play_memory | 🟢 | |
| play_queue, record_queue | ✅ | deep-buffer tiers (80/209) HW-verified via `guard_sweep_test` |
| play_sd_wav | ✅ | in gates (SD via USDHC) |
| play_sd_raw | 🟢 | same SD path as play_sd_wav |
| play_serialflash_raw | 🟣 | needs the SerialFlash library (not in the evkb manifest) + an external flash chip |
| Quantizer, Resampler | 🟢 | portable C++/float support classes (used by async_input_spdif3) |

## Hardware I/O nodes (whole-file `__IMXRT1062__` guards)

As of the full-library build (2026-07-22) these all **compile clean as empty
objects** as part of `#include <Audio.h>`, but remain **uninstantiable** on
RT1176 — there is no port behind the guard, so the class body is empty.
Instantiating one is a **link error, not a compile error**.
`input_i2s`/`output_i2s` are the exception: fully ported (the template below).

| Component | Status | Notes |
|---|---|---|
| input_i2s / output_i2s | ✅ | ported (SAI1 + WM8962); the template for every port below |
| input_i2s2 / output_i2s2 | 🟠 | SAI2 exists on RT1176; EVKB pin routing unaudited — check RevC3 before starting |
| input_i2s_quad / output_i2s_quad | 🟣🟠 | extra SAI1 data lines; EVKB dedicates SAI1 to the codec — needs pin audit + external codec |
| input_i2s_hex/oct, output_i2s_hex/oct | ❌ (practically) | Teensy 4.1-specific multi-pin builds; not sensible on EVKB |
| input_tdm / output_tdm (+ tdm2) | 🟣🟠 | SAI TDM mode is supported by the silicon; WM8962 is I2S-only — needs an external TDM codec |
| output_spdif | 🟠 | software S/PDIF over SAI1 — conflicts with the codec's use of SAI1 |
| output_spdif2 | 🟠 | over SAI2 (pin audit first) |
| output_spdif3, input_spdif3, async_input_spdif3 | 🟠 | RT1176 has the dedicated SPDIF block; **RevC3 sheet 17 associates `GPIO_AD_14` (header D7) with SPDIF** — most promising new-peripheral port. async variant also needs `arm_dcache_delete` (core has it) |
| output_mqs | 🟠 | RT1176 has MQS (via IOMUXC GPR); EVKB routing unaudited — cheap "speaker on a pin" win if a header pin muxes it |
| input_pdm, input_pdm_i2s2 | 🟠 | T4 trick uses SAI; RT1176 has a dedicated PDM/MICFIL block — better as a new driver. No PDM mic on the EVKB (onboard mic is analog into WM8962) |
| output_pt8211, output_pt8211_2 | 🟣🟠 | external PT8211 DAC over SAI — needs SAI pins on the header |
| output_adat | ❌ | KINETISK-only upstream (no Teensy-4 support either) |
| output_pwm | ❌ | Kinetis FTM. An RT1176 FlexPWM equivalent would be new work (MQS is the intended replacement) |
| output_dac, output_dacs | ❌ (as-is) | Kinetis DAC. **RT1176 has DAC12, already HW-verified in the core (`analogWriteDAC0`)** — a new native `output_dac12` node is attractive |
| input_adc, input_adcs | ❌ (as-is) | Kinetis ADC+PDB. LPADC + a timer could back a new native node |

## Codec control drivers (pure I2C over Wire — no platform code)

| Component | Status | Notes |
|---|---|---|
| control_wm8962 | ✅ | the EVKB's onboard codec |
| control_sgtl5000, wm8731, wm8960, cs42448, cs4272, ak4558, tlv320aic3206 | 🟢 | should compile and run over Wire, but no such codec on the EVKB — only testable with external boards |

## Support files

| File | Status | Notes |
|---|---|---|
| memcpy_audio.S | ✅ | `__ARM_ARCH_7EM__`-guarded asm |
| spi_interrupt | ✅ | compiled in the sd_wav gate |
| data_*.c | 🟢/✅ | pure tables |
| utility/dspinst.h | ✅ | inline DSP asm, CM7-fine |
| utility/sqrt_integer.{c,h} | 🟢 | needed by rms/tonedetect |
| utility/imxrt_hw.cpp (`set_audioClock`) | 🟠 | RT1062 CCM code; the 1176 ports do their own clocking — port or replace when SAI2/SPDIF/MQS work starts |
| utility/pdb.h | ❌ | Kinetis PDB |
| Audio.h (master include) | ✅ | full library builds for RT1176; HW-verified via evkb `audio_h_test` |

## CM4 ownership (dual-core) — ✅ HW-verified 2026-07-22

**The CM4 can own the entire audio pipeline**, interrupt-driven (not DMA).
Proven on the EVKB by the evkb `examples/dualcore/cm4_audio_test` capstone: the
CM4 owns the WM8962 codec over LPI2C5, interrupt-driven SAI1 I/O, the AudioStream
graph, and CMSIS-DSP `analyze_fft256`, while the CM7 boots the image and parks in
WFI (`cm7_audio_isers=0` — the CM7 enabled no audio interrupt; audible 1 kHz on
J101). This retires the old "anything on the CM4 is out of scope" note for audio.

| Capability | Status | Notes |
|---|---|---|
| `output_i2s_int` / `input_i2s_int` (interrupt-driven SAI1 nodes, no eDMA) | ✅ | new nodes; HW-verified on the **CM7** (evkb `i2s_int_test`) and on the **CM4** (evkb `cm4_audio_test`). The SAI1 combined FIFO IRQ (line 76) reaches the CM4 NVIC (RM Tables 4-1/4-2). |
| `sai1176.c` (shared SAI register/clock core) | ✅ | one HW-verified SAI1 sequence both worlds compile (Phase-3.3 idiom, like `lpspi1176.c`/`lpi2c1176.c`); adds `SAI1176_PLL_EXTERNAL` for the CM7-pre-arm split (finding 1). |
| `input_i2s` / `output_i2s` (DMA SAI1 nodes) | CM7-only (forever) | the main-eDMA completion IRQs are CM7-domain (RM Table 4-1); the CM4's DMA is `eDMA_LPSR`, which does not carry the SAI dma-request. DMA-fed audio stays on the CM7. |
| CMSIS-DSP on the M4 | ✅ | the CMSIS-DSP amalgams compile into a CM4 image; known-answer FFT HW-verified via evkb `cm4_fft_test`. DSP-heavy nodes can run off the CM7. |

**Three silicon findings from the capstone (EVKB 2026-07-22):**
1. **The CM4 cannot drive the ANATOP Audio PLL.** The AI-write handshake to the
   ANATOP PLL register file hangs when issued from the M4 (QEMU fakes the
   handshake, so it only surfaces on silicon). Fix: the CM7 pre-arms the 44.1 kHz
   Audio PLL before `Multicore.begin()` and the CM4 image is built
   `-DSAI1176_PLL_EXTERNAL` to skip the AI writes; the CM4 still owns the clock
   root/LPCG/pads/SAI/codec/graph. This is the gate's default build.
2. **The SAI I/O ISR must outrank the AudioStream graph.** At SAI IRQ priority
   224 (below `software_isr`=208) the fft256 graph update preempted RX service on
   the 400 MHz M4 and the RX FIFO overflowed (`rx_overflows=0x3FF`). At priority
   192 (SAI outranks the graph — standard Teensy audio design, which the CM7
   hides because DMA services the FIFO) the CM4 runs clean.
3. **QEMU FIFO metrics are anti-correlated with silicon.** The QEMU SAI model
   does not enforce FIFO drain/fill timing, so `underruns`/`fef`/`rx_overflows`
   read the OPPOSITE of the board (QEMU shows graph-starve underruns where HW is
   clean, and vice-versa). The QEMU gate asserts only the deterministic verdict
   (`AUDIO_CM4_DET`); the full `AUDIO_CM4=PASS` (which adds FIFO health) is the
   HW oracle.

## Roadmap

**Phase A — no hardware needed, biggest surface unlock — ✅ COMPLETE 2026-07-22**
1. **CMSIS-DSP — DONE 2026-07-21**: landed as a pinned manifest library in
   evkb (`import_evkb_cmsis_dsp()` in `evkb.cmake`, CMSIS-DSP v1.17.1 +
   CMSIS_6, Apache-2.0 — passes the license firewall) rather than inside
   `cores/imxrt1176`. Proven by two HW-verified gates:
   `examples/framework/arm_math_test` (known-answer FFT/FIR/sin) and
   `examples/audio/filter_fir_test` (sine → AudioFilterFIR boxcar →
   AudioAnalyzeFFT256 + peak, zero Audio source changes). The former 🔵 rows
   are unblocked (filter_fir + analyze_fft256 already ✅).
2. **Guard sweep — DONE 2026-07-21**: chip lists switched to
   `__ARM_ARCH_7EM__` in synth_karplusstrong, synth_simple_drum,
   synth_wavetable, effect_delay.h, play_queue.h, record_queue.h; dead
   `arm_math` includes stripped (synth_waveform.h+.cpp, analyze_notefreq.cpp)
   plus a stale SerialFlash include in synth_wavetable.cpp. All six components
   HW-verified via the evkb `examples/audio/guard_sweep_test` gate (six runtime
   stages, red-then-green, QEMU == HW transcripts).
3. **"Audio.h compiles" gate — DONE 2026-07-22**: the evkb
   `examples/audio/audio_h_test` gate `#include <Audio.h>`, compiles every
   portable node, and runs a known-answer chain (waveform → AudioFilterFIR →
   AudioAnalyzeFFT256 → assert bin), HW-verified (QEMU == HW). This pins the
   whole 🟢 tier and catches regressions. **Phase A is now COMPLETE.**

**Phase B — new EVKB-testable peripherals (pin-mux audit against RevC3 first)**
4. SPDIF block port (`output_spdif3` → `input_spdif3` → async): `GPIO_AD_14`/D7
   is flagged SPDIF on RevC3 sheet 17 — verify direction and mux.
5. `output_mqs` if a header pin muxes MQS_L/R.
6. SAI2 (`i2s2` pair) if its pins reach the headers.

**Phase C — external hardware**
7. PT8211, TDM codec, quad-channel SAI1, external-codec control drivers,
   effect_delay_ext (SPI RAM), play_serialflash_raw (SerialFlash lib +
   manifest pin).

**Native new nodes (not ports) worth considering**
- `output_dac12` — mono analog out on the already-HW-verified DAC12 (TP18).
- LPADC input node; MICFIL/PDM input node.

**Out of scope**: hex/oct I2S (T4.1 pinouts), ADAT (Kinetis-only), Kinetis
DAC/ADC/PWM nodes as-is; DMA-fed audio on the CM4 (main-eDMA completion IRQs are
CM7-domain — see "CM4 ownership (dual-core)"; interrupt-driven CM4 audio is done).
