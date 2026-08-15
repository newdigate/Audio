/* Audio Library for Teensy - 16-step TB-303 style sequencer
 * Copyright (c) 2026, Nic Newdigate
 * MIT license -- see seq_step.h for the full text.
 */

#include "seq_step.h"

void AudioStepSequencer::step(int i, uint8_t note, bool gate, bool accent, bool slide) {
	if (i < 0 || i >= STEPS) return;        // ignore an out-of-range index
	if (note > 127) note = 127;
	__disable_irq();
	pattern_[i].note   = note;
	pattern_[i].gate   = gate;
	pattern_[i].accent = accent;
	pattern_[i].slide  = slide;
	__enable_irq();
}

AcidStep AudioStepSequencer::step(int i) const {
	if (i < 0 || i >= STEPS) return AcidStep();   // a rest
	return pattern_[i];
}

void AudioStepSequencer::clear() {
	__disable_irq();
	for (int i = 0; i < STEPS; i++) pattern_[i] = AcidStep();
	currentStep_ = -1;
	// ★ A NOTE SOUNDING WHEN THE PATTERN IS CLEARED IS RELEASED -- but not from
	// here. clear() runs in USER CONTEXT and must not push into the queue:
	// update() sets eventCount_ = 0 at the top of every block, so an event
	// pushed between two blocks is overwritten before a drainer running at block
	// rate could ever see it. Recording that a release is OWED and letting
	// update() emit it in-band costs one flag and has no race at all.
	//
	// Dropping it instead is not an option, whatever the queue contract says: a
	// caller that clears mid-playback (the step_seq_test sketch does, between
	// measurement phases, with the transport still running) would leave the
	// voice sounding with nothing that could ever release it -- an audible stuck
	// note for the rest of the session.
	//
	// ★ THE FLAG IS ONLY EVER SET, NEVER CLEARED, HERE. Writing it as
	// `clearPending_ = heldPending_` looks equivalent and is not: two clear()
	// calls between two blocks -- entirely ordinary in setup() code -- would
	// have the second one, which finds nothing sounding, cancel the release the
	// first one owed. update() is the only thing that retires it.
	if (heldPending_) {
		clearNote_    = (uint8_t)heldNote_;
		clearPending_ = true;
	}
	// Drop the sounding-note state wholesale. heldSlide_ is only ever READ as
	// `heldPending_ && heldSlide_`, so clearing it is not required for
	// correctness -- it is cleared so that no single field can outlive the note
	// it describes, which is the invariant the rest of this file relies on. It
	// is also what stops the deferred release from resurrecting a tie: the
	// cleared note is released outright, never slid into whatever step the new
	// pattern happens to start with.
	heldNote_ = -1; heldPending_ = false; heldSlide_ = false;
	__enable_irq();
}

void AudioStepSequencer::gateLength(float fraction) {
	// Clamped so a gate can neither be zero-length nor swallow the next step.
	if (!(fraction >= 0.1f)) fraction = 0.1f;     // also catches NaN
	if (fraction > 0.9f)     fraction = 0.9f;
	uint8_t t = (uint8_t)(fraction * TICKS_PER_STEP + 0.5f);
	if (t < 1) t = 1;
	__disable_irq();
	gateTicks_ = t;
	__enable_irq();
}

void AudioStepSequencer::accentVelocity(uint8_t v) {
	if (v > 127) v = 127;
	__disable_irq(); accentVel_ = v; __enable_irq();
}

void AudioStepSequencer::normalVelocity(uint8_t v) {
	if (v > 127) v = 127;
	__disable_irq(); normalVel_ = v; __enable_irq();
}

