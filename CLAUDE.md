# CLAUDE.md

Guidance for working on RitterOS, for people and coding agents. `README.md`
describes the system, the roadmap and the history of every step;
`docs/ritterfs.md` is the RitterFS on-disk format; `docs/manual.md` the
user's manual; `docs/commands/NAME.md` each program's page (`man NAME` on
RitterOS, from /system/doc); `docs/install.md` the install guide;
`docs/security-audit.md` the security reviews; `sdk/CLAUDE.md` how
programs are written outside the tree; `drivers/CLAUDE.md` the driver
interface.

RitterOS is a from-scratch operating system for AMD64 PCs (and AArch64),
built and tested on an Apple Silicon Mac in QEMU (q35 UEFI PC with NVMe;
ARM `virt` under HVF), and run on real PCs.

## How work is done

- **Small steps.** Each step (numbered 1, 2a, ..., 65a-4) adds one thing
  that can be seen working. After coding: build, run the tests that touch
  the change on both architectures, add a README history row, and update
  this file only with what a later change must know.
- A step that adds or changes a program updates its page (title and first
  usage line exactly as its PROGRAM_HELP: `python3 scripts/doccheck.py`
  checks, `--index` rewrites docs/commands/README.md) and the manual where
  its topic is; a shell feature updates the manual too.
- **Words and names.** Background programs are *elves* (`elvesd` keeps
  them running), never "services" or "daemons". Devices have Unix-style
  names shown as `/dev/<name>`: `nvme0n1`, `nvme0n1p2`, `sata0`,
  `usbdisk0`, `kbd0`, `mouse0`, `eth0`, `wlan0`, `pcm0`, `usbserial0`.
- **The shell is a program** (`user/bin/sh.c`), started by `login`.
  Kernel commands are only a rescue console (`rescue>`: hardware and file
  system checks when the system cannot start); nothing that reads or
  edits file contents runs in the kernel.
- **Design choices that bind new work:**
  - RitterOS writes to disk only when the user asks (see below).
  - Settings are `key = value` files in `/system/config` (a user's own in
    `~/.config`). Whoever reads a settings file writes it when it is
    missing, with its text from `common/settings_defaults.h` (every key at
    its default, with comments). A program changing a setting changes
    only its line.
  - All network access goes through the firewall's rules (`guard`,
    `guardd`); elves do not manage their own security.
  - Drivers: one per hardware family, but what the hardware can report
    (outputs, limits, wiring, sizes: registers, PCI config, firmware
    tables, the EDID) is read from it; a driver's own tables only for
    what nothing reports, as a documented fallback.
  - The network stack is the same for every card: what a card must do is
    an operation in `struct netdev`; no protocol reaches into a driver.
  - Wi-Fi offloads to the firmware what it can; the country's rules are
    the firmware's.
  - The shell: built-ins only where a program cannot do it; otherwise
    POSIX.
  - Processes: POSIX open file descriptions, fork with copy-on-write,
    posix_spawn on fork + exec, 256 descriptors and 256 threads per
    program, 8 MiB stacks. Kernel stacks are 32 KiB.

## Version control

- git, branch `main`. Every commit is a tested state, tested in scope: run
  the tests that touch what changed on both architectures (`ONLY=name
  ONLY_ARCH=x86 make test`, then `ONLY_ARCH=arm`), then commit. Put the
  step and what changed in the message.
- The release is the file `VERSION` (year.month; a second in a month
  year.month.N), tagged with the same name. make writes `build/version.h`
  (the release and the commit) only when it changes; only
  `kernel/release.c` includes it. `uname -a` shows both. The kernel's
  debug information names the checkout `.` (`-ffile-prefix-map`), so
  every checkout of a tag builds the same kernel as its release's images
  (GitHub Releases: `ritteros-VERSION-<arch>.img.gz`, SHA256SUMS).
- `build/` and `tools/` are not in git: `make` and
  `scripts/setup-toolchain.sh` recreate them.
