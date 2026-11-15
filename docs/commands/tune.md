# tune: play a sound file (WAV) through the speakers

```
tune FILE
```

Plays FILE through the sound device `/dev/pcm0`, the one you hear (the
speakers or the headphone jack of the machine's first audio controller),
and ends when it is over: when it has been heard. (68y) It plays through
the sound server `soundd` when it runs (an elf on every machine), so it
plays beside other programs' sound and under the master volume
(`volume`); without `soundd` it writes to `/dev/pcm0` itself. `tune` came with the media suite (pictures,
sound and video, read by `libimage`, `libaudio` and `libvideo`).

FILE is a WAV file of PCM samples: 8 or 16 bits, mono or stereo, at any
sample rate. The output plays 48 kHz, 16-bit stereo, so `tune` converts
what is needed first: another rate is resampled to 48 kHz, and mono is
played on both channels. The whole file is read into memory before it
plays.

The speakers belong to the person at the machine: `tune` plays for root
and for the user at the console (in `Term` too), through `soundd` or
`/dev/pcm0` alike (68z); a user logged in over SSH gets "Permission
denied". Without `soundd` one program at a time has the device (another
gets "Device or resource busy"). A
machine without an audio controller has no `/dev/pcm0`. `tune` takes no options; Ctrl-C stops
it.

## Exit status and messages

- `tune: playing FILE (N frames)` when it starts (a frame is one sample for
  each channel), `tune: done` at the end: `0`.
- `1`, the reason on its standard error:
  - `tune: FILE: No such file or directory`, or `tune: FILE: Invalid
    argument` (not a WAV file, or one it cannot play: compressed, 24 or 32
    bits, more than two channels);
  - `tune: resample: REASON`;
  - `tune: /dev/pcm0: No such file or directory` (no audio controller),
    `tune: /dev/pcm0: Permission denied` (you are not at the console) or
    `tune: /dev/pcm0: Device or resource busy` (another program has it,
    only without `soundd`);
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

[`soundd`](soundd.md), [`Reel`](Reel.md) (videos with sound),
[`Imageview`](Imageview.md), [`cat`](cat.md) (`cat sound.raw > /dev/pcm1`
plays raw samples on another device)
