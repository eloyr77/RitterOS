# displays: list the screen, graphics devices, outputs and monitors; set a mode; what the last sleep changed

```
displays [set OUTPUT WxH[@HZ] | sleep]
```

Prints the kernel's report on the screen (since step 66a, roadmap item 33):

- `screen:` the mode the screen is in now, the one the firmware set when
  the machine started (the bootloader chooses it: `video=WxH` on the
  command line, else the firmware's own), with its refresh rate when the
  monitor's EDID passed on by the firmware lists it;
- every graphics device on PCI (display controllers), in bus order. One
  RitterOS has a driver for is named `gpu0`, `gpu1`, ... with its outputs,
  numbered by kind across all devices (`vga0`, `hdmi0`, `hdmi1`, ...). Each
  output says whether a monitor is there and, read over its cable (the
  monitor's EDID), its name, its native mode and refresh, every mode it
  lists, and whether that output shows the picture. A device without a
  driver says so (`no driver for these graphics yet`).

The drivers look at every output every 2 seconds, so a monitor plugged in,
swapped or pulled out shows here, and in the kernel's log (`gpu0: hdmi0
connected: 'NAME', native 1920x1080 at 60.000 Hz, 12 modes listed`, `gpu0:
hdmi0 disconnected`), within 2 seconds.

`displays set OUTPUT WxH[@HZ]` (66b-2) shows that output's monitor in
another mode: one the monitor lists (`modes:` above), at `HZ` frames a
second, or without it the monitor's usual rate for that size. The console
redraws at the new size, and so does windowd when it runs (66b-5: its
windows are kept on the new screen, maximized ones fill it, and the pointer
reaches all of it). Root may, or the user at the machine's own screen and
keyboard. It prints what was set (`hdmi0 now 1920x1080 at 60.000 Hz
(pixel clock 148.500 MHz)`), or why not, and the kernel's log says the
same. The mode lasts until the machine restarts; for one at every start,
write it in `/system/config/displays` (66b-6), one line per output:

```
# OUTPUT = WxH[@HZ]
hdmi0 = 1920x1080@60
```

The kernel sets it once the system's disk is mounted, before anyone logs
in, and logs `gpu0: hdmi0 now 1920x1080 at 60.000 Hz
(/system/config/displays)`, or why not (`displays: line 2 of
/system/config/displays: not OUTPUT = WxH[@HZ]`, `displays: hdmi5 in
/system/config/displays: no such output`, or the reasons `displays set`
gives). `screen:` then says `set from /system/config/displays`.

So far the J1900's Intel graphics and (66g-2) the laptop's AMD graphics
can do it. On AMD, the output that shows the picture over DisplayPort (the
built-in eDP panel), with its trained link kept, a mode the panel's EDID
lists with the plane's size and line time as the firmware set them (its
memory schedule is kept), within the display clock and the link's
bandwidth; the laptop's panel lists one mode, 1920x1080 at 60 Hz, and
`displays set edp0 1920x1080` sets it again (the picture stops for about
35 ms); `displays` then shows `mode sets: 1; the last 1920x1080 at 60.036
Hz: the OTG back after 28 ms, the stream after 35 ms`. On Intel, the HDMI
output that shows the picture, up to HDMI's 225 MHz on that chip (the board's VBT is too old
to give its own limit), so 2560x1440 at 60 Hz (241.5 MHz) is not offered
there. The memory settings the firmware left (its watermarks) are kept: a
mode must fit in the reserve they keep in the display's buffer (66b-7;
`displays` shows both: 1920x1080 needs 120 lines of 241).

Where a driver can set modes, a last line says what writing to the screen
has cost (66b-3): within the firmware's framebuffer, which the boot maps
write-combining, and beyond it, where a larger mode reaches (made
write-combining too when the mode is set): `screen writes: 609417 KB in
248 ms within the firmware's framebuffer (write-combining), 2398 MB/s; 5730
KB in 2 ms beyond it, 2359 MB/s`.

Drivers so far: Intel's Bay Trail graphics (the J1900's, 8086:0f30-0f33),
whose VGA and HDMI outputs are read over their DDC wires; DisplayPort
monitors are not read yet. AMD's Cezanne graphics (the laptop's Ryzen
5800H, 1002:1638; 66e-1) list their outputs from the card's own video BIOS
(AtomBIOS, handed over by the firmware in its ACPI table VFCT); the
built-in panel (eDP) is read over its AUX channel (66e-2b). It takes no arguments; anyone may run it.

`displays sleep` (37g-5c-2) prints what the graphics drivers kept over the
last sleep (`power sleep`): on the AMD graphics, the registers of display
pipe 0's path (758 on the Cezanne, 642 on the VivoBook's Picasso, whose
display engine is DCN 1.0, since 37g-6) (its clocks, the hub, the plane, the colour and
blending stages, the timing generator, the encoder, the panel's power and
backlight, its AUX channel and the PHY) read before the sleep and after
it, block by block with how many changed, and each register that did,
before and after. The kernel's log has the counts by block at the wake.
Since 37g-5c-3 the driver brings the panel back after the wake (the video
BIOS's tables for the display engine's start, its clocks, the panel's
power, the PHY and the backlight; link training; the pipe's registers as
before; the plane's picture address), and a line says how that went (`the
panel after it: the panel back: ... trained (clock recovery in N,
equalization in N, level 0xNN), N registers put back (N did not take),
the plane's address in use, the stream on (N ms)`, or the step where it
stopped). On the Picasso the bring-up goes as Linux's DCN 1.0 code: the
video BIOS's asic_init first when its own mark says the chip lost its
set-up, the display clock as before the sleep and the reference clock
through the graphics' power-management processor, link training with the
patterns the panel lists. Other lines: the graphics' power-management
processor as the driver finds it before it sends it anything (`now: ...
(ready)`, the Cezanne), the display engine's memory requests now (faults
and the replies' status; on the Picasso the replies' and what asic_init
did: `asic_init: run (BIOS_SCRATCH_7 0x00000000 before, 0x00000200
after; N us): done`, or `not needed`), and the registers not as before
the sleep after the bring-up. The host
VM, VGA mode and the memory hub's FB offset are only read, never put
back.
Without a sleep since the start, or without such a driver, a line says so
(`no graphics driver here keeps its registers over a sleep`).