- No machine's addresses, network names, MAC addresses or firmware tables
  (ACPI, video BIOS: the vendors' code) go into git.

## Hard constraints

- **Everything stays inside the project folder.** No system-wide
  installs. The toolchain (zig, QEMU with our device models) is built by
  `scripts/setup-toolchain.sh` into `tools/` (pinned versions).
- **RitterOS is GPL-3.0-or-later** (`LICENSE`; the README's "Licence"
  section has the notice and the table of third-party files).
  Third-party content must be public domain or GPL-compatible; a licence
  and its notice go beside the content and into the README table.
  Agreed exceptions: Mozilla's root certificates (MPL-2.0, `certs/`),
  redistributable Wi-Fi firmware, the Unicode data, X.org's Adobe bitmap
  fonts (`fonts/`).
- **RitterOS's own code is written by us:** compiler, JavaScript engine,
  cryptography, decoders, drivers. GPL code may only be separate programs
  on the disk, never copied into the sources. Facts (register layouts,
  protocol details) may come from any source; the code is ours.
- **Normal boots never write to the disk.** Writing is opt-in on the
  kernel command line (`disktest`, `fswrite`, `scrub`...) or a program the
  user runs. Exceptions: the first start (root's password, `/home/root`,
  `~/.config`), new cards' settings files, elf files, the logs (`logd`),
  and a missing settings file written with its defaults by its reader.

## Commands

```sh
make                      # build both architectures: build/ritteros-x86_64.img,
                          # build/ritteros-aarch64.img (one disk per architecture)
make test                 # every test boot, x86 and ARM halves side by side,
                          # ~6,400 checks; must end "All checks passed."
ONLY=name ONLY_ARCH=x86 make test   # one test function (exact name, may take
                          # arguments: ONLY='wkeys_shell_test ps2')
make run CMDLINE="..."    # interactive boot (x86-64), serial terminal; Ctrl-A X quits.
                          # It boots build/ritteros-<arch>.img and stores a password
                          # there: never cut a stick from that file
make run ARCH=aarch64     # the same on ARM
make TOOLCHAIN=ritteros   # build user space with our own cc/as/ld
make clean                # also deletes the disk image and its RitterFS
python3 scripts/ritterfs.py check|ls|cat build/ritteros-x86_64.img 133120 1961984 [PATH]
python3 scripts/ritterfs.py put build/ritteros-x86_64.img 133120 1961984 PATH LOCAL_FILE
```

Filter build output with `grep -E "error:|warning:"` (plain `error` matches
`-Werror`), and check `make`'s exit code too.

**Running `make test`** (long: run it in the background):
- One run at a time (runs share `build/test` and `build/test-run.log`):
  check `ps` first (`ps aux | grep "[t]est.sh"` also counts a checking shell
  whose command line names test.sh: look at the lines). Leftover
  `qemu-system` processes make the next boot fail ("Failed to get write
  lock").
- Each half runs in `build/test/<arch>/` (images, `<name>.log`/`.stderr`,
  monitor socket) with its own host ports (`PORT_BASE` 47530 x86, 47630
  ARM). Progress: `grep -c ^PASS build/test/*.out`; each boot prints
  `TIME name: N s`.
- **No `make` while it runs** (boots copy `build/esp`/`build/system` as they
  go), and `make test` builds first, so a source edit during one half
  reaches the other. **Never edit `scripts/test.sh` while it runs** (bash
  reads it as it goes). A run whose result cannot count is stopped at once.
  The `sdk` test runs `make sdk`: library edits during a run reach later
  boots.
- Report results only from the run's own output (exit status, PASS count,
  FAIL lines, last line), read after the completion notice.
- A wrong `ONLY=` name prints "command not found" and then "all checks
  passed" with zero checks: confirm the expected PASS lines appear. A test
  helper defined inside another test function is not callable elsewhere.
- A failure that does not repeat: copy its evidence (`build/test/<arch>/
  <name>.*`, the run's log) aside before running anything again.
- The Mac's shell is zsh: `status` is read-only (use `rc`), an unmatched
  glob aborts, `$OPTS` is not word-split. macOS limits Unix socket paths to
  104 bytes (QEMU monitor sockets live in `build/`).


## Architecture in brief

- `boot/`: the UEFI bootloader (PE32+, `common/efi.h`, architecture parts
  in `arch/<arch>/boot_arch.c`). It reads `/system/<arch>/kernel.elf`
  from RitterFS on partition 2, builds the kernel's first page tables,
  exits boot services and jumps to the kernel with a `struct boot_info`
  (`common/boot_info.h`: new fields go at the end and `BOOT_INFO_VERSION`
  goes up, since installed machines keep their bootloader when a kernel
  is replaced). The ESP holds only the bootloader and
  `\ritteros\cmdline.txt`.
- `kernel/`: ELF at `0xFFFFFFFF80000000`, System V ABI,
  `-mgeneral-regs-only` (no FP/SIMD in the kernel), no libc. Physical
  memory only through the direct map (`phys_to_virt`); the lower half
  belongs to programs. Device registers above 4 GiB (and all on ARM)
  need `paging_map_mmio`.
- `common/`: compiled for kernel, bootloader and programs; no kernel or
  UEFI dependencies (RitterFS reading, AES, CRC-32, strings).
- `arch/<arch>/`: everything architecture-specific, behind
  `kernel/arch_api.h`, `boot/boot_arch.h`, `common/serial.h`,
  `kernel/paging.h`. Shared code has no inline assembly or
  architecture-only types; every `make` compiles shared code for the
  other architecture too, so violations fail the build.
- `drivers/`: kernel modules (`/system/<arch>/drivers/NAME.mod`) against
  the driver interface in `drivers/include/` (versioned; recorded in
  `drivers/abi/`, checked by `scripts/abicheck.py`: removing or changing
  needs a new major version).
- `user/`: programs (`user/bin` command line, `user/apps` windowed app
  bundles, `user/test` test programs) against our C library (`user/lib`,
  headers in `user/include`); system call numbers in
  `common/syscalls.h` (the kernel returns -ERR_*, wrappers set errno).
  Every program is dynamic (`libritteros.so.1`, `libwindow`, `libimage`,
  `libaudio`, `libvideo`, `libtls`, started by `ld.so`). Every program in
  `user/bin` declares `PROGRAM_HELP("usage", "description")`.
- `sdk/`: how programs are written outside the tree (`sdk/CLAUDE.md`,
  `sdk/ritteros.mk`, `sdk/template/`, `sdk/examples/`, installed as
  `/system/sdk`). It is part of the interface: a change to the library,
  headers, `user/user.ld`, start code or shell syntax updates the guide
  and template, and the `sdk` test must pass. `sdk/API.md` is generated
  (`scripts/sdkapi.py`). Shared libraries' interfaces are recorded in
  `sdk/abi/` and checked by `scripts/abicheck.py`; additions are recorded
  with `--update`.
- Disks: one image per architecture made by `mkdisk.py --arch ARCH`.
  Objects are in `build/<arch>/`, installed files in
  `build/system/<arch>/`.
- Boot order (`kernel/main.c`): console, GDT/IDT, paging, page allocator,
  heap, firmware info (ACPI, SMBIOS, UEFI time), interrupts and the tick,
  scheduler, PCI, NVMe, USB, disks, RitterFS (its check on a thread beside
  the boot), modules, then `login`. The log ends "RitterOS kernel: boot
  complete".
- Several cores: each core's state is a `struct cpu` (`kernel/percpu.h`)
  found with `arch_this_cpu()`. Spinlocks are ticket locks; every lock has
  a name, and the lock checker (`lockcheck`, on with `selftest`) panics on
  a sleep while holding a spinlock and checks the order by name. Read
  per-core state only with interrupts off or under a spinlock.

## Rules a change must keep

- The kernel never dereferences a program's pointer: `copy_user` checks
  the page table and may fault pages in, so it never runs under a
  spinlock.
- Waiting: `wait_event(queue, condition)` and friends; the code that
  makes the condition true calls `wait_queue_wake_all`. A new kind of wait
  is wait_prepare, then look at the condition, then wait_cancel or
  wait_commit (never look, then sleep).
- Locks: a spinlock for short sections and anything an interrupt handler
  touches, a mutex for long ones. Never sleep, take a mutex, use the
  console or the heap in an interrupt handler. `spin_unlock_only` gives a
  lock back without restoring interrupts: never `spin_unlock(l, 0)`.
- No unbounded waits on hardware: every loop that waits for a device has
  a limit and a way on.
- Device registers only through `mmio_read*`/`mmio_write*`: under HVF,
  QEMU can only emulate simple `ldr`/`str`.
- RitterFS: on-disk structures must match `docs/ritterfs.md`,
  `scripts/ritterfs.py` and `common/rfs_format.h`; change all three. Never
  bump the format version for a compatible addition. Host-side changes go
  through `ritterfs.install()` (copy-on-write, superblock last).
- A new pollable kind of file marks its queues `pollable`, calls
  `poll_watch` on every queue it could be woken on before reading the
  state, and detaches a queue before freeing it.
- New library functions go into `libctest.c`, compared line for line with
  the Mac's libc. No static working state in library code that threads
  may share.
- The toolchain (`cc`, `as`, `ld`, `make`) is our own, compared with
  clang's and LLVM's output; its output must never depend on the machine
  it runs on.
- windowd composites, clients draw; windowd never writes to a client from
  its input thread. Colours come from `window_color`; the Open dialog is
  `window_choose`, a file's app is `window_app_for`.
- iBrowser and the JavaScript engine: every recursive parser has a depth
  limit; values in C code's own memory are rooted while it calls anything
  that allocates; the security rules of fetch/XHR live in dom.c.

## Code conventions

- C11, `-Wall -Wextra -Werror`, freestanding. Header comments explain what a
  module does; comments explain *why*, at the density of the existing code.
- `kprintf`/`ksnprintf` are format-checked: `uint64_t` is `%lu`/`%lX`,
  pointers `%p`, strings without NUL `%.4s`; constants in formatted output
  `uint64_t`-typed (`((uint64_t)4096)`). Several kprintf calls that belong
  together go between `kprintf_lock`/`kprintf_unlock`; one line is one call.
- Device registers only through `mmio_read*`/`mmio_write*`
  (`arch/<arch>/mmio.h`): under HVF QEMU can only emulate simple
  `ldr`/`str` (pairs or writeback abort QEMU with `Assertion failed: (isv)`).
- **No unbounded waits on hardware.** Every loop that waits for a device has
  a limit and a way on (real hardware may be missing, half there, or never
  answer).
- Locks: a spinlock (interrupts off) for short sections and anything an
  interrupt handler touches, a mutex (recursive, sleeping) for long ones. (65e-10)
  A mutex waiter spins first while the owner runs on another core (20 us,
  4 looks), never past a thread asleep on it; spinning counts as `(spun)`.
  Never sleep, take a mutex, use the console or the heap in an interrupt
  handler. Code that runs with interrupts off must be fast in our own cc's
  code too (a page-allocator search once held interrupts off for
  milliseconds: 20 ms pings).
- Waiting: `wait_event(queue, condition)` / `wait_event_until(...)` /
  `wait_event_interruptible`; the code that makes the condition true calls
  `wait_queue_wake_all`. A condition may take what it finds (waitpid's
  removes the child): the macros stop at its first true look (65e-5). A wake-up
  of an empty queue takes no lock (65e-11, `nobody_waits` after a fence);
  anything that puts a thread on a queue fences before the caller looks at
  its condition (wait_prepare, poll_watch): keep that for any new way in. A new kind of wait is wait_prepare -> look at the
  condition -> wait_cancel or wait_commit (never look, then sleep); a
  condition guarded by another spinlock prepares under it and commits after
  (mutex, futex). Only the idle thread uses `arch_wait_for_interrupt`.
- Use the heap (`kmalloc`) and page allocator (`pmm_alloc_page`), not large
  static buffers; DMA buffers need physical addresses.
- RitterFS: on-disk structures must match `docs/ritterfs.md`,
  `scripts/ritterfs.py` and `common/rfs_format.h`; change all three and keep
  the host checker and the kernel check in agreement. Never bump the format
  version for a compatible addition (installed bootloaders refuse an
  unknown version). Host-side changes go through `ritterfs.install()`
  (copy-on-write, superblock last).
- Makefile: a variable used in a rule's prerequisites must be defined above
  the rule; a flag change does not relink (delete the binary); assembly of
  the library is listed in `USER_LIB_SRCS` by hand.

## Testing rules

- Every visible feature gets checks in `scripts/test.sh` (`expect`/`reject`
  on the serial log, screenshots via `scripts/check_screen.py`, whose
  positions and colors mirror `kernel/main.c` and `kernel/console.[ch]`;
  session screenshot modes start with `term-`). Log text changes update the
  expectations. Tests run on both architectures (`P=arm-` prefixes names);
  add new tests to both, or say why one has none.
- A test is only proven when it has been seen to fail (break the code on
  purpose; the broken variant must still compile, and must differ in a case
  the test has).
- Test boots use fresh images from `make_image`; every test image gets
  `selftest` (`SELFTEST=` makes one without). Test images have logd and
  netnamesd off (`LOGD=on`, `NETNAMESD=on` before make_image when needed).
  `build/ritteros-<arch>.img` is never booted by a test (it becomes the
  stick).
- Counts and IDs come from the tools, never hard-coded: RitterFS block and
  object counts from `ritterfs.py check` (they depend on the kernel's size),
  GUIDs from `mkdisk.py`, `disks` output from `scripts/expect_disks.py`,
  certificate counts from openssl.
- Real-hardware behavior is imitated in QEMU where possible: `-serial none`,
  `qemu-xhci,msi=on,msix=off` (MSI only), `msi=off,msix=off` (polling),
  `ich9-usb-ehci1`, a stick as the only disk (`usbboot`), `usblate`,
  `noruntime`. Measure per-block costs with polling when in doubt.
- Sessions: `boot()` runs `log_in` (answers the first-start password with
  `ROOT_PASSWORD`, logs in as root, `cd /`); `prompt_wait` counts prompts
  (`PROMPT_RE`; the login prompt is `^login: ([^/]|$)`), `command_output`
  finds a command's output after a prompt. `serial_type` is `printf "$1"`
  and `command_output` passes through `awk -v`: both interpret backslashes
  and `%` (`typed_form`/`matched_form` double them; test programs take
  markers like `^H`). A program printing another shell's transcript must not
  print its prompts as they are. Kernel messages after a prompt: send an
  Enter first. Waits after login: `dhcp_wait` (dhclient, sshd, guard rules
  loaded), `dnsd_wait`. Network tests reaching ports other than 22 need a
  rules file (`guard_rules IMAGE RULE...`).
- `ONLY` takes a function with arguments (`eval`): ONLY='programs_test "..."
  "..." 11' runs the normal boot's program checks with the leak check.
- In test.sh, `check ... eval '...'` sees check's own `$1`, `$2`: copy a
  caller's argument into a named local first. macOS's bash 3.2 cannot parse
  a here document with an unbalanced apostrophe inside `$(...)`: use
  `want_text VAR <<'LINES'`. Bash arithmetic is signed 64-bit (kernel
  addresses are negative). A SIGKILL can cut the last serial line short:
  parse only complete lines, numbers with `[0-9][0-9]*`.
- QEMU's monitor: every call `nc -w 3 -U`; one client at a time, so a group
  of commands goes over one connection with pauses (`monitors CMD...`).
  Hot plug of "the same stick again" uses a clone (`cp -c`) with the same
  `serial=`; a replug waits 2 s. Typed input goes through a named pipe kept
  open on fd 7 (`input-path=`); `boot()` fails a session whose bytes never
  left it. Every test machine has QEMU's GDB stub; a hung session runs
  `scripts/gdbprobe.py` into `<name>.hang` (it must detach).
- QEMU input: `sendkey` goes to the latest keyboard (USB); the `ps2` tests
  remove `usb-kbd`; AltGr is `alt_r`, keys `tab`, `ret`, `spc`; ARM needs
  `-device usb-mouse` for QMP's pointer; QMP pointer moves in steps of at
  most 300 pixels (QEMU's PS/2 mouse queues 16 bytes); a quick double click
  is four QMP commands in one connection. Press at least ~8 pixels inside a
  window (its edge resizes); the screen's centre has the mouse pointer.
- Test servers on the Mac: `scripts/testdns.py`, `testnet.py`,
  `testroute.py` (dgram networks, DHCP, attacks), `testwifi.py` (the access
  point), `openssl s_server -tls1_3` (LibreSSL 3.3.6; `-WWW` for files with
  a status line, `-HTTP` for whole answers), python http.server. Tests never
  use the internet. Ports are offsets from `PORT_BASE`.
- Checking against the Mac: many programs are built on the Mac against its
  libc (a stub ritteros.h, never `-I user/include`) and compared with the
  Mac's own tools or libraries, under ASan/UBSan, plus fuzzing. The Mac is
  not always right: where it departs from POSIX/C we follow the standard
  (or GNU) and leave the case out, and say so in the test. Better oracles:
  `scripts/mathref.py`, `complexref.py` (Python decimal), jsc for
  JavaScript, ImageIO and WebKit (`qlmanage`) for images and SVG, clang's
  objects for our toolchain (a scratch cmp_obj.py comparing sections).

