/* jni_fake.c -- fake JNI environment implementing the JNI surface libhidapi.so
 * / libSDL2.so / libopenbor.so call: org.libsdl.app.SDLActivity's Context/
 * storage/clipboard/text-input getters, SDLAudioManager's audioOpen/
 * audioWrite*Buffer/audioClose (bridged to audio.c), plus the generic
 * object/string/array machinery every JNI-calling native library needs.
 *
 * Object model, local-reference registry and JNIEnv/JavaVM tables are taken
 * from the SOTN/NBA-Jam Switch wrapper lineage (Andy Nguyen / fgsfds); the
 * method/field dispatch below targets the real, public SDL2 android-project
 * JNI contract instead of a game-specific one. MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "jni_fake.h"
#include "audio.h"
#include "util.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  singleton, never freed
};

enum obj_kind {
  K_GENERIC = 0, K_ACTIVITY, K_CONTEXT, K_ASSETMGR, K_FILE, K_SURFACE,
};

typedef struct { uint32_t tag; int kind; int fd; int64_t off; int64_t len; char str[300]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; char name[64]; char sig[112]; } FakeID;

// ---------------------------------------------------------------------------
// local reference registry
// ---------------------------------------------------------------------------

#define MAX_LOCALS 8192
#define MAX_FRAMES 32
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS) locals[locals_top++] = ref;
    mutexUnlock(&locals_lock);
  }
  return ref;
}

static void free_ref(void *ref) {
  if (!ref) return;
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    default: break; // TAG_ID / TAG_CLASS are never freed
  }
}

static void delete_local(void *ref) {
  if (!ref) return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) { locals[i] = locals[--locals_top]; free_ref(ref); break; }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// constructors
// ---------------------------------------------------------------------------

static void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return reg_local(s);
}

static void *make_file(const char *path) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  o->kind = K_FILE;
  strncpy(o->str, path, sizeof(o->str) - 1);
  return reg_local(o);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING) return s->utf;
  return "";
}

// class + context singletons (never freed)
static FakeObject g_cls_generic, g_activity, g_context, g_assetmgr, g_surface, g_displaymetrics;

static void init_singletons(void) {
  g_cls_generic.tag = TAG_CLASS;
  g_activity.tag = g_context.tag = g_assetmgr.tag = g_surface.tag = TAG_CLASS;
  g_activity.kind = K_ACTIVITY;
  g_context.kind  = K_CONTEXT;
  g_assetmgr.kind = K_ASSETMGR;
  g_surface.kind  = K_SURFACE;
  // getDisplayDPI() returning NULL (unhandled in call_object, falling to
  // its default) turned out not to be the harmless stub it looked like:
  // SDL2's Android video backend calls this to seed nativeWidth/
  // nativeHeight (used to size the destination rect for the ONE
  // SDL_RenderCopy that actually draws real game/menu content, as opposed
  // to the backdrop) -- a NULL DisplayMetrics meant that rect came out
  // degenerate, so that RenderCopy silently drew nothing every single
  // frame while the backdrop (a separate, unconditional dst=NULL
  // RenderCopy) kept drawing fine, which is exactly the "renders forever,
  // only ever paints an all-zero backdrop texture" on-device trace showed.
  // get_int_field/get_float_field already answer widthPixels/heightPixels/
  // densityDpi/xdpi/ydpi correctly for ANY object regardless of identity,
  // so this only needs to be non-NULL, not a specifically-typed object.
  g_displaymetrics.tag = TAG_CLASS;
}

void *jni_activity_class(void)   { return &g_cls_generic; }
void *jni_activity_object(void)  { return &g_activity; }
void *jni_new_string(const char *s) { return jni_make_string(s); }

// ---------------------------------------------------------------------------
// method/field ID pool
// ---------------------------------------------------------------------------

#define MAX_IDS 256
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig ? sig : ""))
      return &id_pool[i];
  if (id_count >= MAX_IDS) return &id_pool[0];
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig ? sig : "", sizeof(id->sig) - 1);
#if VERBOSE_JNI
  debugPrintf("[jni] id: %s %s\n", name, sig ? sig : "");
#endif
  return id;
}

// ---------------------------------------------------------------------------
// method dispatch (by name; instance + static share handlers)
// ---------------------------------------------------------------------------

// pulls the first vararg as a jshortArray-shaped FakePriArray and feeds it to
// the audio bridge; shared by the void- and int-returning audioWriteShortBuffer
// overloads (SDL2 has used both across versions).
static void dispatch_audio_write(va_list va) {
  FakePriArray *a = va_arg(va, void *);
  if (a && a->tag == TAG_PRIARR && a->data && a->len > 0)
    sorx_audio_write_s16((const int16_t *)a->data, a->len);
}

static juint call_int(void *recv, const char *name, va_list va) {
  FakeObject *o = recv;
#if VERBOSE_JNI
  debugPrintf("[jni] call_int: %s\n", name);
#endif
  if (!strcmp(name, "clipboardHasText"))           return 0;
  if (!strcmp(name, "isScreenKeyboardShown"))      return 0;
  if (!strcmp(name, "hasScreenKeyboardSupport"))   return 0;
  if (!strcmp(name, "shouldMinimizeOnFocusLoss"))  return 0;
  if (!strcmp(name, "showTextInput"))              return 0;
  if (!strcmp(name, "isAndroidTV"))                return 0;
  if (!strcmp(name, "isChromebook"))               return 0;
  if (!strcmp(name, "isDeXMode"))                  return 0;
  if (!strcmp(name, "isTablet"))                   return 0;
  if (!strcmp(name, "sendMessage"))                return 1;
  if (!strcmp(name, "messageboxShowMessageBox"))   return -1;
  if (!strcmp(name, "getManifestEnvironmentVariables")) return 0;
  if (!strcmp(name, "getIntExtra"))                return 0;
  if (!strcmp(name, "audioWriteShortBuffer")) { dispatch_audio_write(va); return 0; }
  if (!strcmp(name, "getFd"))                      return o ? (juint)o->fd : 0;
  (void)va;
  return 0;
}

static juint call_long(void *recv, const char *name, va_list va) {
  FakeObject *o = recv;
  if (!strcmp(name, "getStartOffset"))    return o ? (juint)o->off : 0;
  if (!strcmp(name, "getDeclaredLength")) return o ? (juint)o->len : 0;
  if (!strcmp(name, "getLength"))         return o ? (juint)o->len : 0;
  (void)va;
  return 0;
}

static void *call_object(void *recv, const char *name, va_list va) {
  FakeObject *o = recv;
#if VERBOSE_JNI
  debugPrintf("[jni] call_object: %s\n", name);
#endif
  if (!strcmp(name, "getContext"))          return &g_context;
  if (!strcmp(name, "getAssets"))           return &g_assetmgr;
  if (!strcmp(name, "getActivityInstance")) return &g_activity;
  if (!strcmp(name, "getNativeSurface"))    return &g_surface;
  if (!strcmp(name, "getDisplayDPI"))       return &g_displaymetrics;
  if (!strcmp(name, "getExternalStorageState")) return jni_make_string("mounted");
  if (!strcmp(name, "clipboardGetText"))    return jni_make_string("");
  if (!strcmp(name, "getMessage"))          return jni_make_string("");
  if (!strcmp(name, "getPackageName"))      return jni_make_string(ANDROID_PKG);
  // Context dirs: OpenBOR/SDL2 route all of these into config.data_root /
  // config.save_root -- our fake File objects just carry the path string.
  if (!strcmp(name, "getFilesDir"))          return make_file(config.save_root);
  if (!strcmp(name, "getCacheDir"))          return make_file(config.save_root);
  if (!strcmp(name, "getExternalFilesDir"))  { (void)va_arg(va, void *); return make_file(config.data_root); }
  if (!strcmp(name, "getExternalCacheDir"))  return make_file(config.save_root);
  if (!strcmp(name, "getAbsolutePath") || !strcmp(name, "getCanonicalPath") || !strcmp(name, "getPath"))
    return jni_make_string(o ? o->str : "");
  if (!strcmp(name, "audioOpen")) {
    int rate = (int)va_arg(va, juint);
    /* audioFormat */ (void)va_arg(va, juint);
    int channels = (int)va_arg(va, juint);
    int frames = (int)va_arg(va, juint);
    int out_rate, out_channels, out_frames;
    sorx_audio_open(rate, channels, frames, &out_rate, &out_channels, &out_frames);
    int *result = calloc(4, sizeof(int));
    result[0] = out_rate;
    result[1] = 2; // android.media.AudioFormat.ENCODING_PCM_16BIT (SDL2 switches on
                    // this exact constant; ENCODING_DEFAULT/1 fell through to "unsupported"
                    // and made SDL2 immediately close the device it just opened)
    // The actual channel COUNT (1 or 2), not a boolean "isStereo" flag as
    // previously assumed here: disassembly of this exact libSDL2.so's
    // Android_JNI_OpenAudioDevice shows it copies this array slot straight
    // into SDL_AudioSpec.channels with no boolean-to-count translation
    // (`ldr w8,[x0,#8]` / `strb w8,[x22,#6]`) -- the byte landing in
    // spec.channels IS this value, verbatim. Sending 1 for stereo made SDL2
    // (and OpenBOR, reading the confirmed spec back) believe the device was
    // MONO, which more than doubled the per-sample-pair buffer length
    // Android_JNI_OpenAudioDevice derives from channels*samples a few
    // instructions later; our bridge then folded pairs of what were really
    // consecutive mono samples into fake (L,R) stereo frames, halving every
    // buffer's true duration -- exactly the confirmed-on-device "audio
    // plays back at ~2x pitch/speed, both music and SFX equally" symptom
    // (a channel-count bug, not a sample-rate or timing one).
    result[2] = out_channels;
    result[3] = out_frames;
    return make_pri_array_adopt(result, 4, sizeof(int));
  }
  (void)va;
  return NULL;
}

