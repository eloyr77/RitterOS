# RitterOS security audit (overnight)

Scope: the whole code base, with the threat model of (a) a local
unprivileged user running arbitrary programs and (b) an attacker on the
LAN or the internet reaching the machine through the firewall's default
open ports (icmp, tcp 22 for sshd, udp 53 for dnsd). Method: reading the
code, a host fuzzer of the disk parsers (fat.c, exfat.c, gpt.c) under
AddressSanitizer and UBSan, and a non-destructive reachability check of
the live J1900: ping, the SSH banner, and one benign DNS
query were answered; no destructive test was run against it.

The parsing of untrusted data is solid. The fuzzer ran about 11 million
rounds in all (FAT12/16/32 and GPT 2 and 1 million each, exFAT 4 million),
each round mutating only the sectors a clean parse reads and then
mounting, walking, reading every file and making a few changes, with no
crash, out-of-bounds access or undefined behaviour reported by
AddressSanitizer or UBSan.

The fuzzer found no out-of-bounds
access in the FAT, exFAT or GPT parsers; the network stack's header
bounds and reply sizes were read and found correct, with no memory
disclosure. The real problems were logic: privilege checks that were
missing, and denial of service in TCP, dnsd and the firewall.

## Fixed in this pass (step 49a; each built and, where noted, tested)

1. **Any user could signal any process** (was critical). SYS_KILL did not
   compare the caller's user with the target's, so an unprivileged user
   could end root's processes, login and the elves, and (through login's
   end) reach the passwordless rescue console. `process_signal` now
   refuses a process of another user unless the caller is root.
   Tested: `perm` ("signal the root process 1: Operation not permitted").

2. **Any user could bind a privileged port.** SYS_BIND had no check below
   1024, so a user who ended dnsd or sshd (finding 1) could take port 53
   or 22. Ports below 1024 are now root's only. Tested: `perm`.

