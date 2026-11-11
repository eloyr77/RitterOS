# power: what the firmware offers for power management: sleep states, devices

```
power [tables DIR | names | eval NAME | sleep [SECONDS]]
```

Prints the kernel's report on what the machine's firmware offers for power
management (since step 37a, roadmap item 37), the same as the rescue
console's `power` command. It is read from the firmware's ACPI tables
without running any of their code (the kernel's AML interpreter, since
37b-2, runs them into a namespace: `power names`); later steps add the
battery, the lid and putting the machine to sleep. Anyone may run it.

The lines:

- `firmware:` the FADT's revision, the machine's profile (desktop,
  mobile, ...) and flags: `hardware-reduced` (no PM1 registers, as on ARM
  machines), `low-power S0 idle (Modern Standby)` (the firmware prefers its
  deepest idle to sleep state S3: many modern laptops sleep that way), the
  power and sleep buttons as fixed features or devices;
- `registers:` (not on hardware-reduced machines) the PM1 and GPE
  registers, the interrupt the firmware signals on (SCI), and the
  latencies of processor states C2 and C3 (`none` when the firmware says
  they are not there);
- `sleep states in the firmware's tables:` each `\_Sx_` the firmware
  names, with its values for the PM1a and PM1b registers (S3 is suspend to
  RAM, S4 to disk, S5 off), and those it does not name;
- `FACS:` the firmware's control structure: the waking vector (where the
  processor starts after S1-S3) and whether a 64-bit wake is supported;
- `device:` the power devices the tables describe, by their ACPI IDs, with
  the names the firmware gives them: the battery (PNP0C0A), the AC adapter
  (ACPI0003), the lid (PNP0C0D), the power and sleep buttons, the embedded
  controller (PNP0C09), the low-power S0 device (PNP0D80), AMD's or Intel's
  power controllers, processors;
- `AML:` (since 37b-4) the interpreter: its objects and tables, the
  methods it ran and its errors, what the boot's `_INI` did, the
  firmware's accesses to memory, I/O ports, PCI configuration and the
  embedded controller (reads and writes), those refused (the kernel's
  RAM) and those with no way there, the notifications the firmware's
  code sent;
