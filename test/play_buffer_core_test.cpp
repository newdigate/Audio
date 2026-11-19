// Host tests for play_buffer_core.h (AudioPlayerCore): the in-memory
// variable-rate player behind AudioPlayer. No Arduino, no audio hardware.
//
//   c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I. \
//       -o /tmp/pbc test/play_buffer_core_test.cpp && /tmp/pbc
//
// Expected: every check prints nothing, the last line is "PASS (<n> checks)",
// exit 0, and no sanitizer report.
#include "play_buffer_core.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(name, cond) do { g_checks++; if (!(cond)) { g_fails++; \
    std::printf("FAIL %s  (%s:%d: %s)\n", name, __FILE__, __LINE__, #cond); } } while (0)

static const uint32_t BLK = 128;
struct Out { std::vector<int16_t> l, r; };
static Out render(AudioPlayerCore &p, uint32_t n) {
    Out o; o.l.resize(n); o.r.resize(n);
    for (uint32_t i = 0; i < n; i += BLK) {
        uint32_t k = (n - i < BLK) ? n - i : BLK;
        p.render(o.l.data() + i, o.r.data() + i, k);
    }
    return o;
}
static bool all_zero(const std::vector<int16_t> &v, size_t from = 0) {
    for (size_t i = from; i < v.size(); i++) if (v[i] != 0) return false;
    return true;
}

static void test_interp() {
    CHECK("interp_endpoints t=0", audioplayer_interp4(100, 200, 300, 400, 0.0f) == 200);
    CHECK("interp_endpoints t~1", std::abs(audioplayer_interp4(100, 200, 300, 400, 0.999f) - 300) <= 2);
    auto poly = [](double x) { return x * x * x - 20 * x * x + 5 * x; };
    int16_t y = audioplayer_interp4((int16_t)poly(10), (int16_t)poly(11), (int16_t)poly(12), (int16_t)poly(13), 0.5f);
    CHECK("interp_cubic_exact", std::abs(y - (int)std::lround(poly(11.5))) <= 1);
    int16_t hi = audioplayer_interp4(-32768, 32767, 32767, -32768, 0.5f);   // overshoots above full scale
    int16_t lo = audioplayer_interp4(32767, -32768, -32768, 32767, 0.5f);
    CHECK("interp_saturates high", hi == 32767);
    CHECK("interp_saturates low", lo == -32768);
}

// Stereo source: L[i] = i (wraps as int16 past 32767), R[i] = -i.
static std::vector<int16_t> stereo_ramp(uint32_t frames) {
    std::vector<int16_t> s(frames * 2);
    for (uint32_t i = 0; i < frames; i++) { s[2 * i] = (int16_t)i; s[2 * i + 1] = (int16_t)(-(int32_t)i); }
    return s;
}

static void test_unity() {
    auto s = stereo_ramp(4096);
    AudioPlayerCore p; p.setSource(s.data(), 4096, 2, 44100); p.setRate(1); p.play();
    render(p, BLK);                                   // settling block
    double p0 = p.position();
    CHECK("unity settled on a whole frame", p0 == std::floor(p0) && p0 > 0);
    Out o = render(p, 1024);
    bool ok = true;
    for (int i = 0; i < 1024; i++) ok = ok && o.l[i] == (int16_t)(p0 + i) && o.r[i] == (int16_t)(-(p0 + i));
    CHECK("unity_rate_is_identity", ok);
    CHECK("unity position", p.position() == p0 + 1024);
}

static void test_half_rate() {
    std::vector<int16_t> s(3000);
    for (int i = 0; i < 3000; i++) s[i] = (int16_t)(10 * i);
    AudioPlayerCore p; p.setSource(s.data(), 3000, 1, 44100); p.setRate(0.5f); p.play();
    render(p, BLK);
    double p0 = p.position();
    Out o = render(p, 512);
    bool ok = true, same = true;
    for (int i = 0; i < 512; i++) {
        ok = ok && std::abs(o.l[i] - (10.0 * (p0 + 0.5 * i))) <= 1.0;
        same = same && o.l[i] == o.r[i];
    }
    CHECK("half_rate slope", ok);
    CHECK("half_rate mono on both outputs", same);
}

static void test_reverse() {
    auto s = stereo_ramp(4096);
    AudioPlayerCore p; p.setSource(s.data(), 4096, 2, 44100); p.seek(2000); p.setRate(-1); p.play();
    render(p, BLK);
    Out o = render(p, 512);
    bool ok = true;
    for (int i = 1; i < 512; i++) ok = ok && (o.l[i] - o.l[i - 1] == -1);
    CHECK("reverse descends by 1", ok);
    CHECK("reverse position below start point", p.position() < 2000 - 512);
}

static void test_48k() {
    std::vector<int16_t> s(60000, 0);
    AudioPlayerCore p; p.setSource(s.data(), 60000, 1, 48000); p.setRate(1); p.play();
    render(p, BLK);
    double p0 = p.position();
    render(p, 44100);
    CHECK("source_rate_48k", std::fabs((p.position() - p0) - 48000.0) <= 1.0);
}

static void test_ends() {
    std::vector<int16_t> s(1000, 1000);
    AudioPlayerCore p; p.setSource(s.data(), 1000, 1, 44100); p.setRate(1); p.play();
    Out o = render(p, 2048);
    CHECK("stops_at_end not playing", !p.isPlaying());
    CHECK("stops_at_end position", p.position() == 999);
    CHECK("stops_at_end silence after", all_zero(o.l, 1200) && all_zero(o.r, 1200));
    CHECK("stops_at_end played something", o.l[300] == 1000);

    AudioPlayerCore q; q.setSource(s.data(), 1000, 1, 44100); q.seek(500); q.setRate(-1); q.play();
    Out r = render(q, 2048);
    CHECK("stops_at_start_in_reverse not playing", !q.isPlaying());
    CHECK("stops_at_start_in_reverse position", q.position() == 0);
    CHECK("stops_at_start_in_reverse silence after", all_zero(r.l, 800));
}

static void test_starved() {
    std::vector<int16_t> s(4000, 500);
    AudioPlayerCore p; p.setSource(s.data(), 4000, 1, 44100); p.setAvailableFrames(1000); p.setRate(1); p.play();
    Out o = render(p, 2048);
    CHECK("starved_holds still playing", p.isPlaying());
    CHECK("starved_holds position", p.position() <= 999 && p.position() >= 990);
    CHECK("starved_holds silent tail", all_zero(o.l, 1200));
    p.setAvailableFrames(4000);
    double before = p.position();
    render(p, BLK);
    CHECK("starved_holds resumes", p.position() > before + 100);
}

static void test_paused() {
    std::vector<int16_t> s(4000, 500);
    AudioPlayerCore p; p.setSource(s.data(), 4000, 1, 44100); p.setRate(1); p.play();
    render(p, 256); p.pause(); render(p, 256);       // let the pause take effect
    double pos = p.position();
    Out o = render(p, 512);
    CHECK("paused_is_silent zeros", all_zero(o.l) && all_zero(o.r));
    CHECK("paused_is_silent position", p.position() == pos);
    CHECK("paused_is_silent flag", !p.isPlaying());
}

static void test_resource() {
    std::vector<int16_t> a(4000, 500), b(4000, 700);
    AudioPlayerCore p; p.setSource(a.data(), 4000, 1, 44100); p.setRate(1); p.play();
    render(p, 256);
    p.setSource(b.data(), 4000, 1, 44100);
    Out o = render(p, 256);
    CHECK("resource_while_playing silent", all_zero(o.l));
    CHECK("resource_while_playing position 0", p.position() == 0 && !p.isPlaying());
    p.play(); render(p, 256);
    p.setAvailableFrames(0);
    double pos = p.position();
    Out z = render(p, 256);
    CHECK("available 0 while playing: silent", all_zero(z.l));
    CHECK("available 0 while playing: holds", p.position() <= pos);
}

static void test_tiny() {
    int16_t z[8] = {0};
    for (uint32_t frames : {0u, 1u, 3u}) {
        AudioPlayerCore p; p.setSource(frames ? z : nullptr, frames, 2, 44100); p.setRate(1); p.play();
        Out o = render(p, 256);
        CHECK("tiny_buffers zeros", all_zero(o.l) && all_zero(o.r));
        CHECK("tiny_buffers stopped", !p.isPlaying());
        AudioPlayerCore q; q.setSource(frames ? z : nullptr, frames, 2, 44100); q.setRate(-1); q.play();
        render(q, 256);
        CHECK("tiny_buffers reverse stopped", !q.isPlaying());
    }
}

static void test_long() {
    const uint32_t frames = 24000000;
    int16_t *s = (int16_t *)std::calloc(frames, sizeof(int16_t));
    AudioPlayerCore p; p.setSource(s, frames, 1, 44100); p.seek(23000000); p.setRate(0.01f); p.play();
    render(p, BLK);
    double p0 = p.position();
    render(p, 44100);
    CHECK("long_buffer_precision", std::fabs((p.position() - p0) - 441.0) <= 1.0);
    std::free(s);
}


// ---- Task 2: rate changes ramp across the block ----
static std::vector<int16_t> sine(uint32_t frames, double hz, double amp) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; i++) s[i] = (int16_t)std::lround(amp * std::sin(2.0 * M_PI * hz * i / 44100.0));
    return s;
}
static int max_delta(const std::vector<int16_t> &v) {
    int m = 0;
    for (size_t i = 1; i < v.size(); i++) { int d = std::abs(v[i] - v[i - 1]); if (d > m) m = d; }
    return m;
}
static const double SINE_MAX_DELTA = 20000.0 * 2.0 * M_PI * 441.0 / 44100.0;   // per sample at rate 1

