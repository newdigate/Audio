/* Audio Library for Teensy - transport: tempo, song position and loop
 * Copyright (c) 2026, Nic Newdigate
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef transport_h_
#define transport_h_

#include <Arduino.h>
#include <AudioStream.h>

// Tempo, song position and a musical loop, advanced on the AUDIO BLOCK CLOCK.
//
// It is an AudioStream with no inputs and no outputs purely so update_all()
// calls update() once per block. That matters: the audio graph runs on its own
// clock and drifts against wall time (measured 0.81x wall under QEMU, 1.0x on
// silicon), so a transport ticked from loop() or an IntervalTimer would slide
// against the audio it is sequencing.
//
// ★ TWO THINGS ARE NEEDED FOR update() TO RUN AT ALL, and neither is automatic
// for a node with no AudioConnection:
//
//  1. `active` must be true. AudioStream's constructor sets it FALSE
//     (AudioStream.h:140) and the only thing that ever sets it true is
//     AudioConnection::connect() (AudioStream.cpp:222,225); software_isr skips
//     every node whose `active` is false (AudioStream.cpp:323). This class has
//     no connection -- it is a clock source, not an audio-data node -- so the
//     constructor sets `active = true` itself. Delete that line and update()
//     is never called, samples() stays 0 and every query is frozen.
//  2. SOMETHING must pend IRQ_SOFTWARE so update_all() runs. In a normal graph
//     that is an I/O node's DMA (AudioOutputI2S in the transport_test example).
//     A graph containing ONLY a transport and no I/O node has no such source
//     and would need AudioStream::update_setup().
//
// TWO COUNTERS, TWO QUESTIONS -- do not confuse them:
//   samples()/seconds()  ELAPSED AUDIO TIME. Monotonic, unaffected by looping.
//   beats()/bars()/...   SONG POSITION. Derived from the tick phase, so it
//                        WRAPS at the loop seam.
// In the mulch Transport these are one number; a reader porting from it will
// expect seconds() to be the playhead, and here it is not.
class AudioTransport : public AudioStream
{
public:
	static const uint32_t PPQN = 96;        // pulses per quarter note; 4x MIDI clock's 24
	static const int MAX_TICKS_PER_BLOCK = 8;
	// ★ Minimum loop length. Not arbitrary: the seam carries the overshoot as
	// `(to - loopEnd_) % len`, and that modulo must never actually reduce or
	// the surplus boundaries vanish silently. The overshoot is under one
	// block's advance, which the 999 BPM tempo clamp caps at 4.64 ticks, so any
	// minimum above 5 makes the modulo an identity. 8 ticks is 1/12 of a beat
	// -- below any musical grid, and comfortably above the cap.
	static const uint32_t MIN_LOOP_TICKS = 8;

	AudioTransport() : AudioStream(0, NULL) {
		recalcTickInc();
		active = true;    // MUST be last: publishes this node to update_all()
		                  // only once tickInc_ is valid. See the ★ note above --
		                  // this line is load-bearing, not boilerplate.
	}

	// ★ EVERY MUTATOR BELOW IS USER CONTEXT ONLY -- all of tempo, beatsPerBar,
	// play, pause, stop, rewindBar, forwardBar, loop, looping, externalClock
	// and externalPulse. Each mutates multi-word state that update() reads at
	// audio-ISR priority, so each wraps its stores in __disable_irq() /
	// __enable_irq() and ends with an UNCONDITIONAL __enable_irq(). Calling any
	// of them from an ISR, or from inside another critical section, therefore
	// re-enables interrupts early. (Same house rule as synth_acidbass.cpp /
	// effect_envelope.cpp.)

	// tempo & state
	void tempo(float bpm);
	void beatsPerBar(uint8_t n);
	void play();
	void pause();
	void stop();                            // rewinds to loop start, else to 0
	void rewindBar();
	void forwardBar();
	bool playing() const { return playing_; }
	float tempo() const  { return bpm_; }
	uint8_t beatsPerBar() const { return beatsPerBar_; }

	// loop -- bounds in BARS, so they survive tempo changes. USER CONTEXT ONLY.
	// Both bounds snap to whole ticks and the length is forced to at least
	// MIN_LOOP_TICKS; loopTicks() reports what was actually stored.
	void loop(float startBar, float endBar);
	void looping(bool on);
	bool looping() const { return looping_; }

	// query -- see the two-counter note above. Safe to call unguarded from any
	// context: each reads a single field. (samples() is 64-bit and so not an
	// atomic load on Cortex-M7 -- guard it at the call site if you need it
	// consistent with another read.)
	uint64_t samples() const { return samplesElapsed_; }
	float seconds() const { return (float)samplesElapsed_ / AUDIO_SAMPLE_RATE_EXACT; }
	float beats() const   { return (float)(tickPhase_ >> 32) / (float)PPQN; }
	float bars() const    { return beats() / (float)beatsPerBar_; }
	int barNumber() const { return (int)bars() + 1; }
	int beatInBar() const { return (int)beats() % beatsPerBar_ + 1; }
	uint32_t ticksPerBar() const   { return PPQN * beatsPerBar_; }
	uint32_t loopTicks() const     { return (uint32_t)((loopEnd_ - loopStart_) >> 32); }
	uint32_t loopStartTick() const { return (uint32_t)(loopStart_ >> 32); }

	// --- THE SEQUENCER INTERFACE: ticks that began during the block just
	// processed. Three things a consumer must know:
	//
	// 1. DECLARE YOUR CONSUMER AFTER THE TRANSPORT. AudioStream appends each
	//    node to the tail of first_update (AudioStream.h:147-154) and
	//    software_isr walks that list head-first, so update order IS
	//    construction order. A consumer declared before the transport sees the
	//    PREVIOUS block's span -- a constant one-block latency, not drift, but
	//    not what you meant either.
	// 2. THESE ARE NOT SAFE TO READ UNGUARDED FROM USER CONTEXT, unlike the
	//    position queries above. update() sets tickCount_ = 0 and then fills
	//    the arrays, so a user-context reader can catch a partially built span.
	//    A consumer running inside update_all()'s serial walk is stable by
	//    construction; anything in loop() must snapshot under __disable_irq().
	// 3. FOLD THE INDEX BEFORE DERIVING A STEP. tickAt() returns an ABSOLUTE
	//    musical tick, so across a 1-bar seam the stream runs ...383, 384, 1, 2
	//    and a bare `tickAt(i) / PPQN` yields FIVE quarters per bar, firing two
	//    events about 5 ms apart. 384 and 0 are the same musical position. The
	//    canonical fold is:
	//        (tickAt(i) - loopStartTick()) % loopTicks()
	int      tickCount() const { return tickCount_; }
	uint32_t tickAt(int i) const {
		return (i >= 0 && i < tickCount_) ? tickIdx_[i] : 0;
	}
	uint16_t tickOffsetAt(int i) const {
		return (i >= 0 && i < tickCount_) ? tickOff_[i] : 0;
	}
	bool tickOverflow() const { return overflow_; }   // >8 ticks in one block
	// True when the playhead jumped backward during the block just processed --
	// either the loop seam, or a re-entry normalisation after the playhead was
	// left outside the loop. At 120 BPM a block holds 0 or 1 tick, so a
	// consumer cannot infer this from tickAt() decreasing.
	bool wrapped() const { return wrapped_; }

	// external clock (MIDI sync scaffolding). USER CONTEXT ONLY -- note that
	// externalPulse() is guarded like the rest, so it CANNOT be called from a
	// MIDI receive ISR. Latch the pulse and call this from loop().
	//
	// ★ externalClock(true) MAKES THE WHOLE TICK INTERFACE INERT. update()
	// returns before emit(), so tickCount() is permanently 0, wrapped() never
	// becomes true and the loop never wraps -- externalPulse() advances the
	// phase, and position queries follow it, but nothing is emitted. A
	// sequencer driven off tickCount() will simply stop. Only samplesElapsed_
	// keeps advancing, because audio is still being produced.
	void externalClock(bool on);
	void externalPulse();                   // one MIDI pulse = PPQN/24 ticks

	virtual void update(void);

private:
	void recalcTickInc();                   // tick increment per block, from bpm_
	static uint64_t tickIncFor(float bpm);  // the same arithmetic, no stores
	void emit(uint64_t from, uint64_t to, uint32_t sampleBase, uint32_t sampleSpan);

	// Bars -> 32.32 tick phase, ROUNDED TO A WHOLE TICK.
	//
	// ★ The rounding is not cosmetic. The seam splits a block at loopEnd_ and
	// resumes at loopStart_, so both bounds must carry the same way when the
	// overshoot is added: writing a and b for their fractional tick parts, the
	// per-wrap error is [a+c >= 1] - [b+c >= 1] for an overshoot fraction c,
	// and it vanishes for all c exactly when a == b. Rounding here forces
	// a == b == 0. Without it, loop(0, 0.1) -- an ordinary call, since loop()
	// validates only endBar > startBar -- asks for 38.4 ticks and silently
	// drops boundaries at the seam, which is a sequencer step that never fires.
	static uint64_t barsToWholeTickPhase(float bars, uint8_t bpb) {
		double ticks = (double)bars * (double)bpb * (double)PPQN;
		if (!(ticks > 0.0)) return 0;             // negative, zero and NaN alike
		if (ticks > 4294967294.0) ticks = 4294967294.0;   // keep the cast in range
		return (uint64_t)(ticks + 0.5) << 32;
	}

	// 32.32 fixed point: upper 32 bits are the tick index, lower 32 the fraction.
	// Read arithmetically only by update(), so not volatile.
	uint64_t tickPhase_   = 0;
	uint64_t tickInc_     = 0;              // ticks per block, 32.32
	// Loop bounds and configuration: written in user context, read by update().
	uint64_t loopStart_   = 0;
	uint64_t loopEnd_     = 0;
	float    bpm_         = 120.0f;
	uint8_t  beatsPerBar_ = 4;
	bool     playing_     = false;
	bool     looping_     = false;
	bool     external_    = false;
	// ★ WRITTEN BY update() AT ISR PRIORITY, READ BY INLINE CONST ACCESSORS, so
	// these must be volatile. Without it `while (t.samples() == s) {}` is a
	// legal infinite loop at -O2: the compiler may hoist the load out. Code in
	// this tree survives today only because it calls yield() in the wait, an
	// opaque call that forces a reload -- which is luck, not a guarantee.
	volatile uint64_t samplesElapsed_ = 0;
	volatile int      tickCount_ = 0;
	volatile bool     overflow_  = false;
	volatile bool     wrapped_   = false;
	// The span arrays are ISR-written too, but are guarded by tickCount_ rather
	// than being volatile themselves -- see note 2 on the sequencer interface.
	uint32_t tickIdx_[MAX_TICKS_PER_BLOCK] = {0};
	uint16_t tickOff_[MAX_TICKS_PER_BLOCK] = {0};
};

#endif
