/* gpuarena.c -- dedicated contiguous arena for GPU buffers.
 *
 * WHY
 * libdrm_nouveau backs EVERY GPU buffer (textures, framebuffers) with
 * memalign(0x1000, size). Those are large, page-aligned, and churned
 * constantly as the engine creates and destroys them -- for this port,
 * specifically once real (non-stubbed) video playback creates and destroys
 * a fresh set of real GL textures for each of the 9 intro/logo/attract-mode
 * clips. Serving them from newlib's general heap shreds it: once
 * fragmented, a multi-MB CONTIGUOUS page-aligned run cannot be placed even
 * with plenty free in aggregate. memalign then returns NULL ->
 * nouveau_bo_new fails -> GL_OUT_OF_MEMORY -> incomplete framebuffer -> the
 * compositor is handed something invalid and the console wedges.
 *
 * This is what a from-scratch investigation on this exact port could never
 * pin down: paired ENTRY/RETURNED tracing around every GL call (see
 * imports.c's glDrawArrays_wrap/SDL_RenderPresent_wrap) confirmed the real
 * glDrawArrays() call itself -- invoked from inside SDL_RenderPresent's own
 * internal batched-draw flush -- simply never returns, reliably a few
 * seconds after the FIRST video whose completion hands control back to
 * interactive engine code. Six other hypotheses (a GL state leak, a stale
 * texture id, CPU clock throttling, a stuck thread join, a stuck mutex, a
 * stuck condvar) were each individually confirmed clean on real hardware
 * before this one -- a driver-level wedge from heap fragmentation, not a
 * hang in anything this process's own code directly controls, matches
 * every one of those negative results at once: it would look exactly like
 * a stuck glDrawArrays from the outside, on whichever call happened to need
 * the next buffer the fragmented heap could no longer place.
 *
 * THE FIX
 * Reserve one big contiguous region ONCE, before the video subsystem's own
 * GL texture churn ever starts. Hand GPU buffers out of it with a page
 * bitmap, so they can never fragment newlib and newlib can never fragment
 * them. Small or non-page-aligned requests still go to newlib.
 *
 * Ported from cloverpit_nx (itself ported from pvz_fusion_nx), where this
 * is the confirmed, tested fix for the identical symptom (a fixed-point
 * freeze after repeated GPU texture/framebuffer churn) on this same
 * hardware/driver combination.
 *
 * DEADLOCK NOTE, inherited and important: never call debugPrintf while
 * holding gpua_lock. Logging writes to the SD card and can allocate
 * internally, which re-enters __wrap_memalign -> gpua_alloc ->
 * mutexLock(gpua_lock). libnx mutexes are not recursive, so that
 * self-deadlocks the calling thread. Record the outcome under the lock;
 * report it after releasing.
 *
 * MIT.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>

#include "util.h"

/* Provided by the linker's --wrap: the genuine newlib entry points. */
extern void *__real_memalign(size_t align, size_t size);
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t n, size_t size);
extern void *__real_realloc(void *p, size_t size);
extern void  __real_free(void *p);

#define GPUA_PAGE  0x1000u
/* Starting request. Shrinks in 64 MB steps until it fits, so a smaller
 * console or a heavier payload degrades instead of failing. This port's own
 * pak full-file cache already reserves ~395 MB, so keep this modest --
 * video textures at 480x270 are small, and the arena costs only address
 * space it would otherwise have fragmented anyway. */
#define GPUA_BYTES ((size_t)128 * 1024 * 1024)
#define GPUA_FLOOR ((size_t)32  * 1024 * 1024)
/* Below this, newlib is fine: small allocations do not cause the large-run
 * fragmentation this arena exists to prevent. */
#define GPUA_MIN   (64u * 1024)
/* ABOVE this, it is not a GPU buffer nouveau would allocate for this port's
 * own video textures (480x270 YV12 planes: Y is ~130KB, U/V ~32KB each, well
 * under a megabyte) or its normal game-content textures. 32 MB is generous
 * headroom for a full-screen 1920x1080 RGBA render target should one ever
 * be allocated this way, without swallowing this port's own much larger,
 * differently-aligned allocations (the pak's own ~395 MB memalign(0x1000, ..)
 * full-file cache buffer, which must NOT be routed into this arena -- see
 * the comparison-direction warning below). */
