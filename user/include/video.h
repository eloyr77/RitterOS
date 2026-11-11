/*
 * libvideo (user/lib/video.c): read a video as a stream of frames (each a
 * `struct image`) plus an optional audio track (`struct audio_buffer`). The
 * container so far is RVID, a simple own format (a header, the audio PCM, then
 * raw RGB frames) -- real codecs like H.264 are a much larger undertaking; the
 * interface is shaped so they slot in behind it later.
 */
#ifndef RITTEROS_VIDEO_H
#define RITTEROS_VIDEO_H

#include "media.h"

struct video;                           /* opaque: the open file and decode state */

/* Open a video file. 0 with *v set, or -1 with errno (EINVAL: not a video). */
int video_open(const char *path, struct video **v);

/* Its dimensions, frames per second, and whether it has an audio track. */
void video_info(struct video *v, int *w, int *h, int *fps, int *has_audio);

/* Decode the next frame into *frame (RGBA8888, pixels malloc'd); its time in
 * nanoseconds in *pts_ns. 1 with a frame, 0 at the end, -1 on error. */
int video_read_frame(struct video *v, struct image *frame, int64_t *pts_ns);

/* The whole audio track as one buffer (samples malloc'd), if any. 0 or -1. */
int video_audio(struct video *v, struct audio_buffer *out);

void video_close(struct video *v);

#endif