static void test_ramp() {
    std::vector<int16_t> s(20000, 0);
    AudioPlayerCore p; p.setSource(s.data(), 20000, 1, 44100); p.setRate(1); p.play();
    render(p, 2 * BLK);
    double p0 = p.position();
    p.setRate(2); render(p, BLK);
    CHECK("ramp_position ramp block", std::fabs((p.position() - p0) - 128 * 1.5) <= 1.0);
    double p1 = p.position();
    render(p, BLK);
    CHECK("ramp_position next block", std::fabs((p.position() - p1) - 256.0) <= 1.0);
}

static void test_ramp_no_step() {
    auto s = sine(16000, 441.0, 20000.0);
    AudioPlayerCore p; p.setSource(s.data(), 16000, 1, 44100); p.seek(8000); p.setRate(1); p.play();
    std::vector<int16_t> all;
    for (float r : {1.0f, 0.5f, -1.0f, 1.0f, 0.25f, -0.5f, 1.0f}) {
        p.setRate(r);
        Out o = render(p, BLK);
        all.insert(all.end(), o.l.begin(), o.l.end());
    }
    CHECK("ramp_no_step", max_delta(all) < 1.2 * SINE_MAX_DELTA);
    CHECK("ramp_no_step made sound", max_delta(all) > 100);
}

