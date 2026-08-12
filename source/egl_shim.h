/* egl_shim.h -- dlopen/dlsym bridge to mesa EGL/GLES.
 * MIT license; see LICENSE. */

#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

void *dlopen_fake(const char *filename, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
char *dlerror_fake(void);

// SDLActivity's real nativeRunMain dlopen()s the game library and dlsym()s
// "SDL_main" itself; since dlopen_fake ignores the requested path, stash the
// address we already resolved from the loaded libopenbor.so here so dlsym_fake
// can hand it back under that name and let SDL2's own thread-spawning code run
// unmodified.
void egl_shim_set_native_main(void *addr);

#endif
