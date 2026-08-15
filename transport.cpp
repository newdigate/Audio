/* Audio Library for Teensy - transport: tempo, song position and loop
 * Copyright (c) 2026, Nic Newdigate
 * MIT license -- see transport.h for the full text.
 */

#include "transport.h"

// Ticks advanced per block, in 32.32 fixed point:
//   ticks/block = PPQN * bpm * AUDIO_BLOCK_SAMPLES / (60 * sr)
// Computed in double and converted once per tempo change, so the per-block
// path is a single 64-bit add. Rounding is under 2^-32 = 2.3e-10 tick per
// block, i.e. 2e-4 tick over a million blocks -- inaudible over any session.
uint64_t AudioTransport::tickIncFor(float bpm) {
	double t = (double)PPQN * (double)bpm * (double)AUDIO_BLOCK_SAMPLES
	           / (60.0 * (double)AUDIO_SAMPLE_RATE_EXACT);
	return (uint64_t)(t * 4294967296.0);
}

void AudioTransport::recalcTickInc() { tickInc_ = tickIncFor(bpm_); }

void AudioTransport::tempo(float bpm) {
	// ★ The NEGATED form is deliberate -- do not "simplify" it to `bpm < 20.0f`.
	// Every comparison against NaN is false, so a plain `<` lets NaN through
	// BOTH clamps: bpm_ becomes NaN, the increment arithmetic turns that into
	// tickInc_ == 0, and the transport silently freezes while tempo() cheerfully
	// reports NaN. `!(bpm >= 20.0f)` is true for NaN, so NaN clamps to the floor
	// like any other out-of-range value.
	if (!(bpm >= 20.0f)) bpm = 20.0f;
	if (bpm > 999.0f)    bpm = 999.0f;
	// Double arithmetic OUTSIDE the critical section; guard only the stores.
	uint64_t inc = tickIncFor(bpm);
	__disable_irq();
	bpm_     = bpm;
	tickInc_ = inc;
	__enable_irq();
}

void AudioTransport::beatsPerBar(uint8_t n) {
	if (n < 1)  n = 1;
	if (n > 32) n = 32;
	// Loop bounds are stored as a phase derived from the OLD beatsPerBar, so
	// they must be re-derived or a time-signature change would silently move
	// the loop. Recover the bar values first, then rebuild at the new signature.
	// Safe to read unguarded: update() never writes these three fields.
	float startBar = (float)((double)loopStart_ / 4294967296.0 / (double)PPQN / (double)beatsPerBar_);
	float endBar   = (float)((double)loopEnd_   / 4294967296.0 / (double)PPQN / (double)beatsPerBar_);
	uint64_t s = barsToWholeTickPhase(startBar, n);
	uint64_t e = barsToWholeTickPhase(endBar,   n);
	// Rebuilding at a coarser signature can round both bounds onto the same
	// tick, or below the minimum; apply exactly the floor loop() does, so a
	// loop can never be destroyed or shortened past safety by a signature
	// change. Re-derivation through barsToWholeTickPhase() also preserves the
	// whole-tick property the seam depends on.
	if (e < s + ((uint64_t)MIN_LOOP_TICKS << 32)) e = s + ((uint64_t)MIN_LOOP_TICKS << 32);
	__disable_irq();
	beatsPerBar_ = n;
	loopStart_   = s;
	loopEnd_     = e;
	__enable_irq();
}

void AudioTransport::play()  { __disable_irq(); playing_ = true;  __enable_irq(); }
void AudioTransport::pause() { __disable_irq(); playing_ = false; __enable_irq(); }

void AudioTransport::stop() {
	__disable_irq();
	playing_   = false;
	tickPhase_ = looping_ ? loopStart_ : 0;
	tickCount_ = 0;
	overflow_  = false;
	wrapped_   = false;
	__enable_irq();
}

void AudioTransport::rewindBar() {
	uint64_t bar = barsToWholeTickPhase(1.0f, beatsPerBar_);
	__disable_irq();
	tickPhase_ = (tickPhase_ > bar) ? tickPhase_ - bar : 0;
	__enable_irq();
}

void AudioTransport::forwardBar() {
	uint64_t bar = barsToWholeTickPhase(1.0f, beatsPerBar_);
	__disable_irq();
	tickPhase_ += bar;
	__enable_irq();
}

void AudioTransport::loop(float startBar, float endBar) {
	if (!(endBar > startBar)) return;       // ignore an invalid range
	if (startBar < 0.0f) startBar = 0.0f;
	uint64_t s = barsToWholeTickPhase(startBar, beatsPerBar_);
	uint64_t e = barsToWholeTickPhase(endBar,   beatsPerBar_);
	// Floor the LENGTH, not just guard against collapse. A range under
	// MIN_LOOP_TICKS passes the endBar > startBar test above and would leave a
	// loop shorter than one block's advance, at which point the seam's modulo
	// starts reducing and boundaries vanish with tickOverflow() false -- see
	// MIN_LOOP_TICKS in transport.h. Widening is better than ignoring the call:
	// update()'s `loopEnd_ > loopStart_` guard reads an empty range as "no loop
	// at all", silently discarding a call that passed validation.
	if (e < s + ((uint64_t)MIN_LOOP_TICKS << 32)) e = s + ((uint64_t)MIN_LOOP_TICKS << 32);
	__disable_irq();
	loopStart_ = s;
	loopEnd_   = e;
	__enable_irq();
}

void AudioTransport::looping(bool on) { __disable_irq(); looping_ = on; __enable_irq(); }

void AudioTransport::externalClock(bool on) { __disable_irq(); external_ = on; __enable_irq(); }

