/* gpuarena.h -- dedicated contiguous arena for GPU buffers.
 * Ported from cloverpit_nx/pvz_fusion_nx. MIT license; see LICENSE. */

#ifndef __GPUARENA_H__
#define __GPUARENA_H__

// Arms the arena. Call once, after module loading/heap setup is done and
// before the game's own render thread starts making GL calls -- see
// gpuarena.c's own comment on why timing matters here.
void gpua_enable(void);

// Peak/live usage, for logging once frames are flowing.
unsigned gpua_peak_mb(void);
unsigned gpua_live_mb(void);

#endif
