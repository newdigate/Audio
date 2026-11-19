/* play_buffer_core.h - AudioPlayerCore: variable-rate playback of 16-bit PCM
 * held in memory.  Header-only and Arduino-free so it is unit-tested on the
 * host (test/play_buffer_core_test.cpp); AudioPlayer (play_buffer.h) wraps it
 * as an AudioStream.
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The 4-point interpolation is the one in teensy-variable-playback
 * (github.com/newdigate/teensy-variable-playback, MIT, same author), its
 * fastinterpolate(), restated for t in [0,1) between the two middle points.
 */
#ifndef play_buffer_core_h_
#define play_buffer_core_h_

#include <stdint.h>

/* Lagrange polynomial through p0..p3 at x = -1, 0, 1, 2, evaluated at t in
 * [0,1).  Saturates to int16. */
static inline int16_t audioplayer_interp4(int16_t p0, int16_t p1, int16_t p2, int16_t p3, float t)
{
    const float tm1 = t - 1.0f, tm2 = t - 2.0f, tp1 = t + 1.0f;
    const float y = (float)p0 * (-t * tm1 * tm2 * (1.0f / 6.0f))
                  + (float)p1 * (tp1 * tm1 * tm2 * 0.5f)
                  + (float)p2 * (-tp1 * t * tm2 * 0.5f)
                  + (float)p3 * (tp1 * t * tm1 * (1.0f / 6.0f));
    const float r = y < 0.0f ? y - 0.5f : y + 0.5f;
    if (r >= 32767.0f) return 32767;
    if (r <= -32768.0f) return -32768;
    return (int16_t)r;
}

class AudioPlayerCore {
public:
    void setOutputRate(float hz) { if (hz > 0.0f) outRate_ = hz; }

    /* channels 1 or 2, interleaved.  The memory is not owned.  Resets to
     * frame 0, paused, with every frame available. */
    void setSource(const int16_t *pcm, uint32_t frames, uint8_t channels, float sampleRate)
    {
        pcm_ = pcm;
        frames_ = pcm ? frames : 0;
        avail_ = frames_;
        ch_ = (channels == 1) ? 1 : 2;
        srcRate_ = sampleRate > 0.0f ? sampleRate : outRate_;
        pos_ = 0;
        step_ = 0;
        playing_ = false;
        starved_ = false;
    }
    void setAvailableFrames(uint32_t n) { avail_ = n < frames_ ? n : frames_; }
    /* Rate changes, play() and pause() take effect as a linear ramp of the
     * step across the next render() block: no click, no slope jump. */
    void setRate(float rate) { rate_ = rate; }
    void play() { if (avail_ > 0) playing_ = true; }
    void pause() { playing_ = false; }          /* isPlaying() is false at once; the ramp-down still renders */
    bool isPlaying() const { return playing_; }
    void seek(double frame)
    {
        if (avail_ == 0) { pos_ = 0; return; }
        if (!(frame > 0.0)) frame = 0.0;
        if (frame > (double)(avail_ - 1)) frame = (double)(avail_ - 1);
        pos_ = (int64_t)(frame * 4294967296.0);
        starved_ = false;
    }
    double   position() const { return (double)pos_ / 4294967296.0; }
    uint32_t lengthFrames() const { return frames_; }
    uint32_t availableFrames() const { return avail_; }

    /* Always writes n samples to both outputs. */
    void render(int16_t *left, int16_t *right, uint32_t n)
    {
        /* Ramp from the current step to the target over this block.  Sample i
         * uses the step at the middle of its interval, so the block advances
         * exactly n * (start + target) / 2. */
        const int64_t start = step_;
        const int64_t target = playing_ ? stepFor(rate_) : 0;
        const int64_t delta = target - start;
        bool moving = playing_ || start != 0;
        for (uint32_t i = 0; i < n; i++) {
            left[i] = 0;
            right[i] = 0;
            if (!moving || avail_ == 0) continue;
            const int64_t step = start + (delta * (int64_t)(2 * i + 1)) / (int64_t)(2 * n);
            const int64_t next = pos_ + step;
            if (starved_) {
                if (step > 0 && (next >> 32) >= (int64_t)avail_) continue;
                starved_ = false;
            }
            sample(left[i], right[i]);
            if (next < 0) {                                   /* ran off the start */
                pos_ = 0;
                playing_ = false;
                moving = false;
            } else if ((next >> 32) >= (int64_t)avail_) {
                if (avail_ >= frames_) {                      /* the real end */
                    pos_ = (int64_t)(frames_ - 1) << 32;
                    playing_ = false;
                    moving = false;
                } else {
                    starved_ = true;                          /* load has not got here yet: hold */
                }
            } else {
                pos_ = next;
            }
        }
        step_ = moving ? target : 0;
    }

private:
    int64_t stepFor(float rate) const
    {
        return (int64_t)((double)rate * (double)srcRate_ / (double)outRate_ * 4294967296.0);
    }
    /* Neighbour index clamped to the frames that are valid to read. */
    uint32_t at(int64_t i) const
    {
        if (i < 0) return 0;
        if (i >= (int64_t)avail_) return avail_ - 1;
        return (uint32_t)i;
    }
    void sample(int16_t &l, int16_t &r) const
    {
        const int64_t i = pos_ >> 32;
        const float t = (float)(uint32_t)(pos_ & 0xFFFFFFFFu) * (1.0f / 4294967296.0f);
        const uint32_t a = at(i - 1), b = at(i), c = at(i + 1), d = at(i + 2);
        if (ch_ == 1) {
            l = r = audioplayer_interp4(pcm_[a], pcm_[b], pcm_[c], pcm_[d], t);
        } else {
            l = audioplayer_interp4(pcm_[2 * a], pcm_[2 * b], pcm_[2 * c], pcm_[2 * d], t);
            r = audioplayer_interp4(pcm_[2 * a + 1], pcm_[2 * b + 1], pcm_[2 * c + 1], pcm_[2 * d + 1], t);
        }
    }

    const int16_t *pcm_ = nullptr;
    uint32_t frames_ = 0, avail_ = 0;
    uint8_t  ch_ = 2;
    float    srcRate_ = 44100.0f, outRate_ = 44100.0f;
    int64_t  pos_ = 0;                 /* Q32.32 source frames */
    int64_t  step_ = 0;                /* Q32.32 frames per output sample, as left by the last block */
    float    rate_ = 1.0f;
    bool     playing_ = false, starved_ = false;
};

#endif