```
gpu0: MODEL at BUS:DEV.FN
  now: the power-management processor's firmware flags 0xFLAGS, its display mailbox's answer 0xANSWER after N us (ready)
  pipe 0's registers over sleep N: CHANGED of 758 changed (before -> after)
  the panel after it: HOW (N ms)
  memory requests now: DCN_VM_FAULT_STATUS 0x..., the address 0x..., DCHUBBUB_SDPIF_CFG0 0x..., ODM0_OPTC_INPUT_GLOBAL_CONTROL 0x...
  BLOCK: CHANGED of COUNT changed
    REGISTER 0xBEFORE -> 0xAFTER
```

## Exit status and messages

- `0`: the report was printed, or the mode was set.
- `1`: the kernel did not give the report (nothing is printed), or the mode
  was not set (`displays: hdmi0: the monitor does not list 1366x768 at 60
  Hz`, `displays: hdmi1: no monitor ...`, `displays: dp0: no such output`).
- `2`: wrong arguments (the usage is printed).

## Examples

On QEMU (its Bochs display has no driver here):

```
ritteros:/$ displays
screen: 1280x800, the firmware's mode
00:01.0 1234:1111: no driver for these graphics yet
```

On the J1900, a monitor on its HDMI socket (port B). The outputs are the
ones the board's own table lists (its VBT, in the memory the firmware
leaves for the graphics driver: 66b-4): VGA and HDMI on port B. The
firmware passed on only the EDID's first block; read over the cable, the
second lists the monitor's 120 and 144 Hz modes too. The driver's own
lines count how its reads went (`no answer`: nobody on those wires), say
what the VBT listed and where the limits come from, then show the mode as
the hardware has it (66b-1): each pipe's timings and the output it
drives, its clock (the PLL's dividers give the pixel clock, and with the
totals the refresh), the plane that reads the picture from memory, and
the memory the firmware set aside for graphics, and (66c-2) the vertical
blanks counted (the moments between frames, an interrupt each) and, once
a program has flipped (66d), the screen buffers, which one shows and how
many flips:

```
ritteros:/$ displays
screen: 1600x1200 at 60.000 Hz, the firmware's mode; its EDID: 'DELL S2721DGF'
gpu0: Intel Bay Trail HD Graphics at 00:02.0; outputs looked at every 2 s
  vga0   the VGA port  no monitor
  hdmi0  port B        'DELL S2721DGF', native 2560x1440 at 59.951 Hz, EDID 256 bytes; showing the picture
         modes: 2560x1440@60 720x400@70 640x480@60 640x480@75 800x600@60 800x600@75 1024x768@60 1024x768@75 1280x1024@75 1920x1200@60 1920x1080@60 1680x1050@60 1600x1200@60 1280x1024@60 1280x800@60 1152x864@75 2048x1152@60 2560x1440@144 2560x1440@120
  GMBUS: clock register 0x14d; as the firmware left it GMBUS0 0x0, GMBUS2 0x0
  vga0   pins 2: 8 reads, 0 whole, 8 no answer, 0 out of time, 0 never idle (the last: GMBUS2 0x8600 at byte 0)
  hdmi0  pins 5: 10 reads, 10 whole, 0 no answer, 0 out of time, 0 never idle
  VBT: version 181, in the OpRegion (version 2) at 0xad6d2018; 1 child devices: [port 1, type 0x60d2, DDC 5, AUX 0x10]; VGA on the board (DDC 2)
  outputs from the VBT; display clock 333 MHz (the most a mode may need: 299 MHz)
  hotplug live status: port B 1, port C 0
  pipe A: 1600x1200 (source 1600x1200), total 2160x1250, sync 1664-1856 / 1201-1204; drives hdmi0
    PLL: on, locked, m1 3 m2 81 n 5 p1 3 p2 2: pixel clock 162.000 MHz, 60.000 Hz
    plane A: format 6 (32-bit BGRX), linear, stride 6400 bytes, at GPU address 0x0 (the aperture's 0xc0000000; memory 0xaf000000)
  pipe B: off
  stolen memory: 0xaf000000, 256 MiB; GTT 2048 MiB of addresses
  vertical blanks: 949 (949 interrupts); pipe A's frame 1812
  screen buffers: 2 of 8100 KB (GPU addresses 0x0 and 0x2000000); showing buffer 0; 4 flips
(all devices: 7 looks, 1 EDIDs read, 0 reads failed)
screen writes: 530266 KB in 217 ms within the firmware's framebuffer (write-combining), 2385 MB/s; 0 KB in 0 ms beyond it
```

