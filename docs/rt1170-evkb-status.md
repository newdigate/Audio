# Audio library — RT1170-EVKB (i.MX RT1176, CM7) status & roadmap

Status of every component in this fork against the `imxrt1176` core on the
MIMXRT1170-EVKB. **CM7 only** — the audio graph cannot run on the CM4 (SAI/eDMA
completion IRQs are CM7-domain).

Audited 2026-07-20 by sweeping every source file's platform guards and hardware
register usage. Three guard families decide a file's fate:

> **Changelog 2026-07-21**: CMSIS-DSP landed in the evkb manifest
> (`import_evkb_cmsis_dsp()`) — the 🔵 tier is retired; `filter_fir` and
> `analyze_fft256` are now HW-verified via the evkb `filter_fir_test` gate.

| Guard style | Effect on RT1176 |
|---|---|
| `#if defined(__ARM_ARCH_7EM__)` (architecture) | **Works as-is** — the CM7 defines it, full DSP path taken |
| `#if defined(KINETISK) \|\| defined(__IMXRT1062__)` (chip list) | **Silently degraded/dead** — RT1176 matches neither; `update()` compiles to nothing, or a smaller `#else` fallback is taken |
| whole file wrapped in `#if defined(__IMXRT1062__)` | **Compiles empty** — hardware I/O node, needs a real port |

Legend:
✅ verified in an evkb gate (QEMU + real-hardware) ·
🟢 expected-compatible, not yet exercised on 1176 ·
🟡 needs a small guard fix (add `__IMXRT1176__` / switch to `__ARM_ARCH_7EM__`) ·
🔵 (retired 2026-07-21 — CMSIS-DSP landed in the evkb manifest; formerly: blocked on arm_math) ·
🟠 needs an RT1062→RT1176 hardware port ·
🟣 needs external hardware (and a port where noted) ·
❌ N/A — Kinetis-only peripheral, no RT1176 equivalent of the same shape

## Verified today (compiled by evkb gates, HW-verified on the EVKB)

`input_i2s` (SAI1 RX, mic on WM8962 **right** channel), `output_i2s` (SAI1 TX
DMA), `control_wm8962`, `play_sd_wav`, `synth_sine`, `analyze_peak`,
`filter_fir`, `analyze_fft256`, `memcpy_audio.S`, `spi_interrupt`,
`data_waveforms.c`. `AudioStream` itself lives in the core (dispatch on spare
`IRQ_SOFTWARE=44`, 44.1 kHz).

## Synths

| Component | Status | Notes |
|---|---|---|
| synth_sine | ✅ | in gates (fork already stripped its unused `arm_math` include) |
| synth_waveform | 🟢 | arch-guarded + bandlimit tables; `.h` has an unused `arm_math` include to strip |
| synth_dc, synth_pinknoise, synth_whitenoise, synth_pwm | 🟢 | arch/`KINETISL`-split — full path taken on CM7 |
| synth_tonesweep | 🟢 | unblocked — CMSIS-DSP (`arm_math`) now in the evkb manifest; needs its own gate (`arm_sin_q31`) |
| synth_karplusstrong | 🟡 | `KINETISK \|\| __IMXRT1062__` around the whole `update()` — **silently dead** today |
| synth_simple_drum | 🟡 | same silent-dead pattern |
| synth_wavetable | 🟡 | same silent-dead pattern |

## Effects

| Component | Status | Notes |
|---|---|---|
| bitcrusher, chorus, combine, envelope, fade, granular, midside, multiply, rectifier, reverb, freeverb, wavefolder, waveshaper | 🟢 | pure DSP, arch-guarded or unguarded |
| effect_delay | 🟡 | works, but `DELAY_QUEUE_SIZE` falls to the small `#else` — add 1176 to the `.h` guard for the 4-second buffer |
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
| analyze_notefreq | 🟢 | `arm_math` include is unused (no calls) — strip it like synth_sine did |
| analyze_fft256 | ✅ | HW-verified via evkb `filter_fir_test` |
| analyze_fft1024 | 🟢 | unblocked — CMSIS-DSP (`arm_math`) now in the evkb manifest; needs its own gate (`arm_cfft_radix4_q15` + q15 twiddle tables) |

## Mixer / queues / players

| Component | Status | Notes |
|---|---|---|
| mixer | 🟢 | arch-guarded; used implicitly nowhere yet — should just work |
| play_memory | 🟢 | |
| play_queue, record_queue | 🟡 | work, but buffer counts fall to the small `#else` (32/53 vs 80/209) — one-line guard fix each |
| play_sd_wav | ✅ | in gates (SD via USDHC) |
| play_sd_raw | 🟢 | same SD path as play_sd_wav |
| play_serialflash_raw | 🟣 | needs the SerialFlash library (not in the evkb manifest) + an external flash chip |
| Quantizer, Resampler | 🟢 | portable C++/float support classes (used by async_input_spdif3) |

## Hardware I/O nodes (all compile empty today — whole-file `__IMXRT1062__` guards)

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
| Audio.h (master include) | 🟡 | **not currently compilable on 1176** — the guard sweep across its 81 includes is still pending. Gates cherry-pick individual headers instead |

## Roadmap

**Phase A — no hardware needed, biggest surface unlock**
1. **CMSIS-DSP — DONE 2026-07-21**: landed as a pinned manifest library in
   evkb (`import_evkb_cmsis_dsp()` in `evkb.cmake`, CMSIS-DSP v1.17.1 +
   CMSIS_6, Apache-2.0 — passes the license firewall) rather than inside
   `cores/imxrt1176`. Proven by two HW-verified gates:
   `examples/framework/arm_math_test` (known-answer FFT/FIR/sin) and
   `examples/audio/filter_fir_test` (sine → AudioFilterFIR boxcar →
   AudioAnalyzeFFT256 + peak, zero Audio source changes). The former 🔵 rows
   are unblocked (filter_fir + analyze_fft256 already ✅).
2. **Guard sweep** (the 🟡 rows): add `__IMXRT1176__` — or better, switch the
   chip lists to `__ARM_ARCH_7EM__` — in synth_karplusstrong, synth_simple_drum,
   synth_wavetable, effect_delay.h, play_queue.h, record_queue.h. Strip the
   dead `arm_math` includes (synth_waveform.h, analyze_notefreq).
3. **"Audio.h compiles" gate**: an evkb QEMU gate that `#include <Audio.h>`,
   compiles every portable node, and runs a known-answer chain
   (waveform → filter → fft → assert bin). This pins the whole 🟢 tier and
   catches regressions.

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
DAC/ADC/PWM nodes as-is; anything on the CM4.
