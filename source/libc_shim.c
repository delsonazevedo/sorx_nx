/* libc_shim.c -- bionic<->newlib libc/NDK wrappers for libhidapi.so,
 * libSDL2.so and libopenbor.so. Converting wrappers where the ABIs differ;
 * exact matches are forwarded straight from imports.c.
 *
 * Object-model lineage: the fortify/_chk, stdio-over-fake-__sF, ANativeWindow
 * and POSIX-semaphore shims below started as the NBA Jam / SOTN Switch
 * wrapper's libc_shim (Andy Nguyen, fgsfds lineage); extended here for the
 * additional bionic surface SDL2's Android backend and OpenBOR touch (fortify
 * variants, mallinfo/sysinfo, ALooper/ASensor, system properties).
 * MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <time.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"
#include "util.h"

// openbor's NDK r21e build reads this global directly instead of the
// TLS-based tpidr_el0+0x28 canary tls_setup_guard() sets up for the older
// NDK r20 SDL2/hidapi builds; any fixed nonzero value works since we never
// intentionally corrupt a return address.
uintptr_t __stack_chk_guard = 0xA5A5C0DE1234BEEFull;

// ---------------------------------------------------------------------------
// fortify (_chk): ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memcpy(dst, src, n);
}
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memmove(dst, src, n);
}
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcpy(dst, src);
}
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcat(dst, src);
}
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen; return strncat(dst, src, n);
}
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t slen) {
  (void)dstlen; (void)slen; return strncpy(dst, src, n);
}
char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen; return strchr(s, c);
}
char *__strrchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen; return strrchr(s, c);
}
size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen; return strlen(s);
}
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va);
}
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsprintf(s, fmt, va);
}
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen; return read(fd, buf, count);
}
// bare "printf" is already routed to debugPrintf (see imports.c), but a
// fortified NDK build calls this instead -- if this is the symbol actually
// being used for engine printf()s like "SDL video Renderer: ...", the bare
// "printf" hook would never see them at all (confirmed: that exact string
// never showed up in our own log despite the hook existing), explaining why
// they were invisible to our own logging even though they clearly execute.
int __printf_chk_fake(int flag, const char *fmt, ...) {
  (void)flag;
  char buf[512];
  va_list va; va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  return debugPrintf("%s", buf);
}

// ---------------------------------------------------------------------------
// misc bionic
// ---------------------------------------------------------------------------

static int gettid_fake(void) {
  u64 id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) && id)
    return (int)(id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  if (number == ARM64_SYS_GETTID) return gettid_fake();
  errno = ENOSYS;
  return -1;
}

void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

// bionic clockids (REALTIME=0, MONOTONIC=1, ...) differ from newlib's;
// translate the id and fall back to the libnx tick so it never fails.
// bionic and newlib timespec match on arm64 LP64.
int clock_gettime_fake(int clk, void *ts_) {
  struct timespec *ts = ts_;
  if (!ts) { errno = EFAULT; return -1; }
  clockid_t real = (clk == 0 || clk == 5) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  if (clock_gettime(real, ts) == 0)
    return 0;
  uint64_t ns = armTicksToNs(armGetSystemTick());
  ts->tv_sec = (int64_t)(ns / 1000000000ull);
  ts->tv_nsec = (int64_t)(ns % 1000000000ull);
  return 0;
}

void android_set_abort_message_fake(const char *msg) { (void)msg; }

size_t __ctype_get_mb_cur_max_fake(void) { return 1; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    case BIONIC_SC_PHYS_PAGES: return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}

// SDL2's android video backend probes a couple of system properties for
// device-specific workarounds; report a plain "no such property" for
// everything except the SDK level, which some code paths gate on.
int __system_property_get_fake(const char *name, char *value) {
  if (name && !strcmp(name, "ro.build.version.sdk")) {
    strcpy(value, "21");
    return 2;
  }
  value[0] = 0;
  return 0;
}

void __android_log_write_fake(int prio, const char *tag, const char *msg) {
  (void)prio;
  debugPrintf("[%s] %s\n", tag ? tag : "", msg ? msg : "");
}

// signals: the engines install crash/segv handlers we never trigger; stub them
int sigaction_fake(int sig, const void *act, void *oldact) {
  (void)sig; (void)act; (void)oldact; return 0;
}
void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int sigaddset_fake(void *set, int sig) { (void)set; (void)sig; return 0; }
int sigemptyset_fake(void *set) { (void)set; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *oldset) {
  (void)how; (void)set; (void)oldset; return 0;
}

// ---------------------------------------------------------------------------
// memory / process info probes (advisory-only: exact bit-for-bit bionic
// struct layouts don't matter here, only the fields the engines actually read)
// ---------------------------------------------------------------------------

struct bionic_mallinfo mallinfo_fake(void) {
  struct bionic_mallinfo mi;
  memset(&mi, 0, sizeof(mi));
  mi.fordblks = 512 * 1024 * 1024; // report plenty of "free" heap
  return mi;
}

int sysinfo_fake(void *info) {
  // bionic's struct sysinfo (64-bit): matches this layout with no trailing
  // padding array (20 - 2*sizeof(long) - sizeof(int) == 0 on LP64).
  struct {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram;
    unsigned long totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
  } *s = info;
  memset(s, 0, sizeof(*s));
  s->totalram = 4ull * 1024 * 1024 * 1024;
  s->freeram = 2ull * 1024 * 1024 * 1024;
  s->mem_unit = 1;
  return 0;
}

int getpagesize_fake(void) { return 0x1000; }

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF: libSDL2/libopenbor bind std streams to
// &__sF[N]; these absorb accesses to those fake FILEs and forward everything
// else straight through to newlib.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f, *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr) return 0;
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr || is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (!f) return -1;
  if (is_fake_file(f)) return c;
  return fputc(c, f);
}
int fflush_fake(FILE *f) {
  if (!f || is_fake_file(f)) return 0;
  return fflush(f);
}
int fclose_fake(FILE *f) {
  if (!f || is_fake_file(f)) return 0;
  return fclose(f);
}
int ferror_fake(FILE *f) {
  if (!f || is_fake_file(f)) return 0;
  return ferror(f);
}
// Mirrored into our own log too: this is how the engine actually writes
// Logs/OpenBorLog.txt ("SDL video Renderer: ..." et al go through fprintf()
// on a FILE* OpenBOR opened itself, not bare printf()/__printf_chk -- so
// without this, those lines are invisible to sorx_debug.log and impossible
// to correlate in time against our own [egl]/[draw]/[input] events).
int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return 0;
  char buf[512];
  va_list va; va_start(va, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  debugPrintf("%s", buf);
  fputs(buf, f);
  return n;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) return 0;
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, va);
  debugPrintf("%s", buf);
  fputs(buf, f);
  return n;
}
int fseek_fake(FILE *f, long off, int whence) {
  if (!f || is_fake_file(f)) return -1;
  return fseek(f, off, whence);
}
int putchar_fake(int c) { return c; }
int puts_fake(const char *s) { (void)s; return 0; }

// Defined later (dirent section, after DirCache exists) -- evicts a
// cached directory listing so a subsequent opendir() re-scans for real
// instead of replaying a listing captured before this new file/dir
// existed. Forward-declared here so both fopen_fake (save/config writes)
// and vpak_materialize/vpak_mkdirs_for (pak extraction) can invalidate the
// exact parent they just added an entry to.
static void dircache_invalidate_parent_of(const char *path);

FILE *fopen_fake(const char *path, const char *mode) {
  if (!path) return NULL;
  FILE *f = fopen(path, mode);
  if (f && strchr(mode, 'r'))
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
  if (f && (strchr(mode, 'w') || strchr(mode, 'a')))
    dircache_invalidate_parent_of(path);
#if VERBOSE_IO
  debugPrintf("fopen(\"%s\", \"%s\") -> %p\n", path, mode, (void *)f);
#endif
  return f;
}

// ---------------------------------------------------------------------------
// bor.pak read-ahead: OpenBOR's own archive layer reads its (large, ~370+ MB)
// pak through many small scattered read()/lseek() calls -- each one a
// separate round trip to the SD card. Wrap read()/lseek()/close() for the
// pak's descriptor with a large adaptive read-ahead buffer, collapsing many
// small reads into few big ones (same technique as the NBA Jam/SOTN Switch
// wrapper's OBB read-ahead, just keyed on the "bor.pak" basename here).
// ---------------------------------------------------------------------------

// A sliding read-ahead window (this used to be up to 512KB, adaptively
// grown on detected sequential access) turned out not to be the real fix:
// on-device timestamps (every debugPrintf line now carries a real elapsed-
// seconds prefix -- see util.c) showed OpenBOR's own packfile reader
// serving 358 MB of a ~395 MB pak through 1.4 MILLION read() calls in the
// first 11 seconds alone, just to reach the main menu -- an average read of
// ~256 bytes. That's not "occasionally scattered", it's this custom
// engine's own catalog-scan algorithm fundamentally reading in tiny
// pieces; a window only helps when the *pattern* has locality a refill can
// capture, and at this call volume even a 100% cache-hit window still pays
// real, measurable per-call overhead (branch, bounds check, refill-or-not
// decision) 1.4 million times over. The only way to actually flatten that
// is to remove the window concept entirely: load the whole file into RAM
// once (a single sequential pass -- far friendlier to the SD card than
// scattered small reads even at the same total byte count) and serve every
// later read() as a plain memcpy against a fixed buffer, no bounds-window
// bookkeeping per call at all. ~395 MB is a rounding error against the
// ~2GB+ this port already has free in full-RAM/title-override mode.
typedef struct {
  int fd;            // -1 = free slot
  off_t pos;         // logical file position
  off_t size;        // file size == the buffer's exact allocated length
  uint8_t *buf;      // the whole file, loaded once
  char path[128];    // last path this slot was tracking, for the reopen-reuse check below
} PakFd;

#define PAK_NFD 4
static PakFd s_pak[PAK_NFD];
static int s_pak_inited;
// Unlike VirtWinFd's s_virtwin_lock and SmallCacheEntry's s_smallcache_lock
// below (both added specifically because more than one thread can touch
// them), this table had no lock at all until real (non-stubbed) webm
// playback exposed the gap: OpenBOR's video decode very plausibly runs on
// its own thread, and bor.pak's own fd gets closed and reopened constantly
// by the main thread's packfile reader (thousands of times a run, see
// pak_track()'s own comment) -- on this OS, a closed fd's small integer
// number is immediately eligible for reuse by literally the next open() any
// thread makes, video decode's included. Two threads mutating (pak_track())
// or reading-then-mutating (pak_find() callers) the same s_pak[] slots with
// no synchronization is a plain data race on its own, worse still if a
// video's real fd number happens to coincide with whatever bor.pak's fd
// currently is: read_fake()/lseek_fake() would silently serve/advance
// against the WRONG (bor.pak's) buffer for that call, no crash at the call
// site itself, just quietly wrong bytes fed to the video decoder -- a
// plausible root cause for a second video's decode corrupting state and
// crashing shortly after, which is exactly what was observed on-device.
static Mutex s_pak_lock;

static uint64_t g_pak_reads, g_pak_cold_loads, g_pak_bytes, g_pak_sd_bytes;

// Total bytes served from any tracked pak fd so far; main.c's loop samples
// this to detect "loading" (heavy pak reads, same technique as the NBA Jam
// Switch wrapper's OBB read-ahead this is descended from) and boost the CPU
// during those windows -- see main()'s adaptive-boost comment for why a
// single cpu_boost(1) at startup isn't enough on its own.
uint64_t pak_bytes_total(void) { return g_pak_bytes; }

static int path_is_pak(const char *p) {
  // Not just "bor.pak": OpenBOR's normal layout keeps the actual selectable
  // game(s) as separate *.pak files under Paks/ (bor.pak alone, with an empty
  // Paks/, leaves the engine with nothing to select -- it never left the
  // loading screen). Any of them can be exactly as large and scattered-read
  // as bor.pak, so match the read-ahead by extension, not by one fixed name.
  size_t len = strlen(p);
  if (len < 4) return 0;
  const char *ext = p + len - 4;
  return ext[0] == '.' && (ext[1] == 'p' || ext[1] == 'P') &&
         (ext[2] == 'a' || ext[2] == 'A') && (ext[3] == 'k' || ext[3] == 'K');
}

// ---------------------------------------------------------------------------
// Pak aliasing: OpenBOR only ever treats files under Paks/ as selectable
// "games" (see README's install notes) -- an empty Paks/ leaves its menu
// with nothing to pick and it never leaves the loading screen -- but the
// bytes it actually reads once a game IS selected are byte-for-byte
// identical to bor.pak (this port only ever ships the one game). The
// release layout used to work around that by shipping bor.pak twice, once
// at the root and once again under Paks/: doubles the SD footprint, doubles
// vpak_catalogs_build_once()'s indexing work (two full catalog parses of
// the same ~380 MB of data), and -- since path_is_pak()'s read-ahead cache
// above is keyed by path, not content -- doubles the read-ahead traffic
// too, since the two paths could never share a cache slot despite being the
// same file underneath.
//
// Disassembly of this exact libopenbor.so's Menu() confirms its Paks/ scan
// only ever does opendir()+readdir()+strcasecmp() on each entry's filename
// to build the sorted game list -- no stat(), no open() of the candidates
// themselves -- so a purely virtual directory entry (see the "Paks" special
// case in dircache_get() below) is enough to satisfy it; open_fake() only
// has to make the real open() succeed once a game is actually picked and
// openpackfile() tries to read it, which the alias table below does by
// transparently retrying against the real path.
//
// Deliberately a fallback, not a replacement: it only ever engages once the
// real open() at the virtual path has already failed, so an SD card still
// set up the old way (a real file physically present under Paks/) keeps
// working exactly as before, untouched.
typedef struct { const char *virt; const char *real; } PakAlias;
static const PakAlias s_pak_aliases[] = {
  { "Paks/1.0.0.pak", "bor.pak" },
};
#define PAK_ALIAS_N (sizeof(s_pak_aliases) / sizeof(s_pak_aliases[0]))

// True if `path` refers to the same location as `suffix`, either exactly or
// as a path ending in "/" + suffix. On-device logging showed
// Menu()'s paksDir-based opendir() actually arriving as a full absolute
// path ("/switch/sorx_nx/Paks", not the bare "Paks" its paksDir global
// alone would suggest -- something upstream prepends the resolved data
// root), while our own vpak_catalog_load("bor.pak") call uses a bare
// relative one, and other call sites in this .so have separately been
// observed using a "./" prefix (see dircache_invalidate_parent_of).
// Matching only the exact alias string missed the absolute form entirely
// -- confirmed on-device as an empty game-select menu -- so match on the
// path's trailing component instead, which is form-agnostic and still
// requires a real "/" boundary (so e.g. "SomeOtherPaks" can't accidentally
// match a "Paks" suffix).
static int path_ends_with_component(const char *path, const char *suffix) {
  size_t pl = strlen(path), sl = strlen(suffix);
  if (pl < sl) return 0;
  const char *tail = path + (pl - sl);
  if (strcmp(tail, suffix) != 0) return 0;
  return tail == path || tail[-1] == '/';
}

static const char *pak_alias_real(const char *path) {
  for (size_t i = 0; i < PAK_ALIAS_N; i++)
    if (path_ends_with_component(path, s_pak_aliases[i].virt)) return s_pak_aliases[i].real;
  return NULL;
}

// Video overrides: every *.webm in bor.pak's catalog (see vpak_materialize()'s
// comment further down) is a real 1920x1080 VP8 file -- confirmed by probing
// all 9 directly (ffprobe) after this was raised as a "why can't we replicate
// the phone's instant video playback" question. Software-decoding any of them
// at their native resolution stalls rendering (logo_sega.webm, only 681KB,
// hung just as hard as the 35MB intro.webm -- resolution, not file size or
// duration, is what makes each frame expensive). Re-encoding to 480x270 (1/16
// the pixel count) got real video playing and visible on-device (after also
// fixing a crash, a GL state leak, and a stale-texture-id bug this exposed
// along the way -- all still in place below, genuine fixes independent of
// whether real video content is ever actually used), but a driver-level
// hang (confirmed via paired ENTRY/RETURNED GL tracing in imports.c: the
// real glDrawArrays() call itself, invoked from inside SDL_RenderPresent's
// own internal batch flush, simply never returns) surfaces reliably a few
// seconds after the first video whose own completion hands control back to
// interactive engine code -- GPU resource churn from repeatedly creating and
// destroying real YUV textures across a run, most likely. That's inside
// Mesa/nouveau, not fixable from here. DISABLED (empty table -- every path
// falls through to the existing, always-safe stub-empty behavior below) as
// of that finding; the mechanism itself, and everything built on top of it,
// is left in place rather than ripped out in case a future attempt (a
// different resolution, a texture-pooling strategy that avoids the create/
// destroy churn, or simply a fixed driver) wants to pick this back up.
static const PakAlias s_video_overrides[] = {
};
#define VIDEO_OVERRIDE_N (sizeof(s_video_overrides) / sizeof(s_video_overrides[0]))

static const char *video_override_real(const char *path) {
  for (size_t i = 0; i < VIDEO_OVERRIDE_N; i++)
    if (path_ends_with_component(path, s_video_overrides[i].virt)) return s_video_overrides[i].real;
  return NULL;
}

// Temporary diagnostic only (not a cache, no behavior change): the pooled-
// GL-texture theory for the second-playwebm() crash was disproven (still
// crashes with YUV textures excluded from the pool -- see imports.c), so
// this traces the plain real read()/lseek()/close() calls open_fake() below
// already makes on a video override fd, to see whether the second playback's
// read pattern looks any different from the first's before whatever's
// actually wrong crashes it. Single-slot: only one video ever plays at a
// time, confirmed sequential in the logs.
static int s_video_trace_fd = -1;
static char s_video_trace_path[192];
static uint64_t s_video_trace_reads;
static uint64_t s_video_trace_bytes;
static off_t s_video_trace_size;

// Exported (see libc_shim.h) so imports.c's GL/draw wrappers can trace in
// detail during video playback without glDrawArrays_wrap's shared static
// counters (0,10,20,300...) getting eaten up hours earlier at boot before
// any video runs -- separate on/off signal instead of trying to reuse those.
int g_video_playing;

static void pak_io_init(void) {
  if (s_pak_inited) return;
  for (int i = 0; i < PAK_NFD; i++) s_pak[i].fd = -1;
  s_pak_inited = 1;
}

static PakFd *pak_find(int fd) {
  if (!s_pak_inited) return NULL;
  for (int i = 0; i < PAK_NFD; i++) if (s_pak[i].fd == fd) return &s_pak[i];
  return NULL;
}

static void pak_track(int fd, const char *path) {
  pak_io_init();
  struct stat st;
  off_t fsize = (fstat(fd, &st) == 0) ? st.st_size : 0;

  // OpenBOR's own openPackfile()-style fallback repeatedly opens+closes
  // bor.pak/Paks/*.pak directly for its own internal linear scanning --
  // confirmed on-device via "[pak] closing fd=N" immediately followed by
  // "read-ahead enabled for fd=N" again, over and over, for the entire run.
  // The file's bytes never change between opens, so reusing a still-loaded
  // slot for the same path (matched even while its previous fd is already
  // closed, i.e. fd<0) turns a same-content reopen into a free hit against
  // the buffer already resident in RAM instead of a fresh load; `pos` still
  // resets to 0 since a fresh open() always starts there.
  for (int i = 0; i < PAK_NFD; i++) {
    if (s_pak[i].fd < 0 && s_pak[i].buf && s_pak[i].size == fsize && !strcmp(s_pak[i].path, path)) {
      s_pak[i].fd = fd;
      s_pak[i].pos = 0;
      debugPrintf("[pak] full-file cache REUSED for fd=%d (\"%s\", %lld MB already resident)\n",
                  fd, path, (long long)(s_pak[i].size >> 20));
      return;
    }
  }

  for (int i = 0; i < PAK_NFD; i++) {
    if (s_pak[i].fd >= 0) continue;
    uint64_t t0 = armGetSystemTick();
    uint8_t *buf = fsize > 0 ? memalign(0x1000, (size_t)fsize) : NULL;
    if (!buf) {
      // Fall through without tracking this fd at all: read_fake()/
      // lseek_fake() see pak_find() return NULL for it and transparently
      // pass every call straight to the real read()/lseek() instead, same
      // as any never-tracked fd. Slower (back to whatever the underlying
      // fs driver does per call) but always correct, and cheap insurance
      // against a ~395 MB single allocation ever failing on a loaded system.
      debugPrintf("[pak] full-file cache: memalign(%lld MB) failed for \"%s\", falling back to uncached\n",
                  (long long)(fsize >> 20), path);
      return;
    }
    size_t got_total = 0;
    while (got_total < (size_t)fsize) {
      size_t chunk = (size_t)fsize - got_total;
      if (chunk > 4 * 1024 * 1024) chunk = 4 * 1024 * 1024;
      ssize_t got = read(fd, buf + got_total, chunk);
      if (got <= 0) break;
      got_total += (size_t)got;
    }
    if (got_total != (size_t)fsize) {
      debugPrintf("[pak] full-file cache: short read for \"%s\" (%zu/%lld bytes), falling back to uncached\n",
                  path, got_total, (long long)fsize);
      free(buf);
      return;
    }
    lseek(fd, 0, SEEK_SET); // the real fd's own position is irrelevant from here on (read_fake/lseek_fake never touch it again), but leave it sane regardless
    s_pak[i].buf = buf;
    s_pak[i].size = fsize;
    s_pak[i].pos = 0;
    snprintf(s_pak[i].path, sizeof(s_pak[i].path), "%s", path);
    s_pak[i].fd = fd;
    g_pak_sd_bytes += got_total;
    g_pak_cold_loads++;
    double load_s = (double)armTicksToNs(armGetSystemTick() - t0) / 1e9;
    debugPrintf("[pak] full-file cache LOADED for fd=%d (\"%s\", %lld MB in %.3fs)\n",
                fd, path, (long long)(fsize >> 20), load_s);
    return;
  }
  debugPrintf("[pak] full-file cache: no free slot for \"%s\" (all %d in use), falling back to uncached\n",
              path, PAK_NFD);
}

// ---------------------------------------------------------------------------
// Virtual pak-backed files: OpenBOR's own pak resolution (openPackfile's
// linear catalog scan when a loose file is missing, or -- worse -- the
// pak_init()/FileCaching-enabled cached-header path it switches to instead
// once no loose "data" directory exists) turned out to be unreliable in this
// custom 4.0 build: with no loose data/ dir present it hangs indefinitely
// during/after "Initializing video" instead of ever reaching "Loading
// sprites", so `pak_init()` in this build cannot be trusted for real asset
// resolution. Since we've independently confirmed the exact pak catalog
// format (headerstart in the trailing 4 bytes, then a linear sequence of
// { pns_len, filestart, filesize, name } records, names stored with
// backslashes) by parsing bor.pak directly, we do the resolution ourselves:
// index every *.pak we can find once, then serve any asset lookup that
// misses as a real loose file by handing back a fake fd backed directly by
// the matching pak byte range. The .so never sees the difference -- from its
// point of view every asset is a loose file -- so its own (buggy) pak
// fallback and FileCaching path are never exercised at all for individual
// asset reads. `data/` itself is kept present but empty purely to keep
// `isRawData()`-style detection steering pak_init() away from the broken
// cached-header path at startup.
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t filestart, filesize;
  char name[96]; // normalized: lowercase, forward slashes
} PakCatEntry;

typedef struct {
  char path[64];
  PakCatEntry *entries;
  int count;
  int src_fd; // lazily-opened, kept open for the process's lifetime
} PakCatalog;

#define MAX_CATALOGS 8
static PakCatalog s_cat[MAX_CATALOGS];
static int s_cat_count;
static int s_cat_built;

static void vpak_norm(const char *in, char *out, size_t outsz) {
  size_t j = 0;
  if (in[0] == '.' && in[1] == '/') in += 2;
  for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
    char c = in[i];
    if (c == '\\') c = '/';
    else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    out[j++] = c;
  }
  out[j] = 0;
}

static int vpak_cmp(const void *a, const void *b) {
  return strcmp(((const PakCatEntry *)a)->name, ((const PakCatEntry *)b)->name);
}

static void vpak_catalog_load(const char *path) {
  if (s_cat_count >= MAX_CATALOGS) return;
  int fd = open(path, O_RDONLY | O_BINARY);
  if (fd < 0) return;
  off_t size = lseek(fd, 0, SEEK_END);
  uint32_t headerstart;
  if (size < 8 || lseek(fd, size - 4, SEEK_SET) != size - 4 || read(fd, &headerstart, 4) != 4 ||
      headerstart >= (uint32_t)size) {
    close(fd);
    return;
  }
  if (lseek(fd, headerstart, SEEK_SET) != (off_t)headerstart) { close(fd); return; }

  size_t cap = 4096, n = 0;
  PakCatEntry *arr = malloc(cap * sizeof(PakCatEntry));
  if (!arr) { close(fd); return; }

  off_t pos = headerstart;
  for (;;) {
    uint32_t rec[3];
    if (read(fd, rec, 12) != 12) break;
    uint32_t pns_len = rec[0], filestart = rec[1], filesize = rec[2];
    if (pns_len <= 12 || pns_len > 4096 || pos + (off_t)pns_len > size) break;
    size_t namelen = pns_len - 12;
    char namebuf[256];
    if (namelen >= sizeof(namebuf)) namelen = sizeof(namebuf) - 1;
    if (read(fd, namebuf, namelen) != (ssize_t)namelen) break;
    namebuf[namelen] = 0;
    if (n == cap) {
      cap *= 2;
      PakCatEntry *na = realloc(arr, cap * sizeof(PakCatEntry));
      if (!na) break;
      arr = na;
    }
    vpak_norm(namebuf, arr[n].name, sizeof(arr[n].name));
    arr[n].filestart = filestart;
    arr[n].filesize = filesize;
    n++;
    pos += pns_len;
    if (lseek(fd, pos, SEEK_SET) != pos) break;
  }
  close(fd);
  if (n == 0) { free(arr); return; }
  qsort(arr, n, sizeof(PakCatEntry), vpak_cmp);
  PakCatalog *c = &s_cat[s_cat_count++];
  snprintf(c->path, sizeof(c->path), "%s", path);
  c->entries = arr;
  c->count = (int)n;
  c->src_fd = -1;
  debugPrintf("[vpak] indexed %s: %d entries\n", path, (int)n);
}

static void vpak_catalogs_build_once(void) {
  if (s_cat_built) return;
  s_cat_built = 1;
  vpak_catalog_load("bor.pak");
  DIR *d = opendir("Paks");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      size_t l = strlen(e->d_name);
      if (l > 4 && e->d_name[l - 4] == '.' &&
          (e->d_name[l - 3] == 'p' || e->d_name[l - 3] == 'P') &&
          (e->d_name[l - 2] == 'a' || e->d_name[l - 2] == 'A') &&
          (e->d_name[l - 1] == 'k' || e->d_name[l - 1] == 'K')) {
        char full[128];
        snprintf(full, sizeof(full), "Paks/%s", e->d_name);
        vpak_catalog_load(full);
      }
    }
    closedir(d);
  }
  debugPrintf("[vpak] %d catalog(s) loaded\n", s_cat_count);
}

// A fake-fd approach (serving read()/lseek() ourselves against a made-up
// descriptor) turned out not to be trustworthy here: every test showed the
// .so's own openPackfile()-equivalent successfully opening and seeking our
// fake fd, then closing it again WITHOUT ever issuing a single read() --
// yet OpenBOR still fatals with "Can't read from packfile". That only makes
// sense if this custom build's readpackfile() goes through some internal
// function-pointer indirection that we can't see or control (plausibly one
// only wired up by the real pak_init(), which we deliberately keep
// short-circuited via isRawData() to dodge its OTHER, separately-confirmed-
// broken cached-header reader) -- so it can fail before ever touching any
// symbol we've shimmed. Rather than guess further at a closed binary's
// internals, sidestep all of it: materialize the real bytes onto the SD
// card at the exact path being requested and let the real open()/fopen()
// succeed. From that point on every seek/read/stdio call is against a
// genuine kernel file -- nothing about this build's pak-handling internals
// can matter anymore. One-time cost per asset ever touched; persists across
// runs (no re-extraction on a later boot).
// Two SD-card-specific costs dominate materializing thousands of small
// files: directory-entry creation/lookup (FAT-style media is slow at
// this specifically, independent of actual data volume) and reopening the
// ~380 MB source pak from scratch per asset. Both are easy to avoid: cache
// which directories are already known to exist so repeat mkdir() calls for
// the same folder (every file in "data/chars/heroes/axel/sor3/" after the
// first) are skipped entirely, and keep one persistently-open fd per
// catalog's underlying pak file instead of open()/close() per asset.
#define MKDIR_CACHE_MAX 512
static char s_mkdir_cache[MKDIR_CACHE_MAX][300];
static int s_mkdir_cache_n;

static int vpak_dir_known(const char *dir) {
  for (int i = 0; i < s_mkdir_cache_n; i++)
    if (strcmp(s_mkdir_cache[i], dir) == 0) return 1;
  return 0;
}

static void vpak_mkdirs_for(const char *path) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (!vpak_dir_known(buf)) {
      mkdir(buf, 0777);
      if (s_mkdir_cache_n < MKDIR_CACHE_MAX)
        snprintf(s_mkdir_cache[s_mkdir_cache_n++], sizeof(s_mkdir_cache[0]), "%s", buf);
      dircache_invalidate_parent_of(buf); // a new subdir just appeared in buf's parent
    }
    *p = '/';
  }
}

static int vpak_source_fd(PakCatalog *c) {
  if (c->src_fd < 0) c->src_fd = open(c->path, O_RDONLY | O_BINARY);
  return c->src_fd;
}

// Forward-declared: defined below with the rest of vpak_materialize()'s
// helpers, but vpak_open_virtual() (which precedes them in the file so it
// can sit next to pak_resident_buf(), its other prerequisite) needs it too.
static int ends_with_ci(const char *s, const char *suffix);

// True if `path`'s catalog pak (e.g. "bor.pak") is currently fully
// resident in PakFd's own full-file cache (s_pak[] above), returning a
// pointer straight at its buffer. Works whether that pak's own real fd is
// open or closed right now -- pak_track()'s reuse design keeps buf
// resident either way -- since this never touches .fd at all.
static const uint8_t *pak_resident_buf(const char *path, off_t *out_size) {
  for (int i = 0; i < PAK_NFD; i++) {
    if (s_pak[i].buf && !strcmp(s_pak[i].path, path)) {
      if (out_size) *out_size = s_pak[i].size;
      return s_pak[i].buf;
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Virtual asset windows: bor.pak is already fully resident in RAM (PakFd's
// full-file cache) and its catalog (built once in
// vpak_catalogs_build_once(), a couple hundred KB parsed up front) already
// knows the exact byte range of every asset inside it -- the SAME
// information vpak_materialize() below has always used to physically copy
// each asset out to a real loose file on the SD card the first time
// OpenBOR ever asks for it. That copy (a full read+write+rename, plus
// mkdir/dircache bookkeeping the first time any file appears in a given
// directory) is a real, one-time cost per asset, and every open() of the
// materialized copy afterward -- including on every boot after the first,
// since materialized files persist across runs -- was, until now, plain
// uncached real SD-card I/O (the gap a since-disabled small-file cache
// attempt tried and failed to close safely). Serving the asset directly out
// of the byte range already sitting in bor.pak's resident buffer skips
// both costs entirely: no SD write ever, for any asset this pak's catalog
// covers, on any boot.
//
// A prior attempt at something in this spirit -- a made-up file descriptor
// with no real open() behind it at all -- is documented (see
// vpak_materialize()'s comment below) as the reason the loose-file copy
// exists in the first place: this build's openPackfile()-equivalent
// internals were confirmed on real hardware to do *something* with the
// descriptor (never identified precisely) that only works if it's a real,
// kernel-issued one. This is a different design specifically to avoid that
// failure mode: every virtual window is backed by an actual open() against
// bor.pak itself, so OpenBOR sees a completely ordinary, real fd -- only
// read()/lseek()/close() on it are ever intercepted, the exact same
// technique pak_track()'s own full-file cache already uses successfully
// for the whole pak, just windowed to one entry's byte range instead of
// the whole file.
//
// Deliberately its own table, not an extension of PakFd: entries here
// never own the memory they point into (buf is a plain offset into
// whichever PakFd slot's buffer is still resident, freed only when THAT
// slot is, never by anything here), so this must never be confused with
// pak_track()'s reuse/eviction semantics.
//
// A real, dedicated open() fd whose window is smaller than the underlying
// pak file raises an obvious question: does anything ever fstat() one of
// these to size a read buffer, and get bor.pak's real ~395MB back instead
// of the asset's actual size? Checked directly rather than assumed: `readelf
// --dyn-syms` on all three loaded .so's (libopenbor.so, libSDL2.so,
// libhidapi.so) shows zero undefined references to stat/fstat/lstat/access
// (confirmed the UND-symbol filter itself isn't just empty by accident --
// libopenbor.so alone has 188 other undefined FUNC imports). None of them
// are ever wired into the shim table for the same reason: nothing calls
// them. BOR's own file-sizing idiom is the portable fseek(SEEK_END)+ftell()
// one instead, which already goes through lseek_fake() above and has
// always computed VirtWinFd sizes correctly. No stat/fstat shim needed.
#define VIRTWIN_MAX 256
typedef struct {
  int fd;              // -1 = free slot; else a real, dedicated open() fd
  off_t pos;
  off_t size;
  const uint8_t *buf;  // points INTO another slot's resident buffer; never freed here
} VirtWinFd;

static VirtWinFd s_virtwin[VIRTWIN_MAX];
static Mutex s_virtwin_lock;
static uint64_t g_virtwin_opens;
static int s_virtwin_inited;

// Zero-initialized statics mean every slot's `fd` starts at 0, not -1 --
// indistinguishable from "slot 0 is in use by real fd 0" under the `fd < 0`
// free-slot test everything else here uses. Without this, the very first
// vpak_open_virtual() call would find zero free slots (every one of them
// looks "in use") and this whole mechanism would silently never engage,
// always falling back to vpak_materialize() -- same class of bug as
// pak_io_init() above exists to avoid for s_pak[].
static void virtwin_init(void) {
  if (s_virtwin_inited) return;
  for (int i = 0; i < VIRTWIN_MAX; i++) s_virtwin[i].fd = -1;
  s_virtwin_inited = 1;
}

static VirtWinFd *virtwin_find(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < VIRTWIN_MAX; i++)
    if (s_virtwin[i].fd == fd) return &s_virtwin[i];
  return NULL;
}

// Returns a newly open()'d, real fd on success (already registered and
// ready for read_fake()/lseek_fake()/close_fake() to serve from `buf`), or
// -1 for anything short of that -- not in any catalog, its pak isn't
// resident, the dedicated open() failed, or the table's full. open_fake()
// treats -1 here as "fall back to vpak_materialize()" unconditionally, so
// any surprise on real hardware degrades to the already-proven path
// instead of a hard failure.
static int vpak_open_virtual(const char *path) {
  virtwin_init();
  vpak_catalogs_build_once();
  PakCatEntry key;
  vpak_norm(path, key.name, sizeof(key.name));
  PakCatalog *c = NULL;
  PakCatEntry *hit = NULL;
  for (int ci = 0; ci < s_cat_count; ci++) {
    hit = bsearch(&key, s_cat[ci].entries, s_cat[ci].count, sizeof(PakCatEntry), vpak_cmp);
    if (hit) { c = &s_cat[ci]; break; }
  }
  if (!hit) return -1; // vpak_materialize()'s own bsearch will log "not found" on fallback

  off_t resident_size = 0;
  const uint8_t *resident = pak_resident_buf(c->path, &resident_size);
  if (!resident || (off_t)hit->filestart + (off_t)hit->filesize > resident_size) return -1;

  // open_fake() only ever reaches vpak_open_virtual() for a .webm path once
  // video_override_real() (see the PakAlias-style table above) has already
  // been tried and found nothing on the SD card -- so this is the fallback
  // for a missing override, same stub policy as vpak_materialize()'s own: an
  // "empty" window (size 0) makes playwebm() fail its container-header check
  // immediately, same as an empty materialized file would, without decoding
  // anything.
  int is_stub = ends_with_ci(path, ".webm");
  off_t winsize = is_stub ? 0 : (off_t)hit->filesize;
  const uint8_t *winbuf = is_stub ? NULL : resident + hit->filestart;

  int fd = open(c->path, O_RDONLY | O_BINARY);
  if (fd < 0) return -1;

  mutexLock(&s_virtwin_lock);
  int slot = -1;
  for (int i = 0; i < VIRTWIN_MAX; i++) {
    if (s_virtwin[i].fd < 0) { slot = i; break; }
  }
  if (slot < 0) {
    mutexUnlock(&s_virtwin_lock);
    close(fd);
    debugPrintf("[vpak] virtual window table full (%d in use), falling back to materialize for \"%s\"\n",
                VIRTWIN_MAX, path);
    return -1;
  }
  s_virtwin[slot].fd = fd;
  s_virtwin[slot].pos = 0;
  s_virtwin[slot].size = winsize;
  s_virtwin[slot].buf = winbuf;
  g_virtwin_opens++;
  mutexUnlock(&s_virtwin_lock);

  debugPrintf("[vpak] \"%s\": virtual window into %s @ %u (%lld bytes)%s\n",
              path, c->path, hit->filestart, (long long)winsize, is_stub ? " (webm, stubbed empty)" : "");
  return fd;
}

// Every *.webm in the pak (intro.webm, blank/logo_*/start_* -- 9 total,
// confirmed by dumping the catalog) is intro/splash/start-screen content,
// none of it gameplay. Software-decoding intro.webm on-device (no hardware
// codec path here) was measured stalling rendering for minutes -- it alone
// is 35 MB, by far the largest of the nine. As of the video_override_real()
// table above, this unconditional stub is only reached when no low-res
// replacement is present on the SD card; with one present, open_fake()
// returns straight from the override open() and never gets here at all.
static int ends_with_ci(const char *s, const char *suffix) {
  size_t ls = strlen(s), lsuf = strlen(suffix);
  if (lsuf > ls) return 0;
  for (size_t i = 0; i < lsuf; i++) {
    char a = s[ls - lsuf + i], b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return 1;
}

// Materializing an empty stub instead of the real bytes makes playwebm()
// fail its own container-header check immediately (same "an error
// occurred" path it already takes for a genuinely missing/corrupt file)
// and move on, instead of spending real time decoding video.
//
// Tried making this size-conditional -- stub only intro.webm (35 MB,
// measured stalling rendering for minutes) and the repeatedly-replayed
// start_*.webm attract clips, letting the four small boot logos (blank.webm
// at 4.9 KB up to logo_openbor.webm at 1.6 MB) decode for real, on the
// theory that small file size meant a quick, safe decode and that actually
// playing them through would give in_titlescreen's one-time setup
// (loadCfg()/defaultCfg(), which the all-stubbed boot sequence blew past in
// well under half a second -- too fast for it to run even once, leaving
// musicStyle and other globals uninitialized and crashing the very first
// "play main menu music" call) the several real frames it needs. Confirmed
// on-device this assumption was wrong: logo_sega.webm decoded on-device as
// "Video track: resolution=1920*1080 ... 29.97 frames/second" -- a small
// file size says nothing about frame resolution, and software-decoding
// 1080p video with no hardware codec path hung the process. Back to
// stubbing all nine unconditionally; musicStyle's actual fix is a bundled
// default Saves/SORX.set (see check_data() in main.c) so loadCfg() has real
// values to load on a first run regardless of how fast the boot sequence
// completes, instead of depending on decode timing at all.

static int vpak_materialize(const char *path) {
  vpak_catalogs_build_once();
  PakCatEntry key;
  vpak_norm(path, key.name, sizeof(key.name));
  PakCatalog *c = NULL;
  PakCatEntry *hit = NULL;
  for (int ci = 0; ci < s_cat_count; ci++) {
    hit = bsearch(&key, s_cat[ci].entries, s_cat[ci].count, sizeof(PakCatEntry), vpak_cmp);
    if (hit) { c = &s_cat[ci]; break; }
  }
  if (!hit) {
    // Unconditional (not gated behind VERBOSE_IO): open_fake() only reaches
    // here after a real open() already failed, so this is never the hot
    // path a busy asset-extraction pass runs through -- it fires at most
    // once per genuinely-missing path, and telling "genuinely not in the
    // pak" apart from "found a stale/corrupt loose file so we never even
    // got called" has already cost real debugging time once.
    debugPrintf("[vpak] \"%s\" not found in any of %d catalog(s)\n", path, s_cat_count);
    return -1;
  }

  // Write to a sibling temp name and rename() into place only once the
  // copy is fully verified -- any of the many ways a run can end early
  // (crash, hang needing a hard reset, power-cycling the console) previously
  // left a truncated file sitting at the FINAL path, which then looked
  // exactly like a legitimately materialized asset forever after: no
  // "[vpak] materialized" line and no "not found" either, because
  // open_fake()'s real open() just succeeds on the corrupt leftover and
  // vpak_materialize() never runs again for that path. (Root cause of an
  // on-device "Unable to load file ...truck05.png" where the pak's own
  // catalog confirms that exact entry is present and fine.) rename() on a
  // file already fully written and closed is effectively atomic even on
  // FAT/exFAT for this same-directory case, so a crash mid-copy now only
  // ever leaves an orphaned ".vpaktmp" file, never a corrupt one at the
  // path OpenBOR will actually open.
  vpak_mkdirs_for(path);
  char tmp[320];
  snprintf(tmp, sizeof(tmp), "%s.vpaktmp", path);
  int dst = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
  if (dst < 0) { debugPrintf("[vpak] materialize(\"%s\"): create failed\n", path); return -1; }

  if (ends_with_ci(path, ".webm")) {
    close(dst);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    dircache_invalidate_parent_of(path); // a new file just appeared in path's parent
    debugPrintf("[vpak] \"%s\": intro/splash video, stubbed empty instead of decoding %u bytes\n",
                path, hit->filesize);
    return 0;
  }

  int src = vpak_source_fd(c);
  if (src < 0) { close(dst); unlink(tmp); return -1; }
  if (lseek(src, hit->filestart, SEEK_SET) != (off_t)hit->filestart) { close(dst); unlink(tmp); return -1; }

  static uint8_t buf[64 * 1024];
  uint32_t remaining = hit->filesize;
  int ok = 1;
  while (remaining > 0) {
    size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    ssize_t got = read(src, buf, chunk);
    if (got <= 0) { ok = 0; break; }
    ssize_t wrote = write(dst, buf, (size_t)got);
    if (wrote != got) { ok = 0; break; }
    remaining -= (uint32_t)got;
  }
  close(dst);
  if (!ok) { unlink(tmp); debugPrintf("[vpak] materialize(\"%s\"): copy failed\n", path); return -1; }
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    debugPrintf("[vpak] materialize(\"%s\"): rename failed\n", path);
    return -1;
  }
  dircache_invalidate_parent_of(path); // a new file just appeared in path's parent
  debugPrintf("[vpak] materialized \"%s\" from %s @ %u (%u bytes)\n", path, c->path, hit->filestart, hit->filesize);
  return 0;
}