static void test_rate_zero() {
    auto s = sine(16000, 441.0, 20000.0);
    AudioPlayerCore p; p.setSource(s.data(), 16000, 1, 44100); p.seek(8000); p.setRate(1); p.play();
    render(p, 2 * BLK);
    p.setRate(0); render(p, BLK);
    double pos = p.position();
    Out o = render(p, 256);
    bool constant = true;
    for (int i = 1; i < 256; i++) constant = constant && o.l[i] == o.l[0];
    CHECK("rate_zero_holds constant", constant);
    CHECK("rate_zero_holds value", std::abs(o.l[0] - (int)std::lround(20000.0 * std::sin(2.0 * M_PI * 441.0 * pos / 44100.0))) <= 2);
    CHECK("rate_zero_holds position", p.position() == pos);
    CHECK("rate_zero_holds still playing", p.isPlaying());
}

static void test_play_pause_ramps() {
    auto s = sine(16000, 441.0, 20000.0);
    AudioPlayerCore p; p.setSource(s.data(), 16000, 1, 44100); p.seek(8000); p.setRate(1);
    p.play();
    Out a = render(p, BLK);
    CHECK("play_from_pause_ramps first block", std::fabs((p.position() - 8000.0) - 64.0) <= 1.0);
    render(p, BLK);
    double p1 = p.position();
    p.pause();
    CHECK("pause: isPlaying false at once", !p.isPlaying());
    Out b = render(p, BLK);
    CHECK("pause ramps down over one block", std::fabs((p.position() - p1) - 64.0) <= 1.0);
    CHECK("pause block is not cut to silence", max_delta(b.l) > 0 && max_delta(b.l) < 1.2 * SINE_MAX_DELTA);
    double p2 = p.position();
    Out c = render(p, BLK);
    CHECK("after the pause block: silent and still", all_zero(c.l) && p.position() == p2);
    (void)a;
}

