/* libc_shim.h -- bionic-compatible libc/NDK wrappers for libhidapi.so,
 * libSDL2.so and libopenbor.so. MIT license; see LICENSE. */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

// stack protector: openbor's NDK r21e build references the global-guard
// variant directly (rather than bionic's TLS-based tpidr_el0+0x28 one, which
// tls_setup_guard() covers for the older NDK r20 SDL2/hidapi builds).
extern uintptr_t __stack_chk_guard;

// fortify (_chk): ignore the object-size argument
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t slen);
char *__strchr_chk_fake(const char *s, int c, size_t slen);
char *__strrchr_chk_fake(const char *s, int c, size_t slen);
size_t __strlen_chk_fake(const char *s, size_t slen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va);
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen);
int __printf_chk_fake(int flag, const char *fmt, ...);

// misc bionic
long syscall_fake(long number, ...);
void sincos_fake(double x, double *s, double *c);
int clock_gettime_fake(int clk, void *ts);
void android_set_abort_message_fake(const char *msg);
size_t __ctype_get_mb_cur_max_fake(void);
long sysconf_fake(int name);
int __system_property_get_fake(const char *name, char *value);
void __android_log_write_fake(int prio, const char *tag, const char *msg);

// signals (stubbed -- the engines install handlers we never raise)
int sigaction_fake(int sig, const void *act, void *oldact);
void *signal_fake(int sig, void *handler);
int sigaddset_fake(void *set, int sig);
int sigemptyset_fake(void *set);
int pthread_sigmask_fake(int how, const void *set, void *oldset);

// mem/process info the engines probe for headroom checks
struct bionic_mallinfo {
  int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
struct bionic_mallinfo mallinfo_fake(void);
int sysinfo_fake(void *info);
int getpagesize_fake(void);

// stdio over fake __sF
extern uint8_t fake_sF[3][0x100];
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);
int fseek_fake(FILE *f, long off, int whence);
int putchar_fake(int c);
int puts_fake(const char *s);
FILE *fopen_fake(const char *path, const char *mode);
int open_fake(const char *path, int flags, ...);

// bor.pak read-ahead: transparent for every other fd (see libc_shim.c)
ssize_t read_fake(int fd, void *dst, size_t count);
off_t lseek_fake(int fd, off_t off, int whence);
int close_fake(int fd);

// dirent: bionic's struct dirent (uint64 d_ino + int64 d_off + d_reclen +
// d_type + d_name[256]) is laid out completely differently from devkitA64
// newlib's (ino_t + d_type + d_name[256]) -- OpenBOR, compiled against
// bionic, would read d_name at the wrong offset (garbled filenames) if handed
// a raw newlib dirent, so these convert.
void *opendir_fake(const char *name);
void *readdir_fake(void *dirp);
int closedir_fake(void *dirp);

// ANativeWindow -> NWindow
void *ANativeWindow_fromSurface_fake(void *env, void *surface);
void ANativeWindow_release_fake(void *win);
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format);
int32_t ANativeWindow_getWidth_fake(void *win);
int32_t ANativeWindow_getHeight_fake(void *win);

// ALooper / ASensor*: no real sensors on Switch; SDL2's accelerometer probe
// degrades gracefully when ASensorManager_getInstance returns NULL.
void *ALooper_forThread_fake(void);
void *ALooper_prepare_fake(int opts);
int ALooper_pollAll_fake(int timeoutMillis, int *outFd, int *outEvents, void **outData);
void *ASensorManager_getInstance_fake(void);
int ASensorManager_getSensorList_fake(void *mgr, void *list);
void *ASensorManager_createEventQueue_fake(void *mgr, void *looper, int ident, void *cb, void *data);
int ASensorManager_destroyEventQueue_fake(void *mgr, void *queue);
int ASensorEventQueue_enableSensor_fake(void *queue, void *sensor);
int ASensorEventQueue_disableSensor_fake(void *queue, void *sensor);
int ASensorEventQueue_getEvents_fake(void *queue, void *events, size_t count);
const char *ASensor_getName_fake(void *sensor);
int ASensor_getType_fake(void *sensor);

// POSIX semaphores (pointer-indirected: bionic sem_t is 16 bytes on LP64)
int sem_init_fake(void **s, int pshared, unsigned int value);
int sem_destroy_fake(void **s);
int sem_post_fake(void **s);
int sem_wait_fake(void **s);
int sem_trywait_fake(void **s);
int sem_getvalue_fake(void **s, int *val);

// bytes served from any tracked bor.pak/Paks/*.pak fd so far -- main.c's
// loop polls this to detect active loading and adaptively re-boost the CPU.
uint64_t pak_bytes_total(void);

// 1 while a real (non-stubbed) video override file is open, 0 otherwise --
// imports.c's GL/draw wrappers use this to trace in detail around video
// playback without waiting on their own shared, long-since-exhausted
// first-N-calls counters.
extern int g_video_playing;

// Eager, whole-pak extraction: walks every catalog entry (bor.pak +
// Paks/*.pak) and ensures a real loose file exists on disk for each one,
// so the game never depends on on-demand materialization racing its own
// early asset reads. Call once, early in main(), after chdir()'ing into
// the data root (materialize's paths are CWD-relative) and before the SDL
// thread/game starts -- safe to show progress via the plain libnx console
// here since no EGL/GL context exists yet. progress may be NULL.
typedef void (*VpakExtractProgressFn)(uint32_t done, uint32_t total, const char *name);
void vpak_extract_all(VpakExtractProgressFn progress);

#endif