// Explicit, eager, whole-pak extraction: on-demand materialization (every
// open_fake() call above) never guarantees 100% coverage from a genuinely
// empty data/ -- vpak_open_virtual() above serves plenty of assets straight
// out of RAM without ever touching disk, and whatever OpenBOR's own code
// reads before the game's first real gameplay frame (confirmed: settings
// files like data/video.txt) only gets ONE chance at open_fake() with no
// retry if that specific asset wasn't ready yet. Walking every catalog
// entry once up front and materializing whatever's missing removes that
// timing dependency entirely -- after this returns, every single asset the
// pak has is a real loose file on disk, exactly matching what OpenBOR's own
// unmodified Android build does at first launch. Cheap on every boot after
// the first: stat() beats a real copy by orders of magnitude, so a fully-
// extracted data/ turns this into a few thousand quick existence checks,
// not a repeat of the actual ~377MB copy.
void vpak_extract_all(VpakExtractProgressFn progress) {
  vpak_catalogs_build_once();
  uint32_t total = 0;
  for (int ci = 0; ci < s_cat_count; ci++) total += (uint32_t)s_cat[ci].count;
  uint32_t done = 0;
  for (int ci = 0; ci < s_cat_count; ci++) {
    PakCatalog *c = &s_cat[ci];
    for (int i = 0; i < c->count; i++) {
      PakCatEntry *e = &c->entries[i];
      struct stat st;
      int need = 1;
      if (stat(e->name, &st) == 0) {
        // A stubbed webm is deliberately 0 bytes on disk (see
        // vpak_materialize()'s own webm branch above) -- that IS the
        // correct, fully-materialized state for one, not a sign it needs
        // re-copying.
        int is_stub = ends_with_ci(e->name, ".webm");
        if (is_stub ? st.st_size == 0 : (uint32_t)st.st_size == e->filesize) need = 0;
      }
      if (need) vpak_materialize(e->name);
      done++;
      if (progress) progress(done, total, e->name);
    }
  }
}