static void call_void(void *recv, const char *name, va_list va) {
  (void)recv;
#if VERBOSE_JNI
  debugPrintf("[jni] call_void: %s\n", name);
#endif
  if (!strcmp(name, "audioWriteShortBuffer")) { dispatch_audio_write(va); return; }
  if (!strcmp(name, "audioClose"))            { sorx_audio_close(); return; }
  (void)va; // clipboardSetText / setOrientation / setActivityTitle / cursors /
            // minimizeWindow / pollInputDevices / pollHapticDevices / haptics:
            // all no-ops.
}

static float call_float(void *recv, const char *name, va_list va) {
  (void)recv; (void)va;
  if (!strcmp(name, "getRefreshRate")) return 60.0f;
  return 0.0f;
}

// ---------------------------------------------------------------------------
// field access
// ---------------------------------------------------------------------------

static juint get_int_field(void *obj, FakeID *id) {
  FakeObject *o = obj;
  // Report the same API level our .so's were built against (NDK "for Android
  // 21"): the safest floor, least likely to steer SDL2 into a newer-OS-only
  // code path we haven't implemented.
  if (!strcmp(id->name, "SDK_INT"))     return 21;
  if (!strcmp(id->name, "descriptor"))  return o ? (juint)o->fd : 0;
  if (!strcmp(id->name, "widthPixels")) return (juint)screen_width;
  if (!strcmp(id->name, "heightPixels"))return (juint)screen_height;
  if (!strcmp(id->name, "densityDpi"))  return 160;
  return 0;
}

