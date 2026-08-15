/* Audio Library for Teensy - 16-step TB-303 style sequencer
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

#ifndef seq_step_h_
#define seq_step_h_

#include <Arduino.h>
#include <AudioStream.h>
#include "transport.h"

#define SEQ_NOTE_OFF 0
#define SEQ_NOTE_ON  1

// One pattern step -- the four things a TB-303 stores.
struct AcidStep {
	uint8_t note   = 33;      // 0..127
	bool    gate   = false;   // false = rest
	bool    accent = false;
	bool    slide  = false;   // tie into the next step
};

// One emitted event. sampleOffset says WHERE in the block the step falls; the
// drainer may apply it at block granularity (AudioSynthAcidBass has no offset
// parameter) but the queue does not throw the information away.
struct SeqEvent {
	uint8_t  type         = SEQ_NOTE_OFF;
	uint8_t  note         = 0;
	uint8_t  velocity     = 0;
	bool     slide        = false;
	uint16_t sampleOffset = 0;
};

// 16-step TB-303 style sequencer, clocked by an AudioTransport.
//
// ★ THE TRANSPORT IS TAKEN BY REFERENCE IN THE CONSTRUCTOR, AND THAT IS THE
// POINT. AudioTransport requires its consumers to be declared AFTER it --
// update order is construction order (AudioStream.h:147-154 appends at the
// tail, software_isr walks head-first), so a node declared earlier reads the
// PREVIOUS block's tick span, a silent 2.9 ms staleness. transport.h can only
// warn about that in prose. Binding by reference makes it unrepresentable:
// you cannot pass a reference to an object that has not been constructed.
//
//     AudioTransport     transport;
//     AudioStepSequencer seq(transport);   // will not compile in the wrong order
//
// ★ `active = true` in the constructor is NOT boilerplate. AudioStream's
// constructor sets active = false (AudioStream.h:140) and only
// AudioConnection::connect() sets it true (AudioStream.cpp:222,225);
// software_isr skips every inactive node (AudioStream.cpp:323). This node has
// no audio connections, so without that line update() is never called at all.
//
// ★ TICK 0 IS NEVER EMITTED AT PHASE 0. AudioTransport::emit() records tick
// boundaries strictly inside (from, to] (transport.cpp:132), so a run does not
// open on step 0: folded tick 0 is produced only at the loop seam, when the
// absolute tick equal to the loop length is crossed. A caller measuring "how
// many times did step 0 fire" is therefore counting SEAMS, and a run shorter
// than one loop fires step 0 zero times. This is the transport's behaviour, not
// this class's, and it is not worked around here -- synthesising a boundary at
// phase 0 would double-fire step 0 on every restart.
class AudioStepSequencer : public AudioStream
{
public:
	static const int STEPS          = 16;
	static const int TICKS_PER_STEP = AudioTransport::PPQN / 4;   // 24, a 16th
	static const int PATTERN_TICKS  = STEPS * TICKS_PER_STEP;     // 384, one 4/4 bar
	static const int MAX_EVENTS     = 8;

	AudioStepSequencer(AudioTransport &t)
		: AudioStream(0, NULL), transport_(t) {
		active = true;    // ★ LOAD-BEARING, NOT BOILERPLATE -- see the note
		                  // above. This node has no AudioConnection, so nothing
		                  // else will ever set it and update() would never run.
	}

	// pattern -- USER CONTEXT ONLY (these take __disable_irq guards)
	void step(int i, uint8_t note, bool gate, bool accent, bool slide);
	AcidStep step(int i) const;
	void clear();
	void gateLength(float fraction);      // clamped 0.1..0.9, default 0.5
	void accentVelocity(uint8_t v);       // default 127
	void normalVelocity(uint8_t v);       // default 80

	// -1 until the first step fires, so "nothing has played yet" is
	// distinguishable from "step 0 played".
	int currentStep() const { return currentStep_; }

	// THE EVENT QUEUE -- events emitted during the block just processed.
	// Same contract as AudioTransport's tick span, deliberately: valid only for
	// that block, stable inside update_all()'s walk, and a USER-CONTEXT reader
	// must snapshot under __disable_irq() because update() rebuilds it.
	//
	// ★ READING DOES NOT CONSUME. update() clears the queue at the top of each
	// block and refills it, so it keeps reporting the same events until the next
	// block arrives. A drainer called more often than once per block re-applies
	// every event; gate on the transport's own sample counter.
	int             eventCount() const { return eventCount_; }
	const SeqEvent &eventAt(int i) const {
		return (i >= 0 && i < eventCount_) ? events_[i] : empty_;
	}
	bool eventOverflow() const { return overflow_; }

	virtual void update(void);

private:
	void emitOff(uint8_t note, uint16_t offset);
	void push(uint8_t type, uint8_t note, uint8_t vel, bool slide, uint16_t offset);

	AudioTransport &transport_;
	AcidStep  pattern_[STEPS];
	SeqEvent  events_[MAX_EVENTS];
	SeqEvent  empty_;                       // returned for an out-of-range index
	volatile int  eventCount_ = 0;
	volatile bool overflow_   = false;
	volatile int  currentStep_ = -1;
	uint8_t  gateTicks_    = TICKS_PER_STEP / 2;   // 12 == 0.5 gate
	uint8_t  accentVel_    = 127;
	uint8_t  normalVel_    = 80;
	// sounding-note state
	int      heldNote_     = -1;            // -1 = nothing sounding
	bool     heldSlide_    = false;         // the sounding step had slide set
	uint32_t heldOffTick_  = 0;             // folded tick at which to release
	bool     heldPending_  = false;         // a release is still owed
	// ★ DEFERRED RELEASE FOR clear(). clear() runs in user context and cannot
	// push into the queue itself -- update() clears eventCount_ at the top of
	// every block, so a user-context push is overwritten before any drainer
	// running at block rate could see it. Instead clear() records that a release
	// is owed and update() emits it in-band on the next block. Written in user
	// context, consumed at audio-ISR priority, hence volatile.
	volatile bool    clearPending_ = false;
	volatile uint8_t clearNote_    = 0;
};

#endif