// ---------------------------------------------------------------------------
// Small-file cache: bor.pak's own full-file cache (PakFd above) only ever
// covers *.pak files. Every individual asset vpak_materialize() extracts
// from it -- sprites, sounds, scripts, potentially thousands over a session,
// and persisted as real loose files across runs so they only ever get
// materialized once ever, not once per boot -- is read back afterward
// through completely uncached, real SD-card I/O, on every boot from the
// second one on. Those assets are provably static once materialized
// (written once, atomically via the tmp+rename dance above; nothing in this
// game rewrites data/ content afterward -- only Saves/-style state changes,
// and that path never goes through materialize() or lives under data/), so
// caching their whole contents in RAM the same way as the pak itself is
// safe: unlike the pak, there's no "reopen finds different bytes" risk to
// guard against here.
//
// Deliberately a fully separate table from PAK_NFD/s_pak, not a bigger
// version of it: an eviction under memory/slot pressure here must never be
// able to reach bor.pak's own resident 395 MB slot. Bounded two ways --
// SMALLCACHE_BUDGET caps total resident bytes, SMALLCACHE_MAX_FILE excludes
// anything unexpectedly large (falls back to plain uncached I/O, exactly
// like a file this table never learned about) -- with LRU eviction of
// closed entries once either limit is hit.
// Kill switch, same pattern as imports.c's TEX_POOLING_ENABLED: on-device,
// the very first file this cache ever tracked (data/sprites/shadow1.png,
// confirmed byte-for-byte intact both inside bor.pak and pre-existing on
// the SD card) failed to decode ("incomplete compressed stream")
// immediately after being cached -- 100% reproducibly, same file, same
// size, every run. Adding a mutex around every access didn't change the
// outcome, which argues against a data race (races are rarely this
// deterministic) and for either a plain logic bug in this new code or an
// interaction with however OpenBOR actually reads a just-opened fd (e.g.
// if it mmap()s rather than read()s, something this cache's own internal
// populate-then-lseek(0) dance was never tested against). Flip to 0 to
// confirm/rule this subsystem out entirely while root-causing it -- with
// it off, every open() this would have handled just falls through to
// real, uncached I/O, identical to before this cache existed.
#define SMALLCACHE_ENABLED 0

