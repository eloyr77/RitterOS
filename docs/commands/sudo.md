# sudo: run a command as root, with your own password (users in /system/config/sudo)

```
sudo COMMAND [ARGUMENTS...] | -k
```

Runs COMMAND as root after asking for your own password (it is not shown
as you type). (68e) A correct password counts for 5 minutes in the same
session (the same terminal, or SSH connection): a sudo meanwhile asks
nothing. `sudo -k` forgets it at once, so the next sudo asks again. The password is one line: given on standard input
(a script's), the lines after it are COMMAND's input (68d). Only the users named in `/system/config/sudo` may,
a file root writes:

```
users = anna, bob
nopassword = anna
```

(41h-7) The users also in `nopassword` are never asked for a password: for
a program that deploys or administers without a person (an agent over
SSH); whoever has such a user's login has root, so root lists only those
it means to.

Names are apart by commas or spaces. Anyone else is refused without a
password asked. Every new system has the file from the start (since step
63n: a fresh disk, the stick, and every `osinstall`), with comments and
`users =` empty, so only root may until root adds names; without the file
too only root may. Root itself is never asked. `sudo` came with step 50c.

COMMAND is a program, not a shell built-in: a name with a `/` is that
file, a bare name is looked for in `/system/<arch>/bin`, then
`/system/<arch>/apps` (42b: inside an app bundle, `NAME.program/NAME`). It runs in the current folder with the terminal, in
sudo's own process group (so Ctrl-C reaches both), and `sudo` ends with
its exit code. A wrong password is answered after a pause of 2 seconds, so
guessing stays slow. Every use, allowed or refused, is written to the log
`login` (`/system/logs/login.log`), with control characters in the
command's name shown as `?`.

`sudo` is a setuid program: root's, with mode `4755`, so it runs as root
whoever starts it, and `getuid()` tells it who that was.

## Exit status and messages

- The command's own exit code, once it ran.
- `1`, with one of:
  - `sudo: NAME may not run commands as root (not in
    /system/config/sudo)`: NAME is not in the list.
  - `sudo: wrong password`.
  - `sudo: COMMAND: REASON`: the command could not be started (`No such
    file or directory`, `Permission denied`).
  - `sudo: it does not run as root (it must be root's, with mode 4755)`:
    sudo's own file lost its setuid bit or owner.
  - `sudo: no password given`: the input ended at the password prompt.
- `2`: no COMMAND: `usage: sudo COMMAND [ARGUMENTS...] | -k`.

## Examples

```
ritteros:/home/anna$ sudo useradd carl
Password for anna:
useradd: carl: user ID 1002, home /home/carl; set a password with: passwd carl
ritteros:/home/anna$ sudo id
Password for anna:
uid=0(root) gid=0
ritteros:/home/bob$ sudo reboot
sudo: bob may not run commands as root (not in /system/config/sudo)
```

## See also

[`su`](su.md), [`id`](id.md), [`passwd`](passwd.md), [`chmod`](chmod.md)
(the setuid bit), [`useradd`](useradd.md)
