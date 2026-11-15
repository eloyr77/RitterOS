# soundd: the sound server: lets several programs play at once

```
soundd
```

The sound device `/dev/pcm0` takes one program at a time (another gets
"Device or resource busy"). `soundd` lets several play at once: it listens
on the local socket `/run/sound`, and each program that connects there
streams 48 kHz, 16-bit, stereo PCM (the same samples it would write to
`/dev/pcm0`). `soundd` adds all of them up, sample by sample, and plays the
sum (cut to the 16-bit range where it overflows). Each program's samples go
in about 43 ms (2,048 frames) ahead of what is playing, and a program that
falls behind skips forward instead of playing the past. A program that
closes its connection is left out of the mix once what it sent has played.
Arguments are ignored.

(68w) `soundd` opens `/dev/pcm0` when a program's sound comes and lets it go
a second after the last sound, so another program may have the device
alone meanwhile; while `soundd` holds it, it keeps the device's own buffer
short (about 43 ms). A device another program holds, or none, plays
nothing: the mix goes on at real time and is not heard.

A program that starts its connection with the four bytes `RSND` sends
messages, each an 8-byte header, its type and its length in bytes (two
32-bit numbers, the machine's byte order), then those bytes:

| Type | Message | |
|---|---|---|
| 1 | sound | the bytes are PCM (whole frames) |
| 2 | delay | `soundd` answers with 4 bytes: the bytes this program sent that are not yet heard (its part of the mix and the device's buffer before it), for keeping sound with pictures |
| 3 | reset | this program's sound not yet played is taken out of the mix (pause, seek); its next sound starts about 43 ms ahead |
| 4 | volume | (68x) 8 bytes: the master volume, 0-100, and mute, 0 or 1 (each -1: as it is); `soundd` answers with the two as they are now (the program `volume` sends it) |

Any other program's bytes are PCM, as before 68w.

(68x) The master volume turns the whole mix down (the amplitude times the
volume / 100 cubed, as PulseAudio's scale), or mutes it. `soundd` reads it
from `/system/config/sound` at its start (`volume = 0-100`, `mute = yes` or
`no`; written with its defaults when missing) and changes those lines when
a program sets it. Any program it plays for may.

`soundd` is an elf on every machine: login makes its file,
`/system/config/elves/soundd`, when it is missing, and `elvesd` keeps it
running. It also runs as a background job, `soundd &` (when the elf is
off). The speakers belong to the person at the machine: run as the elf
(root's), it mixes root's programs and the console user's (the user whose
job has the console: their programs wherever they run, in `Term` too) and
refuses a user logged in from elsewhere, as `/dev/pcm0` does (68z, the
user's: sound only for local and root users; it asks the kernel at each
connection, SYS_SOUND's `SOUND_MAY_PLAY`). Started by a user, it mixes only
that user's and root's programs and refuses others (since step 49k).
The players `tune` and `Reel` play through `soundd` when it runs (68y;
`tune` ends once `soundd` played it all, `Reel` drops its sound with a
reset when it stops), else through `/dev/pcm0` themselves.

## Exit status and messages

- `soundd: mixing on /run/sound`: it is ready.
- `soundd: refused a program of user UID (this is user UID's sound)`: a
  program of another user connected; it is closed, and `soundd` goes on.
- `soundd: refused a program of user UID (not the user at the console)`:
  (68z) the elf's, for a user logged in from elsewhere; the same.
- `soundd: /dev/pcm0: REASON`: the device could not be opened when sound
  came (`Device or resource busy`: another program has it; `No such file
  or directory` on a machine without an audio controller); said once a
  reason, and `soundd` goes on.
- `1`: `soundd: cannot listen on /run/sound: REASON` (another `soundd`
  runs already), `soundd: no pipe: REASON` or `soundd: no output thread`.

Otherwise `soundd` runs until it is stopped.

## Examples

```
ritteros:/$ elf
elvesd: running (process 2)
soundd           on   running (process 9)     soundd
ritteros:/$ elf stop soundd
elf: soundd off (elvesd stops it within 2 s)
ritteros:/$ soundd &
[1] 14
soundd: mixing on /run/sound
```

## See also

[`volume`](volume.md), [`elf`](elf.md), [`tune`](tune.md), [`Reel`](Reel.md),
[`elvesd`](elvesd.md), [`kill`](kill.md)
