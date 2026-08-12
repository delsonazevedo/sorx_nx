/* audio.h -- synchronous audio bridge backing SDLAudioManager's JNI contract
 * (audioOpen/audioWriteShortBuffer/audioClose), fed to libnx audout.
 * MIT license; see LICENSE. */

#ifndef __AUDIO_H__
#define __AUDIO_H__

#include <stdint.h>

// Negotiates a format and opens the audout stream (once). Always "accepts"
// the requested rate/channels so SDL2's own audio converter never has to
// engage; the sample-rate conversion to audout's fixed 48 kHz stereo s16
// happens internally in sorx_audio_write_s16.
void sorx_audio_open(int sample_rate, int channels, int desired_frames,
                      int *out_rate, int *out_channels, int *out_frames);

// Blocking write of interleaved s16 PCM (n_samples = frames * channels),
// mirroring AudioTrack.write()'s blocking semantics.
void sorx_audio_write_s16(const int16_t *data, int n_samples);

void sorx_audio_close(void);

#endif
