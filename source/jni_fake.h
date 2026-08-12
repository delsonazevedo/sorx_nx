/* jni_fake.h -- fake JNI environment for libhidapi.so / libSDL2.so /
 * libopenbor.so's org.libsdl.app.* JNI surface.
 * MIT license; see LICENSE. */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

void jni_init(void);

// class handed to native setup / GetMethodID calls, and the fake Activity
// instance object returned by getContext()/SDL_AndroidGetActivity().
void *jni_activity_class(void);
void *jni_activity_object(void);

void *jni_new_string(const char *s);

#endif