void AudioTransport::externalPulse() {
	uint64_t pulse = ((uint64_t)(PPQN / 24)) << 32;   // 4 ticks at 96 PPQN
	__disable_irq();
	if (external_) tickPhase_ += pulse;
	__enable_irq();
}

// Record every tick boundary strictly inside (from, to], with its sample offset
// inside the block. `sampleBase`/`sampleSpan` describe which part of the block
// this segment covers, so a loop wrap can call this twice with the right offsets.
//
// The half-open convention is what makes the seam exact. `first` is the lowest
// integer tick strictly above `from` and `last` the highest at or below `to`,
// so a boundary landing exactly on a segment edge is counted by the segment
// that ENDS there and never by the one that starts there -- no duplicate, no
// loss. It also means tick 0 is not emitted at phase 0: the count reported over
// a run is the number of boundaries CROSSED, not the number of ticks visited.
void AudioTransport::emit(uint64_t from, uint64_t to,
                          uint32_t sampleBase, uint32_t sampleSpan) {
	if (to <= from) return;                 // makes `den` below strictly positive
	uint64_t first = (from >> 32) + 1;
	uint64_t last  = to >> 32;
	uint64_t den   = to - from;
	for (uint64_t t = first; t <= last; t++) {
		if (tickCount_ >= MAX_TICKS_PER_BLOCK) { overflow_ = true; return; }
		uint64_t at = t << 32;
		// Where in this segment the boundary falls, scaled into samples.
		uint32_t off = sampleBase + (uint32_t)(((at - from) * (uint64_t)sampleSpan) / den);
		if (off > AUDIO_BLOCK_SAMPLES - 1) off = AUDIO_BLOCK_SAMPLES - 1;
		tickIdx_[tickCount_] = (uint32_t)t;
		tickOff_[tickCount_] = (uint16_t)off;
		tickCount_ = tickCount_ + 1;
	}
}

void AudioTransport::update(void) {
	// Elapsed AUDIO time advances whether or not the transport is playing:
	// audio is still being produced, and this counter answers "how much".
	samplesElapsed_ = samplesElapsed_ + AUDIO_BLOCK_SAMPLES;
	tickCount_ = 0;
	overflow_  = false;
	wrapped_   = false;
	if (!playing_ || external_) return;

	// ★ RE-ENTRY NORMALISATION. Several ordinary sequences leave the playhead
	// at or past loopEnd_ -- forwardBar() past the end, enabling a loop that
	// lies behind the current position, or beatsPerBar() re-deriving bounds at
	// a coarser signature. The wrap test below requires `from < loopEnd_`, so
	// without this the transport would escape its loop permanently and only
	// stop() would recover it. Under-run needs no handling: the playhead simply
	// runs forward into the loop and is captured by the seam. Doing it here
	// rather than in each mutator covers all three sites and any future one.
	if (looping_ && loopEnd_ > loopStart_ && tickPhase_ >= loopEnd_) {
		uint64_t len = loopEnd_ - loopStart_;
		tickPhase_ = loopStart_ + (tickPhase_ - loopStart_) % len;
		wrapped_   = true;      // the playhead did jump backward this block
	}

	uint64_t from = tickPhase_;
	uint64_t to   = from + tickInc_;

	if (looping_ && loopEnd_ > loopStart_ && to >= loopEnd_ && from < loopEnd_) {
		// The block straddles the seam. Emit the two segments separately so
		// every boundary is counted exactly once and its sample offset stays
		// inside the block -- the seam neither loses nor duplicates a tick.
		//
		// Boundary bookkeeping, which the LOOP gate asserts: segment A reports
		// (loopEnd_>>32) - (from>>32), segment B reports
		// ((loopStart_+over)>>32) - (loopStart_>>32), and their sum is exactly
		// (to>>32) - (from>>32) -- the count an unlooped block would have
		// reported for the same phase advance. The next block resumes from
		// loopStart_+over, whose floor is loopStart_>>32, so the telescoping
		// continues unbroken. Wrapping changes WHICH tick indices appear, never
		// HOW MANY boundaries are crossed.
		//
		// ★ TWO PRECONDITIONS make that unconditional, and both are enforced
		// rather than assumed:
		//   - both bounds are whole ticks, so they carry alike (see
		//     barsToWholeTickPhase in transport.h);
		//   - the loop is at least MIN_LOOP_TICKS long, so `over` below is the
		//     plain difference and the modulo never actually reduces. If it
		//     did, the reduced-away boundaries would vanish with
		//     tickOverflow() false -- silent loss, the mode all of this exists
		//     to prevent.
		//
		// The tickInc_ divide is safe: tempo() clamps bpm to >= 20, which makes
		// tickInc_ at least 0.0929 ticks/block, never zero.
		uint64_t spanA = loopEnd_ - from;
		uint32_t samplesA = (uint32_t)((spanA * AUDIO_BLOCK_SAMPLES) / tickInc_);
		emit(from, loopEnd_, 0, samplesA ? samplesA : 1);
		uint64_t len  = loopEnd_ - loopStart_;
		uint64_t over = (to - loopEnd_) % len;
		uint64_t newTo = loopStart_ + over;
		emit(loopStart_, newTo, samplesA,
		     AUDIO_BLOCK_SAMPLES > samplesA ? AUDIO_BLOCK_SAMPLES - samplesA : 1);
		tickPhase_ = newTo;
		wrapped_   = true;
	} else {
		emit(from, to, 0, AUDIO_BLOCK_SAMPLES);
		tickPhase_ = to;
	}
}
