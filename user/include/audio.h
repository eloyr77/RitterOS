/*
 * libaudio (user/lib/audio.c): decode and encode sound to and from a
 * `struct audio_buffer` of PCM samples in memory, and the sample operations
 * (resample, mix) that a player and libvideo share. WAV (PCM) so far. This
 * library ends at PCM in RAM; playing it needs the sound device /dev/pcm0.
 */
#ifndef RITTEROS_AUDIO_H
#define RITTEROS_AUDIO_H

#include "media.h"

/* Decode a file (or a buffer) into *out (samples malloc'd). 0, or -1 with
 * errno (EINVAL for a format or encoding we do not read). */
int audio_load(const char *path, struct audio_buffer *out);
int audio_load_memory(const void *data, long n, struct audio_buffer *out);

/* Fill *out's rate/channels/format/frames without the samples (samples = 0). */
int audio_info(const char *path, struct audio_buffer *out);

/* Encode *buf to a WAV file. */
int audio_save(const char *path, const struct audio_buffer *buf);

/* Release buf->samples (and zero the struct). Safe on a zero struct. */
void audio_free(struct audio_buffer *buf);

/* A copy resampled to `rate` (linear) into *out. 0 or -1/errno. */
int audio_resample(const struct audio_buffer *src, int rate, struct audio_buffer *out);

/* Add src into dst (same rate/format/channels) scaled by gain, clamped. */
int audio_mix(struct audio_buffer *dst, const struct audio_buffer *src, float gain);

#endif
