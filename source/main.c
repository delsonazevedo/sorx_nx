/* main.c -- Streets of Rage X (OpenBOR, Android release) Switch wrapper entry
 * point.
 *
 * Loads libhidapi.so, libSDL2.so and libopenbor.so (in that dependency
 * order -- so_util resolves each one's imports against our shim table first,
 * then against every other already-loaded module's real exports), provides a
 * minimal Android-like environment (fake JNI, a bionic->newlib libc shim, an
 * EGL/GLES import bridge, and a JNI-driven audio bridge), and drives SDL2's
 * real org.libsdl.app.SDLActivity native lifecycle. Unlike a raw-GLES2/
 * NativeActivity-style Android port, SDL2 owns EGL and the render loop itself
 * once started -- this wrapper's job is booting it and then, for the rest of
 * the game's life, translating the Switch pad into the onNativeKeyDown/
 * onNativeKeyUp calls SDL2's Android backend expects.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "patch.h"
#include "egl_shim.h"
#include "libc_shim.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module hidapi_mod, sdl2_mod, openbor_mod;

// reserve a slice for the three .so loaders; the rest is the newlib heap
// where OpenBOR's malloc traffic lands. Requires full-RAM mode (title
// override / forwarder) for a comfortable game heap.
#define SO_HEAP_RESERVE (64 * 1024 * 1024)

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = SO_HEAP_RESERVE;
  if (so_reserve > size / 2)
    so_reserve = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t fake_heap_size = size - so_reserve;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(HIDAPI_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", HIDAPI_SO_NAME);
  if (stat(SDL2_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", SDL2_SO_NAME);
  if (stat(OPENBOR_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nPlace it next to the NRO.", OPENBOR_SO_NAME);
  // OpenBOR's isRawData() check (opendir("data") succeeding) is what keeps
  // it on the classic per-file pak scan instead of its own cached/sector
  // reader -- the latter has been observed to corrupt state and OOM-crash
  // against this custom-magic pak. An empty "data" dir shipped in the
  // release package doesn't reliably survive every SD-card transfer method
  // (many drop empty directories), so guarantee it exists here instead of
  // trusting the copy.
  mkdir("data", 0777);
  // Same issue, same fix, for Paks/: now that it ships with no physical
  // file inside it (see the PakAlias table in libc_shim.c -- Paks/1.0.0.pak
  // is served as a virtual entry backed by bor.pak instead of a duplicated
  // 380 MB copy), an empty Paks/ is exactly as likely to get dropped by an
  // SD-card transfer as an empty data/ always was. And unlike data/, this
  // one is load-bearing even with the alias in place: opendir_fake()'s
  // dircache_get() only ever splices in the synthesized virtual entry AFTER
  // a real opendir() on the directory itself already succeeded -- if Paks/
  // doesn't exist at all, that opendir() fails first and the alias code
  // never runs, which is exactly what an empty Menu() game list turned out
  // to mean on-device.
  mkdir("Paks", 0777);
}

// Resolve the app's data directory from the launch CWD so the port works from
// any folder under /switch (not just /switch/sorx_nx). Falls back to the
// compile-time default when the CWD doesn't hold the game's libraries.
static void resolve_data_root(void) {
  char cwd[256];
  if (!getcwd(cwd, sizeof(cwd)) || !cwd[0]) return;
  // drop any "device:" prefix ("sdmc:/switch/x" -> "/switch/x")
  char *colon = strchr(cwd, ':');
  char *base = colon ? colon + 1 : cwd;
  if (!base[0]) return;
  size_t l = strlen(base);
  while (l > 1 && base[l - 1] == '/') base[--l] = 0; // strip trailing slashes
  // only adopt it if the game binary is actually there
  char so[300];
  snprintf(so, sizeof(so), "%s/%s", base, OPENBOR_SO_NAME);
  struct stat st;
  if (stat(so, &st) != 0) return;
  snprintf(config.data_root, sizeof(config.data_root), "%s", base);
  snprintf(config.save_root, sizeof(config.save_root), "%s/save", base);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// SDLActivity / SDLAudioManager / SDLControllerManager entry points
// ---------------------------------------------------------------------------

static int  (*e_nativeSetupJNI)(void *env);
static int  (*e_audioSetupJNI)(void *env);
static int  (*e_controllerSetupJNI)(void *env);
static void (*e_onNativeSurfaceCreated)(void *env, void *cls);
static void (*e_onNativeSurfaceChanged)(void *env, void *cls);
static void (*e_onNativeSurfaceDestroyed)(void *env, void *cls);
static void (*e_onNativeResize)(void *env, void *cls);
static void (*e_nativeRunMain)(void *env, void *cls, void *library, void *function, void *args);
static void (*e_nativePause)(void *env, void *cls);
static void (*e_nativeResume)(void *env, void *cls);
static void (*e_nativeQuit)(void *env, void *cls);
static void (*e_onNativeKeyDown)(void *env, void *cls, int keycode);
static void (*e_onNativeKeyUp)(void *env, void *cls, int keycode);
static int  (*e_JNI_OnLoad)(void *vm, void *reserved);
// Signatures below are not guessed: pulled from a disassembly of this exact
// libSDL2.so (aarch64-none-elf-objdump), reading which argument registers
// each JNI wrapper touches before tail-calling into its internal Android_*
// counterpart -- confirmed against the real, public SDLControllerManager.java
// contract rather than trusted from memory across SDL2 point releases.
static int  (*e_nativeAddJoystick)(void *env, void *cls, int device_id, void *name, void *desc,
                                    int vendor_id, int product_id, int is_accelerometer,
                                    int button_mask, int naxes, int axis_mask, int nhats);
static void (*e_onNativePadDown)(void *env, void *cls, int device_id, int keycode);
static void (*e_onNativePadUp)(void *env, void *cls, int device_id, int keycode);
static void (*e_onNativeJoy)(void *env, void *cls, int device_id, int axis, float value);
static void (*e_onNativeHat)(void *env, void *cls, int device_id, int hat_id, int x, int y);
// Also disassembly-confirmed: 3 ints then 3 floats. Real SDLActivity.java
// normalizes touch coordinates to [0,1] (x/width, y/height) before calling
// this -- not raw pixels.
static void (*e_onNativeTouch)(void *env, void *cls, int touch_device_id, int pointer_finger_id,
                                int action, float x, float y, float pressure);
// Disassembly-confirmed (aarch64-none-elf-objdump on release/switch/sorx_nx/
// libSDL2.so): Java_..._nativeSetScreenResolution(env, cls, w2=surfaceWidth,
// w3=surfaceHeight, w4=deviceWidth, w5=deviceHeight, w6=format, v0=rate)
// tail-calls Android_SetScreenResolution(surfaceWidth, surfaceHeight,
// deviceWidth, deviceHeight, format, rate), which is the ONLY writer of the
// Android_SurfaceWidth/Android_SurfaceHeight (and device-size) globals that
// Android_SendResize -- called from our onNativeResize below -- copies into
// the SDL video device's current display mode. On real Android, Java's
// SDLSurface.surfaceChanged() calls this before ever triggering
// onNativeResize/onNativeSurfaceChanged; we drive no such Java code, so
// without calling it ourselves those globals stay at their BSS zero forever
// and every mode/size derived from them downstream is degenerate -- a
// separate data path from (and not fixed by) the SDL_GetDesktopDisplayMode
// override in imports.c, which only patches SDL's *desktop* query, not this
// window/surface one.
static void (*e_nativeSetScreenResolution)(void *env, void *cls, int surfaceWidth, int surfaceHeight,
                                            int deviceWidth, int deviceHeight, int format, float rate);

static void resolve_entry_points(void) {
  // A real JVM calls this automatically on System.loadLibrary(); it's what
  // sets libSDL2.so's own internal JavaVM* global (used by every thread SDL2
  // spawns itself to AttachCurrentThread). Skipping it leaves that global
  // NULL forever -- SDL2 logs "there is no JavaVM" and any thread that tries
  // to attach anyway dereferences it and crashes.
  e_JNI_OnLoad               = (void *)so_try_find_addr_rx(&sdl2_mod, "JNI_OnLoad");
  e_nativeSetupJNI           = (void *)so_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeSetupJNI");
  e_audioSetupJNI            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLAudioManager_nativeSetupJNI");
  e_controllerSetupJNI       = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_nativeSetupJNI");
  e_onNativeSurfaceCreated   = (void *)so_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceCreated");
  e_onNativeSurfaceChanged   = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceChanged");
  e_onNativeSurfaceDestroyed = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceDestroyed");
  e_onNativeResize           = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeResize");
  e_nativeRunMain            = (void *)so_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeRunMain");
  e_nativePause              = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativePause");
  e_nativeResume             = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeResume");
  e_nativeQuit               = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeQuit");
  e_onNativeKeyDown          = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeKeyDown");
  e_onNativeKeyUp            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeKeyUp");
  e_nativeAddJoystick        = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_nativeAddJoystick");
  e_onNativePadDown          = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativePadDown");
  e_onNativePadUp            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativePadUp");
  e_onNativeJoy              = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativeJoy");
  e_onNativeHat              = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLControllerManager_onNativeHat");
  e_onNativeTouch            = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_onNativeTouch");
  e_nativeSetScreenResolution = (void *)so_try_find_addr_rx(&sdl2_mod, "Java_org_libsdl_app_SDLActivity_nativeSetScreenResolution");
  imports_set_real_rendercopy((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_RenderCopy"));
  imports_set_real_getdesktopdisplaymode((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_GetDesktopDisplayMode"));
  imports_set_real_createtexture((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CreateTexture"));
  imports_set_real_createtexturefromsurface((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CreateTextureFromSurface"));
  imports_set_real_updatetexture((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_UpdateTexture"));
  imports_set_real_destroytexture((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_DestroyTexture"));
  imports_set_real_renderclear((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_RenderClear"));
  imports_set_real_renderpresent((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_RenderPresent"));
  imports_set_real_createthread((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CreateThread"));
  imports_set_real_waitthread((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_WaitThread"));
  imports_set_real_lockmutex((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_LockMutex"));
  imports_set_real_condwait((void *)so_try_find_addr_rx(&sdl2_mod, "SDL_CondWait"));
}

// ---------------------------------------------------------------------------
// input: Switch pad -> both standard Android gamepad keycodes (onNativeKeyDown/
// Up -- reaches anything that treats keyboard-sourced input as a pad) AND a
// real registered SDL joystick (onNativePadDown/Up/onNativeJoy/onNativeHat --
// reaches OpenBOR's direct SDL_JoystickOpen/GetAxis/GetButton/GetHat calls,
// which see nothing at all until a device exists: SDL_NumJoysticks() is 0
// until nativeAddJoystick registers one). Sending both costs nothing and
// covers whichever path the current screen actually reads.
//
// Touch is not forwarded yet; the Switch's handheld touchscreen could feed
// onNativeTouch as a follow-up once input is confirmed reaching the game.
// ---------------------------------------------------------------------------

#define JOY_DEVICE_ID 0

#define AKEYCODE_DPAD_UP    19
#define AKEYCODE_DPAD_DOWN  20
#define AKEYCODE_DPAD_LEFT  21
#define AKEYCODE_DPAD_RIGHT 22
// This game's sdl/control.h (SDL2/Android branch) has NO default joystick
// binding at all -- CONTROL_DEFAULT1_* are all SDL_SCANCODE_* (keyboard),
// e.g. START=SDL_SCANCODE_RETURN, FIRE1=SDL_SCANCODE_A, and no
// "controller_map.txt" ships in the pak to override that (confirmed: it's
// not a catalog entry, only ever a save-generated file we've never seen
// exist). Gamepad-button Android keycodes like KEYCODE_BUTTON_A(96) or
// KEYCODE_BUTTON_START(108) translate to gamepad-only SDL scancodes that
// match nothing in that default map, which is exactly why on-device
// testing showed input events correctly reaching SDL (onNativeKeyDown
// logged every press) yet never registering as FLAG_ANYBUTTON to break
// the attract-mode "PRESS START" loop. Send the actual keyboard Android
// keycodes the defaults expect instead.
#define AKEYCODE_ENTER      66  // SDL_SCANCODE_RETURN -> START
#define AKEYCODE_ESCAPE     111 // SDL_SCANCODE_ESCAPE -> CONTROL_ESC
#define AKEYCODE_A          29  // SDL_SCANCODE_A -> FIRE1
#define AKEYCODE_S          47  // SDL_SCANCODE_S -> FIRE2
#define AKEYCODE_Z          54  // SDL_SCANCODE_Z -> FIRE3
#define AKEYCODE_X          52  // SDL_SCANCODE_X -> FIRE4
#define AKEYCODE_D          32  // SDL_SCANCODE_D -> FIRE5
#define AKEYCODE_F          34  // SDL_SCANCODE_F -> FIRE6

static PadState pad;

static struct { u64 sw; int key; } s_btnmap[] = {
  { HidNpadButton_A,      AKEYCODE_A },       // FIRE1 -> confirm/attack
  { HidNpadButton_X,      AKEYCODE_S },       // FIRE2
  { HidNpadButton_Y,      AKEYCODE_Z },       // FIRE3
  { HidNpadButton_L,      AKEYCODE_X },       // FIRE4
  { HidNpadButton_R,      AKEYCODE_D },       // FIRE5
  { HidNpadButton_ZL,     AKEYCODE_F },       // FIRE6
  { HidNpadButton_ZR,     AKEYCODE_ESCAPE },
  { HidNpadButton_Plus,   AKEYCODE_ENTER },   // START
  { HidNpadButton_Minus,  AKEYCODE_ESCAPE },
  { HidNpadButton_Up,     AKEYCODE_DPAD_UP },
  { HidNpadButton_Down,   AKEYCODE_DPAD_DOWN },
  { HidNpadButton_Left,   AKEYCODE_DPAD_LEFT },
  { HidNpadButton_Right,  AKEYCODE_DPAD_RIGHT },
};

static u64 s_prev_buttons = 0;

static void poll_input(void) {
  void *cls = jni_activity_class();
  padUpdate(&pad);
  const u64 cur = padGetButtons(&pad);

  for (unsigned i = 0; i < sizeof(s_btnmap) / sizeof(*s_btnmap); i++) {
    const u64 m = s_btnmap[i].sw;
    if ((cur & m) && !(s_prev_buttons & m)) {
      debugPrintf("[input] Down %d (key fn=%p pad fn=%p)\n", s_btnmap[i].key, (void *)e_onNativeKeyDown, (void *)e_onNativePadDown);
      if (e_onNativeKeyDown) e_onNativeKeyDown(fake_env, cls, s_btnmap[i].key);
      if (e_onNativePadDown) e_onNativePadDown(fake_env, cls, JOY_DEVICE_ID, s_btnmap[i].key);
    } else if (!(cur & m) && (s_prev_buttons & m)) {
      debugPrintf("[input] Up %d (key fn=%p pad fn=%p)\n", s_btnmap[i].key, (void *)e_onNativeKeyUp, (void *)e_onNativePadUp);
      if (e_onNativeKeyUp) e_onNativeKeyUp(fake_env, cls, s_btnmap[i].key);
      if (e_onNativePadUp) e_onNativePadUp(fake_env, cls, JOY_DEVICE_ID, s_btnmap[i].key);
    }
  }
  s_prev_buttons = cur;

  // left stick -> SDL joystick axes 0 (x) and 1 (y), normalized [-1,1]
  if (e_onNativeJoy) {
    HidAnalogStickState l = padGetStickPos(&pad, 0);
    e_onNativeJoy(fake_env, cls, JOY_DEVICE_ID, 0, l.x / 32767.0f);
    e_onNativeJoy(fake_env, cls, JOY_DEVICE_ID, 1, -l.y / 32767.0f);
  }

  // left stick as a second d-pad source
  static int was_up = 0, was_down = 0, was_left = 0, was_right = 0;
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  const int dz = 0x1A00; // ~20%
  int is_up = l.y > dz, is_down = l.y < -dz, is_left = l.x < -dz, is_right = l.x > dz;
  struct { int is, was, key; } axes[4] = {
    { is_up, was_up, AKEYCODE_DPAD_UP },
    { is_down, was_down, AKEYCODE_DPAD_DOWN },
    { is_left, was_left, AKEYCODE_DPAD_LEFT },
    { is_right, was_right, AKEYCODE_DPAD_RIGHT },
  };
  for (int i = 0; i < 4; i++) {
    if (axes[i].is && !axes[i].was) {
      if (e_onNativeKeyDown) e_onNativeKeyDown(fake_env, cls, axes[i].key);
      if (e_onNativePadDown) e_onNativePadDown(fake_env, cls, JOY_DEVICE_ID, axes[i].key);
    } else if (!axes[i].is && axes[i].was) {
      if (e_onNativeKeyUp) e_onNativeKeyUp(fake_env, cls, axes[i].key);
      if (e_onNativePadUp) e_onNativePadUp(fake_env, cls, JOY_DEVICE_ID, axes[i].key);
    }
  }
  was_up = is_up; was_down = is_down; was_left = is_left; was_right = is_right;

  // combined physical d-pad + stick-threshold direction, also as an SDL hat
  if (e_onNativeHat) {
    static int hat_x = 0, hat_y = 0;
    int nx = ((cur & HidNpadButton_Right) || is_right) - ((cur & HidNpadButton_Left) || is_left);
    int ny = ((cur & HidNpadButton_Up) || is_up) - ((cur & HidNpadButton_Down) || is_down);
    if (nx != hat_x || ny != hat_y) {
      hat_x = nx; hat_y = ny;
      e_onNativeHat(fake_env, cls, JOY_DEVICE_ID, 0, hat_x, hat_y);
    }
  }

  // single-finger touch: real SDLActivity.java normalizes to [0,1] before
  // calling onNativeTouch, so scale the 1280x720 handheld panel accordingly
  // rather than sending raw pixels.
  {
    static int was_touching = 0;
    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
      float nx = (float)ts.touches[0].x / 1280.0f;
      float ny = (float)ts.touches[0].y / 720.0f;
      int action = was_touching ? 2 /* ACTION_MOVE */ : 0 /* ACTION_DOWN */;
      if (!was_touching)
        debugPrintf("[input] touch DOWN raw=(%u,%u) norm=(%.3f,%.3f) fn=%p\n",
                    ts.touches[0].x, ts.touches[0].y, nx, ny, (void *)e_onNativeTouch);
      if (e_onNativeTouch) e_onNativeTouch(fake_env, cls, 0, 0, action, nx, ny, 1.0f);
      was_touching = 1;
    } else if (was_touching) {
      debugPrintf("[input] touch UP fn=%p\n", (void *)e_onNativeTouch);
      if (e_onNativeTouch) e_onNativeTouch(fake_env, cls, 0, 0, 1 /* ACTION_UP */, 0.0f, 0.0f, 0.0f);
      was_touching = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// SDL thread: nativeRunMain blocks here for the game's entire lifetime
// (it dlopens/dlsyms "SDL_main" -- answered by egl_shim's stashed address --
// and calls it directly, exactly as SDLActivity.java's own SDLThread would).
// ---------------------------------------------------------------------------

static Thread s_sdl_thread;
static volatile int s_sdl_thread_done = 0;

static void sdl_thread_fn(void *arg) {
  (void)arg;
  tls_setup_guard(); // bionic stack canary from tpidr_el0+0x28
  void *cls = jni_activity_class();
  debugPrintf(">> SDL thread: nativeRunMain...\n");
  e_nativeRunMain(fake_env, cls, jni_new_string(OPENBOR_SO_NAME), jni_new_string("SDL_main"), NULL);
  debugPrintf(">> SDL thread: SDL_main returned\n");
  s_sdl_thread_done = 1;
}

#if !DEBUG_LOG
// Only updates the console on a percent change (at most 101 lines for the
// whole pass) -- redrawing per-file for several thousand entries would
// spend more time on console output than the extraction itself.
static void extract_progress_cb(uint32_t done, uint32_t total, const char *name) {
  (void)name;
  static int last_pct = -1;
  int pct = total ? (int)(((uint64_t)done * 100) / total) : 100;
  if (pct == last_pct) return;
  last_pct = pct;
  printf("Extracting game assets... %d%% (%u/%u)\n", pct, (unsigned)done, (unsigned)total);
  consoleUpdate(NULL);
}
#endif

int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  resolve_data_root(); // adopt the actual launch folder as the data root
  mkdir(config.save_root, 0777);
  setenv("HOME", config.save_root, 1);
  // OpenBOR's SDL2 build looks here for bor.pak / Paks/ / Saves/ (see README).
  chdir(config.data_root);

  // Before touching EGL/GL at all (so_load below, then the SDL thread,
  // create the game's own window/context) -- the plain libnx console is
  // safe here and nowhere else in this process's life, since nothing else
  // is contending for the display surface yet. Cheap on every boot after
  // the first (see vpak_extract_all()'s own comment): a fully-extracted
  // data/ turns this into a few thousand stat() calls, not a repeat copy.
  //
  // Console UI skipped under DEBUG_LOG: confirmed on-device (Atmosphere
  // crash report) that userAppInit()'s initNxLink() -- which redirects
  // stdout for nxlink BEFORE main() ever runs, whenever DEBUG_LOG is on --
  // and consoleInit()/consoleExit() both fight over that same stdout
  // redirection. consoleExit() has no way to know nxlink already customized
  // it, so whatever it "restores" leaves stdout broken for every printf()
  // after (a Data Abort inside libnx's own ConsoleSwRenderer_drawChar, from
  // the very next debugPrintf() call). Real release builds never call
  // initNxLink() at all (DEBUG_LOG is "off for release", see config.h), so
  // this conflict is specific to debug testing, where nxlink and the log
  // file already give a working progress channel anyway -- extraction
  // itself always runs regardless, just without the on-screen console.
#if !DEBUG_LOG
  consoleInit(NULL);
  printf("Streets of Rage X\n\n");
  vpak_extract_all(extract_progress_cb);
  consoleExit(NULL);
#else
  vpak_extract_all(NULL);
#endif

  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("== SoRX Switch wrapper booting; data_root=%s ==\n", config.data_root);

  // Load in dependency order: hidapi has no deps on the other two, SDL2 needs
  // hidapi, openbor needs SDL2 (and transitively hidapi). so_util resolves
  // each module's imports against the shim table first, then against every
  // other already-loaded module's real exports -- so this is the only order
  // that matters.
  void *base = heap_so_base;
  size_t remaining = heap_so_limit;

  if (so_load(&hidapi_mod, HIDAPI_SO_NAME, base, remaining) < 0)
    fatal_error("Could not load\n%s.", HIDAPI_SO_NAME);
  base = (char *)base + hidapi_mod.load_size; remaining -= hidapi_mod.load_size;
  sorx_resolve_imports(&hidapi_mod);

  if (so_load(&sdl2_mod, SDL2_SO_NAME, base, remaining) < 0)
    fatal_error("Could not load\n%s.", SDL2_SO_NAME);
  base = (char *)base + sdl2_mod.load_size; remaining -= sdl2_mod.load_size;
  sorx_resolve_imports(&sdl2_mod);

  if (so_load(&openbor_mod, OPENBOR_SO_NAME, base, remaining) < 0)
    fatal_error("Could not load\n%s.", OPENBOR_SO_NAME);
  base = (char *)base + openbor_mod.load_size; remaining -= openbor_mod.load_size;
  sorx_resolve_imports(&openbor_mod);

  debugPrintf("== all modules loaded + resolved ==\n");

  so_patch(&openbor_mod);

  // resolve exports before so_finalize maps the code and locks load_base out
  resolve_entry_points();
  if (!e_nativeSetupJNI || !e_onNativeSurfaceCreated || !e_nativeRunMain)
    fatal_error("Could not resolve SDLActivity entry points.");

  uintptr_t sdl_main_addr = so_try_find_addr_rx(&openbor_mod, "SDL_main");
  if (!sdl_main_addr)
    fatal_error("Could not find SDL_main in\n%s.", OPENBOR_SO_NAME);
  egl_shim_set_native_main((void *)sdl_main_addr);

  so_finalize(&hidapi_mod);  so_flush_caches(&hidapi_mod);
  so_finalize(&sdl2_mod);    so_flush_caches(&sdl2_mod);
  so_finalize(&openbor_mod); so_flush_caches(&openbor_mod);
  debugPrintf("== so_finalize ok; running init_arrays ==\n");

  tls_setup_guard();
  so_execute_init_array(&hidapi_mod);
  so_execute_init_array(&sdl2_mod);
  so_execute_init_array(&openbor_mod);
  so_free_temp(&hidapi_mod);
  so_free_temp(&sdl2_mod);
  so_free_temp(&openbor_mod);
  debugPrintf("== init_arrays done ==\n");

  jni_init();

  void *cls = jni_activity_class();
  if (e_JNI_OnLoad) { debugPrintf(">> JNI_OnLoad...\n"); e_JNI_OnLoad(fake_vm, NULL); }
  debugPrintf(">> nativeSetupJNI...\n");
  e_nativeSetupJNI(fake_env);
  if (e_audioSetupJNI) e_audioSetupJNI(fake_env);
  if (e_controllerSetupJNI) e_controllerSetupJNI(fake_env);
  // Register the Switch pad as a real SDL joystick (device 0): OpenBOR links
  // SDL_JoystickOpen/GetAxis/GetButton/GetHat directly, and SDL_NumJoysticks()
  // stays 0 -- so those calls never see anything -- until a device has gone
  // through nativeAddJoystick. onNativeKeyDown/onNativeKeyUp alone (below)
  // only reach whatever separately treats keyboard-sourced input as a pad.
  if (e_nativeAddJoystick) {
    e_nativeAddJoystick(fake_env, cls, JOY_DEVICE_ID, jni_new_string("Switch Controller"),
                         jni_new_string("Switch Controller"), 0, 0, 0,
                         0xFFFFFFFF, 2, 0x3, 1);
    debugPrintf(">> nativeAddJoystick done\n");
  }
  debugPrintf(">> resolved: onNativeTouch=%p onNativeKeyDown=%p onNativePadDown=%p\n",
              (void *)e_onNativeTouch, (void *)e_onNativeKeyDown, (void *)e_onNativePadDown);
  debugPrintf(">> onNativeSurfaceCreated...\n");
  e_onNativeSurfaceCreated(fake_env, cls);
  // Must happen before onNativeResize/onNativeSurfaceChanged (see
  // e_nativeSetScreenResolution's declaration comment above): those read
  // the globals this call is the only thing that ever writes. format=1 is
  // Android's PixelFormat.RGBA_8888, matching the RGBA8888 EGL config
  // eglChooseConfig actually picks (confirmed via egl_shim.c's logging).
  if (e_nativeSetScreenResolution)
    e_nativeSetScreenResolution(fake_env, cls, screen_width, screen_height,
                                 screen_width, screen_height, 1, 60.0f);
  debugPrintf(">> nativeSetScreenResolution(%d,%d)%s\n", screen_width, screen_height,
              e_nativeSetScreenResolution ? "" : " -- NOT FOUND");
  if (e_onNativeResize) e_onNativeResize(fake_env, cls);
  if (e_onNativeSurfaceChanged) e_onNativeSurfaceChanged(fake_env, cls);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  // 0x3B (not 0x2C): the priority band libnx documents as enabling
  // *preemptive* round-robin scheduling on cores 0..2. This thread runs
  // OpenBOR's whole game loop and everything it spawns (imports.c pins those
  // to the same band); without it, this thread and a same-priority worker
  // thread that never blocks/yields could starve each other with no crash.
  if (R_FAILED(threadCreate(&s_sdl_thread, sdl_thread_fn, NULL, NULL, 4 * 1024 * 1024, 0x3B, -2)))
    fatal_error("Could not create the SDL thread.");
  threadStart(&s_sdl_thread);

  if (e_nativeResume) e_nativeResume(fake_env, cls);

  int s_focused = 1, s_paused = 0;
  uint64_t loop_iters = 0;
  debugPrintf(">> entering main loop, initial appletGetFocusState()=%d (InFocus=%d)\n",
              appletGetFocusState(), AppletFocusState_InFocus);
  // Adaptive CPU boost, same technique as the NBA Jam Switch wrapper this
  // loader is descended from: cpu_boost(1) at the top of main() only covers
  // the very first load, and ApmCpuBoostMode_FastLoad doesn't stay latched
  // for the process's whole life -- OpenBOR's menu->game load (sprite/PNG
  // decode, CPU-bound, coinciding with heavy pak reads) and the in-game
  // level loads that follow it were running at normal clocks by the time
  // they actually happened, well after that first boost had lapsed. Poll
  // pak_bytes_total() (libc_shim.c's full-file pak cache, already tracking
  // every byte served) each iteration instead: >64KB served since the last
  // 16ms tick means the pak is being actively read (a real loading screen,
  // not idle gameplay), so re-boost.
  //
  // The idle timeout before dropping back to normal started at the same
  // ~2s (120 ticks) tuning nbajam_nx uses for its OBB reads, but per-line
  // elapsed-time logging (every debugPrintf call, see util.c) showed
  // repeating ~0.3-0.5s gaps during the heavy character-loading stretch
  // with NO pak reads at all in between -- e.g. "full-file cache REUSED"
  // immediately followed several hundred ms later by the next lookup,
  // nothing logged in the gap itself. That's this custom OpenBOR build
  // compiling each character's own script, CPU-only work the full-file
  // cache (a fix for I/O volume) does nothing for and that a 2s idle
  // timeout doesn't survive: real boost-transition logging showed the
  // clock dropping to Normal for 3-12s stretches *during* this same
  // loading window, mid-compile, before the next burst of pak reads
  // re-triggered FastLoad. 900 ticks (~14.4s) comfortably bridges every
  // gap actually observed without meaningfully delaying the drop back to
  // normal once gameplay (genuinely idle pak I/O) starts.
  uint64_t last_pak = pak_bytes_total();
  int boosted = 1; // starts boosted from cpu_boost(1) at the top of main()
  int idle_frames = 0;
#define BOOST_IDLE_TICKS 900
  while (appletMainLoop() && !s_sdl_thread_done) {
    AppletFocusState fs = appletGetFocusState();
    int focused = (fs == AppletFocusState_InFocus);
    if (focused != s_focused) {
      debugPrintf("[focus] changed: %d -> %d (fs=%d) at iter %llu\n", s_focused, focused, fs, (unsigned long long)loop_iters);
      s_focused = focused;
      if (!s_focused && !s_paused && e_nativePause) { e_nativePause(fake_env, cls); s_paused = 1; }
      else if (s_focused && s_paused && e_nativeResume) { e_nativeResume(fake_env, cls); s_paused = 0; }
    }
    if (s_focused) poll_input();
#if VERBOSE_IO
    if (loop_iters % 120 == 0) debugPrintf("[main] loop heartbeat iter=%llu focused=%d\n", (unsigned long long)loop_iters, s_focused);
#endif
    // ~5s samples (312 ticks @ 16ms) regardless of boost state -- a prior
    // capture only ever logged one clock reading for the whole run (right
    // at the very first cpu_boost(1) call, before any transition ever
    // happened to log a second one), showing 1020 MHz -- the Switch's
    // normal, unboosted clock. Sampling continuously through the loading
    // window settles whether that was a one-off (e.g. read before the mode
    // change actually took hold) or the clock genuinely never moves.
    if (loop_iters % 312 == 0) log_cpu_clock_periodic();
    uint64_t cur_pak = pak_bytes_total();
    // g_video_playing counts as activity too: confirmed on-device (clock
    // log) that a playing video reads so little through the pak-tracked fds
    // that this heuristic saw it as idle and dropped to Normal (1020MHz) for
    // 20+ seconds *during* intro.webm's own decode -- software VP8 decode is
    // real, sustained CPU work this pak-throughput signal was never able to
    // see at all. Root cause behind reported choppy video audio and a
    // freeze consistently landing a few seconds after the last video ends.
    if (cur_pak - last_pak > 64 * 1024 || g_video_playing) {
      idle_frames = 0;
      if (!boosted) {
        cpu_boost(1);
        boosted = 1;
        debugPrintf("[boost] -> FastLoad at iter %llu (pak served=%lluMB)%s\n",
                    (unsigned long long)loop_iters, (unsigned long long)(cur_pak >> 20),
                    g_video_playing ? " (video playing)" : "");
      }
    } else if (boosted && ++idle_frames > BOOST_IDLE_TICKS) {
      cpu_boost(0);
      boosted = 0;
      debugPrintf("[boost] -> Normal at iter %llu (pak served=%lluMB)\n",
                  (unsigned long long)loop_iters, (unsigned long long)(cur_pak >> 20));
    }
    last_pak = cur_pak;
    loop_iters++;
    svcSleepThread(16 * 1000 * 1000);
  }

  if (e_nativeQuit) e_nativeQuit(fake_env, cls);
  else if (e_onNativeSurfaceDestroyed) e_onNativeSurfaceDestroyed(fake_env, cls);

  // give SDL_main a moment to unwind (flush saves, tear down audio) before we
  // pull the process out from under it.
  for (int i = 0; i < 120 && !s_sdl_thread_done; i++)
    svcSleepThread(16 * 1000 * 1000);

  threadClose(&s_sdl_thread);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
