# date: the date and time (local or -u UTC); -s sets the clock

```
date [-u] [-d @SECONDS] [-s TIME] [+FORMAT]
```

Prints the date and time now, in the local time (the zone set in
`/system/config/time`) or, with `-u`, in UTC. `-d @SECONDS` prints another
moment instead of now: SECONDS since the start of 1970 (UTC), as Unix
counts time (`date -d @1700000000`). `+FORMAT` says how it is written, with
`strftime`'s conversions: `%Y` the year, `%m` the month, `%d` the day, `%H`
`%M` `%S` the time, `%a` and `%b` the day's and month's short names, `%j`
the day of the year, `%Z` the zone's name (`UTC`, or `UTC-05:00` for a zone five hours behind), `%%` a percent sign, and the
others of C's `strftime`. Without it the form is `%a %b %e %H:%M:%S %Z
%Y`, as on Unix. It has been in RitterOS since step 51d.

**Setting the clock** (since 68l): `date -s TIME`, as root, sets the clock
to TIME and prints it. TIME is `@SECONDS` (since 1970, UTC) or `"YYYY-MM-DD
HH:MM[:SS]"` (or a date alone, at midnight), in the local time, or in UTC
with `-u`, as Linux's `date -s` takes them. The running clock moves at
once (every program's next look at the time sees it, file times too), and
the firmware's clock is set as well (UEFI's SetTime), so the next start
has the new time; the kernel says so in its log (`clock: set to
2026-11-13 02:40:00 UTC by date (pid 412); the firmware's clock too`, or
`could not be set`: a machine without runtime services, `noruntime`).
RitterOS does not ask the network for the time: a machine whose
firmware's clock is wrong (certificates are refused as not yet valid)
gets it from `date -s` once.

## Options

| Option | Meaning |
|---|---|
| `-u` | UTC instead of the local time |
| `-d @SECONDS` | the moment SECONDS after 1970-01-01 00:00 UTC instead of now (negative numbers are before 1970) |
| `-s TIME` | (68l) set the clock to TIME (`@SECONDS` or `"YYYY-MM-DD HH:MM[:SS]"`), root only |
| `+FORMAT` | write the date as FORMAT says (the last one given counts) |

## Exit status and messages

- `0`: the date was printed.
- `1`: a wrong argument (`usage: date [-u] [-d @SECONDS] [-s TIME]
  [+FORMAT]`; `-d` takes only the `@SECONDS` form), SECONDS that is not a
  whole number (`date: @12x: not @SECONDS`), a result longer than 4,095
  bytes (`date: the text is too long`), a TIME that is none (`date: TIME:
  not @SECONDS or YYYY-MM-DD HH:MM[:SS]`), or a clock not set (`date:
  cannot set the clock: Operation not permitted`: not root).

## Examples

```
ritteros:/$ date
Tue Oct 27 14:05:09 UTC-05:00 2026
ritteros:/$ date -u
Tue Oct 27 19:05:09 UTC 2026
ritteros:/$ date +%Y-%m-%d
2026-10-27
ritteros:/$ date -u -d @1700000000 '+%Y-%m-%d %H:%M:%S'
2023-11-14 22:13:20
```

## See also

[`sleep`](sleep.md), [`uptime`](uptime.md), [`touch`](touch.md) (`-t`
gives a file another time)
