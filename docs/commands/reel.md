# reel: play a video (RVID) in a window

```
reel [FILE]
```

Plays a video in a window: the frames at the video's rate, and its sound
track through the speakers (`/dev/pcm0`). It reads RVID, RitterOS's own
simple video format, so far. The window takes the video's size and its
title is FILE; the frames are scaled to fit the window with their
proportions kept, centred on a dark grey background, so the window may be
resized, and a video larger than the screen plays in a window that fits it.
When the video ends the window stays open on the last frame. `reel` is a
windowed app: run it under the window system (`windowd &` first), or open a
`.rvid` file in `owl`. It came with the media suite; since 25e it fits its
window to the screen.

Without FILE it opens an empty window of 480x320 with the hint `File >
Open... to play a video`. **File > Open...** brings up the Open dialog
(starting in the folder of the last video, or the current folder) and plays
the video chosen, stopping the one playing. **File > Close**, the close box
or `q` quits.

The sound is resampled to 48 kHz and played in stereo (a mono track on both
sides). `reel` writes to `/dev/pcm0` itself, so it gets no sound while
another program has the device (`soundd`, `tune`), and a machine without an
audio controller plays the frames silently.

## Exit status and messages

- `0`: the window was closed.
- `1`: FILE could not be read, before a window opened: `reel: FILE:
  REASON` (`No such file or directory`, `Invalid argument` for a file that
  is not RVID); or no window: `reel: cannot open a window (is windowd
  running?)`, `reel: cannot open a window: the screen is another user's`.
- `2`: more than one FILE: `usage: reel [FILE]`.

While it runs it prints `reel: started, no video` (started without FILE),
`reel: playing FILE (WIDTHxHEIGHT, N fps, with audio)` when a video
begins, and `reel: done` when it ends. A video chosen with File > Open...
that cannot be read prints `reel: FILE: REASON` and the window stays.

## Examples

```
ritteros:/$ windowd &
ritteros:/$ reel /home/root/clip.rvid
reel: playing /home/root/clip.rvid (320x240, 25 fps, with audio)
reel: done
ritteros:/$ reel
reel: started, no video
```

## See also

[`imageview`](imageview.md), [`tune`](tune.md), [`soundd`](soundd.md),
[`owl`](owl.md), [`windowd`](windowd.md)