- `embedded controller:` (since 37b-5, on machines with one) the laptop's
  controller of the battery, lid and keys: its device, its ports and
  where they came from (the ECDT table or the device's `_CRS`), the
  firmware's reads and writes of its memory, the waits that timed out
  (`it does not answer` when none is there), and (since 37c-2c) the events
  it raised (the lid, the adapter, the battery, the keys: each runs the
  firmware's `_Qxx`) and how many had no method;
- `battery:` (since 37c-1) each battery the firmware describes and says
  is there: its charge (of its last full charge), what is left and its
  capacities when full and new (mWh or mAh, as its firmware counts),
  whether it is charging or discharging, the rate and the voltage, its
  maker, kind and model; `not in its place` when the firmware says it
  is out, `not read` when the firmware's methods fail;
- `AC adapter:` (since 37c-1) whether it is plugged in;
- `lid:` (since 37c-2c) whether the lid is open or closed;
- `processor states` (since 37d-1) the processors' sleep states the
  firmware lists (`_CST`): each one's name (C1-C3), how it is entered
  (`MWAIT` with a hint, `HLT`, or reading an I/O port) and how long waking
  from it takes; `none from the firmware` without them;
- `processor:` (since 37d-1) how the processor can idle (x86: `MWAIT`,
  and whether its APIC timer keeps running in deep states, ARAT; ARM:
  `WFI`) and, where it has an energy counter (AMD from family 17h, Intel's
  Silvermont), the power the whole processor used over one second: `power`
  waits that second to measure it;
- `idle` (since 37d-2) how the idle cores slept since the start, all
  cores together: each state's entries and its share of the idle time;
- `events:` (since 37c-2a) the firmware's interrupt (the SCI) and what
  came through it: how many interrupts, how many with nothing to handle
  (the kernel switches the SCI off after 10,000 of those in a row); (since
  37c-2b) the general-purpose events (GPEs) whose firmware methods are
  on, how often they ran, failed, or were switched off for coming more
  than 100 times in a second; on ARM the GED (the firmware's event device)
  instead; the power button (fixed, or the firmware's button device, or
  both; since 37g-7 also `keyboards' Power key`), its presses and what `power_button` in `/system/config/power`
  makes them do (since 37g-7, `(N around a sleep, not acted on)`: presses
  before a sleep or while waking, never a power-off or another sleep);
  (since 37c-2c) how often the lid and the AC adapter changed, the setting
  `lid`, and the batteries' notifications; `none` on machines without
  either;
- `methods defined:` how many times the methods for processor sleep states
  (`_CST`), CPPC (`_CPC`), P-states (`_PSS`), turning devices off (`_PS3`,
  `_PR3`), preparing to sleep and waking (`_PTS`, `_WAK`) and devices'
  own functions (`_DSM`) are defined.

`power tables DIR` (since 37b-1; root only) saves every ACPI table as
the firmware gave it, one file each in the folder DIR (which must exist),
named as Linux's `acpidump -b` names them: `dsdt.dat`, `facs.dat`,
`facp.dat`, `apic.dat`, `ssdt1.dat`, `ssdt2.dat`, ... -- for reading them
elsewhere, for example with Intel's `iasl -d` (RitterOS's tests do). The
tables hold the firmware's own code and the machine's details, so only
root may save them:

```
ritteros:/$ mkdir /tmp/acpi
ritteros:/$ power tables /tmp/acpi
power: /tmp/acpi/dsdt.dat: 8146 bytes
power: /tmp/acpi/facs.dat: 64 bytes
power: /tmp/acpi/facp.dat: 244 bytes
...
power: 8 table(s) saved in /tmp/acpi
```

`power names` (since 37b-2; root only) lists the namespace the kernel's
AML interpreter made at the start: the firmware's code in the DSDT and
the SSDTs is run as it loads (named objects are made where their
definitions are, and code outside methods runs then), one object a line:
its path, its type and what it holds. `Integer` with its value,
`String` and `Buffer` with their length, `Package` with its elements,
`Method` with its arguments (`serialized` when the firmware asks for one
caller at a time), `Region` with its address space (0 memory, 1 I/O
ports, 2 PCI configuration, 3 the embedded controller...), address and
length, `RegionField`, `IndexField`, `BankField` and `BufferField` with
their bits (offset+length) and access width, `Device`, `Processor`,
`Power` (a power resource), `Thermal`, `Mutex`, `Event`, `Alias` (with
its target), `Scope`.

Since 37b-4 its code reaches the machine: the firmware's operation
regions are its memory and devices (but never the kernel's own RAM:
such an access is refused and counted), I/O ports (x86) and PCI
configuration space (the device's address found as ACPICA finds it:
the root bridge's `_SEG` and `_BBN`, the bridges' buses, the device's
`_ADR`) and, since 37b-5, a laptop's embedded controller (found from the
ECDT table or as the device `PNP0C09`, its memory read and written a
byte at a time through its two ports); other spaces have no way there
yet. After loading, the boot runs what Linux runs: `\_INI`,
`\_SB._INI`, `_REG` for PCI configuration, then the `_INI` of every
device whose `_STA` says it is there. The boot logs two lines (QEMU's
q35, whose tables have no `_INI`):

```
  ACPI: AML of 1 of 1 tables: 269 objects, 0 errors, 3 ms, stack 1024 bytes
  ACPI: _INI 0 run (0 _STA, 0 devices absent, 0 failed) in 1 ms
```

```
ritteros:/$ power names | head -5
\_GPE Scope
\_GPE._HID String length 8 "ACPI0006"
\_GPE._E02 Method args 0
\_GPE._E01 Method args 0
\_PR_ Scope
```

`power eval NAME` (since 37b-3; root only) evaluates one object of the
namespace and prints its value: a method is run (without arguments), a
field is read, a data object is given as it is. NAME is a path from the
root, with or without its leading backslash and the padding underscores
(`_SB.PCI0._CRS` is `\_SB_.PCI0._CRS`). The first line names the object
and its value; packages list their elements indented, buffers their bytes
16 a line, references the object they name:

```
ritteros:/$ power eval _S5_
\_S5_: Package 4 elements
  Integer 0x0
  Integer 0x0
  Integer 0x0
  Integer 0x0
ritteros:/$ power eval _SB.PCI0.S00._ADR
\_SB_.PCI0.S00_._ADR: Integer 0x0
```

The firmware's code runs as Linux runs it: a method that ends without a
`Return` gives the last value it computed (0 when none), and `\_OSI`
answers yes for the Windows versions up to 2022. Since 37b-4 the method
runs on the machine itself: evaluating `_PS3` or `_OFF` switches a device
off as the firmware does it. On q35, `power eval _SB.HPET._STA` reads
the HPET's registers (`Integer 0xF`: there) and `power eval _SB.PRQA`
the LPC bridge's PCI configuration (`Integer 0xA`: interrupt 10, as the
firmware set it). An object that is not there prints `power: eval: NAME: no such
object`, an evaluation that fails `power: eval: NAME: the evaluation
failed (the kernel's log says why)`, with the interpreter's reason in the
kernel's log (lines starting `aml:`).