#define SMALLCACHE_MAX 512
#define SMALLCACHE_BUDGET (64 * 1024 * 1024)
#define SMALLCACHE_MAX_FILE (8 * 1024 * 1024)

typedef struct {
  int fd;             // -1 = not currently open (buffer may still be resident)
  off_t pos;
  off_t size;
  uint8_t *buf;       // NULL until first successfully cached
  uint64_t last_used; // armGetSystemTick() at last access, for LRU eviction
  char path[192];
} SmallCacheEntry;

static SmallCacheEntry s_smallcache[SMALLCACHE_MAX];
static int s_smallcache_n;          // slots ever used so far (grows up to SMALLCACHE_MAX)
#if SMALLCACHE_ENABLED
static uint64_t s_smallcache_bytes; // sum of currently-resident buf sizes
#endif
static uint64_t g_smallcache_reads, g_smallcache_bytes_served;
// Confirmed on-device necessary, not defensive-programming boilerplate:
// main.c's own thread-priority comment notes OpenBOR spawns worker threads
// of its own ("this thread runs OpenBOR's whole game loop and everything
// it spawns"), and the very first asset this cache ever tracked
// (data/sprites/shadow1.png, a tiny 151-byte file confirmed byte-for-byte
// intact both inside bor.pak and as already-materialized on the SD card)
// immediately failed to decode ("incomplete compressed stream") right after
// being cached. Two threads racing this array with no lock at all --
// concurrently choosing the same free slot, or one reading a buffer mid-
// (re)assignment by the other -- reproduces exactly that signature: a
// correctly-sized entry with corrupted content, for a file that was never
// actually wrong on disk. bor.pak's own PakFd table above has run this
// whole session without a lock and never shown a symptom like this, so
// it's left alone rather than risk introducing a deadlock into
// long-proven code for an unconfirmed problem -- this fixes the newly
// added, confirmed-broken path specifically.
static Mutex s_smallcache_lock;

