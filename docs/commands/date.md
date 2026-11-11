# date: the date and time (local or -u UTC)

```
date [-u] [-d @SECONDS] [+FORMAT]
```

Prints the date and time now, in the local time (the zone set in
`/system/config/time`) or, with `-u`, in UTC. `-d @SECONDS` prints another
moment instead of now: SECONDS since the start of 1970 (UTC), as Unix
counts time (`date -d @1700000000`). `+FORMAT` says how it is written, with
`strftime`'s conversions: `%Y` the year, `%m` the month, `%d` the day, `%H`
`%M` `%S` the time, `%a` and `%b` the day's and month's short names, `%j`
the day of the year, `%Z` the zone's name (`UTC`, or `UTC-05:00` for a zone five hours behind), `%%` a percent sign, and the
others of C's `strftime`. Without it the form is `%a %b %e %H:%M:%S %Z
%Y`, as on Unix. `date` only shows the time: it does not set the clock.
It has been in RitterOS since step 51d.

## Options

| Option | Meaning |
|---|---|
| `-u` | UTC instead of the local time |
| `-d @SECONDS` | the moment SECONDS after 1970-01-01 00:00 UTC instead of now (negative numbers are before 1970) |
| `+FORMAT` | write the date as FORMAT says (the last one given counts) |

## Exit status and messages

- `0`: the date was printed.
- `1`: a wrong argument (`usage: date [-u] [-d @SECONDS] [+FORMAT]`; `-d`
  takes only the `@SECONDS` form), SECONDS that is not a whole number
  (`date: @12x: not @SECONDS`), or a result longer than 4,095 bytes (`date:
  the text is too long`).

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