static float get_float_field(void *obj, FakeID *id) {
  (void)obj;
  if (!strcmp(id->name, "xdpi") || !strcmp(id->name, "ydpi")) return 160.0f;
  if (!strcmp(id->name, "density") || !strcmp(id->name, "scaledDensity")) return 1.0f;
  return 0.0f;
}

static void *get_object_field(void *obj, FakeID *id) {
  (void)obj;
  if (!strcmp(id->name, "WINDOW_SERVICE")) return jni_make_string("window");
  return NULL;
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) { (void)env; (void)name; return &g_cls_generic; }
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return &g_cls_generic; }
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls; return get_id(name, sig);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES) frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result) free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS) locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id->name, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id->name, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, call_object)
CALL_VARIADIC(j_CallIntMethod, juint, call_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, call_int)
CALL_VARIADIC(j_CallLongMethod, juint, call_long)
CALL_VARIADIC(j_CallFloatMethod, float, call_float)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); call_void(recv, id->name, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; call_void(recv, id->name, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// strings
static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }
static juint j_GetStringLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// arrays
static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakePriArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR)) return a->len;
  return 0;
}
static void *j_NewByteArray(void *env, int len) { (void)env; return make_pri_array_adopt(calloc(len ? len : 1, 1), len, 1); }
static void *j_NewShortArray(void *env, int len) { (void)env; return make_pri_array_adopt(calloc(len ? len : 1, 2), len, 2); }
static void *j_NewIntArray(void *env, int len) { (void)env; return make_pri_array_adopt(calloc(len ? len : 1, 4), len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return make_pri_array_adopt(calloc(len ? len : 1, 4), len, 4); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// fields
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls; return get_id(name, sig);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) { (void)env; return get_int_field(obj, id); }
static juint j_GetLongField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return 0; }
static float j_GetFloatField(void *env, void *obj, FakeID *id) { (void)env; return get_float_field(obj, id); }
static void *j_GetObjectField(void *env, void *obj, FakeID *id) { (void)env; return get_object_field(obj, id); }

// direct byte buffers
static void *j_NewDirectByteBuffer(void *env, void *addr, int64_t cap) {
  (void)env; (void)cap; return addr;
}
static void *j_GetDirectBufferAddress(void *env, void *buf) { (void)env; return buf; }
static int64_t j_GetDirectBufferCapacity(void *env, void *buf) { (void)env; (void)buf; return 0; }

// misc
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n; return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls; (void)init;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) return a->items[idx];
  return NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) a->items[idx] = val;
}
static void j_void1(void *env) { (void)env; }
static juint j_unimplemented(void) { return 0; }

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[256];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);
  init_singletons();

  for (int i = 0; i < 256; i++) env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetIntField;            // GetStaticBooleanField (int-width ok)
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField (SDK_INT)
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[178] = (void *)j_NewShortArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[229] = (void *)j_NewDirectByteBuffer;
  env_table[230] = (void *)j_GetDirectBufferAddress;
  env_table[231] = (void *)j_GetDirectBufferCapacity;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
