# volume: the master volume of the sound server (0-100)

```
volume [N | +N | -N | mute | unmute]
```

The master volume of everything played through the sound server `soundd`
(68x): every program's sound, mixed, is turned down by it before it goes to
the speakers. Without an argument `volume` says what it is; with one it
changes it and says what it is now.

- `volume`: what it is, `volume 80`, or `volume 80 (muted)`.
- `volume N`: set it to N, from 0 (nothing) to 100 (as the programs sent
  their sound).
- `volume +N`, `volume -N`: up or down by N, within 0-100.
- `volume mute`: nothing heard, the volume kept; `volume unmute`: heard
  again at that volume.

The scale follows the ear, as PulseAudio's percentages do: the sound's
amplitude is (N / 100) cubed, so 50 is an eighth of it, about 18 dB
quieter. `soundd` keeps the volume in `/system/config/sound` (`volume =`
and `mute =`, changed line by line) and starts with it after a restart.
Any program `soundd` plays for may change it. A program that writes to
`/dev/pcm0` itself (while `soundd` is not playing) is not turned down.

## Exit status and messages

- `0`: the volume as it is now was printed.
- `1`: `volume: no sound server at /run/sound: REASON` (`soundd` is not
  running), or `volume: the sound server did not answer (it plays for root
  and the user at the console only)`: (68z) `soundd` refuses the programs
  of a user logged in from elsewhere, as `/dev/pcm0` does.
- `2`: `usage: volume [N | +N | -N | mute | unmute]`: an unknown argument,
  or a number past 100.

## Examples

```
ritteros:/$ volume
volume 100
ritteros:/$ volume 60
volume 60
ritteros:/$ volume -10
volume 50
ritteros:/$ volume mute
volume 50 (muted)
ritteros:/$ cat /system/config/sound | grep -v '^#'
volume = 50
mute = yes
```

## See also

[`soundd`](soundd.md), [`tune`](tune.md), [`elf`](elf.md)