`power sleep` alone (since 37g-7; root only) sleeps until the power
button wakes the machine (`power: sleeping until woken (the power
button)`), or what its firmware wakes it on itself: on many laptops the
lid opened. The press that woke it does nothing else. The same sleep comes
from `/system/config/power`, read at the start: `power_button = sleep` (a
press), `lid = sleep` (the lid closed) and `sleep_after = MINUTES`
without a key, a click, a mouse move or typing over SSH (0, the default:
never); the kernel then starts `power sleep` and logs why (`acpi: the
lid: closed (lid = sleep): /system/x86_64/bin/power sleep, process N`).
After the wake the screen comes on and the minutes count again.

`power sleep SECONDS` (since 37g-4; root only) puts the machine to sleep
in ACPI S3 ("suspend to RAM": only the memory keeps its contents, the rest
is off) for SECONDS (1 to 86399, a day), and the RTC's alarm wakes it; it
prints `power: sleeping N s` before and `power: awake again` after. Every
driver of the machine's devices first keeps what it needs and afterwards
sets its device up again; if one cannot (its driver has no sleep yet),
the machine does not sleep: `power: sleep: the machine did not sleep`,
and the kernel's log names the driver (`sleep: not asleep: amdgpu cannot
sleep yet`). The kernel's log has a line each way (`sleep: S3 ...`,
`sleep: awake after 3 s (the RTC's alarm); ...`). On the AMD laptops
(the Acer's Cezanne since 37g-5c-3, the VivoBook's Picasso since 37g-6)
the panel comes back a few seconds after the wake (`displays sleep` says
how). The machine is otherwise as before: its programs, its files, its
disks (a SATA disk's link is reset, a USB disk on the controller's own
port is kept as the same disk when it says it is: `disks` and the
kernel's log say so), its network (Wi-Fi joins again, the Intel 8265's
after its firmware starts again), its keyboard and touch pad.

## Example

QEMU's q35 machine:

```
ritteros:/$ power
firmware: ACPI FADT revision 3.0, profile unspecified; flags 0x84A5, power button fixed, sleep button a device or none, C1 on every processor
registers: PM1a event 0x600, PM1a control 0x604, PM1b control 0x0, PM timer 0x608, GPE0 0x620 (16 bytes); SCI 9; C2 latency 4095 us (none), C3 4095 us (none)
sleep states in the firmware's tables: S3 (1, 1), S4 (2, 2), S5 (0, 0); not there: S0 S1 S2
FACS: 64 bytes, version 0, waking vector 0x0 (64-bit 0x0); 64-bit wake not supported, S4BIOS no
device: processor container (ACPI0010): CPUS
methods defined: _CST (processor sleep states) 0, _CPC (CPPC) 0, _PSS (P-states) 0, _PS3 (device off) 0, _PR3 0, _PTS (prepare to sleep) 0, _WAK 0, _DSM 0
```

## Exit status

`0`; `1` when the kernel gives no report, or a table could not be read or
saved (`power: tables: Operation not permitted` for anyone but root;
`power: names: Operation not permitted` and the same for `eval`; `1`
too when the object is not there or its evaluation failed, or the machine
did not sleep); `2` for other arguments.

## See also

[`drivers`](drivers.md), [`cpus`](cpus.md), [`displays`](displays.md)