3. **A recursive instant copy handed over unreadable files.** `cp -r` of a
   directory the user could read, but holding files only root could read,
   gave the user owned, readable copies (the copy belongs to its maker).
   The copy now checks read (and search) on every file and directory in
   the tree; too deep a tree falls back to a byte copy, which keeps the
   usual checks. Tested: `perm` ("copy a directory holding a root-only
   file: Permission denied").

4. **A peer MSS of 0 hung TCP and could crash the kernel** (was critical,
   remote, default port 22). A SYN with MSS option 0 made the send loop
   produce zero-length segments forever under the TCP mutex (all
   networking stalls), and a crafted sequence drove the congestion window
   to 0 and then a divide-by-zero (#DE) on x86. The peer MSS is now
   clamped to at least 64, which keeps every derived value (segment size,
   ssthresh, cwnd) non-zero.

5. **dnsd could be made to loop forever on one packet** (was critical,
   remote, default port 53). A spoofed packet with source = the machine's
   own address (or 127.x) and destination port 53 was accepted, answered,
   and the answer sent back to "itself" through loopback, forever. Two
   fixes: the kernel drops martian sources from a card (127/8, our own
   addresses, the broadcast, multicast/reserved; 0.0.0.0 still allowed for
   DHCP), and dnsd ignores DNS responses (QR=1) instead of answering them.

6. **dnsd cache could be poisoned, and its ports were predictable.** An
   upstream answer was accepted on a 16-bit ID match alone. dnsd now
   accepts the answer only from the server it asked, and only when the
   answer's question matches the one sent; UDP ephemeral ports and the IP
   identification field are now random (they counted up before).

7. **A SYN flood disabled TCP until a reboot** (was high, remote). Only
   `user_closed` slots were freed, so half-open connections that timed out
   (never accepted) stayed forever; 32 of them filled every slot. Such a
   dead, never-accepted connection is now freed by the TCP timer.

## Still open (recommendations; several are the owner's design decision)

These were read and judged real, but their fixes are larger changes or
involve policy choices, so they are left for review rather than changed
overnight.

The six items the user asked to fix next (49b-49f) are now done; they are
marked below. The passwordless rescue console is to be discussed after
them.

- ~~**Replies wait for ARP in the receiving card's thread**~~ (fixed, step
  49c). The kernel learns an on-link sender's MAC from the Ethernet frame of
  each IP packet (so a reply to a host that just sent to us needs no ARP),
  and a thread delivering a received frame (`thread->net_rx`) queues a
  still-unresolved reply as forwarding does (`hold`) instead of blocking.
  Tested by the `security` test (a ping from an on-link source we never
  answer ARP for is answered at once).

- ~~**The firewall's flow table (256) can be flushed by a cheap flood**~~
  (fixed, step 49d). A TCP flow opens only on a SYN; eviction prefers a free
  or expired slot, then the oldest inbound-opened flow over any outbound
  one, and inbound flows never take more than `FLOWS - OUT_RESERVE` (64)
  slots, so a flood of inbound flows cannot evict the machine's own outbound
  flows. Tested by the `security` test (a flood between a UDP request and
  its reply does not drop the reply).

- ~~**No SYN cookies and no cap on half-open connections per listener**~~
  (fixed, step 49f: true SYN cookies, the user's choice). A listener keeps
  no state for a SYN; it answers a SYN-ACK whose sequence number encodes the
  peer's MSS (coarsely) and a keyed hash of the four-tuple and a coarse
  time, and builds the connection only when a valid ACK returns. A SYN flood
  costs no memory and no slot. Tested by the `security` test. Since 65e-25
  the cookie also carries the SYN's window scale and SACK offers (5 bits,
  covered by the hash), so its hash is 24 bits as Linux's (27 before) and
  the coarse time 1 bit (the hash tells the last two counts apart).

- ~~**FIN_WAIT_2 has no timeout**~~ (fixed, step 49e): a connection in
  FIN_WAIT_2 is freed after `FIN_WAIT2_MS` (5 s, shortened as TIME_WAIT is).
  Tested by the `security` test.

- ~~**Orphaned accepted connections** (backlog full at the final ACK)~~
  (fixed, step 49f): with SYN cookies the connection is built at the final
  ACK, and if the accept backlog is full then it is refused with a reset
  rather than left orphaned.

- ~~**No RFC 5961 checks**~~ (fixed, step 49e): a RST resets only on the
  exact next sequence number (in-window gets a challenge ACK, out-of-window
  is dropped), and a segment whose ACK is unacceptable (acknowledging data
  never sent, or absurdly old) gets a challenge ACK and is otherwise
  ignored, so a blind segment cannot move the window or inject data on a
  guessed sequence alone. Tested by the `security` test (a bad-ACK data
  injection is rejected).

- **ARP** (hardened in step 49g; one piece left). The table learns from
  every ARP on the LAN, and since 49c also an on-link sender's MAC from the
  Ethernet frame of an IP packet. Two cheap defences were added in 49g:
  entries are now *sticky* (a fresh entry is not overwritten by a different
  MAC, from an ARP reply, a gratuitous ARP, or a passively learned frame),
  so a flood cannot hijack a neighbour like the gateway; and an echo request
  to a broadcast or multicast address is not answered (anti-smurf). Still
  left (the user deferred it to the console's system-wide security): pinned
  or static entries from a config (strongest for the gateway), and a rogue
  DHCP server's values are only lightly checked. The remaining costs of a
  simple stack.

- ~~**Forwarded packets bypass the firewall**~~ (fixed, step 49h, the
  user's, completing item 12). A packet forwarded between two cards is now
  checked against a `forward` chain (`guard_check_forward` in ipv4.c's
  forward(), after the route is known), with its own default `forward =
  drop` and rules that name the incoming and outgoing cards (`in ethN`,
  `out ethN`); a forwarded connection opens a flow matched both ways.
  Forwarding itself was already a toggle (`net forward`, off by default).
  Tested by the `route` test.

- ~~**The descriptor table has no lock**~~ (fixed, step 49b): two threads of
  one process doing open/pty/socket at once could collide on a slot, and a
  UDP socket closed by one thread while another waited on it could use freed
  memory. A per-process lock (`p->fds`) now makes claiming, installing and
  closing a slot atomic (a claimed slot is `FILE_RESERVED` until installed,
  so open()'s file-system work still runs without the lock), and a blocking
  socket call holds a reference across itself (`socket_grab`), as the pty
  path already did with its hold count. Tested by `threads fds`.

## Design points that are the owner's call (not bugs)

- Anyone at the console has root in effect: Ctrl-D at `login:` gives the
  passwordless rescue console (a deliberate choice so far).
- dnsd is an open resolver (answers everyone), by the item-12 decision
  that access is the firewall's job, not each elf's.
- There are no limits on processes, pseudo-terminals or a program's
  memory (its bss size).
- No sticky bit on world-writable directories; background jobs may write
  to the console; SYS_DEVICE and SYS_REPORT are open to every user.

## Window-system review (overnight, after step 15g)

A review of the window-system code (steps 15a-15g) found no memory
corruption, privilege escalation, double-free/use-after-free, or kernel
crash reachable by an unprivileged program: every user pointer goes through
`copy_user`, and the size/stride/count math done before it is bounded by the
screen size or `SHARE_MAX`. `DISPLAY_FLUSH` bounds, the display owner/root
checks (and PID-reuse of the owner, closed by `display_process_ended` on
exit), the shbuf reference counting, and the PS/2 setup ordering were all
checked and are sound.

Fixed in the review (each built and tested):
- ~~`local_accept` out-of-memory path stranded the client~~ (fixed): on an
  `endpoint_new` failure it dropped the server reference but never hung up,
  so the client blocked forever with no EOF/EPIPE. It now calls `hang_up`
  first, like the listener-drain path.
- ~~`term` mis-encoded code points >= U+10000~~ (fixed): a 4-byte UTF-8
  character was re-emitted through the 3-byte path. The font is BMP-only, so
  such a code point is now shown as U+FFFD.
- ~~`windowd` never reaped its per-connection threads~~ (fixed): each client
  connection spawned a thread that became a zombie on close (this OS has no
  `thread_detach`), so after ~14 connections `windowd` could open no more
  windows. The accept loop now `thread_join`s the finished ones. Proven by
  the `wmany` test (21 windows in turn; fails at the 14th without the fix).

Open items (the owner's call; left untouched deliberately):
- **Local-socket names are a global, unauthenticated namespace** (32 slots,
  `kernel/localsock.c`). A program can bind `windowd`'s well-known name
  before the server (spoofing clients), or bind 32 names to deny `listen`
  system-wide. Not corruption; a trust-boundary/DoS gap. Needs a design
  decision: per-user/per-session namespacing, a reserved privileged name
  class, and/or a per-process listener cap. (Same class as the passwordless
  rescue console above.)
- **No system-wide cap on pinned shared-buffer memory** (`kernel/shbuf.c`).
  One process is bounded (16 fds x 16 MiB), but N processes can exhaust
  physical RAM. Needs a global page budget (a policy decision).
- ~~**A hung display owner leaves the keyboard captured with no escape**~~
  (fixed, step 15h). Ctrl-Alt-Delete is caught in the kernel's keyboard code
  (`press` in `kernel/keyboard.c`) while a program owns the display, and ends
  that program (`display_escape` in `kernel/process.c`, SIGKILL). The owner's
  own teardown (`display_process_ended`) gives the screen back to the text
  console in the owner's thread context, so nothing repaints from the keyboard
  interrupt and even a hung owner (spinning in user space, caught at the next
  timer tick) is escapable. Root-only to acquire, as before. Tested by the
  `wescape` test (Ctrl-Alt-Delete brings the console back).

Noted, not security (perf/correctness; need a design choice or a test that
the screenshot suite cannot provide, so not fixed unattended):
- ~~`windowd` recomposites and flushes the whole screen on every pointer
  event~~ (done, step 15i). windowd now redraws into the back buffer (RAM) but
  flushes only the rectangle that changed — the damaged window, the box around
  the moved pointer, or the union of a raised/moved window and the one that
  lost focus. The final pixels are unchanged (the existing windowd2/termwin
  screenshots verify them), at a fraction of the framebuffer traffic; it was
  the fix for the lag with two windows redrawing at once.
- `windowd`'s input thread forwards an event to a client socket captured
  under the lock but written after releasing it; if that client exits and
  its fd is reused at that instant, a stray message could reach the wrong
  connection (low probability; a correct fix wants a per-window generation
  check or a non-blocking send).
- `SHARE_MAP` maps up to 4096 pages with the `p->fds` spinlock held
  (interrupts off), which can drop timer ticks for a 16 MiB buffer; and the
  AArch64 `paging_user_unmap` does a full TLB flush per page (a by-VA flush
  would be cheaper, but a wrong by-VA invalidation could leave a stale
  mapping to a freed page, which the tests would not catch -- so the safe,
  slower full flush was kept).

## Second review (overnight, after step 16b; fixes in step 49k)

Three read-only reviews ran beside a full test run: local rights (setuid,
`su`/`sudo`, sessions and the console, local sockets, `~/.config`, `/dev`),
Wi-Fi frames from the air (the kernel's `wlan.c`, the MT7921 driver, `wifid`),
and the rest of the newer code (pipes, shared buffers, the display, sound,
the image decoders, the shell). The Wi-Fi and "rest" reviews stopped early
(an API error), after reporting what follows; their remaining areas are
listed under "Not reviewed this time". Each fix has a test that was seen to
fail with the old code.

Fixed (step 49k):

- **A user logged in over SSH could take the console's keyboard** (high). A
  command run by `sshd` without a terminal (on pipes) shares sshd's terminal,
  the console. A program could start a new session from there (spawn with
  `SPAWN_NEW_SESSION`, its input a pipe): that session kept the console as
  its terminal, its leader could make itself the console's foreground
  (`tcsetpgrp`, or `SPAWN_FOREGROUND`), and then passed the "at the console"
  check for the screen, getting every key typed at the machine (a password
  at `login:`), while the console's shell read an end of input and logged
  out (repeated: the passwordless rescue console). Now a new session gets a
  terminal only from its input: a pseudo-terminal's slave, or the console
  itself handed down by a process on the console; any other has none
  (`no_terminal` in `struct process`): no foreground, no echo or raw mode,
  not "at the console". Test `noterminal`. (65e-6: any process of the session the
  terminal belongs to may hand the foreground on, as POSIX; a member of
  another session sharing the console still may not, `noterminal shared`.)
- **An instant copy kept the setuid bit while making the copy the copier's**
  (medium). Root's `cp -r` of a user's folder holding a 4755 file of theirs
  made a root-owned 4755 file: a root shell for that user. `copy_object`
  (kernel/rfs_write.c) now drops the bit whenever it gives the copy an owner,
  as `chown` does. Test `setuid` (root's copy of anna's 4755 file).
- **`sudo` wrote what a caller gave it into root's log as it was** (low to
  medium): a line break in a command's name became a second line of the
  caller's choosing in `/system/logs/login.log`, also for users not allowed
  to use sudo. `su` and `sudo` now write control characters as `?`. Test
  `noterminal sudo`.
- **Any user could use the speakers; `soundd` took any user's sound**
  (low). `/dev/pcmN` opens only for root or the user at the console (the
  user of the console's foreground job, wherever their program runs, e.g. in
  `term`); `soundd` refuses a program of another user (`ritteros_peer_uid`,
  as `windowd`). Test `noterminal` (as user 1000 while root is at the
  console: refused).
- **`su` and `sudo` allowed guesses as fast as PBKDF2 ran** (low): a wrong
  password now costs 2 s.
- **One unprotected frame from anyone in range dropped the Wi-Fi link**
  (high). A broadcast EAPOL "message 1" with the highest replay counter
  passed the kernel (unprotected group frames were refused only after the
  EAPOL branch), `wifid` took it for the access point renewing the pairwise
  key, waited for a message 3 that never came and left the network, even
  with WPA3 and protected management frames. The kernel now drops EAPOL to
  a group address (key messages are always to one station); a failed
  renewal keeps the keys it has (an access point that really renewed them
  and got no answer sends the station away, and that rejoins). Test
  `wifi_join_shell_test wpa2 ritterpass forge`.
- **Message 3 of the 4-way handshake was used without checking its MIC**
  (medium): its receive counter for broadcasts (RSC: a high one makes every
  broadcast look replayed, a low one lets old ones in again) and its replay
  counter came from an unauthenticated frame, and any other EAPOL frame in
  its place ended the handshake. `wifid` now takes only a message 3 with
  the right key information bits, a replay counter not below message 1's,
  the same ANonce and a MIC that checks with the KCK, and ignores others
  while the wait lasts; every EAPOL-Key frame's lengths are checked whole
  (`key_frame`). Test `wifi_join_shell_test wpa2 ritterpass badm3` (a forged
  message 3 before the real one).
- **Unauthenticated frames moved the burst (A-MPDU) reorder window**
  (medium): a plain QoS frame numbered far ahead made the window jump and
  every genuine frame after it "old" (about 2,000 frames lost per forged
  one, under WPA3 too). `receive_data` now buffers only what `deliver_data`
  would take as genuine (the card's opened frames; the layer's after their
  MIC checks, the packet number still checked in order); duplicate detection
  records a sequence number per TID and only after the frame proved
  genuine. Test `forge` (a forged frame far ahead, then four genuine pings
  out of order: all answered).

Still open (reported, not changed):

- **Channel switch announcements in unprotected beacons** are followed
  (`channel_switch` in wlan.c) also with MFP. Beacons are never protected in
  WPA2/WPA3 without beacon protection (802.11-2020's optional BIGTK), and
  Linux follows them too; a forged one moves the station to another channel
  until it notices the access point is gone. A check that the new channel
  is in the card's list and that the access point's next beacons agree would
  narrow it.
- **The global local-socket names** (from 49a) remain: any user can bind
  `/run/windowd` or `/run/sound` first. Clients and servers now check each
  other's user (windowd since 15k, soundd since 49k), so a squatter can deny
  the service but not impersonate it.

Fixed afterwards (step 49l), from a third, read-only review of the areas
the stopped reviews had not reached (lengths and indexes in what is parsed),
each checked against the code first:

- **Shared buffers' addresses grew without a limit** (medium-high, local):
  each `SHARE_MAP` took the next addresses and none came back, so a program
  mapping buffers over and over reached mmap's window (48 TiB), where an
  anonymous mapping's page fault took a buffer's page for its own and
  `munmap` then freed a page still in use. `SHARE_MAP` now refuses
  (ENOMEM) a buffer that would reach the window.
- **sshd before login** (medium, remote): a KEXINIT shorter than its fixed
  part made a length wrap below zero (reads far past the packet), and one
  sized to fill the exchange hash's buffer let the shared secret be written
  past its end (37 bytes). A KEXINIT must now hold its fixed part and be at
  most 16 KiB (OpenSSH's are about 1.5 KB), and the hash input is checked
  before the secret is added. Only the connection's own process was at risk.
- **Pipes and pseudo-terminals: freed under a call** (medium, local, one
  program's threads): a read or write took its hold after a `kmalloc` that
  may sleep, outside the descriptor lock, so another thread closing both
  ends meanwhile freed the object first; `spawn` copied descriptors the
  same way. Holds are now atomic and taken under the descriptor lock
  (`hold_end`, as `socket_grab` since 49b), and spawn copies under it.
- **MT7921** (medium, from the firmware file and the chip): a country rule
  longer than the two-page command buffer was copied into it (commands are
  now refused when larger), a record length near 2^32 wrapped the walk of
  the rules, a capability length did the same, and a receive descriptor's
  length larger than the 2 KiB buffer the chip was given was used as is
  (now taken as empty).
- **wifid** (medium-low, from the air): a short RSN element in a beacon was
  read as far as its declared length, past the frame; authentication and
  association answers were read without a length check (a short frame left
  the previous one's bytes in place) and from any sender. Elements are now
  read only as far as they and the frame go, and answers are taken only
  from the access point, to us, long enough (`reply`); SAE checks the
  group (19) and the confirm's status.
- **HD Audio**: a codec's node range past 255 reached another codec's
  address bits in the verbs; it is cut at 255.

These were checked with the existing tests (pipes, terminals, SSH, the
SDK's deploy over SSH, shared buffers and windows, sound, Wi-Fi joins) on
both architectures; the MT7921 changes only limit lengths and need the
laptop to be seen working. Still open from that review (low): the MT7921's
firmware file headers are read without checking each read, a late command
reply could reach the next command, and wifid's SAE uses hash-to-element
with status 0 where access points may expect 126 (an interoperability
question for a WPA3 network; the user's is WPA2).

Covered since by the third review: the MT7921's receive and event
parsing, wifid's and SAE's parsing, sshd's pre-login parsing, pipes,
shared buffers, `SYS_DISPLAY`'s rectangles (found sound), the HDA codec's
answers, and mmap's argument checks (found sound). The image decoders were
fuzzed when written (25a-25d).