Since 37e-2d the J1900's HDMI port stops its signal when the screen goes
off (the port's enable bit cleared; the pipe keeps running), so the
monitor says "no signal" and goes to standby; the first key starts it
again. A line counts it:

```
  standby: the signal stopped 1 times, back 1 times with the screen
```

Setting a mode on the J1900, and three it refuses (the limit is HDMI's on
this chip: the VBT, version 181, is too old to give one per output):

```
ritteros:/$ displays set hdmi0 1920x1080
hdmi0 now 1920x1080 at 60.000 Hz (pixel clock 148.500 MHz)
ritteros:/$ displays set hdmi0 2560x1440
displays: hdmi0: 2560x1440 at 59.951 Hz: needs 241.500 MHz, more than the 225 MHz the chip's HDMI carries
ritteros:/$ displays set hdmi0 1366x768
displays: hdmi0: the monitor does not list 1366x768
ritteros:/$ displays set hdmi1 1920x1080
displays: hdmi1: no such output
```

On the laptop (66e-1): the NVIDIA graphics have no driver yet; the AMD
graphics list what their video BIOS says: which of the firmware's images
it is, the table versions, the display clock the firmware left and the
reference clock, then every display path (the connector, the encoder that
drives it, the device it is for, the DDC/AUX line and the hotplug pin).
Only the first path has a connector, the built-in panel (eDP): the board
wires the other four encoders to nothing (connector 0), so the laptop's
HDMI and USB-C sockets are the NVIDIA's. The last path is the NVIDIA
module's slot (a generic object, MXM). Then (66e-2a) the video BIOS's
pin table (`gpio_pin_lut`: which register and bit each line and pin the
paths name is) and, per output, what that makes of its lines (the panel:
DDC1, so AUX channel 0, and hotplug pin HPD1), with those registers as
they are now, only read: whether the hotplug pin senses the panel, and
whether the AUX engine is on, who holds it and whether its pads are in
AUX mode. Since 66e-2b the panel's EDID is read over AUX (I2C over
AUX, as DisplayPort carries it), so the panel shows like any monitor;
the driver counts its AUX transfers and their answers, times a whole
read, and shows the panel's DisplayPort capabilities (its DPCD: version,
link rates, lanes; an eDP 1.4 panel lists its own rates). Then (66e-3)
the mode as the hardware has it, only read: the link as the panel says
it was trained (rate, lanes, each lane's state), each timing generator
(OTG) with its timing beside the EDID's and its pixel clock and frame rate
measured over a quarter of a second (so `displays` takes that long here),
the encoder it feeds (DIG), each plane (HUBP) with the address it reads
the picture from, and where that memory is; then (66f) the way its
interrupts take (MSI, the card's interrupt ring, the timing generator's
events) and the vertical blanks counted, the memory the card's firmware
keeps, and, once a program has flipped (66f-3), the two screen buffers:

```
ritteros:/$ displays
screen: 1920x1080, the firmware's mode
01:00.0 10de:249c: no driver for these graphics yet
gpu0: AMD Radeon Graphics (Cezanne) at 05:00.0; outputs looked at every 2 s
  edp0   the panel     '(no name)', native 1920x1080 at 60.036 Hz, EDID 256 bytes; showing the picture
         modes: 1920x1080@60
  VBIOS: image 1 of 2 in the firmware's VFCT table (55428 bytes), 55296 bytes; ''; AtomBIOS data tables 2.1, display objects 1.4
  display clock at boot 700.00 MHz, reference 48.00 MHz (dce_info)
  display paths: 6: [eDP 0x3114, encoder 0x211e, device 0x0002, DDC/AUX line 0x90, hotplug pin 1] [none 0x0000, encoder 0x221e, device 0x0000, DDC/AUX line 0x90, hotplug pin 0] [none 0x0000, encoder 0x2120, device 0x0000, DDC/AUX line 0x90, hotplug pin 0] [none 0x0000, encoder 0x2220, device 0x0000, DDC/AUX line 0x90, hotplug pin 0] [none 0x0000, encoder 0x2121, device 0x0000, DDC/AUX line 0x90, hotplug pin 0] [generic 0x7103, encoder 0x0000, device 0x0000]
  gpio_pin_lut: 10 pins (ID: register.bit): 0x01: 0x5db5.0 0x02: 0x5db5.8 0x03: 0x5db5.16 0x04: 0x5db5.24 0x05: 0x5db5.26 0x90: 0x5d91.8 0x91: 0x5d95.8 0x92: 0x5d99.8 0x93: 0x5d9d.8 0x94: 0x5da1.8
  edp0   line 0x90: DDC1 (register 0x5d91), AUX channel 0; pin 1: HPD1 (register 0x5db5 bit 0)
         HPD1: sense 1, on 1 (DC_HPD_INT_STATUS 0x1a000012, DC_HPD_CONTROL 0x10fa09c4)
         AUX0: on 1, follows HPD select 0; held by nobody; pad in AUX mode 1 (AUX_CONTROL 0x01040101, ARB_CONTROL 0x00000000, SW_STATUS 0x01000001, INTERRUPT_CONTROL 0x00000040, DC_GPIO_DDC1_MASK 0xcf410000)
         AUX0: 77 transfers: 77 ACK, 0 DEFER, 0 NACK, 0 out of time, 0 bad replies, 0 with HPD low, 0 engine busy, 0 short reads; EDID: 20 reads, 20 whole, the last of 128 bytes or more in 3455 us
         DPCD: DisplayPort 1.4, link rates (its table) 1.62 2.16 2.43 2.70 3.24 4.32 5.40 8.10 Gbit/s a lane, 4 lanes; eDP 1.4a (0x04); 14 00 44 41 00 00 01 c0 02 00 02 00 00 0b 80 00
         link (the panel's DPCD): 8.10 Gbit/s a lane (its table's rate 7), 4 lanes, enhanced framing; lanes: 0 CES 1 CES 2 CES 3 CES (C clock recovered, E equalized, S symbols locked), aligned
  OTG0: on, 1920x1080, total 2000x1111; H: sync 32, back porch 0, front porch 48; V: sync 5, back porch 16, front porch 10
        edp0's EDID, its first timing: 1920x1080, total 2000x1111; H: sync 32, back porch 0, front porch 48; V: sync 5, back porch 16, front porch 10; 133.400 MHz
        measured over 250 ms: pixel clock 133.419 MHz, 60.044 frames a second
        DIG0: on, DisplayPort; its back end takes front ends 0x01; 4 lanes, link trained 1, video stream on 1, RGB, 8 bits a component (DIG_FE_CNTL 0x01000000, DIG_BE_CNTL 0x00000100)
  OTG1: off
  OTG2: off
  OTG3: off
  HUBP0: on, format 8 (32-bit ARGB8888), 1920x1080 from (0,0), pitch register 1920, linear (tiling 0x00000080); surface at 0xf400000000, the card's memory + 0x0 (memory 0x810000000; through BAR 0 0xfc10000000)
  HUBP1: blanked, no surface
  HUBP2: blanked, no surface
  HUBP3: blanked, no surface
  the card's memory in the display engine's space: 0xf400000000 - 0xf41fffffff, at memory 0x810000000 (DCN_VM_FB_LOCATION, _OFFSET); BAR 0 0xfc10000000, 256 MiB
  interrupts: MSI at 0xa0 (control 0x0085: on 1, 4 vectors offered, 64-bit 1), MSI-X at 0xc0 (control 0x0003)
  IH: RB_CNTL 0x10330195 (ring on 1, interrupts on 1, 2^10 dwords, write-back 1, snoop 1, MC_SPACE 1); ring at 0x0000000000009fe0 (x 256), read 0x5e0, write 0x5e0, write-back to 0x00000000009ff000; IH_CNTL 0x01000000, STATUS 0x00040847 (idle 1), CHICKEN 0x00000010 (physical addresses 1), DOORBELL_RPTR 0x00000000
  OTG0's events: GLOBAL_SYNC_STATUS 0x00104101 (VSTARTUP: interrupt on 1, occurred 0, pending 0), VSTARTUP_PARAM 0x0000000d, VUPDATE_PARAM 0x014001f4, VREADY_PARAM 0x0000012c, INTERRUPT_CONTROL 0x00000000
        VUPDATE (live bit) rose 3 times in 50 ms, at lines 8, 8, 8 (of 1111)
  vertical blanks: 6667 (OTG0's VSTARTUP; 6667 interrupts, 6667 ring entries, 0 others (the last 0x00000000), 0 overflows); OTG0's frame 6753
  the firmware's own memory (vram_usagebyfirmware): from 0 KiB, 0 KiB for the firmware, 0 KiB for the driver
  screen buffers: 2 of 8294 KB (HUBP0; the display engine's 0xf400000000 and 0xf402000000, through BAR 0 0xfc10000000 and 0xfc12000000); pitch register 1920; showing buffer 0; 2 flips
(all devices: 0 looks, 0 EDIDs read, 0 reads failed)
```

Since 37e-2a the AMD graphics also show the panel's backlight, only read:
whether it is on (the panel's power sequencer, or its BLON override),
whether the panel is powered, and the brightness the backlight's PWM gives,
as a level of 0xffff (its duty cycle over its period), with the registers;
then what the firmware's ATIF interface answers when the driver starts:
its functions and notifications (whether it asks the driver to change the
brightness, by Notify 0x81: the firmware's brightness keys and `_BCM` go
that way, so the graphics driver is the one that sets the backlight), and
the backlight's characteristics: the default brightness on AC and on
battery and the lowest and highest input signal. The old laptop:

```
  backlight: on (as the power sequencer; the power sequencer on, state 4, BLON high), the panel powered; PWM on, level 0x9c00 of 0xffff (60.9%: duty 0x9858 of a period of 0x0fa0, 12 bits, fractional)
  backlight registers: BL_PWM_CNTL 0xc0009858, _CNTL2 0x00000000, _PERIOD_CNTL 0x000c0fa0, _GRP1_REG_LOCK 0x00000000; LVTMA_PWRSEQ_CNTL 0x00030011, _STATE 0x0000041f, _REF_DIV 0x00060f9f
  ATIF: version 1, functions 0x0000c007, notifications 0x000001d1 (brightness change requests: yes)
  ATIF's system parameters: valid 0x00000003, flags 0x00000001: requests by Notify 0x81
  ATIF's backlight: ok; the default 27% on AC, 15% on battery; the input signal 17-255 (of 255); 9 data points
```

Since 37e-2b the driver switches the panel's backlight off when the screen
goes off (the power sequencer's BLON held low through its override; the
panel stays powered and the brightness is kept) and back on with the first
key; a line counts the switches and, while it is off, gives the register
as it was:

```
  backlight switched off 1 times, on 1 times with the screen
```

Since 37e-2c it also dims the panel (the PWM's duty cycle, set as Linux
sets it: the register group locked, the duty written, unlocked, its update
waited for) and counts that, with the lowest level the panel takes (ATIF's
lowest input signal, or Linux's default 12 of 255 without one):

```
  brightness set 3 times (0 not seen updated within 10 ms); the panel's lowest level 0x1111 (ATIF's lowest signal)
```

The VivoBook's AMD graphics (Picasso, a Raven) show the same lines; their
other registers are not read (the driver knows the display engine's layout
for Cezanne only).

Since 37g-5b the AMD graphics run the video BIOS's command tables (AtomBIOS:
the chip's own procedures, in the byte code the driver interprets) and list
them: their number, Linux's name where the master list gives one, their
size in bytes and revision. When the driver starts it runs two that only
ask: getsmuclockinfo for system PLL 0's frequency (as Linux's display
code calls it), and processauxchanneltransaction reading the panel's first
16 DPCD bytes, compared with the driver's own AUX read where it has one
(Cezanne). The line before them counts what the tables did. The old
laptop:

```
  AtomBIOS: 34 command tables (master list 2.1), BAR 5's 131072 registers; run: 246 instructions, 49 reads, 13 writes, 360 us waited, 0 calls
    getsmuclockinfo (41): system PLL 0's VCO 1872.00 MHz (4 us)
    processauxchanneltransaction (78): DPCD 0x000: reply 0x00, 16 bytes: 14 00 44 41 00 00 01 c0 02 00 02 00 00 0b 80 00; the same as our AUX read (390 us)
    tables (index, bytes, revision):
      0 asic_init 460 2.1, 4 digxencodercontrol 788 1.5, 6 75 1.1, 9 202 2.1, 12 setpixelclock 1052 1.7, 13 enabledisppowergating 1230 2.1,
      ...
      76 dig1transmittercontrol 268 1.6, 77 4289 2.2, 78 processauxchanneltransaction 539 1.2, 79 90 1.2
```

A table that stops early says why (`stopped: a loop that does not end at
0x...`, `a register past BAR 5`: the interpreter checks every read of the
image, every parameter and every register).

## See also

[`cpus`](cpus.md), [`net`](net.md), [`windowd`](windowd.md)