// ---- Task 3: scratch mode ----
// Renders `ms` of output in 128-sample blocks, calling scratchTo(target(t))
// every 441 samples (a 100 Hz touch panel).  Records per-block positions.
template <typename F>
static std::vector<double> scratch_run(AudioPlayerCore &p, F target, int ms, std::vector<int16_t> *audio = nullptr,
                                       double *t0 = nullptr) {
    static double clock = 0;                        // output samples since the run began
    if (t0) clock = *t0;
    std::vector<double> pos;
    const uint32_t total = (uint32_t)(ms * 44.1);
    uint32_t since = 441;
    for (uint32_t done = 0; done < total; done += BLK) {
        if (since >= 441) { p.scratchTo(target(clock), (float)since); since = 0; }
        int16_t l[BLK], r[BLK];
        p.render(l, r, BLK);
        if (audio) audio->insert(audio->end(), l, l + BLK);
        since += BLK; clock += BLK;
        pos.push_back(p.position());
    }
    if (t0) *t0 = clock;
    return pos;
}
static AudioPlayerCore scratch_setup(const std::vector<int16_t> &s, double at) {
    AudioPlayerCore p; p.setSource(s.data(), (uint32_t)s.size(), 1, 44100); p.seek(at); p.setRate(1); p.play();
    int16_t l[BLK], r[BLK];
    p.render(l, r, BLK); p.render(l, r, BLK);
    return p;
}

static void test_scratch_hold() {
    auto s = sine(200000, 441.0, 20000.0);
    AudioPlayerCore p = scratch_setup(s, 100000);
    const double P = p.position() - 2000;
    p.scratchBegin();
    CHECK("isScratching", p.isScratching());
    double t = 0;
    scratch_run(p, [&](double) { return P; }, 150, nullptr, &t);
    CHECK("scratch_hold_converges", std::fabs(p.position() - P) < 1.0);
    std::vector<int16_t> a;
    scratch_run(p, [&](double) { return P; }, 100, &a, &t);
    CHECK("scratch_hold_converges then constant", max_delta(a) <= 1);
    CHECK("scratch_hold stays put", std::fabs(p.position() - P) < 1.0);
}

static void test_scratch_velocity() {
    auto s = sine(400000, 441.0, 20000.0);
    for (double v : {1.0, -2.0, 0.25}) {
        AudioPlayerCore p = scratch_setup(s, 200000);
        const double start = p.position();
        p.scratchBegin();
        double t = 0;
        auto f = [&](double tt) { return start + v * tt; };
        scratch_run(p, f, 150, nullptr, &t);
        double a = p.position();
        auto pos = scratch_run(p, f, 100, nullptr, &t);
        double mean = (pos.back() - a) / (pos.size() * (double)BLK);
        CHECK("scratch_follows_velocity mean", std::fabs(mean - v) <= 0.03 * std::fabs(v));
        bool ripple_ok = true; double prev = a;
        for (double q : pos) { double r = (q - prev) / BLK; prev = q; ripple_ok = ripple_ok && std::fabs(r - v) <= 0.10 * std::fabs(v); }
        CHECK("scratch_follows_velocity no stepping at the touch rate", ripple_ok);
    }
}

static void test_scratch_limits() {
    auto s = sine(400000, 441.0, 20000.0);
    {   // step and hold: no overshoot
        AudioPlayerCore p = scratch_setup(s, 100000);
        const double T = p.position() + 5000;
        p.scratchBegin();
        double t = 0; bool ok = true;
        for (double q : scratch_run(p, [&](double) { return T; }, 400, nullptr, &t)) ok = ok && q <= T + 5.0;   /* 0.1 % of the 5000-frame jump */
        CHECK("scratch_no_overshoot", ok);
        CHECK("scratch_no_overshoot arrives", std::fabs(p.position() - T) < 1.0);
    }
    {   // far target: rate limited to 8 frames per sample
        std::vector<int16_t> big(3000000, 0);
        AudioPlayerCore p = scratch_setup(big, 1000);
        p.scratchBegin();
        double t = 0, prev = p.position(); bool ok = true, fast = false;
        for (double q : scratch_run(p, [&](double) { return 2001000.0; }, 300, nullptr, &t)) {
            ok = ok && (q - prev) <= 8.0 * BLK + 1.0; fast = fast || (q - prev) > 7.5 * BLK; prev = q;
        }
        CHECK("scratch_rate_limit", ok);
        CHECK("scratch_rate_limit reaches the limit", fast);
    }
    {   // clamped to what is loaded
        AudioPlayerCore p = scratch_setup(s, 500);
        p.setAvailableFrames(1000);
        p.scratchBegin();
        double t = 0; bool ok = true;
        for (double q : scratch_run(p, [&](double) { return 50000.0; }, 300, nullptr, &t)) ok = ok && q <= 999.0;
        CHECK("scratch_clamped high", ok && p.position() > 990.0);
        for (double q : scratch_run(p, [&](double) { return -10.0; }, 300, nullptr, &t)) ok = ok && q >= 0.0;
        CHECK("scratch_clamped low", ok && p.position() < 1.0);
        CHECK("scratch at the ends does not stop playback intent", p.isPlaying());
    }
}

