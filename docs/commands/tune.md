# tune: play a sound file (WAV) through the speakers

```
tune FILE
```

Plays FILE through the sound device `/dev/pcm0`, the one you hear (the
speakers or the headphone jack of the machine's first audio controller),
and ends when it is over. `tune` came with the media suite (pictures,
sound and video, read by `libimage`, `libaudio` and `libvideo`).

FILE is a WAV file of PCM samples: 8 or 16 bits, mono or stereo, at any
sample rate. The output plays 48 kHz, 16-bit stereo, so `tune` converts
what is needed first: another rate is resampled to 48 kHz, and mono is
played on both channels. The whole file is read into memory before it
plays.

The speakers belong to the person at the machine: `/dev/pcm0` opens for
root and for the user at the console (in `term` too); a user logged in
over SSH gets "Permission denied". One program at a time has the device;
to let several play at once, run the mixer `soundd`. A machine without an
audio controller has no `/dev/pcm0`. `tune` takes no options; Ctrl-C stops
it.

## Exit status and messages

- `tune: playing FILE (N frames)` when it starts (a frame is one sample for
  each channel), `tune: done` at the end: `0`.
- `1`, the reason on its standard error:
  - `tune: FILE: No such file or directory`, or `tune: FILE: Invalid
    argument` (not a WAV file, or one it cannot play: compressed, 24 or 32
    bits, more than two channels);
  - `tune: resample: REASON`;
  - `tune: /dev/pcm0: No such file or directory` (no audio controller) or
    `tune: /dev/pcm0: Permission denied` (you are not at the console);
  - `tune: playback failed: REASON`.
- `2`: no FILE, or more than one: `usage: tune FILE`.

## Examples

```
ritteros:/$ tune /home/root/bell.wav
tune: playing /home/root/bell.wav (48000 frames)
tune: done
ritteros:/$ tune notes.txt
tune: notes.txt: Invalid argument
```

## See also

[`soundd`](soundd.md), [`reel`](reel.md) (videos with sound),
[`imageview`](imageview.md), [`cat`](cat.md) (`cat sound.raw > /dev/pcm1`
plays raw samples on another device)