// True if `path` (tolerating a "./" prefix, or any absolute prefix before
// it -- e.g. "/switch/sorx_nx/data/...") has "data" as one of its path
// components. That directory is exclusively populated by pak-derived
// content (it's the same "data/" check_data() creates so OpenBOR's own
// isRawData() steers away from its broken cached-header pak reader), so
// this is a safe, simple stand-in for "is this file static game data" that
// doesn't depend on which specific code path happened to open it (freshly
// materialized just now, or already sitting on the SD card from an earlier
// run -- both need to hit this cache for it to help on anything but a
// first-ever boot).
static int path_is_static_asset(const char *path) {
  const char *p = path;
  if (p[0] == '.' && p[1] == '/') p += 2;
  const char *d = strstr(p, "data/");
  return d && (d == p || d[-1] == '/');
}

static SmallCacheEntry *smallcache_find(int fd) {
  for (int i = 0; i < s_smallcache_n; i++)
    if (s_smallcache[i].fd == fd) return &s_smallcache[i];
  return NULL;
}

#if SMALLCACHE_ENABLED
static void smallcache_evict_slot(int idx) {
  SmallCacheEntry *e = &s_smallcache[idx];
  free(e->buf);
  s_smallcache_bytes -= (uint64_t)e->size;
  e->buf = NULL;
  e->size = 0;
  e->path[0] = 0;
}
#endif

