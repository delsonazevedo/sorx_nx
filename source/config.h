/* config.h -- Streets of Rage X (OpenBOR/Android) Switch wrapper configuration.
 * MIT license; see LICENSE. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Loaded in this order (so_util resolves each module's imports against the
// shim table first, then against every other already-loaded module's
// exports): hidapi has no game-side deps, SDL2 depends on hidapi, openbor
// depends on SDL2 (and transitively hidapi). All three are fopen()'d relative
// to the NRO's directory, so keep the app tree together under /switch/sorx_nx/.
#define HIDAPI_SO_NAME  "libhidapi.so"
#define SDL2_SO_NAME    "libSDL2.so"
#define OPENBOR_SO_NAME "libopenbor.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "sorx_debug.log"

// '/'-absolute paths resolved against the default sdmc device. DATA_ROOT holds
// the app tree the user prepares under release/switch/sorx_nx/ (the three
// .so's, bor.pak / assets/, paks/). SAVE_ROOT holds saves/config so OpenBOR's
// "Saves"/"Config" folders and screenshot output persist there.
#define DEFAULT_DATA_ROOT "/switch/sorx_nx"
#define DEFAULT_SAVE_ROOT "/switch/sorx_nx/save"

// absolute so the log lands in the app dir regardless of the launch CWD
#define LOG_PATH DEFAULT_DATA_ROOT "/sorx_debug.log"

// Master debug switch: log file (<data_root>/sorx_debug.log), nxlink stdout,
// and all debugPrintf/[io]/[audio]/[jni] output. Off for release.
#define DEBUG_LOG 0
// Per-file-operation logging (open/stat/access/fopen, plus every generic
// read/lseek/close under 4KB). Was needed to trace the pak/vpak file-
// resolution issues; now that those are fixed, this fires on nearly every
// I/O call OpenBOR makes (thousands of small config/script reads) and the
// per-line fflush cost alone was measured materially slowing boot -- leave
// off unless actively debugging file I/O again. Requires DEBUG_LOG too.
#define VERBOSE_IO 0
// Per-call JNI dispatch logging (method names hit through call_object/call_int/
// etc). Invaluable while bringing up a new libSDL2.so build, but fires on
// every JNI call (some, like getManifestEnvironmentVariables, are polled
// continuously) -- same per-line cost problem as VERBOSE_IO. Requires
// DEBUG_LOG too.
#define VERBOSE_JNI 0
// EGL/window lifecycle logging (config chosen, surface/context creation,
// window dimensions, swap-buffer failures) plus per-draw-call GL state
// tracing. Was on throughout the black-screen investigation (root cause:
// the missing nativeSetScreenResolution() call in main.c, since fixed) --
// confirmed no longer needed for that. Kept firing every ~60 frames/300
// draws for the life of a run, though, its debugPrintf/fflush-per-line cost
// (same problem VERBOSE_IO/VERBOSE_JNI already avoid, see above) was still
// landing on whichever thread happened to log, periodically starving
// whatever else was runnable on the same pinned cores -- confirmed via
// audio.c's own timing instrumentation as a real, if intermittent (2-15ms
// spikes), contributor to audio's small remaining real-time-vs-content-time
// gap after the actual 2x-speed channel-count bug was fixed. Same rule as
// the other two now: leave off unless actively debugging EGL/GL again.
#define VERBOSE_EGL 0

extern int screen_width;
extern int screen_height;

// Android package id OpenBOR's SDL2 Android build expects for its data paths
// (SDL_AndroidGetExternalStoragePath-style getters route through this).
#define ANDROID_PKG "org.openbor.engine"

typedef struct {
  int screen_width;   // -1 = auto
  int screen_height;
  char data_root[256];
  char save_root[256];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