#define GPUA_MAX   ((size_t)32 * 1024 * 1024)

static uint8_t  *gpua_base;
static size_t    gpua_pages;
static uint8_t  *gpua_used;      /* 1 byte per page: in use            */
static uint32_t *gpua_runlen;    /* run length, recorded at first page */
static size_t    gpua_hint;
static Mutex     gpua_lock;
static int       gpua_state;     /* 0 = untried, 1 = ready, -1 = disabled */
/* The arena stays inert until gpua_enable() is called, so it can never be
 * triggered by this port's own boot-time allocations (the pak's own
 * ~395 MB full-file cache buffer chief among them) -- only by the graphics
 * driver once the engine is actually running. Belt and braces alongside
 * GPUA_MAX: either one alone would keep that buffer out of the arena. */
static int       gpua_enabled;
static size_t    gpua_live_pages, gpua_peak_pages;

void gpua_enable(void) {
  gpua_enabled = 1;
  debugPrintf("[gpua] arena enabled (routing %uKB..%uMB page-aligned allocations)\n",
              (unsigned)(GPUA_MIN >> 10), (unsigned)(GPUA_MAX >> 20));
}

static void gpua_init(void) {
  if (!gpua_enabled) return;
  if (gpua_state) return;
  size_t got_mb = 0;
  int report = 0;

  mutexLock(&gpua_lock);
  if (!gpua_state) {
    size_t want = GPUA_BYTES;
    uint8_t *b = NULL;
    while (want >= GPUA_FLOOR) {
      b = (uint8_t *)__real_memalign(GPUA_PAGE, want);
      if (b) break;
      want -= (size_t)32 * 1024 * 1024;
    }
    if (b) {
      gpua_pages  = want / GPUA_PAGE;
      gpua_used   = (uint8_t *)__real_calloc(gpua_pages, 1);
      gpua_runlen = (uint32_t *)__real_calloc(gpua_pages, sizeof(uint32_t));
      if (gpua_used && gpua_runlen) {
        gpua_base = b; gpua_hint = 0; gpua_state = 1;
        got_mb = want >> 20; report = 1;
      } else {
        __real_free(gpua_used); __real_free(gpua_runlen); __real_free(b);
        gpua_used = NULL; gpua_runlen = NULL;
        gpua_state = -1; report = 2;
      }
    } else {
      gpua_state = -1; report = 3;
    }
  }
  mutexUnlock(&gpua_lock);

  if (report == 1)
    debugPrintf("[gpua] GPU arena reserved: %u MB contiguous\n", (unsigned)got_mb);
  else if (report == 2)
    debugPrintf("[gpua] GPU arena DISABLED (bitmap alloc failed)\n");
  else if (report == 3)
    debugPrintf("[gpua] GPU arena DISABLED (no contiguous region) -- expect the "
                "fragmentation freeze\n");
}

static void *gpua_alloc(size_t sz) {
  gpua_init();
  if (gpua_state != 1) return NULL;
  size_t need = (sz + GPUA_PAGE - 1) / GPUA_PAGE;
  if (!need || need > gpua_pages) return NULL;

  void *out = NULL;
  mutexLock(&gpua_lock);
  for (int pass = 0; pass < 2 && !out; pass++) {     /* hint first, then wrap */
    size_t i   = pass ? 0 : gpua_hint;
    size_t end = pass ? gpua_hint : gpua_pages;
    while (i + need <= end) {
      size_t run = 0;
      while (run < need && !gpua_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) gpua_used[i + k] = 1;
        gpua_runlen[i] = (uint32_t)need;
        gpua_hint = i + need;
        if (gpua_hint >= gpua_pages) gpua_hint = 0;
        gpua_live_pages += need;
        if (gpua_live_pages > gpua_peak_pages) gpua_peak_pages = gpua_live_pages;
        out = gpua_base + i * GPUA_PAGE;
        break;
      }
      i += run + 1;                                   /* skip past the blocker */
    }
  }
  mutexUnlock(&gpua_lock);
  return out;
}

