/* audio.c -- synchronous audio bridge backing SDLAudioManager's JNI contract.
 *
 * libSDL2.so's Android audio backend (SDL_androidaudio.c) drives audio through
 * plain named JNI calls on SDLAudioManager -- audioOpen(rate, format, channels,
 * frames) -> int[4] {rate, format, channels, frames}, then repeated blocking
 * audioWriteShortBuffer(short[]) calls from SDL's own audio thread, then
 * audioClose(). It also links libOpenSLES.so directly for an alternate native
 * backend, but that one only engages if slCreateEngine succeeds; imports.c
 * makes it fail outright (SL_RESULT_FEATURE_UNSUPPORTED) so SDL2 always falls
 * back to this JNI path, which is simpler to host correctly: since the
 * contract is already synchronous (the caller blocks until the buffer is
 * consumed), audout's own AudioOutBuffer/WaitPlayFinish pairing provides that
 * blocking for free, with no feeder thread needed.
 *
 * jni_fake's audioOpen/audioWriteShortBuffer/audioClose dispatch cases call
 * straight into this module. MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>

#include "audio.h"
#include "util.h"

static int s_ready;      // audout stream open
static int s_src_channels;
static int s_src_rate;
static double s_rphase;  // phase-continuous linear resampler state
static int16_t s_prevL, s_prevR;

// Timing ground-truth: reported symptom is "audio plays back ~2x too fast"
// with otherwise-correct pitch/content and a stable, normal-speed game loop,
// which doesn't point at any single line in the resample math below (that
// math was re-derived and checks out algebraically) -- it could equally be
// s_src_rate not being what the content actually is, in_frames being
// miscounted upstream, or audoutWaitPlayFinish not blocking for the real
// buffer duration. Rather than guess further, measure directly: track how
// many seconds of audio *content* (at s_src_rate) have been submitted versus
// how much real wall-clock time (via the same armGetSystemTick used by
// clock_gettime_fake) has actually elapsed while doing so. A genuine 2x
// speed bug shows up here as content-seconds accumulating at ~2x real-
// seconds, regardless of which stage of the pipeline is actually at fault.
static uint64_t s_dbg_tick0;
static uint64_t s_dbg_in_frames_total;

static int16_t *s_out_mem;
static size_t s_out_cap; // bytes, 0x1000-aligned

static AudioOutBuffer s_ab;

static void audout_ensure(void) {
  if (s_ready) return;
  if (R_SUCCEEDED(audoutInitialize())) {
    audoutStartAudioOut();
    s_ready = 1;
    debugPrintf("[audio] audout started: %luHz %uch\n",
                (unsigned long)audoutGetSampleRate(), (unsigned)audoutGetChannelCount());
  } else {
    debugPrintf("[audio] audoutInitialize failed\n");
  }
}

void sorx_audio_open(int sample_rate, int channels, int desired_frames,
                      int *out_rate, int *out_channels, int *out_frames) {
  audout_ensure();
  s_src_rate = sample_rate > 0 ? sample_rate : 48000;
  s_src_channels = (channels == 1) ? 1 : 2;
  s_rphase = 0.0;
  s_prevL = s_prevR = 0;
  debugPrintf("[audio] open: %dHz %uch (audout is fixed 48000Hz stereo s16)\n", s_src_rate, s_src_channels);
  s_dbg_tick0 = armGetSystemTick();
  s_dbg_in_frames_total = 0;
  // Always report the request as fully satisfied: SDL2 then hands us s16
  // stereo PCM at exactly this rate/channel count via audioWriteShortBuffer,
  // and we resample/upmix to audout's native 48 kHz stereo internally.
  if (out_rate) *out_rate = s_src_rate;
  if (out_channels) *out_channels = s_src_channels;
  // On-device measurement (both the coarse content-seconds-vs-real-seconds
  // ratio and a direct per-call audout append+wait timing) confirmed the
  // "audio plays back sped up" symptom is a real-time deficit, not a pitch/
  // resample bug: this game requests tiny ~512-frame (~11.6ms at 44100 Hz)
  // buffers, and audout's own append+WaitPlayFinish round trip carries
  // meaningful fixed per-call IPC latency -- large enough relative to an
  // 11.6ms buffer to cost it 10-30% extra real time on top of its own
  // nominal duration, run after run. With OpenBOR's own mixer presumably
  // syncing to a real wall-clock reference, falling that far behind reads on
  // its end as "audio underrunning" and it visibly catches up by skipping
  // ahead -- perceived as everything playing sped up. The fix is fewer,
  // bigger round trips: the frame count returned here becomes the actual
  // per-callback buffer size SDL2's Android audio thread allocates and
  // fills before every audioWriteShortBuffer call (mirroring how a real
  // AudioTrack negotiates its buffer from this same return value), so
  // requesting more than what was asked for still works and costs nothing
  // structural -- the resampler above already sizes itself from the actual
  // in_frames of whatever buffer arrives, not from this value. ~2048 frames
  // (~46ms at 44100 Hz) keeps latency low enough to be imperceptible for
  // game audio while cutting the fixed per-round-trip cost's share from
  // ~11.6ms's worth down to a quarter or less.
  //
  // 2048 frames cut the coarse content/real deficit from ~30% to ~8%
  // (confirmed via the timing diagnostics below), but ~8% remained --
  // disabling VERBOSE_EGL (a separate, since-ruled-out theory: the render
  // thread's own logging stealing cycles from this one) made zero measured
  // difference, and the per-call breakdown still shows a resample loop that
  // should cost microseconds instead intermittently costing several
  // milliseconds (once even 61ms), with the same real vs. nominal bias on
  // the audout round trip itself. That pattern -- fixed-ish latency
  // independent of buffer content, showing up as wall-clock time on trivial
  // CPU work -- is OS-level thread scheduling jitter: this audio work and
  // the render thread share the same pinned cores (0..2), so it
  // occasionally gets preempted mid-buffer. Same fix as before, one more
  // step: doubling the buffer again halves how often that same roughly
  // fixed jitter has to happen per second of audio, and halves its
  // proportional cost when it does. ~93ms latency is still fine for game
  // audio.
#define AUDIO_MIN_FRAMES 4096
  int frames = desired_frames > 0 ? desired_frames : 4096;
  if (frames < AUDIO_MIN_FRAMES) frames = AUDIO_MIN_FRAMES;
  if (out_frames) *out_frames = frames;
}

static int ensure_cap(size_t bytes) {
  size_t need = (bytes + 0xFFF) & ~(size_t)0xFFF;
  if (need <= s_out_cap) return 1;
  free(s_out_mem);
  s_out_mem = memalign(0x1000, need);
  s_out_cap = s_out_mem ? need : 0;
  return s_out_mem != NULL;
}

static uint64_t s_dbg_last_end_tick;
static uint8_t s_dbg_have_last_end;

void sorx_audio_write_s16(const int16_t *data, int n_samples) {
  if (!s_ready || !data || n_samples <= 0) return;

  uint64_t t_entry = armGetSystemTick();

  const unsigned ch = s_src_channels ? (unsigned)s_src_channels : 2;
  const size_t in_frames = (size_t)n_samples / ch;
  if (in_frames == 0) return;

  s_dbg_in_frames_total += in_frames;
  static uint64_t dbg_calls = 0;
  dbg_calls++;
  int dbg_verbose = (dbg_calls <= 20) || (dbg_calls % 200) == 0;
  if (dbg_verbose) {
    double content_s = (double)s_dbg_in_frames_total / (s_src_rate ? s_src_rate : 1);
    double real_s = (double)armTicksToNs(armGetSystemTick() - s_dbg_tick0) / 1e9;
    // The ~2048-frame buffering fix cut the coarse deficit from ~30% to
    // ~8%, but per-call audout round-trip timing (added below) now shows
    // that specific cost has itself dropped to near zero (roughly
    // symmetric +/-10% noise, no systematic bias) -- meaning the remaining
    // ~8% gap lives OUTSIDE the audout call entirely. "gap" here is the
    // time between the PREVIOUS call's audout wait finishing and THIS
    // call starting: our own resample loop's cost is measured separately,
    // right below, so whatever's left over in "gap" is time spent upstream
    // of us -- the game's own next-chunk mixing, JNI dispatch overhead, or
    // thread scheduling -- not anything this file's code is doing.
    double gap_s = s_dbg_have_last_end ?
        (double)armTicksToNs(t_entry - s_dbg_last_end_tick) / 1e9 : -1.0;
    debugPrintf("[audio] write#%llu n_samples=%d ch=%u in_frames=%zu total_in_frames=%llu "
                "content=%.3fs real=%.3fs ratio(content/real)=%.3f gap_since_prev=%.4fs\n",
                (unsigned long long)dbg_calls, n_samples, ch, in_frames,
                (unsigned long long)s_dbg_in_frames_total, content_s, real_s,
                real_s > 0.0 ? content_s / real_s : 0.0, gap_s);
  }

  size_t out_frames;
  if (s_src_rate == 48000) {
    if (!ensure_cap(in_frames * 2 * sizeof(int16_t))) return;
    int16_t *o = s_out_mem;
    for (size_t i = 0; i < in_frames; i++) {
      int16_t l = data[i * ch];
      *o++ = l;
      *o++ = (ch >= 2) ? data[i * ch + 1] : l;
    }
    out_frames = in_frames;
  } else {
    const double r = (double)s_src_rate / 48000.0; // input frames advanced per output frame
    size_t maxout = (size_t)((double)in_frames / r) + 4;
    if (!ensure_cap(maxout * 2 * sizeof(int16_t))) return;
    int16_t *o = s_out_mem;
    size_t oc = 0;
    for (size_t i = 0; i < in_frames; i++) {
      int16_t curL = data[i * ch];
      int16_t curR = (ch >= 2) ? data[i * ch + 1] : curL;
      while (s_rphase < 1.0 && oc < maxout) {
        double t = s_rphase;
        o[oc * 2]     = (int16_t)(s_prevL + (curL - s_prevL) * t);
        o[oc * 2 + 1] = (int16_t)(s_prevR + (curR - s_prevR) * t);
        oc++;
        s_rphase += r;
      }
      s_rphase -= 1.0;
      s_prevL = curL; s_prevR = curR;
    }
    out_frames = oc;
  }
  if (out_frames == 0) return;

  s_ab.next = NULL;
  s_ab.buffer = s_out_mem;
  s_ab.buffer_size = s_out_cap;
  s_ab.data_size = out_frames * 2 * sizeof(int16_t);
  s_ab.data_offset = 0;

  // Isolate WHERE the coarse real-time-vs-content-time gap is actually
  // going, split three ways: our own resample loop (t_entry..t_pre, fixable
  // here if it's ever significant), audout's append+wait round trip
  // (t_pre..now, platform/IPC latency -- already confirmed near-zero net
  // bias after the 2048-frame buffering fix), and the gap_since_prev logged
  // above (time spent upstream of this function entirely, between one call
  // returning and the next one starting -- the game's own next-chunk mixing
  // or JNI dispatch, nothing in this file touches that window at all).
  uint64_t t_pre = dbg_verbose ? armGetSystemTick() : 0;
  double resample_s = dbg_verbose ? (double)armTicksToNs(t_pre - t_entry) / 1e9 : 0.0;

  audoutAppendAudioOutBuffer(&s_ab);

  // Block like AudioTrack.write() in blocking mode until this buffer drains.
  AudioOutBuffer *rel = NULL;
  u32 cnt = 0;
  audoutWaitPlayFinish(&rel, &cnt, UINT64_MAX);

  uint64_t t_end = armGetSystemTick();
  if (dbg_verbose) {
    double actual_s = (double)armTicksToNs(t_end - t_pre) / 1e9;
    double nominal_s = (double)out_frames / 48000.0;
    debugPrintf("[audio] audout round-trip#%llu out_frames=%zu resample=%.4fs nominal=%.4fs actual=%.4fs "
                "overhead=%.4fs (%.1f%%)\n",
                (unsigned long long)dbg_calls, out_frames, resample_s, nominal_s, actual_s,
                actual_s - nominal_s, nominal_s > 0.0 ? 100.0 * (actual_s - nominal_s) / nominal_s : 0.0);
  }
  s_dbg_last_end_tick = t_end;
  s_dbg_have_last_end = 1;
}

void sorx_audio_close(void) {
  if (!s_ready) return;
  audoutStopAudioOut();
  audoutExit();
  s_ready = 0;
  free(s_out_mem);
  s_out_mem = NULL;
  s_out_cap = 0;
  debugPrintf("[audio] closed\n");
}