static void test_scratch_smooth_and_end() {
    auto s = sine(400000, 441.0, 20000.0);
    AudioPlayerCore p = scratch_setup(s, 200000);
    p.setRate(0.92f);
    int16_t l[BLK], r[BLK]; p.render(l, r, BLK);
    const double start = p.position();
    std::vector<int16_t> a;
    p.scratchBegin();
    double t = 0, peak = 1.0, prev = start;
    // back at 3x, forward at 2x, hold: stair-stepped at 100 Hz by scratch_run
    auto f = [&](double tt) { double ms = tt / 44.1; return ms < 120 ? start - 3.0 * tt : ms < 300 ? start - 3.0 * 5292 + 2.0 * (tt - 5292) : start - 3.0 * 5292 + 2.0 * (13230 - 5292); };
    for (double q : scratch_run(p, f, 400, &a, &t)) { peak = std::fmax(peak, std::fabs(q - prev) / BLK); prev = q; }
    p.scratchEnd();
    CHECK("scratchEnd clears isScratching", !p.isScratching());
    for (int i = 0; i < 2; i++) { p.render(l, r, BLK); a.insert(a.end(), l, l + BLK); }
    CHECK("scratch_smooth", max_delta(a) < 1.5 * peak * SINE_MAX_DELTA);
    CHECK("scratch_smooth moved fast", peak > 2.5);
    double b = p.position(); p.render(l, r, BLK);
    CHECK("scratch_end_resumes at the set rate", std::fabs((p.position() - b) / BLK - 0.92) <= 0.001);

    AudioPlayerCore q; q.setSource(s.data(), (uint32_t)s.size(), 1, 44100); q.seek(100000);   // paused
    q.scratchBegin(); double t2 = 0;
    scratch_run(q, [&](double tt) { return 100000.0 + tt; }, 200, nullptr, &t2);
    CHECK("scratch from paused moves", q.position() > 100000.0 + 5000);
    q.scratchEnd();
    q.render(l, r, BLK); double c = q.position(); q.render(l, r, BLK);
    CHECK("scratch_end_resumes paused: stops", q.position() == c && !q.isPlaying());
}

// ---- Task 4: what the AudioStream wrapper needs ----
static void test_idle() {
    std::vector<int16_t> s(8000, 100);
    AudioPlayerCore p;
    CHECK("isIdle with no source", p.isIdle());
    p.setSource(s.data(), 8000, 1, 44100);
    CHECK("isIdle when paused", p.isIdle());
    p.play(); CHECK("not idle when playing", !p.isIdle());
    render(p, BLK); p.pause();
    CHECK("not idle until the ramp-down block has been rendered", !p.isIdle());
    render(p, BLK); CHECK("idle after the ramp-down block", p.isIdle());
    p.scratchBegin(); CHECK("not idle when scratching", !p.isIdle());
    p.scratchEnd(); render(p, BLK); render(p, BLK); CHECK("idle again after scratchEnd while paused", p.isIdle());
}

// ---- Final review: play() before any frame has loaded ----
static void test_play_before_frames() {
    std::vector<int16_t> s(8000, 700);
    AudioPlayerCore p; p.setSource(s.data(), 8000, 1, 44100); p.setAvailableFrames(0); p.setRate(1);
    p.play();
    CHECK("play() with a source but no frames yet is remembered", p.isPlaying());
    Out o = render(p, 256);
    CHECK("silent and still while nothing is loaded", all_zero(o.l) && p.position() == 0);
    p.setAvailableFrames(4000);
    Out q = render(p, 512);
    CHECK("starts once frames arrive", p.position() > 200 && q.l[400] == 700);
    AudioPlayerCore n; n.play();
    CHECK("play() with no source is refused", !n.isPlaying());
}

int main() {
    test_interp(); test_unity(); test_half_rate(); test_reverse(); test_48k();
    test_ends(); test_starved(); test_paused(); test_resource(); test_tiny(); test_long();
    test_ramp(); test_ramp_no_step(); test_rate_zero(); test_play_pause_ramps();
    test_scratch_hold(); test_scratch_velocity(); test_scratch_limits(); test_scratch_smooth_and_end();
    test_idle(); test_play_before_frames();
    if (g_fails) { std::printf("FAILED %d of %d checks\n", g_fails, g_checks); return 1; }
    std::printf("PASS (%d checks)\n", g_checks);
    return 0;
}