static void smallcache_track(int fd, const char *path) {
#if !SMALLCACHE_ENABLED
  (void)fd; (void)path;
  return;
#else
  struct stat st;
  off_t fsize = (fstat(fd, &st) == 0) ? st.st_size : -1;
  if (fsize <= 0 || fsize > SMALLCACHE_MAX_FILE) return;

  mutexLock(&s_smallcache_lock);

  for (int i = 0; i < s_smallcache_n; i++) {
    if (s_smallcache[i].fd < 0 && s_smallcache[i].buf && s_smallcache[i].size == fsize &&
        !strcmp(s_smallcache[i].path, path)) {
      s_smallcache[i].fd = fd;
      s_smallcache[i].pos = 0;
      s_smallcache[i].last_used = armGetSystemTick();
#if VERBOSE_IO
      debugPrintf("[smallcache] REUSED \"%s\" (%lld bytes)\n", path, (long long)fsize);
#endif
      mutexUnlock(&s_smallcache_lock);
      return;
    }
  }

  int slot = -1;
  for (int i = 0; i < s_smallcache_n; i++) {
    if (s_smallcache[i].fd < 0 && !s_smallcache[i].buf) { slot = i; break; }
  }
  if (slot < 0 && s_smallcache_n < SMALLCACHE_MAX) slot = s_smallcache_n++;
  if (slot < 0 || s_smallcache_bytes + (uint64_t)fsize > SMALLCACHE_BUDGET) {
    int victim = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < s_smallcache_n; i++) {
      if (s_smallcache[i].fd >= 0 || !s_smallcache[i].buf) continue;
      if (s_smallcache[i].last_used < oldest) { oldest = s_smallcache[i].last_used; victim = i; }
    }
    if (victim >= 0) {
      smallcache_evict_slot(victim);
      if (slot < 0) slot = victim;
    }
  }
  if (slot < 0) { mutexUnlock(&s_smallcache_lock); return; } // every slot genuinely in use right now: skip caching this one, real I/O still works

  // Reserve the slot (fd set to a sentinel so no other thread's reuse/evict
  // scan can touch it) before releasing the lock for the actual file read
  // below -- memalign+read of up to SMALLCACHE_MAX_FILE bytes is real work
  // worth not serializing every other thread's cache lookups behind, but
  // the slot's bookkeeping fields themselves are only ever safe to touch
  // under the lock.
  s_smallcache[slot].fd = INT32_MAX; // reservation sentinel: >=0 so no scan mistakes it for free, but no real fd is ever this value
  s_smallcache[slot].buf = NULL;
  mutexUnlock(&s_smallcache_lock);

  uint8_t *buf = memalign(0x1000, (size_t)fsize);
  size_t got_total = 0;
  if (buf) {
    while (got_total < (size_t)fsize) {
      ssize_t got = read(fd, buf + got_total, (size_t)fsize - got_total);
      if (got <= 0) break;
      got_total += (size_t)got;
    }
  }

  mutexLock(&s_smallcache_lock);
  if (!buf || got_total != (size_t)fsize) {
    free(buf);
    s_smallcache[slot].fd = -1; // release the reservation; stays uncached
    mutexUnlock(&s_smallcache_lock);
    return;
  }
  lseek(fd, 0, SEEK_SET);

  s_smallcache[slot].buf = buf;
  s_smallcache[slot].size = fsize;
  s_smallcache[slot].pos = 0;
  s_smallcache[slot].fd = fd;
  s_smallcache[slot].last_used = armGetSystemTick();
  snprintf(s_smallcache[slot].path, sizeof(s_smallcache[slot].path), "%s", path);
  s_smallcache_bytes += (uint64_t)fsize;
  uint64_t bytes_now = s_smallcache_bytes;
  mutexUnlock(&s_smallcache_lock);
  debugPrintf("[smallcache] cached \"%s\" (%lld bytes, %llu/%lluMB budget used, slot %d/%d)\n",
              path, (long long)fsize, (unsigned long long)(bytes_now >> 20),
              (unsigned long long)(SMALLCACHE_BUDGET >> 20), slot + 1, SMALLCACHE_MAX);
#endif
}

int open_fake(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
  }

  // Checked before the plain open() below, not after it fails like every
  // other fallback in this function: vpak_materialize()'s webm stub policy
  // (see its own comment further up) has, across many earlier builds/test
  // runs, physically written real files at these exact "data/videos/*.webm"
  // paths on the SD card -- some empty stubs, and at least one round with
  // real, full 1080p content from a since-reverted experiment. Any of those
  // leftovers makes the plain open() below succeed immediately, silently
  // shadowing the override forever (confirmed on-device: zero "[video]"
  // trace lines the whole run, yet real -- still 1080p -- decode activity
  // and the same crash as before this override system existed). Checking
  // the override first means it always wins regardless of SD-card history,
  // exactly like a fresh card with no leftovers at all would behave.
  if ((flags & O_ACCMODE) == O_RDONLY && ends_with_ci(path, ".webm") && !path_is_pak(path)) {
    const char *override_path = video_override_real(path);
    if (override_path) {
      int ofd = open(override_path, flags, mode);
      if (ofd >= 0) {
        struct stat st;
        s_video_trace_fd = ofd;
        snprintf(s_video_trace_path, sizeof(s_video_trace_path), "%s", override_path);
        s_video_trace_reads = 0;
        s_video_trace_bytes = 0;
        s_video_trace_size = (fstat(ofd, &st) == 0) ? st.st_size : -1;
        g_video_playing = 1;
        debugPrintf("[video] \"%s\": playing low-res override \"%s\" (fd=%d, %lld bytes)\n",
                    path, override_path, ofd, (long long)s_video_trace_size);
        return ofd;
      }
      debugPrintf("[video] \"%s\": override \"%s\" not found, falling back to normal resolution\n",
                  path, override_path);
    }
  }

  int fd = open(path, flags, mode);
#if VERBOSE_IO
  debugPrintf("open(\"%s\", 0x%x) -> %d\n", path, flags, fd);
#endif
  if (fd >= 0) {
    if ((flags & O_ACCMODE) == O_RDONLY && path_is_pak(path)) {
      mutexLock(&s_pak_lock);
      pak_track(fd, path);
      mutexUnlock(&s_pak_lock);
    }
    else if ((flags & O_ACCMODE) == O_RDONLY && path_is_static_asset(path))
      // Covers BOTH the "just materialized it this call" case below AND
      // the far more common one on any boot after the first: the asset
      // already exists as a loose file from an earlier run and this
      // branch -- a plain, immediately-successful open() -- is the only
      // one that ever runs for it.
      smallcache_track(fd, path);
    else if (flags & O_CREAT)
      dircache_invalidate_parent_of(path);
    return fd;
  }
  if ((flags & O_ACCMODE) == O_RDONLY && !path_is_pak(path)) {
    fd = vpak_open_virtual(path);
    if (fd < 0 && vpak_materialize(path) == 0) {
#if VERBOSE_IO
      debugPrintf("open(\"%s\"): real open failed, retrying after materialize\n", path);
#endif
      fd = open(path, flags, mode);
#if VERBOSE_IO
      debugPrintf("open(\"%s\") -> %d (materialized from pak)\n", path, fd);
#endif
      if (fd >= 0 && path_is_static_asset(path)) smallcache_track(fd, path);
    }
  } else if ((flags & O_ACCMODE) == O_RDONLY && path_is_pak(path)) {
    const char *real = pak_alias_real(path);
    if (real) {
      fd = open(real, flags, mode);
      debugPrintf("open(\"%s\"): real open failed, retrying aliased to \"%s\" -> %d\n", path, real, fd);
      if (fd >= 0) {
        mutexLock(&s_pak_lock);
        pak_track(fd, real);
        mutexUnlock(&s_pak_lock);
      }
    }
  }
  return fd;
}

static void pak_log_progress(void) {
#if VERBOSE_IO
  static uint64_t next = 64 * 1024 * 1024;
  if (g_pak_bytes >= next) {
    next = g_pak_bytes + 64 * 1024 * 1024;
    debugPrintf("[pak] served=%lluMB (from RAM) sd_read=%lluMB (one-time cold loads) reads=%llu cold_loads=%llu\n",
                (unsigned long long)(g_pak_bytes >> 20), (unsigned long long)(g_pak_sd_bytes >> 20),
                (unsigned long long)g_pak_reads, (unsigned long long)g_pak_cold_loads);
  }
#endif
}

ssize_t read_fake(int fd, void *dst, size_t count) {
  // Unconditional entry trace for the active video fd, regardless of which
  // branch below ends up serving it: the per-branch tracing added earlier
  // only counted the final real-read() fallback, and showed 0 calls for a
  // video that visibly decoded and rendered real frames -- either reads
  // aren't how it gets bytes at all, or (more likely, given a mystery
  // position jump already seen on this exact fd) one of the OTHER branches
  // (PakFd/VirtWinFd/SmallCacheEntry) is quietly matching this fd number and
  // serving it from the WRONG buffer without ever reaching the code that
  // was counting. This settles which, instead of guessing again.
  if (fd == s_video_trace_fd)
    debugPrintf("[video-io] fd=%d read_fake ENTRY count=%zu\n", fd, count);
  mutexLock(&s_pak_lock);
  PakFd *p = pak_find(fd);
  if (p) {
    if (fd == s_video_trace_fd)
      debugPrintf("[video-io] fd=%d read_fake MATCHED PakFd (pos=%lld size=%lld) -- WRONG BUFFER\n",
                  fd, (long long)p->pos, (long long)p->size);
    // No window/refill logic at all: the whole file is already resident in
    // p->buf (loaded once in pak_track()), so every call is just a bounds
    // check and a memcpy -- the fix for OpenBOR's own packfile reader
    // issuing millions of tiny reads per pak isn't making each one cheaper
    // to refill, it's making there be nothing left to refill.
    size_t avail = (p->pos < p->size) ? (size_t)(p->size - p->pos) : 0;
    size_t n = count < avail ? count : avail;
    if (n > 0) memcpy(dst, p->buf + p->pos, n);
    p->pos += (off_t)n;
    g_pak_reads++; g_pak_bytes += n;
    pak_log_progress();
    mutexUnlock(&s_pak_lock);
    return (ssize_t)n;
  }
  mutexUnlock(&s_pak_lock);
  mutexLock(&s_virtwin_lock);
  VirtWinFd *vw = virtwin_find(fd);
  if (vw) {
    if (fd == s_video_trace_fd)
      debugPrintf("[video-io] fd=%d read_fake MATCHED VirtWinFd (pos=%lld size=%lld) -- WRONG BUFFER\n",
                  fd, (long long)vw->pos, (long long)vw->size);
    // Same bounds-check-then-memcpy as PakFd's read above, just against a
    // borrowed sub-range of another slot's buffer instead of one this
    // struct owns outright -- a stubbed webm entry (buf==NULL, size==0)
    // naturally falls out of this exact same path as an immediate,
    // correct EOF (avail computes to 0), no separate branch needed for it.
    size_t avail = (vw->pos < vw->size) ? (size_t)(vw->size - vw->pos) : 0;
    size_t n = count < avail ? count : avail;
    if (n > 0) memcpy(dst, vw->buf + vw->pos, n);
    vw->pos += (off_t)n;
    mutexUnlock(&s_virtwin_lock);
    return (ssize_t)n;
  }
  mutexUnlock(&s_virtwin_lock);
  mutexLock(&s_smallcache_lock);
  SmallCacheEntry *sc = smallcache_find(fd);
  if (sc) {
    if (fd == s_video_trace_fd)
      debugPrintf("[video-io] fd=%d read_fake MATCHED SmallCacheEntry (pos=%lld size=%lld) -- WRONG BUFFER\n",
                  fd, (long long)sc->pos, (long long)sc->size);
    size_t avail = (sc->pos < sc->size) ? (size_t)(sc->size - sc->pos) : 0;
    size_t n = count < avail ? count : avail;
    if (n > 0) memcpy(dst, sc->buf + sc->pos, n);
    sc->pos += (off_t)n;
    sc->last_used = armGetSystemTick();
    g_smallcache_reads++; g_smallcache_bytes_served += n;
    mutexUnlock(&s_smallcache_lock);
    return (ssize_t)n;
  }
  mutexUnlock(&s_smallcache_lock);
  ssize_t r = read(fd, dst, count);
  if (fd == s_video_trace_fd) {
    s_video_trace_reads++;
    if (r > 0) s_video_trace_bytes += (uint64_t)r;
    debugPrintf("[video-io] fd=%d read(count=%zu) -> %zd (call #%llu, %llu/%lld bytes so far)\n",
                fd, count, r, (unsigned long long)s_video_trace_reads,
                (unsigned long long)s_video_trace_bytes, (long long)s_video_trace_size);
  }
#if VERBOSE_IO
  if (count <= 4096) debugPrintf("read(fd=%d, count=%zu) -> %zd\n", fd, count, r);
#endif
  return r;
}

