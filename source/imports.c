/* imports.c -- resolves libhidapi.so's, libSDL2.so's and libopenbor.so's
 * dynamic imports against newlib, host zlib/mesa, and our shims. so_util
 * cross-resolves module exports automatically (SDL_* against the loaded
 * libSDL2.so, hid_* against the loaded libhidapi.so) as long as all three are
 * so_load()'d before any of them is so_resolve()'d, so this table only needs
 * to cover the libc/NDK/GL/zlib surface none of the three provide themselves.
 * The list below is the union of each .so's actual UND dynsym entries.
 * MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <setjmp.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include <switch.h>

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "egl_shim.h"
#include "jni_fake.h"

// real libc/gcc symbols whose addresses we forward verbatim
extern int   __cxa_atexit(void (*)(void *), void *, void *);
extern void  __stack_chk_fail(void);

// ---------------------------------------------------------------------------
// C++ runtime surface libhidapi.so needs (its CHIDDevice class uses new/
// delete): implemented directly so we don't have to pull in libstdc++.
// ---------------------------------------------------------------------------

static void *cxx_new(size_t sz) __asm__("_Znwm");
static void *cxx_new(size_t sz) { return malloc(sz ? sz : 1); }
static void *cxx_new_arr(size_t sz) __asm__("_Znam");
static void *cxx_new_arr(size_t sz) { return malloc(sz ? sz : 1); }
static void cxx_delete(void *p) __asm__("_ZdlPv");
static void cxx_delete(void *p) { free(p); }
static void cxx_delete_arr(void *p) __asm__("_ZdaPv");
static void cxx_delete_arr(void *p) { free(p); }

// ---------------------------------------------------------------------------
// process termination: OpenBOR's own borShutdown() (reached from a script
// exception, a fatal asset error, or normal quit) always ends by calling
// exit()/abort(). Newlib's real implementations on devkitA64/libnx raise a
// fatal svcBreak, which Atmosphère reports as a "User Break" crash --
// confirmed on-device: a clean "Release ... Done! / **** Done ****"
// shutdown sequence completing in full, immediately followed by exactly
// this crash type. Route both through __libnx_exit() instead, the same
// clean-quit path main.c's own normal end-of-program already uses -- lets
// a script error or fatal asset error end the process without Atmosphère
// treating a deliberate, already-logged shutdown as an unhandled crash.
// ---------------------------------------------------------------------------

extern void NX_NORETURN __libnx_exit(int rc);
static void exit_fake(int code) { __libnx_exit(code); }
static void abort_fake(void) { __libnx_exit(1); }

// ---------------------------------------------------------------------------
// bionic logging / errno
// ---------------------------------------------------------------------------

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
#if DEBUG_LOG
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("[%s] %s\n", tag ? tag : "", string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}

static int *__errno_fake(void) { return &errno; }

// ---------------------------------------------------------------------------
// pthread: bionic's opaque types are zero-inited inline, so lazily back them
// with heap newlib objects stashed in the caller's first pointer slot.
// ---------------------------------------------------------------------------

#define BIONIC_RECURSIVE_MARK 0x8000
#define BIONIC_ERRCHECK_MARK  0x4000

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

static int ensure_mutex(pthread_mutex_t **uid) {
  if (!*uid) return pthread_mutex_init_fake(uid, NULL);
  const uintptr_t v = (uintptr_t)*uid;
  if (v == BIONIC_RECURSIVE_MARK || v == BIONIC_ERRCHECK_MARK) {
    int attr = 1;
    return pthread_mutex_init_fake(uid, &attr);
  }
  return 0;
}

static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x10000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid); if (r) return r;
  return pthread_mutex_lock(*uid);
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid); if (r) return r;
  return pthread_mutex_trylock(*uid);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int r = ensure_mutex(uid); if (r) return r;
  return pthread_mutex_unlock(*uid);
}

static int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}
static int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
#if VERBOSE_IO
  debugPrintf("[cond] broadcast(%p)\n", (void *)cnd);
#endif
  return pthread_cond_broadcast(*cnd);
}
static int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
#if VERBOSE_IO
  debugPrintf("[cond] signal(%p)\n", (void *)cnd);
#endif
  return pthread_cond_signal(*cnd);
}
static int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd && (uintptr_t)*cnd > 0x10000) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (ensure_mutex(mtx)) return -1;
#if VERBOSE_IO
  debugPrintf("[cond] wait(%p) -- blocking...\n", (void *)cnd);
#endif
  int r = pthread_cond_wait(*cnd, *mtx);
#if VERBOSE_IO
  debugPrintf("[cond] wait(%p) done\n", (void *)cnd);
#endif
  return r;
}
static int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (ensure_mutex(mtx)) return -1;
#if VERBOSE_IO
  debugPrintf("[cond] timedwait(%p) -- blocking...\n", (void *)cnd);
#endif
  int r = pthread_cond_timedwait(*cnd, *mtx, t);
#if VERBOSE_IO
  debugPrintf("[cond] timedwait(%p) done rc=%d\n", (void *)cnd, r);
#endif
  return r;
}

static int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

static int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
static int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }
static int pthread_getschedparam_fake(pthread_t t, int *policy, void *param) {
  (void)t; if (policy) *policy = 0; (void)param; return 0;
}

// libnx docs: priority 0x2C is the usual main-thread priority, but 0x3B
// (0x3F on core 3) is the specific band that enables *preemptive* round-robin
// scheduling on the Switch's kernel -- threads at other priorities only give
// up the CPU voluntarily (blocking syscall/sleep), so two same-priority
// threads where one never blocks can starve the other forever with no crash
// to show for it. devkitA64's pthread_setschedprio isn't wired up (hidden
// behind an unset _POSIX_THREAD_PRIORITY_SCHEDULING guard), so set the raw
// kernel priority via svcSetThreadPriority from inside the new thread itself
// (threadGetCurHandle() works regardless of how the thread was actually
// created, since devkitA64's pthread_create is itself built on libnx Thread).
#define SORX_THREAD_PRIORITY 0x3B

// new engine threads need tpidr_el0 pointing at a stack-guard block first
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  Result pr = svcSetThreadPriority(threadGetCurHandle(), SORX_THREAD_PRIORITY);
#if VERBOSE_IO
  debugPrintf("[thread] entry=%p starting (setThreadPriority -> 0x%x)\n", (void *)ts.entry, pr);
#else
  (void)pr;
#endif
  void *ret = ts.entry(ts.arg);
#if VERBOSE_IO
  debugPrintf("[thread] entry=%p returned\n", (void *)ts.entry);
#endif
  return ret;
}

static int pthread_create_fake(pthread_t *thread, const void *attr, void *entry, void *arg) {
  (void)attr;
#if VERBOSE_IO
  debugPrintf("[thread] pthread_create entry=%p\n", entry);
#endif
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

static int sched_yield_fake(void) { svcSleepThread(0); return 0; }
static int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
static int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
static int pthread_setschedparam_fake(pthread_t t, int policy, const void *param) {
  (void)t; (void)policy; (void)param; return 0;
}

static int fputs_fake(const char *s, FILE *f) {
  if (!f) return -1;
  return fputs(s ? s : "", f);
}

// ---------------------------------------------------------------------------
// draw-call counters: eglSwapBuffers succeeding every frame only proves the
// swap chain is alive, not that anything is actually being drawn -- a
// legitimately black screen (nothing bound/drawn, or every glClear painting
// black) looks identical to a broken one from the swap side alone. Logged in
// lockstep with egl_shim's periodic eglSwapBuffers trace (first 10 frames,
// then ~1/sec) so each line answers "how much drawing happened this frame".
// ---------------------------------------------------------------------------

static uint64_t g_clear_calls, g_drawarrays_calls, g_drawarrays_verts, g_teximage_calls;
static float g_last_clear_color[4] = { -1, -1, -1, -1 };
static GLuint g_last_bound_tex = 0; // set by glBindTexture_wrap, read by glDrawArrays_wrap
static GLenum g_active_tex_unit = GL_TEXTURE0; // set by glActiveTexture_wrap
static GLuint g_unit_bound_tex[32]; // per-unit last-bound texture, indexed by unit - GL_TEXTURE0

// Per-texture-id last-known size + "has any nonzero byte ever" state, updated
// silently (plain array writes, no debugPrintf) on every texImage2D/
// texSubImage2D call so it costs nothing per-frame -- surfaced only
// piggybacked onto the already-bounded glDrawArrays log lines below,
// letting a draw's log line answer "is the currently-bound texture's
// content ever actually real" without needing its own per-call logging
// (which for texSubImage2D fires every frame and would reintroduce the
// exact per-line fflush cost problem already fixed once for VERBOSE_IO).
#define TEX_TRACK_MAX 8192
static uint16_t g_tex_w[TEX_TRACK_MAX], g_tex_h[TEX_TRACK_MAX];
static uint8_t g_tex_had_data[TEX_TRACK_MAX];
static uint8_t g_tex_max_alpha[TEX_TRACK_MAX]; // highest alpha byte ever seen for this tex id
static void tex_track_size(GLuint tex, GLsizei w, GLsizei h) {
  if (tex < TEX_TRACK_MAX) { g_tex_w[tex] = (uint16_t)w; g_tex_h[tex] = (uint16_t)h; }
}
// The one confirmed content-holding texture id (real, varied pixel data,
// uploaded every frame) -- 800x480 is the backdrop's own fixed size, so a
// smaller, real-data texture is unambiguously the actual game/menu content
// SDL2's own batched renderer keeps failing to ever bind for the matching
// draw (see the forced-rebind workaround at glDrawArrays_wrap below).
static GLuint g_content_tex_id = 0;

// The current video's own Y/U/V plane texture ids (populated in
// glBindTexture_wrap right after SDL_CreateTexture_wrap creates a YUV
// texture -- see is_planar_yuv_format() further down -- since a YUV
// SDL_Texture is backed by three separate real GL textures, one per plane,
// bound across texture units 0-2 in sequence every time it's touched).
// glDrawArrays_wrap's forced-rebind workaround below needs to recognize
// these specifically: an on-device vidgl trace confirmed a blanket
// "skip the rebind whenever a video is playing" was too broad -- an
// unrelated ordinary RGBA draw (its own separate glDrawArrays call) can
// legitimately happen while g_video_playing is still 1, and it still needs
// the SAME rebind workaround this build's SDL2 renderer bug always required.
// Excluding only draws actually bound to one of these ids leaves that other
// draw's own fix intact while still protecting the video's own draw (whose
// Y-plane, bound on unit 0, was otherwise getting silently replaced by a
// stale unrelated RGBA texture -- confirmed the cause of the reported
// washed-out/oversaturated video colors).
#define YUV_PLANE_IDS_MAX 3
static GLuint g_yuv_plane_ids[YUV_PLANE_IDS_MAX];
static int g_yuv_plane_count;
static int g_expect_yuv_planes; // set by SDL_CreateTexture_wrap right after creating a YUV texture

// Confirmed on-device as the root cause of a Data Abort inside this exact
// function (Atmosphere crash report: PC in tex_track_data, LR in
// glTexSubImage2D_wrap, running on the SDL video-decode thread): this always
// assumed 4 bytes/pixel (RGBA), true for every upload this was ever
// exercised against before real (non-stubbed) video existed. A YUV video
// texture's Y/U/V planes upload through this exact same glTexSubImage2D path
// but as single-channel (1 byte/pixel) luminance/chroma data with no alpha
// channel at all -- scanning w*h*4 bytes against a buffer that only has
// w*h*1 reads up to 4x past its real end. Silent luck (still-mapped adjacent
// heap memory) let the first video's own out-of-bounds read go unnoticed;
// the second video's differently-placed allocation is what finally faulted.
// GL_RGBA is the only format this alpha-tracking heuristic was ever designed
// for anyway (chroma/luminance planes have no alpha to track), so requiring
// it is a correctness fix, not just a crash workaround.
#define GL_RGBA_FMT 0x1908
static void tex_track_data(GLuint tex, const void *pixels, GLsizei w, GLsizei h, GLenum format) {
  if (tex >= TEX_TRACK_MAX || !pixels || w <= 0 || h <= 0 || format != GL_RGBA_FMT) return;
  const uint8_t *p = pixels;
  size_t n = (size_t)w * h * 4;
  // Alpha-channel check: content can pass "has any nonzero byte" purely on
  // R/G/B while its alpha is uniformly 0 -- with GL_BLEND on and the usual
  // (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) factors that draws with zero visible
  // effect (fully transparent), looking identical to "nothing on screen"
  // despite every earlier check (rect, bind, draw call, swap) reporting
  // success. Keep scanning max alpha even after g_tex_had_data latches, since
  // that latch used to skip re-scanning entirely and would have hidden this.
  for (size_t i = 3; i < n; i += 4) {
    if (p[i] > g_tex_max_alpha[tex]) g_tex_max_alpha[tex] = p[i];
  }
  if (g_tex_had_data[tex]) return;
  for (size_t i = 0; i < n; i++) {
    if (p[i]) {
      g_tex_had_data[tex] = 1;
      if (!(w == 800 && h == 480)) g_content_tex_id = tex;
      return;
    }
  }
}

#if VERBOSE_EGL
static int draw_log_verbose(void) {
  static uint64_t frame = 0;
  frame++;
  return (frame <= 10) || (frame % 60 == 0);
}
#endif

// Reported on-device: a real video's colors come out oversaturated/washed,
// and a totally unrelated static image drawn right after the video (no
// video texture involved at all) shows the same symptom -- confirmed NOT a
// bad source file (a stock ffmpeg/libvpx decode of the exact override file
// matches the original's colors). That combination points at GL state set
// up for the YUV draw (shader program, blend, texture units) not getting
// reset before the next ordinary RGBA draw, somewhere in this build's
// GLES2 pipeline. VERBOSE_EGL's per-call logging below is off (its static
// call-count gates are long past their caps by the time any video plays,
// tens of seconds into boot) and turning it on globally would flood the
// whole boot log for a handful of relevant frames -- trace unconditionally
// instead, but only while g_video_playing is set and for a window after it
// clears, to catch the handoff back to normal rendering.
//
// That trailing window was originally a 120-call countdown -- confirmed on
// on-device logs too short: a hang consistently observed 2-3 real seconds
// after the last video's own close() had already exhausted the 120-call
// window a second or more before the actual freeze, leaving zero trace
// coverage of whatever happens in between. Time-based instead (armTicksToNs
// against a real wall-clock budget), long enough to span that whole gap.
static int video_trace_active(void) {
  static uint64_t trailing_until_tick;
  uint64_t now = armGetSystemTick();
  if (g_video_playing) {
    trailing_until_tick = now + armNsToTicks(8ull * 1000 * 1000 * 1000); // 8s
    return 1;
  }
  return trailing_until_tick != 0 && now < trailing_until_tick;
}

static void glClearColor_wrap(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  g_last_clear_color[0] = r; g_last_clear_color[1] = g;
  g_last_clear_color[2] = b; g_last_clear_color[3] = a;
  glClearColor(r, g, b, a);
}
static uint64_t g_draws_since_clear;

static void glClear_wrap(GLbitfield mask) {
  g_clear_calls++;
  g_draws_since_clear = 0; // blit()'s glClear marks the start of a new frame's draws
  if (video_trace_active())
    debugPrintf("[vidgl] clear#%llu mask=0x%x color=(%.2f,%.2f,%.2f,%.2f)\n",
                (unsigned long long)g_clear_calls, mask,
                g_last_clear_color[0], g_last_clear_color[1], g_last_clear_color[2], g_last_clear_color[3]);
  glClear(mask);
#if VERBOSE_EGL
  if (draw_log_verbose())
    debugPrintf("[draw] clear#%llu mask=0x%x color=(%.2f,%.2f,%.2f,%.2f) draws=%llu(%lluverts) teximg_total=%llu\n",
                (unsigned long long)g_clear_calls, mask,
                g_last_clear_color[0], g_last_clear_color[1], g_last_clear_color[2], g_last_clear_color[3],
                (unsigned long long)g_drawarrays_calls, (unsigned long long)g_drawarrays_verts,
                (unsigned long long)g_teximage_calls);
#endif
}
static void glDrawArrays_wrap(GLenum mode, GLint first, GLsizei count) {
  g_drawarrays_calls++;
#if VERBOSE_EGL
  // glViewport is never called by this game at all -- query whatever mesa
  // actually defaulted it to (once) rather than assume it matches the
  // window, since a stale/zero viewport would explain real draws landing
  // nowhere visible.
  if (g_drawarrays_calls == 1) {
    GLint vp[4] = {0,0,0,0};
    glGetIntegerv(GL_VIEWPORT, vp);
    debugPrintf("[draw] current viewport (never explicitly set) = (%d,%d,%d,%d)\n", vp[0], vp[1], vp[2], vp[3]);
  }
#endif
  g_drawarrays_verts += (count > 0) ? (uint64_t)count : 0;
  // Workaround for a confirmed-on-device SDL2 renderer quirk in this build:
  // glBindTexture(content_id) genuinely happens (observed ~150 times per a
  // short run, matching every real content upload), but by the time the
  // corresponding glDrawArrays for it fires, the bound texture has reverted
  // to the backdrop's -- every single sampled draw across many multi-
  // thousand-draw runs shows only the backdrop, never content, despite
  // content demonstrably having real data and a correct destination rect
  // every frame. Whatever SDL2-internal batching/state-cache logic causes
  // that revert is opaque to us (compiled into the closed libSDL2.so) and
  // multiple attempts to fix it upstream (pooling, display-mode) didn't
  // change this specific symptom. Direct fix at the last possible moment
  // instead: the second-or-later draw since the last glClear (the
  // backdrop's own is always first) gets forced back onto the one texture
  // id confirmed to hold real content, immediately before the real
  // glDrawArrays call -- overriding whatever SDL2 itself just (re)bound.
  //
  // g_yuv_plane_ids exclusion confirmed necessary on-device (vidgl trace):
  // this condition also matched every YUV video draw -- its Y-plane texture
  // (bound on unit 0, 3-unit Y/U/V setup) is never content_tex_id (that only
  // ever tracks RGBA content, see tex_track_data()'s own format guard), so
  // "bound texture differs from content_tex_id" was true for the video too.
  // The hack then rebound unit 0 from the video's real Y plane onto a stale
  // unrelated RGBA texture while the YUV-conversion shader program stayed
  // active, feeding RGBA bytes through YUV math -- confirmed the source of
  // the reported oversaturated/washed video colors. A first fix gated this
  // on g_video_playing alone and broke a DIFFERENT, still-legitimate draw:
  // an ordinary RGBA overlay drawn while a video happens to also be playing
  // still needs this exact workaround as much as ever, and blanket-skipping
  // it for the whole video window skipped that draw's fix too (confirmed
  // on-device: video image never appeared at all). Checking specifically
  // whether the BOUND texture is one of the video's own plane ids -- not
  // just whether a video happens to be playing -- protects only the video's
  // own draw and leaves every other draw's fix intact.
  int is_yuv_plane_tex = 0;
  for (int i = 0; i < g_yuv_plane_count; i++) {
    if (g_yuv_plane_ids[i] == g_last_bound_tex) { is_yuv_plane_tex = 1; break; }
  }
  int forced_rebind = (!is_yuv_plane_tex && g_draws_since_clear >= 1 &&
                        g_content_tex_id != 0 && g_last_bound_tex != g_content_tex_id);
  if (video_trace_active()) {
    GLint prog = -1;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    GLboolean blend_on = glIsEnabled(GL_BLEND);
    debugPrintf("[vidgl] glDrawArrays#%llu mode=0x%x count=%d bound_tex=%u unit0tex=%u program=%d blend=%d"
                " content_tex_id=%u draws_since_clear=%llu%s\n",
                (unsigned long long)g_drawarrays_calls, mode, count, g_last_bound_tex, g_unit_bound_tex[0],
                prog, blend_on, g_content_tex_id, (unsigned long long)g_draws_since_clear,
                forced_rebind ? " FORCED-REBIND-ABOUT-TO-FIRE" : "");
  }
  if (forced_rebind) {
    // glBindTexture binds onto whatever unit is CURRENTLY active, not
    // necessarily unit 0 -- if SDL2 left a different unit active than the
    // one the fragment shader actually samples from, this call visibly
    // "happens" (tracked, no GL error) but has zero effect on the real draw:
    // exactly matching every prior symptom (bind confirmed via logging, draw
    // succeeds, swap succeeds, yet nothing changes on screen). Force unit 0
    // explicitly -- the near-universal default sampler unit for a simple 2D
    // sprite shader -- before rebinding, instead of trusting whatever unit
    // was left active.
    glActiveTexture(GL_TEXTURE0);
    g_active_tex_unit = GL_TEXTURE0;
    glBindTexture(GL_TEXTURE_2D, g_content_tex_id);
    g_last_bound_tex = g_content_tex_id;
    g_unit_bound_tex[0] = g_content_tex_id;
  }
  g_draws_since_clear++;
  // Skip draws that are provably invisible no-ops instead of letting them
  // through: g_tex_max_alpha[tex] is a running max over every byte ever
  // uploaded to this id, so a confirmed 0 here means every pixel this
  // texture has ever contained was fully transparent -- with correct
  // (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) blending this draw would leave the
  // framebuffer untouched regardless of order. The backdrop (texture_base,
  // 800x480, confirmed always-zero-alpha) is drawn with GL_BLEND disabled
  // (see the glDisable(GL_BLEND)/glEnable(GL_BLEND) toggling around every
  // draw pair), which turns it into a full opaque overwrite instead: with
  // SDL2's own batched renderer free to flush draws in a different order
  // than the RenderCopy calls that queued them (already established above --
  // "SDL2's renderer batches RenderCopy calls internally and only actually
  // issues glBindTexture/glDrawArrays when the batch flushes"), a backdrop
  // draw landing AFTER the real content draw paints solid black over
  // whatever was just correctly rendered, every single frame, right before
  // the swap that "succeeds". Skipping known-all-transparent draws entirely
  // makes the outcome match what correct blending would have produced
  // regardless of draw order or blend-enable state, with zero effect on any
  // draw that has ever shown real (non-zero-alpha) pixel data.
  // NOTE: deliberately NOT gated on g_tex_had_data[dtex] -- that flag only
  // latches when some byte *anywhere* in R,G,B,A was ever nonzero, so a
  // texture that is genuinely all-zero across all four channels (exactly
  // the backdrop's case) leaves had_data permanently false and would defeat
  // this check entirely if required. The alpha-only scan in tex_track_data()
  // runs unconditionally regardless of had_data, so g_tex_max_alpha alone is
  // the right (and sufficient) signal here.
  GLuint dtex = g_last_bound_tex;
  // is_yuv_plane_tex exclusion: g_tex_w/g_tex_h/g_tex_max_alpha are indexed
  // by raw GL texture id, a small integer the driver freely recycles across
  // completely unrelated textures over a long run. tex_track_data() (see
  // its own format guard, added for the earlier crash fix) never updates
  // these for a YUV plane, so a video texture that happens to reuse an id
  // some earlier, unrelated fully-transparent RGBA texture once held
  // inherits that STALE "fully transparent" reading forever -- silently
  // skipping every one of its draws, screen-black despite genuinely correct
  // pixel data already uploaded (confirmed on-device: glTexSubImage2D's own
  // trace showed a real fade-in and plausible chroma the whole time). This
  // was never triggered before the forced-rebind fix above: that bug swapped
  // g_last_bound_tex away from the video's real id before this check ever
  // ran, so it was always reading some OTHER (correctly-tracked) texture's
  // state instead -- fixing that exposed this separate, pre-existing bug.
  int all_transparent = (!is_yuv_plane_tex && dtex < TEX_TRACK_MAX &&
                          g_tex_w[dtex] > 0 && g_tex_h[dtex] > 0 && g_tex_max_alpha[dtex] == 0);
  if (!all_transparent) {
    // Six independent hypotheses (rebind, stale-id skip, CPU throttling,
    // thread join, mutex, condvar) all confirmed clean on-device, the freeze
    // still happens -- last remaining possibility this file can actually
    // test is the REAL underlying GL call itself stalling inside the
    // driver, invisible until now because every call here (unlike the
    // thread/mutex tracing above) was only ever logged once, before the
    // real call, with no paired confirmation it ever returned.
    int traced = video_trace_active();
    if (traced) debugPrintf("[vidgl] real glDrawArrays ENTRY\n");
    glDrawArrays(mode, first, count);
    if (traced) debugPrintf("[vidgl] real glDrawArrays RETURNED\n");
  }
#if VERBOSE_EGL
  // Not just the first 20 calls: the draw pattern is known to loop the same
  // few calls forever, so a head-only window can never prove a given texture
  // (e.g. one that got real pixel data well after the others) is *never*
  // bound for a draw across the run -- only that it wasn't in the first
  // handful of frames. Tracked per distinct texture id instead: each one
  // gets logged the first few times it's actually drawn, however late that
  // first happens, plus occasional GL-error checks throughout.
  //
  // Texture ids climb well past 64 during boot (the old attract-mode
  // texture-recreation loop ran for a while before input started working),
  // so a 64-bit bitmask silently stopped tagging "first draw" for any of
  // them -- switched to a small linear-scan table. And a single sampled
  // draw every 300 can never show a *pair* of draws in the same frame
  // (texture_base then the real content texture, if that's still the
  // question) since the periodic hit is one specific draw number with no
  // guarantee its very next call is also logged -- log a short burst of
  // consecutive draws at each periodic checkpoint instead of just the one.
  static uint32_t seen_ids[256];
  static int seen_count = 0;
  int first_time_for_this_tex = 1;
  for (int i = 0; i < seen_count; i++) {
    if (seen_ids[i] == g_last_bound_tex) { first_time_for_this_tex = 0; break; }
  }
  if (first_time_for_this_tex && seen_count < 256) seen_ids[seen_count++] = g_last_bound_tex;
  int in_burst = (g_drawarrays_calls % 300) < 6; // 6 consecutive draws every 300
  if (g_drawarrays_calls <= 200 || first_time_for_this_tex || in_burst) {
    GLenum err = glGetError();
    GLuint t = g_last_bound_tex;
    const char *data_state = (t < TEX_TRACK_MAX) ? (g_tex_had_data[t] ? "has-real-data" : "NEVER-nonzero") : "?";
    int tw = (t < TEX_TRACK_MAX) ? g_tex_w[t] : -1, th = (t < TEX_TRACK_MAX) ? g_tex_h[t] : -1;
    int maxa = (t < TEX_TRACK_MAX) ? g_tex_max_alpha[t] : -1;
    GLboolean blend_on = glIsEnabled(GL_BLEND);
    // Every prior layer (rect, bind, data, blend, swap) has checked out fine
    // yet nothing appears on the real screen -- one thing never yet checked:
    // if GL_COLOR_WRITEMASK is FALSE on any/all channels, the draw succeeds,
    // the bind is correct, blend is correct, glGetError is clean, and the
    // swap reports success, but literally nothing gets written to the
    // framebuffer. That would explain the disconnect exactly.
    GLboolean cmask[4] = {1,1,1,1};
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    GLboolean scissor_on = glIsEnabled(GL_SCISSOR_TEST);
    GLint srect[4] = {-1,-1,-1,-1};
    if (scissor_on) glGetIntegerv(GL_SCISSOR_BOX, srect);
    GLint active_unit = -1;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_unit);
    debugPrintf("[draw] glDrawArrays#%llu mode=0x%x count=%d tex=%u (%dx%d %s maxA=%d) blend=%d cmask=%d%d%d%d scissor=%d(%d,%d,%d,%d) unit=0x%x unit0tex=%u%s%s%s\n",
                (unsigned long long)g_drawarrays_calls, mode, count, t, tw, th, data_state, maxa, blend_on,
                cmask[0], cmask[1], cmask[2], cmask[3],
                scissor_on, srect[0], srect[1], srect[2], srect[3],
                active_unit, g_unit_bound_tex[0],
                first_time_for_this_tex ? " (first draw with this texture)" : "",
                err != GL_NO_ERROR ? " GL ERROR" : "",
                all_transparent ? " (SKIPPED-all-transparent)" : "");
    if (err != GL_NO_ERROR) debugPrintf("[draw]   -> GL ERROR 0x%x\n", err);
  }
#endif
}
static void glViewport_wrap(GLint x, GLint y, GLsizei w, GLsizei h) {
#if VERBOSE_EGL
  static int n = 0;
  if (n++ < 10) debugPrintf("[draw] glViewport(%d,%d,%d,%d)\n", x, y, w, h);
#endif
  glViewport(x, y, w, h);
}
static void glUseProgram_wrap(GLuint program) {
  int traced = video_trace_active();
  if (traced) debugPrintf("[vidgl] glUseProgram(%u) ENTRY\n", program);
#if VERBOSE_EGL
  static int n = 0;
  if (n++ < 10) debugPrintf("[draw] glUseProgram(%u)\n", program);
#endif
  glUseProgram(program);
  if (traced) debugPrintf("[vidgl] glUseProgram(%u) RETURNED\n", program);
}
static void glActiveTexture_wrap(GLenum texture) {
  g_active_tex_unit = texture;
  if (video_trace_active())
    debugPrintf("[vidgl] glActiveTexture(unit=0x%x)\n", texture);
#if VERBOSE_EGL
  static int n = 0;
  if (n++ < 300) debugPrintf("[draw] glActiveTexture(unit=0x%x)\n", texture);
#endif
  glActiveTexture(texture);
}
static void glBindTexture_wrap(GLenum target, GLuint texture) {
  g_last_bound_tex = texture;
  unsigned unit_idx = g_active_tex_unit - GL_TEXTURE0;
  if (unit_idx < 32) g_unit_bound_tex[unit_idx] = texture;
  int new_yuv_plane = 0;
  if (g_expect_yuv_planes) {
    int known = 0;
    for (int i = 0; i < g_yuv_plane_count; i++) if (g_yuv_plane_ids[i] == texture) { known = 1; break; }
    if (!known && g_yuv_plane_count < YUV_PLANE_IDS_MAX) { g_yuv_plane_ids[g_yuv_plane_count++] = texture; new_yuv_plane = 1; }
    if (g_yuv_plane_count >= YUV_PLANE_IDS_MAX) g_expect_yuv_planes = 0;
  }
  int traced = video_trace_active();
  if (traced)
    debugPrintf("[vidgl] glBindTexture(target=0x%x, tex=%u) on unit=0x%x ENTRY\n", target, texture, g_active_tex_unit);
#if VERBOSE_EGL
  static int n = 0;
  if (n++ < 300) debugPrintf("[draw] glBindTexture(target=0x%x, tex=%u) on unit=0x%x\n", target, texture, g_active_tex_unit);
#endif
  glBindTexture(target, texture);
  if (traced) debugPrintf("[vidgl] glBindTexture(tex=%u) RETURNED\n", texture);
  if (new_yuv_plane) {
    // Confirmed on-device: the video's Y/U/V plane data uploads correctly
    // (a real fade-in to full brightness, plausible chroma -- see
    // glTexSubImage2D_wrap's own trace) yet the screen stays solid black.
    // These planes are NPOT (480x270 / 240x135, not 480x256/256x128) with no
    // mipmaps ever generated for a streaming video texture -- GLES2 requires
    // GL_CLAMP_TO_EDGE wrap and a non-mipmap MIN_FILTER for an NPOT texture
    // to be "complete"; a texture created with the GL defaults (GL_REPEAT,
    // GL_NEAREST_MIPMAP_LINEAR) is incomplete under those conditions and
    // must sample as solid black regardless of what was actually uploaded --
    // matching this symptom exactly. Whatever SDL2's own YUV texture setup
    // does for this (perhaps relying on that real Android hardware GLES2
    // drivers commonly ignore the strict completeness rule for NPOT, unlike
    // Mesa/nouveau here), force the spec-safe combination explicitly on
    // each plane the moment it's first identified.
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (video_trace_active())
      debugPrintf("[vidgl] forced NPOT-safe params on new YUV plane tex=%u\n", texture);
  }
}
// If the game renders into an FBO for some effect (common for scaling/post-
// processing) and the final "blit back to the default framebuffer" step is
// missing or broken, every draw call would still succeed against the FBO --
// with nothing ever reaching the actual visible screen. glBindFramebuffer(0)
// means "back to the real screen"; anything else is offscreen.
static uint64_t g_fbo_bind_calls;
static GLuint g_last_fbo = 0xFFFFFFFFu;
static void glBindFramebuffer_wrap(GLenum target, GLuint framebuffer) {
  g_fbo_bind_calls++;
#if VERBOSE_EGL
  if (g_fbo_bind_calls <= 30 || framebuffer != g_last_fbo)
    debugPrintf("[draw] glBindFramebuffer(target=0x%x, fbo=%u)%s\n", target, framebuffer,
                framebuffer == 0 ? " [default/screen]" : " [OFFSCREEN]");
#endif
  g_last_fbo = framebuffer;
  glBindFramebuffer(target, framebuffer);
}
static void glEnable_wrap(GLenum cap) {
  if (video_trace_active())
    debugPrintf("[vidgl] glEnable(0x%x)%s\n", cap, cap == GL_BLEND ? " [BLEND]" : "");
#if VERBOSE_EGL
  static int n = 0;
  if (n++ < 20) debugPrintf("[draw] glEnable(0x%x)%s\n", cap, cap == GL_BLEND ? " [BLEND]" : "");
#endif
  glEnable(cap);
}
static void glDisable_wrap(GLenum cap) {
  if (video_trace_active())
    debugPrintf("[vidgl] glDisable(0x%x)%s\n", cap, cap == GL_BLEND ? " [BLEND]" : "");
#if VERBOSE_EGL
  static int n = 0;
  if (n++ < 20) debugPrintf("[draw] glDisable(0x%x)%s\n", cap, cap == GL_BLEND ? " [BLEND]" : "");
#endif
  glDisable(cap);
}
static void glBlendFunc_wrap(GLenum sfactor, GLenum dfactor) {
  if (video_trace_active())
    debugPrintf("[vidgl] glBlendFunc(src=0x%x, dst=0x%x)\n", sfactor, dfactor);
#if VERBOSE_EGL
  static int n = 0;
  if (n++ < 10) debugPrintf("[draw] glBlendFunc(src=0x%x, dst=0x%x)\n", sfactor, dfactor);
#endif
  glBlendFunc(sfactor, dfactor);
}
static void glTexImage2D_wrap(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                               GLint border, GLenum format, GLenum type, const void *pixels) {
  g_teximage_calls++;
  tex_track_size(g_last_bound_tex, width, height);
  tex_track_data(g_last_bound_tex, pixels, width, height, format);
#if VERBOSE_EGL
  // Cheap, unconditional id->size record (no pixel scan) so a texture id
  // seen much later in a glDrawArrays log (ids climb past the old-loop
  // leak into the hundreds) can still be traced back to its dimensions --
  // the expensive full-buffer zero-scan below stays capped to the first 10
  // calls, this alone is not.
  debugPrintf("[draw] texImage2D#%llu tex=%u %dx%d\n",
              (unsigned long long)g_teximage_calls, g_last_bound_tex, width, height);
  if (g_teximage_calls <= 10) {
    // Every check so far (shader link, viewport, blend, FBO target) has come
    // back clean, which leaves the pixel data itself: if bor.pak's image
    // decoding silently failed and handed OpenBOR a zeroed buffer instead of
    // erroring, every GL call downstream still "succeeds" and still produces
    // a black screen. Scan for any nonzero byte (bounded: 4 bytes/pixel is
    // the worst case here, so this is at most ~1.5MB, once per texture).
    int any_nonzero = 0;
    uint8_t min_b = 0xFF, max_b = 0x00;
    // format != GL_RGBA guard added alongside tex_track_data()'s (see its own
    // comment): this "upper bound regardless of format" claim was wrong for
    // single-channel YUV plane uploads, the actual on-device crash cause.
    if (pixels && width > 0 && height > 0 && format == GL_RGBA_FMT) {
      size_t n = (size_t)width * height * 4;
      const uint8_t *p = pixels;
      for (size_t i = 0; i < n; i++) {
        if (p[i]) any_nonzero = 1;
        if (p[i] < min_b) min_b = p[i];
        if (p[i] > max_b) max_b = p[i];
      }
    }
    debugPrintf("[draw] texImage2D#%llu %dx%d fmt=0x%x type=0x%x pixels=%p %s (byte range %u..%u)\n",
                (unsigned long long)g_teximage_calls, width, height, format, type, pixels,
                !pixels ? "NULL" : (any_nonzero ? "has data" : "ALL ZERO"), min_b, max_b);
  }
#endif
  glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

// The 3 glTexImage2D calls all alloc with pixels=NULL (a texture-atlas
// pattern: allocate storage, then fill regions via glTexSubImage2D) -- so the
// real image content, if any, has to come through here instead. The first
// upload to the 800x480 texture came back all-zero; tracked separately (by
// size, not a shared call counter) from the 320x240 one so a later, real
// update to the *big* texture can't get lost behind however many times the
// small one (clearly updated every frame, real data) refills its own budget.
static uint64_t g_texsubimage_calls, g_texsubimage_big_calls;
// Correct byte-per-pixel accounting (0 = unrecognized, scan skipped) for the
// video pixel-content diagnostic below -- the same class of mistake as
// tex_track_data()'s original bug, done safely this time.
static int bytes_per_pixel_for_format(GLenum format) {
  switch (format) {
    case 0x1908: return 4; // GL_RGBA
    case 0x1907: return 3; // GL_RGB
    case 0x1909: return 1; // GL_LUMINANCE -- expected for YUV Y/U/V planes
    case 0x1906: return 1; // GL_ALPHA
    case 0x190A: return 2; // GL_LUMINANCE_ALPHA
    default: return 0;
  }
}

static void glTexSubImage2D_wrap(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                  GLsizei width, GLsizei height, GLenum format, GLenum type,
                                  const void *pixels) {
  g_texsubimage_calls++;
  int is_big = (width * height) >= (800 * 480); // the one that came back all-zero
  if (is_big) g_texsubimage_big_calls++;
  tex_track_data(g_last_bound_tex, pixels, width, height, format);
  if (video_trace_active()) {
    // Now that the forced-rebind fix means the video's OWN Y/U/V textures
    // actually get drawn (instead of an accidentally-substituted RGBA one),
    // the video image went black -- checking whether the bytes handed to
    // THIS exact upload call are real decoded pixels or all-zero settles
    // whether that's a genuine empty-frame bug versus something further
    // down the pipeline (e.g. still drawing before the upload happens).
    int bpp = bytes_per_pixel_for_format(format);
    int any_nonzero = 0;
    uint8_t max_b = 0;
    if (pixels && bpp > 0 && width > 0 && height > 0) {
      size_t n = (size_t)width * height * (size_t)bpp;
      const uint8_t *p = pixels;
      for (size_t i = 0; i < n; i++) { if (p[i]) any_nonzero = 1; if (p[i] > max_b) max_b = p[i]; }
    }
    debugPrintf("[vidgl] glTexSubImage2D tex=%u at(%d,%d) %dx%d fmt=0x%x bpp=%d pixels=%p %s max=%u\n",
                g_last_bound_tex, xoffset, yoffset, width, height, format, bpp, pixels,
                !pixels ? "NULL" : (bpp == 0 ? "unknown-fmt" : (any_nonzero ? "has-data" : "ALL-ZERO")),
                max_b);
  }
#if VERBOSE_EGL
  int verbose = is_big ? (g_texsubimage_big_calls <= 30) : (g_texsubimage_calls <= 20);
  if (verbose) {
    int any_nonzero = 0;
    uint8_t min_b = 0xFF, max_b = 0x00;
    // Same format guard as tex_track_data() above -- see its comment.
    if (pixels && width > 0 && height > 0 && format == GL_RGBA_FMT) {
      size_t n = (size_t)width * height * 4;
      const uint8_t *p = pixels;
      for (size_t i = 0; i < n; i++) {
        if (p[i]) any_nonzero = 1;
        if (p[i] < min_b) min_b = p[i];
        if (p[i] > max_b) max_b = p[i];
      }
    }
    debugPrintf("[draw] texSubImage2D#%llu%s tex=%u at(%d,%d) %dx%d fmt=0x%x pixels=%p %s (byte range %u..%u)\n",
                (unsigned long long)g_texsubimage_calls, is_big ? " BIG" : "", g_last_bound_tex,
                xoffset, yoffset, width, height, format, pixels,
                !pixels ? "NULL" : (any_nonzero ? "has data" : "ALL ZERO"), min_b, max_b);
  }
#endif
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

// ---------------------------------------------------------------------------
// SDL_RenderCopy interception: everything above traces raw GL state, which
// answers "what gets drawn" but not "why" -- openbor.c's blit() calls this
// (an SDL2 API, exported by libSDL2.so, not one of our own shims) directly,
// once for the backdrop (dst=NULL, i.e. "whole render target") and once for
// the actual game/menu content texture (dst=a computed rect). Registered in
// dynlib_functions so libopenbor.so's own import of "SDL_RenderCopy"
// resolves to this instead of straight through to libSDL2.so's real export;
// imports_set_real_rendercopy() (called once SDL2 is loaded, from
// resolve_entry_points() in main.c) supplies the real implementation to
// forward to.
//
// A first version of this wrapper, on discovering the content texture's
// dst rect came out {0,0,0,0} (SDL's GLES2 backend silently skips a
// zero-area RenderCopy, no error -- exactly why every glDrawArrays trace
// only ever showed the backdrop), patched it by substituting NULL
// (whole-target stretch) right here. That was too blunt: the very next
// trace showed *14 distinct texture pointers* all hitting this same
// degenerate-rect path, not just the one content composite -- coordinates
// computed from the same broken nativeWidth/nativeHeight feed sprite/UI
// positioning throughout the game, not just this one call site, so
// blanket-stretching every one of them to fullscreen just stacked them
// all on top of each other in whatever order they drew, which is no
// better than black. Fixed at the actual source instead (see
// SDL_GetDesktopDisplayMode_wrap below) -- this wrapper goes back to
// observing only, kept because it's still the clearest place to confirm
// the fix actually worked (rects should stop showing as degenerate at all).
typedef struct { int x, y, w, h; } SdlRectShim;
static int (*e_real_SDL_RenderCopy)(void *renderer, void *texture, const void *srcrect, const void *dstrect);
void imports_set_real_rendercopy(void *fn) { e_real_SDL_RenderCopy = fn; }

static int SDL_RenderCopy_wrap(void *renderer, void *texture, const void *srcrect, const void *dstrect) {
  static void *seen_tex[16];
  static int seen_n = 0;
  static uint64_t calls = 0;
  calls++;
  int first_time = 1;
  for (int i = 0; i < seen_n; i++) if (seen_tex[i] == texture) { first_time = 0; break; }
  if (first_time && seen_n < 16) seen_tex[seen_n++] = texture;

  const SdlRectShim *dr = dstrect;
  int degenerate = dr && (dr->w <= 0 || dr->h <= 0);

  if (calls <= 40 || first_time || (calls % 180 == 0) || degenerate) {
    debugPrintf("[draw] SDL_RenderCopy#%llu tex=%p dst=%s%s\n",
                (unsigned long long)calls, texture,
                dr ? "" : "NULL(=whole target)",
                first_time ? " (first time this texture ptr)" : "");
    if (dr) {
      debugPrintf("[draw]   dst rect = {%d,%d,%d,%d}%s\n", dr->x, dr->y, dr->w, dr->h,
                  degenerate ? " <-- DEGENERATE (zero/negative area)" : "");
    }
  }

  // (No longer checking bound_tex immediately after this call: SDL2's
  // renderer batches RenderCopy calls internally and only actually issues
  // glBindTexture/glDrawArrays when the batch flushes, not synchronously
  // within RenderCopy itself -- every call showed "no bind happened" here
  // regardless of whether it eventually drew correctly, so this measured
  // the wrong moment. glDrawArrays's own logging below is what actually
  // shows the real bind+draw whenever the batch flushes.)
  if (!e_real_SDL_RenderCopy) return -1;
  return e_real_SDL_RenderCopy(renderer, texture, srcrect, dstrect);
}

// SDL_RenderClear/SDL_RenderPresent: SDL2's renderer batches RenderCopy
// calls and only actually flushes them (real glBindTexture/glDrawArrays)
// at specific points -- typically when the batch buffer fills, the render
// target/state changes incompatibly, or at RenderPresent. Since every
// glDrawArrays sample so far only ever shows the backdrop, never the
// content texture despite it receiving real data and a correct dst rect
// every frame, these establish the actual per-frame call sequence directly
// instead of continuing to infer it -- does RenderPresent even get called,
// and in what order relative to the two RenderCopy calls and blit()'s own
// glClear?
static int (*e_real_SDL_RenderClear)(void *renderer);
static void (*e_real_SDL_RenderPresent)(void *renderer);
void imports_set_real_renderclear(void *fn) { e_real_SDL_RenderClear = fn; }
void imports_set_real_renderpresent(void *fn) { e_real_SDL_RenderPresent = fn; }

static int SDL_RenderClear_wrap(void *renderer) {
  static uint64_t calls = 0;
  calls++;
  if (calls <= 40 || calls % 180 == 0) debugPrintf("[draw] SDL_RenderClear#%llu\n", (unsigned long long)calls);
  return e_real_SDL_RenderClear ? e_real_SDL_RenderClear(renderer) : -1;
}

static void SDL_RenderPresent_wrap(void *renderer) {
  static uint64_t calls = 0;
  calls++;
  if (calls <= 40 || calls % 180 == 0) debugPrintf("[draw] SDL_RenderPresent#%llu\n", (unsigned long long)calls);
  // Paired ENTRY/RETURNED, gated the same way as glDrawArrays/glBindTexture/
  // glUseProgram above: this is the one call in the whole per-frame sequence
  // most likely to trigger an actual eglSwapBuffers/vsync wait deep inside
  // the driver, a plausible stall point none of the six already-eliminated
  // hypotheses (GL rebind state, stale texture id, CPU clock, thread join,
  // mutex, condvar) would ever have shown any sign of.
  int traced = video_trace_active();
  if (traced) debugPrintf("[vidgl] real SDL_RenderPresent#%llu ENTRY\n", (unsigned long long)calls);
  if (e_real_SDL_RenderPresent) e_real_SDL_RenderPresent(renderer);
  if (traced) debugPrintf("[vidgl] real SDL_RenderPresent#%llu RETURNED\n", (unsigned long long)calls);
}

// A freeze consistently observed a few seconds after the FIRST video whose
// own completion hands control back to interactive/idle engine code (the
// attract-mode "press start" clip) -- never between the earlier boot videos,
// which just chain straight to the next one -- with no distinguishing GL
// state, file I/O, or GL error anywhere in this file's own tracing right up
// to the exact moment it stops. Consistent with a stuck SDL_CreateThread/
// SDL_WaitThread pairing inside this build's own video subsystem (confirmed
// via the earlier Atmosphere crash report to run decode on its own SDL
// thread) -- tracing thread creation/join directly settles it. Signature is
// the plain 3-arg SDL_CreateThread(fn, name, data) -- stable across every
// non-Windows/OS2 SDL2 release since 2.0.4; the extra-args
// SDL_CreateThread_REAL variant only exists for platforms needing external
// CRT thread begin/end hooks, which this Android/POSIX build is not.
typedef int (*SdlThreadFn)(void *);
static void *(*e_real_SDL_CreateThread)(SdlThreadFn fn, const char *name, void *data);
static void (*e_real_SDL_WaitThread)(void *thread, int *status);
void imports_set_real_createthread(void *fn) { e_real_SDL_CreateThread = fn; }
void imports_set_real_waitthread(void *fn) { e_real_SDL_WaitThread = fn; }
static uint64_t g_threads_created, g_threads_waited;

static void *SDL_CreateThread_wrap(SdlThreadFn fn, const char *name, void *data) {
  g_threads_created++;
  debugPrintf("[thread] SDL_CreateThread name=\"%s\" fn=%p (created=%llu waited=%llu outstanding=%llu)\n",
              name ? name : "(null)", (void *)fn,
              (unsigned long long)g_threads_created, (unsigned long long)g_threads_waited,
              (unsigned long long)(g_threads_created - g_threads_waited));
  return e_real_SDL_CreateThread ? e_real_SDL_CreateThread(fn, name, data) : NULL;
}

static void SDL_WaitThread_wrap(void *thread, int *status) {
  // Logged before AND after the real call on purpose: if this hangs, the
  // log will show ENTRY with no matching RETURNED, pinpointing the freeze
  // to this exact call instead of leaving it a total blank like every other
  // trace in this file has so far.
  debugPrintf("[thread] SDL_WaitThread ENTRY thread=%p\n", thread);
  if (e_real_SDL_WaitThread) e_real_SDL_WaitThread(thread, status);
  g_threads_waited++;
  debugPrintf("[thread] SDL_WaitThread RETURNED thread=%p (created=%llu waited=%llu)\n",
              thread, (unsigned long long)g_threads_created, (unsigned long long)g_threads_waited);
}

// Thread create/join came back perfectly balanced (confirmed on-device) --
// every video's decode/demux/audio thread properly created AND joined,
// none left outstanding by the time of the freeze. Next most likely stuck
// primitive: the condition-variable/mutex pair those same threads almost
// certainly use to hand frames to each other (a classic producer/consumer
// pattern) -- a missed signal (the signaling thread already exited, or
// signaled before the waiter started waiting) blocks SDL_CondWait forever
// without ever showing up as a "thread never joined", since the STUCK
// thread is still alive, just parked here. Gated on video_trace_active()'s
// now-8-real-second window (comfortably covers every freeze delay actually
// observed: 1.7-5.1s after the last video's own close) rather than
// unconditional, since this fires on every audio buffer somewhere in a
// normal run and would otherwise flood the whole boot log.
static int (*e_real_SDL_LockMutex)(void *mutex);
static int (*e_real_SDL_CondWait)(void *cond, void *mutex);
void imports_set_real_lockmutex(void *fn) { e_real_SDL_LockMutex = fn; }
void imports_set_real_condwait(void *fn) { e_real_SDL_CondWait = fn; }

static int SDL_LockMutex_wrap(void *mutex) {
  int traced = video_trace_active();
  if (traced) debugPrintf("[thread] SDL_LockMutex ENTRY mutex=%p\n", mutex);
  int ret = e_real_SDL_LockMutex ? e_real_SDL_LockMutex(mutex) : -1;
  if (traced) debugPrintf("[thread] SDL_LockMutex RETURNED mutex=%p\n", mutex);
  return ret;
}

static int SDL_CondWait_wrap(void *cond, void *mutex) {
  int traced = video_trace_active();
  if (traced) debugPrintf("[thread] SDL_CondWait ENTRY cond=%p mutex=%p\n", cond, mutex);
  int ret = e_real_SDL_CondWait ? e_real_SDL_CondWait(cond, mutex) : -1;
  if (traced) debugPrintf("[thread] SDL_CondWait RETURNED cond=%p mutex=%p -> %d\n", cond, mutex, ret);
  return ret;
}

// The actual source: SDL_GetDesktopDisplayMode() is what seeds video.c's
// nativeWidth/nativeHeight at boot (see the SDL_RenderCopy comment above
// for how far downstream that reaches). Fix it directly here rather than
// patching each of the many rects computed from it: call through to the
// real implementation, then if it came back with a degenerate w/h,
// override with our own known-correct screen_width/screen_height before
// returning. Whatever upstream JNI/display-metrics path this build
// actually relies on (confirmed NOT simply getDisplayDPI returning non-
// NULL) no longer matters once this is patched at its output.
static int (*e_real_SDL_GetDesktopDisplayMode)(int displayIndex, void *mode);
void imports_set_real_getdesktopdisplaymode(void *fn) { e_real_SDL_GetDesktopDisplayMode = fn; }

typedef struct { uint32_t format; int w, h, refresh_rate; void *driverdata; } SdlDisplayModeShim;

static int SDL_GetDesktopDisplayMode_wrap(int displayIndex, void *mode) {
  int ret = e_real_SDL_GetDesktopDisplayMode ? e_real_SDL_GetDesktopDisplayMode(displayIndex, mode) : -1;
  SdlDisplayModeShim *m = mode;
  debugPrintf("[draw] SDL_GetDesktopDisplayMode(%d) -> %d mode={fmt=0x%x w=%d h=%d refresh=%d}\n",
              displayIndex, ret, m ? m->format : 0, m ? m->w : -1, m ? m->h : -1,
              m ? m->refresh_rate : -1);
  if (m && (m->w <= 0 || m->h <= 0)) {
    debugPrintf("[draw] SDL_GetDesktopDisplayMode: degenerate, overriding to %dx%d\n",
                screen_width, screen_height);
    m->w = screen_width;
    m->h = screen_height;
    if (m->refresh_rate <= 0) m->refresh_rate = 60;
    ret = 0;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// SDL_CreateTexture/SDL_DestroyTexture pooling: on-device tracing (both the
// per-texture-id data tracking above and a direct SDL_RenderCopy
// interception) proved video_set_mode() in this build's video.c is being
// called every single rendered frame, continuously, for the whole run --
// not just during the title/attract screen as first assumed. It
// unconditionally destroys and recreates texture_base/texture/buttons on
// every call (unlike window/renderer, which it only creates once, guarded
// by `if (!window && ...)`), so the one holding real game/menu content
// never survives long enough to actually be drawn before being wiped by
// the next call -- textures ids climb by exactly 3 every cycle (confirmed
// past 690 in one test) and every single one ever bound for a draw traces
// back to a freshly-blanked, never-updated allocation. We can't patch
// video.c itself (compiled into the closed .so), so intercept the
// create/destroy pair instead and refuse to actually recreate: a texture
// request matching an already-cached one's (format, access, w, h) hands
// back that same cached handle instead of asking the real driver for a new
// one, and destroy on a cached handle just marks it free instead of
// actually releasing it. From video.c's point of view every "recreate"
// still appears to succeed -- it just keeps landing on the same GL object,
// so whatever content was uploaded to it survives instead of being wiped.
// RE-ENABLED: disabling this to isolate an earlier "content never draws"
// regression showed that regression persisted with pooling OFF too --
// pooling was never the cause. The real bug (confirmed via full
// glBindTexture + SDL_RenderClear/Present instrumentation) is that this
// build's SDL2 renderer always reverts the bound texture back to the
// backdrop's by the time glDrawArrays fires for the content draw,
// regardless of pooling -- now worked around directly in glDrawArrays_wrap
// (forced rebind onto g_content_tex_id for the second-or-later draw since
// the last glClear). With that fix in place, pooling's original purpose
// (stop video_set_mode() from destroying/recreating texture_base/texture/
// buttons every single rendered frame -- confirmed continuing at 15,000+
// draws/7,000+ ids in one run with pooling off) is safe to restore too.
#define TEX_POOLING_ENABLED 1

// Every SDL_CreateTexture call this build has ever made uses access=1
// (STREAMING) -- both the plain-RGB backdrop (fmt=0x16762004) that's been
// reused this way for a run's entire duration without issue, AND, now that
// real (non-stubbed) webm playback exists, a planar YUV video texture
// (fmt=0x32315659, "YV12"). Confirmed on-device: reusing the pooled handle
// for a SECOND video's YV12 texture crashes hard (no exception, no error
// line, log just stops) at the exact instant of the "reused pooled" log
// line -- the first video's own (freshly created, never pooled-reused)
// texture played fine. access alone can't be what's unsafe to reuse, since
// the RGB backdrop is access=1 too and never crashed; the one structural
// difference is the pixel format: SDL2's GLES2 renderer backs a planar YUV
// texture with three separate GL textures (Y/U/V) plus its own conversion
// state behind one opaque handle, versus a single plain GL texture for RGB
// -- somewhere in that extra machinery, this build's video decoder (closed
// source, can't patch it directly) doesn't tolerate a second video's decode
// session landing on a handle a prior video's session already initialized.
// Excluding planar-YUV formats from the pool (always a real create/destroy
// for those, exactly like every video played before this pool ever
// existed) sidesteps it without touching the RGB pooling this was built for.
static int is_planar_yuv_format(uint32_t format) {
  switch (format) {
    case 0x32315659: // SDL_PIXELFORMAT_YV12
    case 0x56555949: // SDL_PIXELFORMAT_IYUV
    case 0x3231564e: // SDL_PIXELFORMAT_NV12
    case 0x3132564e: // SDL_PIXELFORMAT_NV21
      return 1;
    default:
      return 0;
  }
}

typedef struct { void *renderer; uint32_t format; int access, w, h; void *tex; int in_use; } TexPoolEntry;
#define TEX_POOL_MAX 8
static TexPoolEntry s_tex_pool[TEX_POOL_MAX];
static int s_tex_pool_n;

static void *(*e_real_SDL_CreateTexture)(void *renderer, uint32_t format, int access, int w, int h);
static void (*e_real_SDL_DestroyTexture)(void *texture);
void imports_set_real_createtexture(void *fn) { e_real_SDL_CreateTexture = fn; }
void imports_set_real_destroytexture(void *fn) { e_real_SDL_DestroyTexture = fn; }

// Diagnostic only: a hang observed 2-3 real seconds after the last video's
// own close(), with no distinguishing GL state right before it, is
// consistent with a slow resource leak (e.g. real GL textures never
// actually freed) tipping over some driver-side limit after several videos'
// worth of allocations -- these counters settle whether creates and
// completed real destroys for YUV textures specifically stay balanced.
static uint64_t g_yuv_creates, g_yuv_destroys;

static void *SDL_CreateTexture_wrap(void *renderer, uint32_t format, int access, int w, int h) {
#if TEX_POOLING_ENABLED
  if (is_planar_yuv_format(format)) {
    void *tex = e_real_SDL_CreateTexture ? e_real_SDL_CreateTexture(renderer, format, access, w, h) : NULL;
    // Arms glBindTexture_wrap's capture of this new texture's own Y/U/V
    // plane ids (see g_yuv_plane_ids' own comment) -- a fresh set every
    // time, since a genuinely new YUV texture (never pooled, see above)
    // gets a genuinely new trio of real GL texture ids from the driver.
    g_expect_yuv_planes = 1;
    g_yuv_plane_count = 0;
    g_yuv_creates++;
    debugPrintf("[draw] SDL_CreateTexture(%ux%u fmt=0x%x access=%d) -> new %p (YUV, not pooled)"
                " [yuv creates=%llu destroys=%llu]\n",
                w, h, format, access, tex,
                (unsigned long long)g_yuv_creates, (unsigned long long)g_yuv_destroys);
    return tex;
  }
  for (int i = 0; i < s_tex_pool_n; i++) {
    TexPoolEntry *e = &s_tex_pool[i];
    if (!e->in_use && e->renderer == renderer && e->format == format && e->access == access &&
        e->w == w && e->h == h) {
      e->in_use = 1;
      debugPrintf("[draw] SDL_CreateTexture(%ux%u fmt=0x%x access=%d) -> reused pooled %p\n",
                  w, h, format, access, e->tex);
      return e->tex;
    }
  }
  void *tex = e_real_SDL_CreateTexture ? e_real_SDL_CreateTexture(renderer, format, access, w, h) : NULL;
  if (tex && s_tex_pool_n < TEX_POOL_MAX) {
    TexPoolEntry *e = &s_tex_pool[s_tex_pool_n++];
    e->renderer = renderer; e->format = format; e->access = access; e->w = w; e->h = h;
    e->tex = tex; e->in_use = 1;
    debugPrintf("[draw] SDL_CreateTexture(%ux%u fmt=0x%x access=%d) -> new %p (pooled, slot %d/%d)\n",
                w, h, format, access, tex, s_tex_pool_n, TEX_POOL_MAX);
  } else if (tex) {
    debugPrintf("[draw] SDL_CreateTexture(%ux%u fmt=0x%x access=%d) -> new %p (pool full, NOT cached)\n",
                w, h, format, access, tex);
  }
  return tex;
#else
  return e_real_SDL_CreateTexture ? e_real_SDL_CreateTexture(renderer, format, access, w, h) : NULL;
#endif
}

// texture_base and "buttons" (unlike the game-content "texture") are created
// via SDL_CreateTextureFromSurface, not SDL_CreateTexture directly -- a
// separate SDL2 export, so it needs its own interception/pool to actually
// cover the one texture every trace so far shows is the *only* one ever
// drawn (texture_base, the all-zero 800x480 backdrop). Matches SDL_Surface's
// real (stable, public ABI) leading layout so w/h/pixels/pitch line up.
typedef struct { uint32_t flags; void *format; int w, h, pitch; void *pixels; } SdlSurfaceShim;
typedef struct { void *renderer; int w, h; void *tex; int in_use; } TexFromSurfacePoolEntry;
#define TEXFS_POOL_MAX 4
static TexFromSurfacePoolEntry s_texfs_pool[TEXFS_POOL_MAX];
static int s_texfs_pool_n;

static void *(*e_real_SDL_CreateTextureFromSurface)(void *renderer, void *surface);
static int (*e_real_SDL_UpdateTexture)(void *texture, const void *rect, const void *pixels, int pitch);
void imports_set_real_createtexturefromsurface(void *fn) { e_real_SDL_CreateTextureFromSurface = fn; }
void imports_set_real_updatetexture(void *fn) { e_real_SDL_UpdateTexture = fn; }

static void *SDL_CreateTextureFromSurface_wrap(void *renderer, void *surface) {
#if TEX_POOLING_ENABLED
  SdlSurfaceShim *s = surface;
  int w = s ? s->w : 0, h = s ? s->h : 0;
  for (int i = 0; i < s_texfs_pool_n; i++) {
    TexFromSurfacePoolEntry *e = &s_texfs_pool[i];
    if (!e->in_use && e->renderer == renderer && e->w == w && e->h == h) {
      e->in_use = 1;
      // Re-push this surface's current pixels: unlike the plain
      // SDL_CreateTexture pool above (whose caller always re-fills via a
      // separate texSubImage2D/UpdateTexture call right after), callers of
      // CreateTextureFromSurface expect the surface's content to already be
      // in the returned texture -- reusing the handle without this would
      // leave whatever the PREVIOUS surface's content was.
      if (e_real_SDL_UpdateTexture && s) e_real_SDL_UpdateTexture(e->tex, NULL, s->pixels, s->pitch);
      debugPrintf("[draw] SDL_CreateTextureFromSurface(%dx%d) -> reused pooled %p\n", w, h, e->tex);
      return e->tex;
    }
  }
  void *tex = e_real_SDL_CreateTextureFromSurface ? e_real_SDL_CreateTextureFromSurface(renderer, surface) : NULL;
  if (tex && s_texfs_pool_n < TEXFS_POOL_MAX) {
    TexFromSurfacePoolEntry *e = &s_texfs_pool[s_texfs_pool_n++];
    e->renderer = renderer; e->w = w; e->h = h; e->tex = tex; e->in_use = 1;
    debugPrintf("[draw] SDL_CreateTextureFromSurface(%dx%d) -> new %p (pooled, slot %d/%d)\n",
                w, h, tex, s_texfs_pool_n, TEXFS_POOL_MAX);
  }
  return tex;
#else
  return e_real_SDL_CreateTextureFromSurface ? e_real_SDL_CreateTextureFromSurface(renderer, surface) : NULL;
#endif
}

static void SDL_DestroyTexture_wrap(void *texture) {
#if TEX_POOLING_ENABLED
  // A YUV texture is never added to either pool below (see
  // is_planar_yuv_format() above), so it's never matched here either --
  // falls straight through to a real destroy, same as before this pool
  // existed.
  for (int i = 0; i < s_tex_pool_n; i++) {
    if (s_tex_pool[i].tex == texture) {
      s_tex_pool[i].in_use = 0; // keep it alive for reuse; don't actually destroy
      return;
    }
  }
  for (int i = 0; i < s_texfs_pool_n; i++) {
    if (s_texfs_pool[i].tex == texture) {
      s_texfs_pool[i].in_use = 0;
      return;
    }
  }
  // Reaching here means `texture` matched neither pool -- at this point in
  // a run that's almost always a YUV texture (every RGBA one this build
  // creates ends up pooled instead), so this doubles as the create/destroy
  // balance counter's other half. Not a perfectly exclusive signal, but
  // close enough to catch a gross imbalance (a real leak) immediately.
  g_yuv_destroys++;
#endif
  if (e_real_SDL_DestroyTexture) e_real_SDL_DestroyTexture(texture);
}

// A silent shader compile/link failure leaves the renderer bound to a broken
// program: every later glDrawArrays call "succeeds" (no EGL/GL error we were
// already checking) but rasterizes nothing, so the frame stays whatever
// glClear left it at -- a genuinely black screen with a fully "working"
// render loop on top, which is exactly what 29,000+ successful frames with a
// constant draw/texture count and no visible output looks like. Always
// logged (unconditionally, not just under VERBOSE_EGL): this fires at most a
// few times at startup, never per-frame.
static void glCompileShader_wrap(GLuint shader) {
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512]; GLsizei len = 0;
    glGetShaderInfoLog(shader, sizeof(log), &len, log);
    debugPrintf("[shader] COMPILE FAILED (shader=%u): %s\n", shader, log);
  }
}
static void glLinkProgram_wrap(GLuint program) {
  glLinkProgram(program);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512]; GLsizei len = 0;
    glGetProgramInfoLog(program, sizeof(log), &len, log);
    debugPrintf("[shader] LINK FAILED (program=%u): %s\n", program, log);
  }
}

// ---------------------------------------------------------------------------
// OpenSLES: made to fail outright so SDL2's Android audio backend always
// falls back to the JNI path (audioOpen/audioWriteShortBuffer/audioClose,
// bridged to audout in audio.c / jni_fake.c) instead of the native OpenSL ES
// one, which we don't implement.
// ---------------------------------------------------------------------------

#define SL_RESULT_FEATURE_UNSUPPORTED 0xC

static const int iid_engine, iid_play, iid_bufferqueue, iid_volume;
const void *SL_IID_ENGINE                   = &iid_engine;
const void *SL_IID_PLAY                     = &iid_play;
const void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &iid_bufferqueue;
const void *SL_IID_VOLUME                   = &iid_volume;

static uint32_t slCreateEngine_fake(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                                     uint32_t numInterfaces, const void *pInterfaceIds,
                                     const uint8_t *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pEngine) *pEngine = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// GLES1 extension entry points (FBO / draw-texture): not part of the core
// 1.0/1.1 spec mesa links directly, so resolve them lazily via the same
// eglGetProcAddress path real Android GLES drivers require.
// ---------------------------------------------------------------------------

typedef void (*GenericFn)(void);
#define LAZY(name) do { if (!p) p = (GenericFn)eglGetProcAddress(#name); } while (0)

static void glBindFramebufferOES_fake(GLenum target, GLuint fb) {
  static GenericFn p; LAZY(glBindFramebufferOES);
  if (p) ((void (*)(GLenum, GLuint))p)(target, fb);
}
static void glDeleteFramebuffersOES_fake(GLsizei n, const GLuint *fbs) {
  static GenericFn p; LAZY(glDeleteFramebuffersOES);
  if (p) ((void (*)(GLsizei, const GLuint *))p)(n, fbs);
}
static void glGenFramebuffersOES_fake(GLsizei n, GLuint *fbs) {
  static GenericFn p; LAZY(glGenFramebuffersOES);
  if (p) ((void (*)(GLsizei, GLuint *))p)(n, fbs);
}
static GLenum glCheckFramebufferStatusOES_fake(GLenum target) {
  static GenericFn p; LAZY(glCheckFramebufferStatusOES);
  return p ? ((GLenum (*)(GLenum))p)(target) : 0;
}
static void glFramebufferTexture2DOES_fake(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
  static GenericFn p; LAZY(glFramebufferTexture2DOES);
  if (p) ((void (*)(GLenum, GLenum, GLenum, GLuint, GLint))p)(target, attachment, textarget, texture, level);
}
static void glBlendEquationOES_fake(GLenum mode) {
  static GenericFn p; LAZY(glBlendEquationOES);
  if (p) ((void (*)(GLenum))p)(mode);
}
static void glBlendEquationSeparateOES_fake(GLenum modeRGB, GLenum modeAlpha) {
  static GenericFn p; LAZY(glBlendEquationSeparateOES);
  if (p) ((void (*)(GLenum, GLenum))p)(modeRGB, modeAlpha);
}
static void glBlendFuncSeparateOES_fake(GLenum sRGB, GLenum dRGB, GLenum sA, GLenum dA) {
  static GenericFn p; LAZY(glBlendFuncSeparateOES);
  if (p) ((void (*)(GLenum, GLenum, GLenum, GLenum))p)(sRGB, dRGB, sA, dA);
}
static void glDrawTexfOES_fake(GLfloat x, GLfloat y, GLfloat z, GLfloat w, GLfloat h) {
  static GenericFn p; LAZY(glDrawTexfOES);
  if (p) ((void (*)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat))p)(x, y, z, w, h);
}
#undef LAZY

// ---------------------------------------------------------------------------
// import table (union of libhidapi.so + libSDL2.so + libopenbor.so UND dynsyms)
// ---------------------------------------------------------------------------

static const DynLibFunction dynlib_functions[] = {
  // --- OpenSLES (made to fail; see above) ---
  { "slCreateEngine", (uintptr_t)&slCreateEngine_fake },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },

  // --- C++ runtime (libhidapi.so) ---
  { "_Znwm", (uintptr_t)&cxx_new },
  { "_Znam", (uintptr_t)&cxx_new_arr },
  { "_ZdlPv", (uintptr_t)&cxx_delete },
  { "_ZdaPv", (uintptr_t)&cxx_delete_arr },

  // --- bionic runtime / stdio backing ---
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_write", (uintptr_t)&__android_log_write_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "abort", (uintptr_t)&abort_fake },
  { "exit", (uintptr_t)&exit_fake },
  { "_exit", (uintptr_t)&exit_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "getenv", (uintptr_t)&getenv },
  { "setenv", (uintptr_t)&setenv },
  { "raise", (uintptr_t)&raise },

  // --- fortify (_chk) ---
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },

  // --- dynamic loader (routed to the EGL/GLES bridge) ---
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },

  // --- setjmp/longjmp (real newlib symbols) ---
  { "setjmp", (uintptr_t)&setjmp },
  { "_setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "mallinfo", (uintptr_t)&mallinfo_fake },
  { "sysinfo", (uintptr_t)&sysinfo_fake },
  { "getpagesize", (uintptr_t)&getpagesize_fake },

  // --- mem/str ---
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&strlen },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strcoll", (uintptr_t)&strcoll },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strftime", (uintptr_t)&strftime },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "strtod", (uintptr_t)&strtod },
  { "strtol", (uintptr_t)&strtol },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strlcat", (uintptr_t)&strlcat },
  { "strlcpy", (uintptr_t)&strlcpy },
  { "atoi", (uintptr_t)&atoi },
  { "atof", (uintptr_t)&atof },
  { "atol", (uintptr_t)&atol },
  { "isalnum", (uintptr_t)&isalnum },
  { "islower", (uintptr_t)&islower },
  { "isupper", (uintptr_t)&isupper },
  { "isspace", (uintptr_t)&isspace },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsncpy", (uintptr_t)&wcsncpy },

  // --- printf / scanf family ---
  { "printf", (uintptr_t)&debugPrintf },
  { "__printf_chk", (uintptr_t)&__printf_chk_fake },
  { "putchar", (uintptr_t)&putchar_fake },
  { "puts", (uintptr_t)&puts_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "fscanf", (uintptr_t)&fscanf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },

  // --- stdio over fake __sF + buffered fopen ---
  { "fopen", (uintptr_t)&fopen_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell },
  { "rewind", (uintptr_t)&rewind },
  { "fflush", (uintptr_t)&fflush_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "feof", (uintptr_t)&feof },
  { "fgetc", (uintptr_t)&fgetc },
  { "fgets", (uintptr_t)&fgets },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "remove", (uintptr_t)&remove },
  { "lseek", (uintptr_t)&lseek_fake },
  { "open", (uintptr_t)&open_fake },
  { "close", (uintptr_t)&close_fake },
  { "read", (uintptr_t)&read_fake },
  { "mkdir", (uintptr_t)&mkdir },
  { "chdir", (uintptr_t)&chdir },
  { "opendir", (uintptr_t)&opendir_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "closedir", (uintptr_t)&closedir_fake },

  // --- math ---
  { "acos", (uintptr_t)&acos }, { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin }, { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan }, { "atanf", (uintptr_t)&atanf },
  { "atan2", (uintptr_t)&atan2 }, { "atan2f", (uintptr_t)&atan2f },
  { "cos", (uintptr_t)&cos }, { "cosf", (uintptr_t)&cosf },
  { "sin", (uintptr_t)&sin }, { "sinf", (uintptr_t)&sinf },
  { "tan", (uintptr_t)&tan }, { "tanf", (uintptr_t)&tanf },
  { "exp", (uintptr_t)&exp }, { "expf", (uintptr_t)&expf },
  { "pow", (uintptr_t)&pow }, { "powf", (uintptr_t)&powf },
  { "log", (uintptr_t)&log }, { "logf", (uintptr_t)&logf },
  { "log10", (uintptr_t)&log10 }, { "log10f", (uintptr_t)&log10f },
  { "fmod", (uintptr_t)&fmod }, { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp },
  { "modf", (uintptr_t)&modf },
  { "scalbn", (uintptr_t)&scalbn }, { "scalbnf", (uintptr_t)&scalbnf },

  // --- time ---
  { "clock", (uintptr_t)&clock },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "localtime", (uintptr_t)&localtime },
  { "ctime", (uintptr_t)&ctime },
  { "time", (uintptr_t)&time },
  { "usleep", (uintptr_t)&usleep },
  { "nanosleep", (uintptr_t)&nanosleep },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake },

  // --- scheduling ---
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },

  // --- signals (stubbed; never raised) ---
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "signal", (uintptr_t)&signal_fake },
  { "sigaddset", (uintptr_t)&sigaddset_fake },
  { "sigemptyset", (uintptr_t)&sigemptyset_fake },

  // --- semaphores ---
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },

  // --- zlib (host -lz) ---
  { "adler32", (uintptr_t)&adler32 },
  { "crc32", (uintptr_t)&crc32 },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "deflateReset", (uintptr_t)&deflateReset },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "inflateReset2", (uintptr_t)&inflateReset2 },

  // --- ANativeWindow / ALooper / ASensor* ---
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_fake },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_fake },
  { "ALooper_forThread", (uintptr_t)&ALooper_forThread_fake },
  { "ALooper_prepare", (uintptr_t)&ALooper_prepare_fake },
  { "ALooper_pollAll", (uintptr_t)&ALooper_pollAll_fake },
  { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance_fake },
  { "ASensorManager_getSensorList", (uintptr_t)&ASensorManager_getSensorList_fake },
  { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue_fake },
  { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue_fake },
  { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor_fake },
  { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor_fake },
  { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents_fake },
  { "ASensor_getName", (uintptr_t)&ASensor_getName_fake },
  { "ASensor_getType", (uintptr_t)&ASensor_getType_fake },

  // --- EGL ---
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },

  // --- GLES2 core (mesa) ---
  { "glActiveTexture", (uintptr_t)&glActiveTexture_wrap },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer_wrap },
  { "glBindTexture", (uintptr_t)&glBindTexture_wrap },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFunc", (uintptr_t)&glBlendFunc_wrap },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glClear", (uintptr_t)&glClear_wrap },
  { "glClearColor", (uintptr_t)&glClearColor_wrap },
  { "glCompileShader", (uintptr_t)&glCompileShader_wrap },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDisable", (uintptr_t)&glDisable_wrap },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays_wrap },
  { "glEnable", (uintptr_t)&glEnable_wrap },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram", (uintptr_t)&glLinkProgram_wrap },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glTexImage2D", (uintptr_t)&glTexImage2D_wrap },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D_wrap },
  { "SDL_RenderCopy", (uintptr_t)&SDL_RenderCopy_wrap },
  { "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode_wrap },
  { "SDL_CreateTexture", (uintptr_t)&SDL_CreateTexture_wrap },
  { "SDL_CreateTextureFromSurface", (uintptr_t)&SDL_CreateTextureFromSurface_wrap },
  { "SDL_DestroyTexture", (uintptr_t)&SDL_DestroyTexture_wrap },
  { "SDL_RenderClear", (uintptr_t)&SDL_RenderClear_wrap },
  { "SDL_RenderPresent", (uintptr_t)&SDL_RenderPresent_wrap },
  { "SDL_CreateThread", (uintptr_t)&SDL_CreateThread_wrap },
  { "SDL_WaitThread", (uintptr_t)&SDL_WaitThread_wrap },
  { "SDL_LockMutex", (uintptr_t)&SDL_LockMutex_wrap },
  { "SDL_CondWait", (uintptr_t)&SDL_CondWait_wrap },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram_wrap },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport_wrap },

  // --- GLES1 fixed-function core (mesa; SDL_render_gles's immediate-mode path) ---
  { "glColor4f", (uintptr_t)&glColor4f },
  { "glDisableClientState", (uintptr_t)&glDisableClientState },
  { "glEnableClientState", (uintptr_t)&glEnableClientState },
  { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
  { "glMatrixMode", (uintptr_t)&glMatrixMode },
  { "glOrthof", (uintptr_t)&glOrthof },
  { "glPopMatrix", (uintptr_t)&glPopMatrix },
  { "glPushMatrix", (uintptr_t)&glPushMatrix },
  { "glRotatef", (uintptr_t)&glRotatef },
  { "glShaderBinary", (uintptr_t)&glShaderBinary },
  { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
  { "glTexEnvf", (uintptr_t)&glTexEnvf },
  { "glTexParameteriv", (uintptr_t)&glTexParameteriv },
  { "glTranslatef", (uintptr_t)&glTranslatef },
  { "glVertexPointer", (uintptr_t)&glVertexPointer },

  // --- GLES1 extensions (FBO / draw-texture; lazily resolved via eglGetProcAddress) ---
  { "glBindFramebufferOES", (uintptr_t)&glBindFramebufferOES_fake },
  { "glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffersOES_fake },
  { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffersOES_fake },
  { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatusOES_fake },
  { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2DOES_fake },
  { "glBlendEquationOES", (uintptr_t)&glBlendEquationOES_fake },
  { "glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparateOES_fake },
  { "glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparateOES_fake },
  { "glDrawTexfOES", (uintptr_t)&glDrawTexfOES_fake },
};

static const size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void sorx_resolve_imports(so_module *mod) {
  so_relocate(mod);
  so_resolve(mod, (DynLibFunction *)dynlib_functions, (int)dynlib_numfunctions, 1);
}