void AudioStepSequencer::push(uint8_t type, uint8_t note, uint8_t vel,
                              bool slide, uint16_t offset) {
	// ★ SATURATE, never wrap, and report it. This guard is UNREACHABLE through
	// the transport's public API and is kept anyway: tempo() clamps at 999 BPM,
	// which is 4.64 ticks per block, and a block can hold at most ONE step
	// boundary at any loop length (boundaries are 24 folded ticks apart, and a
	// loop shorter than that has exactly one, at folded 0, recurring every
	// MIN_LOOP_TICKS >= 8). So the worst block is a gate-off, a step boundary's
	// two events and a seam drop -- four. The counted alternative, resetting
	// eventCount_ on overflow, would silently rewrite the queue from the start
	// and hand the drainer a reordered stream, which is the failure mode this
	// whole class exists to avoid.
	if (eventCount_ >= MAX_EVENTS) { overflow_ = true; return; }
	events_[eventCount_].type         = type;
	events_[eventCount_].note         = note;
	events_[eventCount_].velocity     = vel;
	events_[eventCount_].slide        = slide;
	events_[eventCount_].sampleOffset = offset;
	eventCount_++;
}

void AudioStepSequencer::emitOff(uint8_t note, uint16_t offset) {
	push(SEQ_NOTE_OFF, note, 0, false, offset);
}