static int gpua_owns(const void *p) {
  return gpua_state == 1 && (const uint8_t *)p >= gpua_base &&
         (const uint8_t *)p < gpua_base + gpua_pages * GPUA_PAGE;
}

static void gpua_free(void *p) {
  size_t i = (size_t)(((uint8_t *)p - gpua_base) / GPUA_PAGE);
  mutexLock(&gpua_lock);
  uint32_t n = (i < gpua_pages) ? gpua_runlen[i] : 0;
  if (n) {
    for (uint32_t k = 0; k < n; k++) gpua_used[i + k] = 0;
    gpua_runlen[i] = 0;
    gpua_live_pages = (gpua_live_pages >= n) ? gpua_live_pages - n : 0;
    if (i < gpua_hint) gpua_hint = i;
  }
  mutexUnlock(&gpua_lock);
}

/* Peak usage, for the render loop to report once frames are flowing. */
unsigned gpua_peak_mb(void) {
  return (unsigned)((gpua_peak_pages * GPUA_PAGE) >> 20);
}
unsigned gpua_live_mb(void) {
  return (unsigned)((gpua_live_pages * GPUA_PAGE) >> 20);
}

/* ---- the wrapped entry points ------------------------------------------- */

/* Route only nouveau's buffer shape: alignment >= 0x1000 AND size >= GPUA_MIN.
 *
 * The comparison direction matters: routing every ordinary large allocation
 * with small alignment into the GPU arena fills it with non-GPU data and
 * starves the buffers it exists to serve -- the exact opposite of the
 * intent. Just as importantly here, this port's own pak full-file cache
 * (libc_shim.c's pak_track(), memalign(0x1000, ~395 MB)) is ALSO
 * page-aligned -- GPUA_MAX excludes it by size alone, and gpua_enable()
 * being called only after that cache is already loaded excludes it by
 * timing too. Either guard alone would keep it out of the arena. */
void *__wrap_memalign(size_t align, size_t size) {
  void *p = NULL;
  if (gpua_enabled && align >= GPUA_PAGE &&
      size >= GPUA_MIN && size <= GPUA_MAX)
    p = gpua_alloc(size);
  if (!p)
    p = __real_memalign(align ? align : 8, size);
  /* A failed page-aligned allocation IS the bug this file exists to prevent,
   * so say so loudly (bounded) rather than letting it surface as a GL error
   * or an unexplained freeze. */
  if (!p && align >= GPUA_PAGE) {
    static unsigned nf;
    if (nf < 3) {
      nf++;
      debugPrintf("[gpua] memalign FAILED size=%u KB (arena live=%u MB peak=%u MB)\n",
                  (unsigned)(size >> 10), gpua_live_mb(), gpua_peak_mb());
    }
  }
  return p;
}

void __wrap_free(void *p) {
  if (!p) return;
  if (gpua_owns(p)) { gpua_free(p); return; }
  __real_free(p);
}

/* realloc must respect arena ownership: __real_realloc would treat an arena
 * pointer as a newlib block and corrupt the heap. */
void *__wrap_realloc(void *p, size_t size) {
  if (p && gpua_owns(p)) {
    /* Copy must be bounded by the OLD block, not the new size: on a grow,
     * copying `size` bytes reads past the end of the arena slot. Recover the
     * old length from the run-length table. */
    size_t old_pages = 0;
    size_t i = (size_t)(((uint8_t *)p - gpua_base) / GPUA_PAGE);
    if (i < gpua_pages) old_pages = gpua_runlen[i];
    size_t oldsz = old_pages * GPUA_PAGE;

    void *q = __real_malloc(size);
    if (q) memcpy(q, p, size < oldsz ? size : oldsz);
    gpua_free(p);
    return q;
  }
  return __real_realloc(p, size);
}

void *__wrap_malloc(size_t size)          { return __real_malloc(size); }
void *__wrap_calloc(size_t n, size_t sz)  { return __real_calloc(n, sz); }
