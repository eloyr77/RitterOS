# Settings: the system's and your settings, in a window

```
Settings [set FILE KEY [VALUE]]
```

Shows the settings in a window, a page each (since 69c, roadmap item 47).
The pages are listed on the left; the chosen one is beside the list. What a
page saves applies at once where it can. `Settings` is a windowed app: open
it from the desktop menu or `Owl` (it is in the apps folder), or run it
under the window system (`windowd &` first).

Each page writes only the lines it changes: a file's comments and its
other lines stay, also lines of yours the page does not show. A file that
is missing starts as its default text. Most pages apply at once: through
the program that owns the setting (`net up`, `displays set`, `volume`,
`power settings`...), or because its reader looks again by itself (the
firewall's `guardd`, the elves' `elvesd`, `sudo`, the time zone); what a
program says shows under the page's button.

## System

The **System** tab (since 69d) holds the machine's settings, the files in
`/system/config`. Its list shows eight pages at a time: the wheel, its
scroll bar or the arrow keys reach the others.

- **Display**: the screen's mode; the **Outputs** the graphics driver
  knows (with their monitors, as `displays` lists them), and beside them
  the **Modes** the chosen output's monitor lists (69h): choose both, then
  **Save** sets it at once (`displays set`) and at every start (its line
  in `/system/config/displays`). The outputs are those of the J1900's
  Intel and the laptops' AMD graphics; without a driver the list is empty
  and the firmware's mode stays.
- **Sound**: the sound server's master volume and mute (`volume`).
- **Memory**: swap, as `swap` shows it; **On** and **Off** turn it on or
  off (also for the next starts), **Make** makes the file of the size given
  (`4G`) and turns swap on.
- **Date & time**: the time zone, chosen from the offsets in use (69h; the
  list opens at the one set): `zone` in `/system/config/time`, programs
  follow at once;
  and **Set** sets the clock (`date -s`, local time `YYYY-MM-DD
  HH:MM[:SS]`).
- **Elves**: the elves as `elf` lists them; choose one, then **Start**,
  **Stop** or **Restart** (`elf start|stop|restart NAME`).
- **Service names**: the names programs may use for ports
  (`/system/config/services`); a row chosen fills **Name** and **Port**,
  **Set** adds or changes, **Remove** takes it out.
- **Power**: what the power button does (switch off, nothing, sleep), what
  closing the lid does (nothing, sleep), and the minutes without input
  until the screen goes off, a panel dims and the machine sleeps (0:
  never). Saved to `/system/config/power` and applied at once (`power
  settings`).
- **Processor**: the processor's clock (performance, balanced, power
  saving, top, firmware; AMD processors with CPPC), saved to
  `/system/config/cpu` and applied on every core at once (`power clock`;
  firmware only from the next start).
- **Network cards** (69e): the cards' files in `/system/config/network`;
  choose a card for its address (`dhcp` or `ADDRESS/N`), gateway and DNS
  server, and the same for IPv6 (`auto`, `dhcp`, `off` or `ADDRESS/N`); an
  empty field takes its line out. **Save** writes them and runs `net up`
  (and `elf restart dhclient` when the card changes between DHCP and a
  fixed address).
