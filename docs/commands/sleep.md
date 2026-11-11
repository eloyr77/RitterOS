# sleep: wait (fractions like 0.5; s, m, h, d)

```
sleep SECONDS...
```

Waits that long, then ends. SECONDS is a number of seconds, also with a
fraction (`sleep 0.5`, `sleep 1.25`), and may end in a unit: `s` seconds,
`m` minutes, `h` hours, `d` days (`sleep 2m`, `sleep 1.5h`). Several are
added up: `sleep 1m 30s` waits 90 seconds. Ctrl-C ends it
at once. `sleep` has
been in RitterOS since step 51d (POSIX's whole seconds; fractions and the
units are common extras, as in GNU's).

## Exit status and messages

- `0`: the time has passed.
- `2`: no SECONDS (`usage: sleep SECONDS...`), or one that is not a time
  (`sleep: 5x: not a time`; negative numbers too); then it does not wait
  at all.

## Examples

```
ritteros:/$ sleep 2
ritteros:/$ sleep 0.25
ritteros:/$ sleep 1m 30s
ritteros:/$ sleep 10 && echo done
done
ritteros:/$ while true; do date; sleep 60; done
ritteros:/$ sleep soon
sleep: soon: not a time
```

## See also

[`date`](date.md), [`uptime`](uptime.md), [`sh`](sh.md) (`&` runs it in
the background), [`kill`](kill.md)
