# soundd: the sound mixer: lets several programs play at once

```
soundd
```

The sound device `/dev/pcm0` takes one program at a time. `soundd` owns
it and lets several programs play at once: it listens on the local socket
`/run/sound`, and each program that connects there streams 48 kHz,
16-bit, stereo PCM (the same samples it would write to `/dev/pcm0`).
`soundd` adds all of them up, sample by sample, and plays the sum (cut to
the 16-bit range where it overflows). Writing to the device paces it to
real time; each program's samples go in about 43 ms (2,048 frames) ahead
of what is playing, and a program that falls behind skips forward instead
of playing the past. A program that closes its connection is simply left
out of the mix. Arguments are ignored.

`soundd` is meant to run as an elf, kept running by `elvesd`:
`elf add soundd soundd`. It also runs as a background job, `soundd &`.
Run as an elf (root's), it mixes every user's programs. Started by a
user, it mixes only that user's and root's programs and refuses others,
since the speakers belong to the person at the machine (since step 49k).
It came with the media suite (its third audio step); the players (`tune`,
`reel`) still write to `/dev/pcm0` themselves.

## Exit status and messages

- `soundd: mixing on /run/sound`: it is ready.
- `soundd: refused a program of user UID (this is user UID's sound)`: a
  program of another user connected; it is closed, and `soundd` goes on.
- `soundd: /dev/pcm0: REASON`: the device could not be opened (`No such
  file or directory` on a machine without an audio controller,
  `Permission denied` for a user who is not at the console); `soundd`
  still takes connections, but nothing is played.
- `1`: `soundd: cannot listen on /run/sound: REASON` (another `soundd`
  runs already), or `soundd: no output thread`.

Otherwise `soundd` runs until it is stopped.

## Examples

```
ritteros:/$ elf add soundd soundd
ritteros:/$ soundd &
[5]
soundd: mixing on /run/sound
```

## See also

[`elf`](elf.md), [`tune`](tune.md), [`reel`](reel.md),
[`elvesd`](elvesd.md), [`kill`](kill.md)
