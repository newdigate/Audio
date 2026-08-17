// Deterministic reproduction of the AudioAnalyzePeak stale-flag race that
// analyze_peak.h's read()/readPeakToPeak() fix (new_output cleared on consume).
//
// Models ONLY the flag/min/max state machine -- no Arduino, no audio, so it
// runs on the host in milliseconds. update() is invoked explicitly where the
// audio ISR would land, which makes an interleaving that is timing-dependent
// on hardware exact here.
//
//   c++ -std=c++17 -Wall -o /tmp/analyze_peak_race_test \
//       test/analyze_peak_race_test.cpp && /tmp/analyze_peak_race_test
//
// Expected: PASS, exit 0. It asserts BOTH directions -- that the pre-fix code
// really does produce the full-scale sentinel under the race, and that the
// post-fix code does not. A test that only checked the fix would still pass if
// the race stopped being modelled at all, which is the failure mode that makes
// regression tests quietly worthless.
//
// ★ THIS TEST WAS WRONG FIRST TIME, and the reason is the finding. The initial
// version called update() on every poll iteration, so available() was always
// backed by fresh data and the stale flag was never observable -- it reported
// "did not reproduce" against code known to be broken. The sketch that exposed
// the bug polls MUCH faster than the audio block rate; that ratio
// (POLLS_PER_BLOCK) is what makes a stale flag visible on its own. Keep it.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>

template <bool READ_CLEARS_FLAG>
struct Peak {
    volatile bool new_output = false;
    int16_t min_sample = 32767;
    int16_t max_sample = -32768;

    // One block of a 0.5-amplitude sine: +/- 16383.
    void update() {
        int min = min_sample, max = max_sample;
        for (int16_t d : {(int16_t)16383, (int16_t)-16383}) {
            if (d < min) min = d;
            if (d > max) max = d;
        }
        min_sample = (int16_t)min;
        max_sample = (int16_t)max;
        new_output = true;
    }
    bool available() {
        bool flag = new_output;
        if (flag) new_output = false;
        return flag;
    }
    float read() {
        int min = min_sample, max = max_sample;
        min_sample = 32767;
        max_sample = -32768;
        if (READ_CLEARS_FLAG) new_output = false;   // <-- the fix
        min = abs(min);
        max = abs(max);
        if (min > max) max = min;
        return (float)max / 32767.0f;
    }
};

// The sketch's setup() window:
//
//     while (millis() - t0 < 500) {
//         if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
//         yield();
//     }
//
// Two things make this the shape that exposes the bug:
//   * the poll loop runs MUCH faster than the audio block rate, so most
//     iterations see available() == false. That is what lets a stale flag be
//     observed on its own, with no update() behind it.
//   * pk is a running MAXIMUM, so one poisoned read outlives every correct one.
//
// POLLS_PER_BLOCK models the first. The ISR is injected between available()
// and read() on exactly one iteration -- on hardware that is a timing
// coincidence; here it is explicit, so the result is deterministic.
template <bool FIXED>
static float run_window(int isr_lands_on_poll) {
    Peak<FIXED> peak;
    const int POLLS_PER_BLOCK = 10;
    float pk = 0.0f;
    for (int poll = 0; poll < 200; poll++) {
        if (poll % POLLS_PER_BLOCK == 0) peak.update();   // the graph, async
        if (peak.available()) {
            if (poll == isr_lands_on_poll) peak.update(); // ★ ISR in the window
            float v = peak.read();
            if (v > pk) pk = v;
        }
    }
    return pk;
}

int main() {
    const int ISR_POLL = 20;                 // any poll on which available() is true
    const float buggy = run_window<false>(ISR_POLL);
    const float fixed = run_window<true>(ISR_POLL);
    const float sentinel = 32768.0f / 32767.0f;

    // Control: with the ISR never landing in the window, even the pre-fix code
    // is correct. This is what proves the test blames the RACE and not merely
    // the class, i.e. that it would not "pass" for the wrong reason.
    const float buggy_no_isr = run_window<false>(-1);
    printf("pre-fix, ISR never in window = %.4f  %s\n", buggy_no_isr,
           (buggy_no_isr > 0.40f && buggy_no_isr < 0.60f)
               ? "(correct -- the class is only wrong under the race)"
               : "<-- unexpected");

    printf("sentinel value            = %.7f (prints as %.4f)\n", sentinel, sentinel);
    printf("pre-fix  peak over window = %.4f  %s\n", buggy,
           buggy > 0.99f ? "<-- REPRODUCED: full-scale from no data" : "(did not reproduce)");
    printf("post-fix peak over window = %.4f  %s\n", fixed,
           (fixed > 0.40f && fixed < 0.60f) ? "<-- correct, in the sketch's 0.40..0.60 band" : "<-- WRONG");

    int rc = 0;
    if (!(buggy > 0.99f))                    { printf("FAIL: race did not reproduce pre-fix\n"); rc = 1; }
    if (!(fixed > 0.40f && fixed < 0.60f))   { printf("FAIL: fix did not hold\n");               rc = 1; }
    printf(rc == 0 ? "PASS\n" : "FAILED\n");
    return rc;
}
