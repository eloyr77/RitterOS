# The commands

A page for every program of RitterOS (on the machine: `man NAME`; `man`
lists them, `man -k WORD` searches). The install guide is
[install.md](../install.md), the user's manual [manual.md](../manual.md).
This list is made by `python3 scripts/doccheck.py --index`.

| Program | What it does |
|---|---|
| [`ar`](ar.md) | make and read archives of object files (static libraries) |
| [`as`](as.md) | assemble |
| [`attr`](attr.md) | list, show, set or remove a file's attributes |
| [`awk`](awk.md) | scan and process text: patterns, fields, arrays (POSIX's awk) |
| [`basename`](basename.md) | a path's last part (without SUFFIX) |
| [`cat`](cat.md) | show files, or what is typed until Ctrl-D |
| [`cc`](cc.md) | compile C programs |
| [`chmod`](chmod.md) | set permissions (MODE in octal, e.g. 644) |
| [`chown`](chown.md) | give files to another user (root only) |
| [`clear`](clear.md) | clear the screen |
| [`cp`](cp.md) | copy a file (-r: a directory), instantly; -p: keep times and modes |
| [`cpus`](cpus.md) | list the processor's cores |
| [`cu`](cu.md) | a terminal on a serial line (~. leaves) |
| [`cut`](cut.md) | parts of each line |
| [`date`](date.md) | the date and time (local or -u UTC) |
| [`dhclient`](dhclient.md) | the DHCP client, an elf: addresses for the cards set to dhcp (and DHCPv6) |
| [`dirname`](dirname.md) | a path without its last part |
| [`disks`](disks.md) | the disks and their partitions; changes to a partition table (root) |
| [`displays`](displays.md) | list the screen, graphics devices, outputs and monitors; set a mode; what the last sleep changed |
| [`dnsd`](dnsd.md) | names (DNS) for this machine and others, an elf: answers kept by their time to live |
| [`drivers`](drivers.md) | list the kernel's modules (drivers): loaded, not needed or refused |
| [`echo`](echo.md) | print the words |
| [`edit`](edit.md) | edit a text file (Ctrl-S saves, Ctrl-Q quits) |
| [`elf`](elf.md) | the elves: programs kept running (/system/config/elves/; changes: root) |
| [`elvesd`](elvesd.md) | keeps the elves running (login starts it): /system/config/elves/ |
| [`env`](env.md) | show the environment variables, or run a command with some set |
| [`false`](false.md) | do nothing, unsuccessfully (exit code 1) |
| [`fetch`](fetch.md) | get a web page or file over HTTP or HTTPS (-i: with the headers) |
| [`find`](find.md) | search for files by name and attributes |
| [`format`](format.md) | make a new, empty RitterFS (or FAT32, exFAT) on a partition or disk (root) |
| [`grep`](grep.md) | print the lines that match a pattern |
| [`guard`](guard.md) | the firewall's rules: show them, change them |
| [`guardd`](guardd.md) | the firewall's elf: loads /system/config/guard into the kernel |
| [`head`](head.md) | the first lines (or bytes) of files |
| [`hello`](hello.md) | the first RitterOS program: where it is in memory |
| [`ibrowser`](ibrowser.md) | iBrowser: show web pages (http, https, HTML files) in a window |
| [`id`](id.md) | the user and group you are |
| [`imageview`](imageview.md) | show an image (PPM, BMP, PNG, JPEG) in a window |
| [`js`](js.md) | run JavaScript (a prompt without a file) |
| [`kill`](kill.md) | ask processes to end (SIGNAL: TERM, KILL, INT, HUP, USR1, USR2 or a number) |
| [`ld`](ld.md) | link programs |
| [`ldd`](ldd.md) | the shared libraries a program needs, and whether they are all there |
| [`ln`](ln.md) | make a link: a second name for a file, or with -s a symbolic link |
| [`logd`](logd.md) | the logs' keeper, an elf: writes /system/logs/NAME.log every 10 s |
| [`login`](login.md) | log in: a user's name and password, then their shell |
| [`ls`](ls.md) | list directories (default: the current one) |
| [`make`](make.md) | build targets from a Makefile's rules (POSIX make with common GNU make) |
| [`man`](man.md) | the manual: a command's page, a topic (install, manual), or a search |
| [`mem`](mem.md) | free and usable memory |
| [`mkdir`](mkdir.md) | create directories (-p: with their parents) |
| [`mount`](mount.md) | list mounted volumes, or mount one (root only) |
| [`mv`](mv.md) | move or rename a file or directory |
| [`net`](net.md) | the network cards; up: apply /system/config/network/ (root); names, forwarding, NAT, the default route, virtual cards, bridges |
| [`netnamesd`](netnamesd.md) | an elf: records new network cards in /system/config/network/names |
| [`notify`](notify.md) | show a notification on the window system's screen (needs windowd) |
| [`open`](open.md) | open files and folders with their apps (needs windowd) |
| [`osinstall`](osinstall.md) | install RitterOS on another disk, the whole disk (root; it asks) |
| [`owl`](owl.md) | a graphical file browser (needs windowd) |
| [`passwd`](passwd.md) | set a user's password (root only so far) |
| [`pci`](pci.md) | list the PCI devices |
| [`ping`](ping.md) | send ICMP echo requests and show the round trips |
| [`power`](power.md) | what the firmware offers for power management: sleep states, devices |
| [`poweroff`](poweroff.md) | switch the machine off |
| [`printf`](printf.md) | print the arguments in a format |
| [`ps`](ps.md) | list the processes |
| [`readlink`](readlink.md) | print what a symbolic link names (-f: the full path) |
| [`reboot`](reboot.md) | restart the machine |
| [`reel`](reel.md) | play a video (RVID) in a window |
| [`rm`](rm.md) | remove files (-r: directories with their contents; -f: missing ones are no error) |
| [`rmdir`](rmdir.md) | remove empty directories |
| [`sed`](sed.md) | edit a stream: replace, delete and insert lines by a script |
| [`sh`](sh.md) | the shell |
| [`sleep`](sleep.md) | wait (fractions like 0.5; s, m, h, d) |
| [`snapshot`](snapshot.md) | list, take or delete file system snapshots (/.snapshots/NAME) |
| [`sort`](sort.md) | the lines in order |
| [`soundd`](soundd.md) | the sound mixer: lets several programs play at once |
| [`ssh`](ssh.md) | log in on another machine (SSH) |
| [`sshd`](sshd.md) | the SSH server: log in from other machines (root) |
| [`su`](su.md) | become another user (root if none) with their password |
| [`sudo`](sudo.md) | run a command as root, with your own password (users in /system/config/sudo) |
| [`swap`](swap.md) | show swap; make the swap file /system/swap, turn swap on or off |
| [`tail`](tail.md) | the last lines (or bytes) of a file; -f: follow it |
| [`tar`](tar.md) | make (c), take apart (x) or list (t) archives of files and folders |
| [`tee`](tee.md) | copy the input to the output and into files |
| [`term`](term.md) | a terminal in a window (FONT of /system/fonts); with a command, runs it (needs windowd) |
| [`test`](test.md) | check files, strings and numbers (exit code 0: true); also [ ... ] |
| [`threads`](threads.md) | list the kernel's threads |
| [`touch`](touch.md) | make empty files, or give files the time now (or another) |
| [`tr`](tr.md) | change, delete or squeeze characters |
| [`true`](true.md) | do nothing, successfully (exit code 0) |
| [`tune`](tune.md) | play a sound file (WAV) through the speakers |
| [`uname`](uname.md) | the system's name, release, commit and machine |
| [`uniq`](uniq.md) | runs of equal lines once |
| [`unmount`](unmount.md) | unmount volumes, e.g. before pulling a stick (root only) |
| [`uptime`](uptime.md) | time since the kernel's timer started |
| [`usb`](usb.md) | list the USB devices |
| [`useradd`](useradd.md) | add a user with a home directory (root only) |
| [`versions`](versions.md) | a file's versions in the snapshots and now |
| [`wc`](wc.md) | count lines, words and bytes |
| [`wifi`](wifi.md) | the wireless link (saves networks; wifid joins them) |
| [`wifid`](wifid.md) | join and hold the wireless link (the elf; see the wifi command) |
| [`windowd`](windowd.md) | the window server: shows windows on the screen (windowd & starts it) |
| [`wsend`](wsend.md) | send keyboard and pointer input to a headless windowd (for tests) |
| [`wshot`](wshot.md) | save the window system's screen as a picture (needs windowd) |
| [`xargs`](xargs.md) | run a command with the input's words as arguments |