off_t lseek_fake(int fd, off_t off, int whence) {
  // Same gap read_fake() just got closed for: only the final real-lseek()
  // fallback was ever traced for the active video fd, so an earlier branch
  // silently matching it (most likely PakFd, given bor.pak's own fd churns
  // through the exact same low numbers this fd gets recycled from) would
  // never show up. Trace entry and every branch here too.
  if (fd == s_video_trace_fd)
    debugPrintf("[video-io] fd=%d lseek_fake ENTRY off=%lld whence=%d\n", fd, (long long)off, whence);
  mutexLock(&s_pak_lock);
  PakFd *p = pak_find(fd);
  if (p) {
    off_t np = (whence == SEEK_SET) ? off
             : (whence == SEEK_CUR) ? p->pos + off
                                    : p->size + off;
    if (np < 0) np = 0;
    if (fd == s_video_trace_fd)
      debugPrintf("[video-io] fd=%d lseek_fake MATCHED PakFd (pos %lld -> %lld) -- WRONG BUFFER\n",
                  fd, (long long)p->pos, (long long)np);
    p->pos = np;
    mutexUnlock(&s_pak_lock);
    return np;
  }
  mutexUnlock(&s_pak_lock);
  mutexLock(&s_virtwin_lock);
  VirtWinFd *vw = virtwin_find(fd);
  if (vw) {
    off_t np = (whence == SEEK_SET) ? off
             : (whence == SEEK_CUR) ? vw->pos + off
                                    : vw->size + off;
    if (np < 0) np = 0;
    if (fd == s_video_trace_fd)
      debugPrintf("[video-io] fd=%d lseek_fake MATCHED VirtWinFd (pos %lld -> %lld) -- WRONG BUFFER\n",
                  fd, (long long)vw->pos, (long long)np);
    vw->pos = np;
    mutexUnlock(&s_virtwin_lock);
    return np;
  }
  mutexUnlock(&s_virtwin_lock);
  mutexLock(&s_smallcache_lock);
  SmallCacheEntry *sc = smallcache_find(fd);
  if (sc) {
    off_t np = (whence == SEEK_SET) ? off
             : (whence == SEEK_CUR) ? sc->pos + off
                                    : sc->size + off;
    if (np < 0) np = 0;
    if (fd == s_video_trace_fd)
      debugPrintf("[video-io] fd=%d lseek_fake MATCHED SmallCacheEntry (pos %lld -> %lld) -- WRONG BUFFER\n",
                  fd, (long long)sc->pos, (long long)np);
    sc->pos = np;
    mutexUnlock(&s_smallcache_lock);
    return np;
  }
  mutexUnlock(&s_smallcache_lock);
  off_t r = lseek(fd, off, whence);
  if (fd == s_video_trace_fd)
    debugPrintf("[video-io] fd=%d lseek(off=%lld, whence=%d) -> %lld\n", fd, (long long)off, whence, (long long)r);
#if VERBOSE_IO
  debugPrintf("lseek(fd=%d, off=%lld, whence=%d) -> %lld\n", fd, (long long)off, whence, (long long)r);
#endif
  return r;
}

int close_fake(int fd) {
  if (fd == s_video_trace_fd)
    debugPrintf("[video-io] fd=%d close_fake ENTRY\n", fd);
  mutexLock(&s_pak_lock);
  PakFd *p = pak_find(fd);
  if (p) {
    if (fd == s_video_trace_fd)
      debugPrintf("[video-io] fd=%d close_fake MATCHED PakFd -- WRONG TABLE\n", fd);
    // No longer unconditional: with the whole file cached in RAM, OpenBOR's
    // own packfile reader still opens+closes this same fd thousands of
    // times a run (see pak_track()'s reuse path) but each cycle is now
    // nearly free, so a per-close line is mostly noise -- the two lines
    // that actually matter (a fresh "LOADED" the first time, "REUSED"
    // every time after) already print unconditionally from pak_track().
#if VERBOSE_IO
    debugPrintf("[pak] closing fd=%d (cache stays resident) served=%lluMB reads=%llu\n",
                fd, (unsigned long long)(g_pak_bytes >> 20), (unsigned long long)g_pak_reads);
#endif
    p->fd = -1;
    mutexUnlock(&s_pak_lock);
    return close(fd);
  }
  mutexUnlock(&s_pak_lock);
  mutexLock(&s_virtwin_lock);
  VirtWinFd *vw = virtwin_find(fd);
  if (vw) vw->fd = -1; // frees the slot; buf is borrowed, never freed here
  mutexUnlock(&s_virtwin_lock);
  if (vw) return close(fd); // this fd was its own dedicated open(), always safe to really close

  mutexLock(&s_smallcache_lock);
  SmallCacheEntry *sc = smallcache_find(fd);
  if (sc) sc->fd = -1; // buffer stays resident for the next open() of this same path
  mutexUnlock(&s_smallcache_lock);
  if (sc) return close(fd);
  if (fd == s_video_trace_fd) {
    debugPrintf("[video-io] fd=%d closing \"%s\": %llu read() calls, %llu/%lld bytes total\n",
                fd, s_video_trace_path, (unsigned long long)s_video_trace_reads,
                (unsigned long long)s_video_trace_bytes, (long long)s_video_trace_size);
    s_video_trace_fd = -1;
    g_video_playing = 0;
  }
#if VERBOSE_IO
  debugPrintf("close(fd=%d)\n", fd);
#endif
  return close(fd);
}

// ---------------------------------------------------------------------------
// dirent: convert devkitA64 newlib's struct dirent (ino_t, d_type, d_name) to
// bionic's (uint64 d_ino, int64 d_off, d_reclen, d_type, d_name) layout so
// OpenBOR reads d_name at the offset it actually expects.
//
// This build re-lists the SAME directories an enormous number of times --
// 17,000+ readdir() calls in one boot, apparently checking per-model script
// callback existence by enumerating a scripts folder fresh for every model
// (a real cost measured on-device: rendering stalled dead for minutes while
// this ran). Directory contents never change over the life of one run, so
// cache each path's full listing on first opendir() and serve every later
// opendir() for that same path straight from RAM -- the SD card is touched
// once per distinct path instead of once per repeat.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};
#pragma pack(pop)

typedef struct {
  char path[200];
  char (*names)[256];
  uint8_t *types;
  int count;
} DirCache;

#define DIRCACHE_MAX 64
static DirCache s_dircache[DIRCACHE_MAX];
static int s_dircache_n;

static DirCache *dircache_get(const char *path) {
  for (int i = 0; i < s_dircache_n; i++)
    if (strcmp(s_dircache[i].path, path) == 0) return &s_dircache[i];
  if (s_dircache_n >= DIRCACHE_MAX) return NULL;
  DIR *d = opendir(path);
  if (!d) return NULL;
  DirCache *c = &s_dircache[s_dircache_n];
  size_t cap = 32;
  c->names = malloc(cap * sizeof(*c->names));
  c->types = malloc(cap * sizeof(uint8_t));
  c->count = 0;
  struct dirent *e;
  while (c->names && c->types && (e = readdir(d)) != NULL) {
    if ((size_t)c->count == cap) {
      cap *= 2;
      char (*na)[256] = realloc(c->names, cap * sizeof(*c->names));
      uint8_t *nt = realloc(c->types, cap * sizeof(uint8_t));
      if (!na || !nt) break;
      c->names = na; c->types = nt;
    }
    snprintf(c->names[c->count], sizeof(c->names[0]), "%s", e->d_name);
    c->types[c->count] = e->d_type;
    c->count++;
  }
  closedir(d);

  // Pak aliasing (see the PakAlias table above): if this directory is one
  // an alias's virtual path points into and no real file of that name is
  // already listed, splice in a virtual entry for it so Menu()'s
  // opendir()+readdir() scan (confirmed via disassembly to do nothing more
  // than that against each entry's filename -- no stat(), no open()) still
  // finds a selectable game. open_fake() is what makes actually opening
  // that virtual name work, by retrying against the alias's real path.
  if (c->names && c->types) {
    for (size_t i = 0; i < PAK_ALIAS_N; i++) {
      const char *virt = s_pak_aliases[i].virt;
      const char *slash = strrchr(virt, '/');
      if (!slash) continue;
      size_t dirlen = (size_t)(slash - virt);
      char dirbuf[64];
      if (dirlen >= sizeof(dirbuf)) continue;
      memcpy(dirbuf, virt, dirlen);
      dirbuf[dirlen] = 0;
      // Same absolute-vs-relative-vs-"./"  mismatch path_ends_with_component
      // exists to paper over for open_fake()'s side of this: dircache_get()
      // can be called with "Paks", "./Paks", or a full
      // "/switch/sorx_nx/Paks"-style absolute path depending on which code
      // in the .so is asking, and all of them mean the same real directory.
      if (!path_ends_with_component(path, dirbuf)) continue;
      const char *base = slash + 1;
      int already = 0;
      for (int j = 0; j < c->count && !already; j++) {
        if (strlen(c->names[j]) != strlen(base)) continue;
        already = 1;
        for (size_t k = 0; base[k]; k++) {
          char a = c->names[j][k], b = base[k];
          if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
          if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
          if (a != b) { already = 0; break; }
        }
      }
      if (already) continue;
      if ((size_t)c->count == cap) {
        cap *= 2;
        char (*na)[256] = realloc(c->names, cap * sizeof(*c->names));
        uint8_t *nt = realloc(c->types, cap * sizeof(uint8_t));
        if (!na || !nt) break;
        c->names = na; c->types = nt;
      }
      snprintf(c->names[c->count], sizeof(c->names[0]), "%s", base);
      c->types[c->count] = DT_REG;
      c->count++;
      debugPrintf("[dircache] \"%s\": no real .pak found, synthesized virtual entry \"%s\"\n", path, base);
    }
  }

  snprintf(c->path, sizeof(c->path), "%s", path);
  s_dircache_n++;
  debugPrintf("[dircache] cached \"%s\": %d entries\n", path, c->count);
  return c;
}