- **Routing**: forwarding between the cards, the private cards
  (masquerade) and the cards the default route prefers, each card a check
  box (69h; in the cards' order) (`net forward`, `net masquerade`, `net
  default`: at once and kept).
- **Virtual cards**: the taps and bridges made at every start; **Add a
  tap** (for a user, or root), **Add a bridge** of the ports checked (the
  wired cards and taps; 69h), **Remove the chosen** (`net tap`, `net
  bridge`). A tap added appears among the ports when the app opens again.
- **Card names**: the cards' fixed names by their MAC address; **Forget
  the chosen** (`net forget`: the name is given again).
- **Wi-Fi**: the country (two letters, `00` for the world) and power saving
  (**Save**: `wifi country`, the radio file, `wifid` started again); the
  saved networks, **Forget the chosen** (`wifi forget`); **Scan** (69h)
  lists the networks in range (name, security, channel, signal: `wifi
  scan`, not while joined), and choosing one fills its name and **Open**;
  a passphrase, then **Join** (`wifi join`: saved, `wifid` joins it). The
  passphrase shows while it is typed and is cleared after.
- **Firewall**: the rules (`guard`'s numbered ones); **Add** a rule as
  `guard add` takes it (`allow in tcp 8080 from 192.168.0.0/24`), **Remove
  the chosen**; and what goes without a matching rule, in, out and
  forwarded: drop (checked) or allow. The list follows once `guardd` has
  loaded the change (within 2 s).
- **Users** (69f): the users; **Add the user** with its name and password
  (twice) (`useradd`, `passwd -S`), **Set the chosen's password**; who may
  use `sudo` and whom `sudo` never asks (`/system/config/sudo`'s `users`
  and `nopassword`).

**Read only, Unlock.** The System pages are root's to change: anyone else
sees "Read only" under the list, and a save says `Only root may change
the system's settings: Unlock them with your password first (a user of
sudo).` (69f, the user's) **Unlock** with your own password: `sudo -S
true` checks it (only the users in `/system/config/sudo` may), and from
then on every change runs through `sudo -S` with it, logged as any sudo
command; a file's line goes through `Settings set` (below). The app keeps
the password while it runs and forgets it at its end. **Sound** follows
the sound server's rule instead: root and the user at the console. A
password shows while it is typed (libwidget's fields show their text) and
is cleared after.

## User

The **User** tab holds your own settings, the files in your `~/.config`:

- **Appearance**: the theme (dark or light), the accent colour (RRGGBB,
  empty for the theme's) and the text size of programs that draw scalable
  text. Saved to `~/.config/windows`; the desktop, the frames and menus
  and every open window change at once (windowd is asked to read the
  settings again).
- **Mouse & touchpad**: the time for a double click, and steady clicks (a
  touchpad's slow creep around a click is held). At once.
- **Window system**: what opens when the window system starts (`startup`:
  from its next start; typed, or chosen from the apps beside it, 69h) and how many frames a second it draws (the screen's
  own rate, or a number from 10 to 1000: at once).
- **File types**: which program a double click in `Owl` (and `open FILE`)
  starts for a file, by its ending. Choose a row to edit it (69h: the
  program also from the apps and programs beside the table); **Set** adds
  or changes the ending's program, **Remove** takes it out. Each writes
  `~/.config/filetypes` at once.
- **Account** (69f): your own password: the present one (not for root),
  the new one twice, **Change** (`passwd -S`).
- **At login**: what happens when you log in at this machine's own screen
  and keyboard (not over SSH): **Start in** the Console or the Window
  system, and the items run at login, in order (typed, or chosen from the
  apps and programs beside them, 69h; **Add**, **Up**, **Down**,
  **Remove**, then **Save**). It writes `~/.config/login`: with Window
  system its first lines are `windowd &` and `wsend wait`, so the window
  system starts first and every item right after it, while it runs; an
  item that is a windowed app gets `&` (it opens once the window system
  answers). The file's comments are kept. It counts from your next login.

## The command

`Settings set FILE KEY [VALUE]` (69f) is the app's one-line writer as a
command: KEY's line of FILE made `KEY = VALUE`, or taken out without
VALUE; the file's other lines and comments stay, a missing file starts as
its default text. FILE must be in `/system/config`; it writes with the
rights of whoever runs it (root, or `sudo Settings set ...`):

```
ritteros:/$ Settings set /system/config/time zone +01:00
settings: /system/config/time: zone = +01:00
```

## Exit status and messages

- `0`: the window was closed.
- `1`: `settings: cannot open a window (is windowd running?)`, or
  `settings: cannot open a window: the screen is another user's`; for
  `set`: `settings: FILE: REASON`.
- `2`: other arguments: `usage: Settings [set FILE KEY [VALUE]]`; `set`'s
  file outside `/system/config`.

While it runs it prints `settings: started`. A page that could not save
says why under its button (`Not saved: FILE: REASON`, or the program's
own message).

## Examples

```
ritteros:/$ windowd &
ritteros:/$ Settings &
settings: started
```

## See also

[`windowd`](windowd.md), [`Owl`](Owl.md), [`login`](login.md),
[`open`](open.md), [`wsend`](wsend.md), [`displays`](displays.md),
[`volume`](volume.md), [`swap`](swap.md), [`date`](date.md),
[`elf`](elf.md), [`power`](power.md), [`net`](net.md), [`wifi`](wifi.md),
[`guard`](guard.md), [`useradd`](useradd.md), [`passwd`](passwd.md),
[`sudo`](sudo.md)
