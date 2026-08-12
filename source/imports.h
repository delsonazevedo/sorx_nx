/* imports.h -- shared import table for libhidapi.so / libSDL2.so /
 * libopenbor.so. MIT license; see LICENSE. */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

void sorx_resolve_imports(so_module *mod);

// Supplies the real libSDL2.so SDL_RenderCopy so our interception wrapper
// (registered under that name in dynlib_functions) can forward to it after
// logging. Call once, after libSDL2.so is loaded and finalized.
void imports_set_real_rendercopy(void *fn);

// Same pattern for SDL_GetDesktopDisplayMode -- the actual source of
// video.c's nativeWidth/nativeHeight, patched to fall back to our known
// screen_width/screen_height when the real call comes back degenerate.
void imports_set_real_getdesktopdisplaymode(void *fn);

// Texture create/destroy pooling: video_set_mode() destroys and recreates
// texture_base/texture/buttons every single rendered frame (confirmed via
// SDL_RenderCopy interception and per-texture-id data tracking), so the one
// holding real content never survives long enough to be drawn. These let
// SDL_CreateTexture(FromSurface)_wrap hand back an already-cached texture
// instead of asking the real driver for a new one each time, and
// SDL_DestroyTexture_wrap merely mark a cached one free instead of actually
// releasing it.
void imports_set_real_createtexture(void *fn);
void imports_set_real_createtexturefromsurface(void *fn);
void imports_set_real_updatetexture(void *fn);
void imports_set_real_destroytexture(void *fn);

// SDL_RenderClear/SDL_RenderPresent: establishes the real per-frame call
// sequence directly (SDL2 batches RenderCopy internally and only actually
// flushes to GL at specific points, not synchronously within RenderCopy).
void imports_set_real_renderclear(void *fn);
void imports_set_real_renderpresent(void *fn);

// SDL_CreateThread/SDL_WaitThread: traces this build's own thread lifecycle
// (video decode runs on its own SDL thread) to pinpoint a freeze suspected
// to be a stuck join/wait rather than anything visible in GL/file tracing.
void imports_set_real_createthread(void *fn);
void imports_set_real_waitthread(void *fn);

// SDL_LockMutex/SDL_CondWait: traces (during/shortly after video playback
// only, see video_trace_active()) whether some thread is parked forever on
// a missed condition-variable signal -- thread create/join came back clean,
// this is the next most likely stuck synchronization primitive.
void imports_set_real_lockmutex(void *fn);
void imports_set_real_condwait(void *fn);

#endif