void AudioStepSequencer::update(void) {
	eventCount_ = 0;
	overflow_   = false;

	// ★ FIRST, the release clear() deferred to us -- see the note there. It goes
	// ahead of everything else because the note it names belongs to the pattern
	// that has already been discarded: nothing later in this block may tie to
	// it, and by the time clear() returned heldPending_ was already false, so
	// nothing later in this block can. Offset 0 because it is owed from before
	// this block began. Emitted whether or not the transport is playing or
	// looping, which is the point -- the usual sequence is clear() while stopped.
	if (clearPending_) {
		emitOff((uint8_t)clearNote_, 0);
		clearPending_ = false;
	}

	// A wrap means the playhead jumped backward. Drop the pending release: the
	// tick it was waiting for will not arrive in order, and a note left held
	// across a restart is the one failure a listener notices immediately.
	//
	// ★ TWO CONSEQUENCES, BOTH ACCEPTED DELIBERATELY.
	//  1. The note-off carries offset 0 rather than the offset of the tick it
	//     was scheduled for, so it can land up to one block (2.9 ms) early. The
	//     alternative -- carrying the release across the seam -- means a note
	//     that outlives its loop, which is audibly worse and much harder to
	//     reason about.
	//  2. It fires for a SLID note too, so a slide whose tie would cross the
	//     seam retriggers instead of gliding. A slide is a tie between two
	//     steps; a seam is where the pattern restarts, and a tie that survives a
	//     restart would glide from the end of the pattern into its beginning
	//     forever.
	if (transport_.wrapped() && heldPending_) {
		emitOff((uint8_t)heldNote_, 0);
		heldNote_ = -1; heldPending_ = false; heldSlide_ = false;
	}

	const uint32_t loopTicks = transport_.loopTicks();
	if (loopTicks == 0) return;             // not reachable via loop(); guard anyway

	for (int i = 0; i < transport_.tickCount(); i++) {
		// FOLD the absolute tick into the loop before deriving a step. An
		// unfolded index runs ...383, 384, 1, 2... across the seam and yields
		// FIVE quarters per bar -- the audible flam already found in
		// acid_bass_test. This is the idiom transport.h documents.
		//
		// ★ THE TWO BRANCHES ARE NOT INTERCHANGEABLE, and the second is not
		// dead code. `abs` is below `base` whenever the playhead is behind the
		// loop start -- ordinary with looping(false), where the transport
		// free-runs from 0 while loop() still reports a start bar above it, and
		// transiently during the re-entry normalisation transport.cpp:164
		// performs. The naive one-liner `(abs - base) % loopTicks` is WRONG
		// there: these are uint32_t, so the subtraction wraps modulo 2^32 and
		// the remainder becomes ((abs - base) mod 2^32) mod loopTicks, which
		// equals the wanted (abs - base) mod loopTicks only when loopTicks
		// divides 2^32 -- i.e. only for power-of-two loop lengths. 384 is not
		// one. The form below computes the true mathematical modulus in
		// unsigned arithmetic: `(base - abs) % loopTicks` is the distance below
		// the loop start, subtracting it from loopTicks reflects it into
		// (0, loopTicks], and the outer `% loopTicks` maps the loopTicks case
		// back to 0.
		uint32_t abs    = transport_.tickAt(i);
		uint32_t base   = transport_.loopStartTick();
		uint32_t folded = (abs >= base) ? ((abs - base) % loopTicks)
		                                : ((loopTicks - ((base - abs) % loopTicks)) % loopTicks);
		uint16_t off    = transport_.tickOffsetAt(i);

		// Release first, unless the sounding step slid -- a slide emits NO
		// note-off at all, which is what makes the voice glide.
		if (heldPending_ && !heldSlide_ && folded == heldOffTick_) {
			emitOff((uint8_t)heldNote_, off);
			heldNote_ = -1; heldPending_ = false;
		}

		if (folded % TICKS_PER_STEP != 0) continue;    // not a step boundary
		int idx = (int)((folded / TICKS_PER_STEP) % STEPS);
		const AcidStep &s = pattern_[idx];
		currentStep_ = idx;
		if (!s.gate) {
			// ★ A REST ENDS A SOUNDING NOTE -- unless that note slid. The spec
			// does not say this; it says only that a rest emits no note-on.
			// Without the release a rest after a gated step sustains straight
			// through the gap, which is not what a rest sounds like: the gap is
			// the point. The slide exemption is the same rule as everywhere
			// else in this file -- a slid note is tied forward and is released
			// by whatever step finally takes it over (or by the seam above),
			// never by an intervening rest.
			if (heldPending_ && !heldSlide_) {
				emitOff((uint8_t)heldNote_, off);
				heldNote_ = -1; heldPending_ = false;
			}
			continue;
		}

		// ★ ORDER IS THE SEQUENCER'S JOB. When the sounding step slid, the new
		// note-on is emitted BEFORE the old note-off, so a drainer applying
		// events blindly in order gets legato and the voice glides. Emitting
		// them the other way round retriggers instead -- the defect the
		// acid_bass example originally shipped.
		bool tie = heldPending_ && heldSlide_;
		// ★ A STALE NON-SLID RELEASE IS HONOURED HERE, and this is not
		// belt-and-braces -- without it a note hangs forever. In the ordinary
		// case it costs nothing: gateTicks_ is clamped below TICKS_PER_STEP, so
		// a non-slid note is always released by its own gate before the next
		// step boundary and heldPending_ is already false. It goes true here
		// only when the folded tick the release was waiting for stopped
		// existing -- which loop() does, mid-playback, with no wrap to trigger
		// the seam handler above: shift a 384-tick loop to loop(0.3f, 0.5f)
		// while a note is pending at folded 118 and the new loop is 77 ticks
		// long, so 118 is never produced again. MEASURED on the host harness
		// against the version without these four lines: 16 note-ons, 13
		// note-offs, three notes left sounding with nothing that would ever
		// release them. Releasing FIRST (rather than after, as a tie does) is
		// what makes it safe when the stale note and the new one share a pitch.
		if (heldPending_ && !tie) {
			emitOff((uint8_t)heldNote_, off);
			heldNote_ = -1; heldPending_ = false;
		}
		push(SEQ_NOTE_ON, s.note, s.accent ? accentVel_ : normalVel_, tie, off);
		if (tie) {
			emitOff((uint8_t)heldNote_, off);
		}
		heldNote_    = s.note;
		heldSlide_   = s.slide;
		heldPending_ = true;
		// ★ THE RELEASE TICK IS FOLDED, AND THAT IS EXACT, not an approximation.
		// Folded ticks advance by one per tick and wrap at loopTicks, so
		// (folded + gateTicks_) % loopTicks IS the folded index gateTicks_ ticks
		// later -- for a 200-tick loop, a step at folded 192 releases at folded
		// 4, which is precisely 12 ticks on. It reduces only when
		// gateTicks_ >= loopTicks, which needs a loop under 22 ticks (the
		// transport's floor is MIN_LOOP_TICKS = 8) -- a loop shorter than a
		// single step, where the note is clipped to the loop rather than
		// outliving it. That is the intended reading throughout: A NOTE NEVER
		// OUTLIVES ITS LOOP. In every ordinary configuration the seam handler
		// above gets there first anyway, because a release scheduled past the
		// loop end is pre-empted by the wrap.
		heldOffTick_ = (folded + gateTicks_) % loopTicks;
	}
}