static void dircache_invalidate(const char *path) {
  for (int i = 0; i < s_dircache_n; i++) {
    if (strcmp(s_dircache[i].path, path) != 0) continue;
    free(s_dircache[i].names);
    free(s_dircache[i].types);
    s_dircache[i] = s_dircache[s_dircache_n - 1];
    s_dircache_n--;
    return;
  }
}

// vpak_materialize/vpak_mkdirs_for create files/dirs under paths named
// without any "./" prefix; opendir() call sites elsewhere in the .so have
// been observed using both "data" and "./data" for what's the same real
// directory, so evict both spellings rather than risk missing the one an
// existing cache entry actually used.
static void dircache_invalidate_parent_of(const char *path) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s", path);
  char *slash = strrchr(buf, '/');
  const char *parent = ".";
  if (slash) { *slash = 0; parent = buf; }
  dircache_invalidate(parent);
  char alt[304];
  if (parent[0] == '.' && parent[1] == '/') {
    dircache_invalidate(parent + 2);
  } else {
    snprintf(alt, sizeof(alt), "./%s", parent);
    dircache_invalidate(alt);
  }
}

typedef struct {
  int used;
  DirCache *cache;
  int pos;
  struct bionic_dirent out;
} DirHandle;

// A DirHandle is just an index+position into an already-cached, read-only
// listing (no real kernel resource behind it), so there's no real cost to
// keeping many outstanding -- unlike the old raw-DIR* tracking table this
// replaced, 16 turned out to not be nearly enough: on-device traces showed
// the engine opening directories markedly faster than it closes them during
// the title-screen loop, exhausting the table and returning NULL from
// opendir() where the old code merely degraded gracefully (falling back to
// raw, non-bionic-converted readdir instead of failing outright).
#define DIRHANDLE_MAX 512
static DirHandle s_dirh[DIRHANDLE_MAX];

static uint64_t g_dirh_opens, g_dirh_closes;

void *opendir_fake(const char *name) {
  DirCache *c = dircache_get(name);
#if VERBOSE_IO
  debugPrintf("opendir(\"%s\") -> %s (%d entries)\n", name, c ? "ok" : "NULL", c ? c->count : 0);
#endif
  if (!c) return NULL;
  for (int i = 0; i < DIRHANDLE_MAX; i++) {
    if (!s_dirh[i].used) {
      s_dirh[i].used = 1;
      s_dirh[i].cache = c;
      s_dirh[i].pos = 0;
      g_dirh_opens++;
      if (g_dirh_opens % 100 == 0)
        debugPrintf("[dirh] opens=%llu closes=%llu outstanding=%llu\n",
                    (unsigned long long)g_dirh_opens, (unsigned long long)g_dirh_closes,
                    (unsigned long long)(g_dirh_opens - g_dirh_closes));
      return &s_dirh[i];
    }
  }
  // Genuinely exhausted even at 512 -- on-device traces show this cascading
  // into a retry loop (texture recreation every iteration, zero
  // eglSwapBuffers for the entire duration) once opendir() starts failing,
  // presumably because some check the caller can't proceed without treats
  // NULL as fatal-but-retry. Whatever is leaking these (opens keep
  // outpacing closes -- see the counters above), returning NULL here is
  // strictly worse than reusing a slot: round-robin steal the
  // longest-outstanding one instead of ever failing this call outright.
  static int next_victim = 0;
  int victim = next_victim;
  next_victim = (next_victim + 1) % DIRHANDLE_MAX;
  debugPrintf("opendir(\"%s\"): out of dir handles (opens=%llu closes=%llu) -- stealing slot %d\n",
              name, (unsigned long long)g_dirh_opens, (unsigned long long)g_dirh_closes, victim);
  s_dirh[victim].used = 1;
  s_dirh[victim].cache = c;
  s_dirh[victim].pos = 0;
  g_dirh_opens++;
  return &s_dirh[victim];
}

int closedir_fake(void *dirp) {
  DirHandle *h = dirp;
  if (h && h->used) g_dirh_closes++;
  if (h) h->used = 0;
  return 0;
}

void *readdir_fake(void *dirp) {
  DirHandle *h = dirp;
  if (!h || h->pos >= h->cache->count) {
#if VERBOSE_IO
    debugPrintf("readdir(%p) -> (NULL/EOF)\n", dirp);
#endif
    return NULL;
  }
  int i = h->pos++;
  struct bionic_dirent *out = &h->out;
  out->d_ino = 1;
  out->d_off = 0;
  snprintf(out->d_name, sizeof(out->d_name), "%s", h->cache->names[i]);
  out->d_reclen = (uint16_t)(offsetof(struct bionic_dirent, d_name) + strlen(out->d_name) + 1);
  out->d_type = h->cache->types[i];
#if VERBOSE_IO
  debugPrintf("readdir(%p) -> %s\n", dirp, out->d_name);
#endif
  return out;
}

// ---------------------------------------------------------------------------
// ANativeWindow -> NWindow: hand SDL the real Switch window so mesa's
// eglCreateWindowSurface lands on it.
// ---------------------------------------------------------------------------

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
#if VERBOSE_EGL
  debugPrintf("[egl] ANativeWindow_fromSurface -> %p (%dx%d)\n", (void *)win, screen_width, screen_height);
#endif
  return win;
}
void ANativeWindow_release_fake(void *win) { (void)win; }
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
#if VERBOSE_EGL
  debugPrintf("[egl] ANativeWindow_setBuffersGeometry(win=%p, %dx%d, format=%d)\n", win, w, h, format);
#endif
  (void)format;
  if (w > 0 && h > 0) nwindowSetDimensions((NWindow *)win, w, h);
  return 0;
}
int32_t ANativeWindow_getWidth_fake(void *win) { (void)win; return screen_width; }
int32_t ANativeWindow_getHeight_fake(void *win) { (void)win; return screen_height; }

// ---------------------------------------------------------------------------
// ALooper / ASensor*: SDL2's Android accelerometer support degrades cleanly
// when the sensor manager can't be reached, so stub the whole surface out.
// ---------------------------------------------------------------------------

void *ALooper_forThread_fake(void) { return NULL; }
void *ALooper_prepare_fake(int opts) { (void)opts; return NULL; }
int ALooper_pollAll_fake(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  (void)outFd; (void)outEvents; (void)outData;
  if (timeoutMillis > 0) svcSleepThread((s64)timeoutMillis * 1000000ll);
  return -1; // ALOOPER_POLL_WAKE / timeout, no events
}
void *ASensorManager_getInstance_fake(void) { return NULL; }
int ASensorManager_getSensorList_fake(void *mgr, void *list) {
  (void)mgr; if (list) *(void **)list = NULL; return 0;
}
void *ASensorManager_createEventQueue_fake(void *mgr, void *looper, int ident, void *cb, void *data) {
  (void)mgr; (void)looper; (void)ident; (void)cb; (void)data; return NULL;
}
int ASensorManager_destroyEventQueue_fake(void *mgr, void *queue) { (void)mgr; (void)queue; return 0; }
int ASensorEventQueue_enableSensor_fake(void *queue, void *sensor) { (void)queue; (void)sensor; return -1; }
int ASensorEventQueue_disableSensor_fake(void *queue, void *sensor) { (void)queue; (void)sensor; return -1; }
int ASensorEventQueue_getEvents_fake(void *queue, void *events, size_t count) {
  (void)queue; (void)events; (void)count; return 0;
}
const char *ASensor_getName_fake(void *sensor) { (void)sensor; return ""; }
int ASensor_getType_fake(void *sensor) { (void)sensor; return 0; }

// ---------------------------------------------------------------------------
// POSIX semaphores via pointer indirection (bionic sem_t is 16 bytes on
// LP64, so a heap FakeSem* fits in the caller's storage)
// ---------------------------------------------------------------------------

typedef struct { Semaphore sem; } FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  if (!fs) return -1;
  semaphoreInit(&fs->sem, value);
  *s = fs;
#if VERBOSE_IO
  debugPrintf("[sem] init(%p) = %p value=%u\n", (void *)s, (void *)fs, value);
#endif
  return 0;
}
int sem_destroy_fake(void **s) {
  if (s && *s) { free(*s); *s = NULL; }
  return 0;
}
int sem_post_fake(void **s) {
#if VERBOSE_IO
  debugPrintf("[sem] post(%p) inited=%d\n", (void *)s, s && *s);
#endif
  if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}
int sem_wait_fake(void **s) {
#if VERBOSE_IO
  debugPrintf("[sem] wait(%p) inited=%d -- blocking...\n", (void *)s, s && *s);
#endif
  if (s && *s) semaphoreWait(&((FakeSem *)*s)->sem);
#if VERBOSE_IO
  debugPrintf("[sem] wait(%p) done\n", (void *)s);
#endif
  return 0;
}
int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0;
  errno = EAGAIN;
  return -1;
}
int sem_getvalue_fake(void **s, int *val) {
  *val = (s && *s) ? (int)((FakeSem *)*s)->sem.count : 0;
  return 0;
}
