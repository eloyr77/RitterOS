/*
 * (66e-1) AMD graphics: the outputs, from the card's own video BIOS.
 *
 * On a laptop the video BIOS of the processor's graphics is not on the
 * card: the firmware hands it over in the ACPI table VFCT (after the
 * table's header: at 0x34 the offset of the first image; each image a
 * 28-byte header, the PCI place, vendor, device and the image's length at
 * +0x18, then the image). The image is a PCI ROM (0x55 0xAA) holding
 * AtomBIOS: at 0x48 the offset of its ROM header ("ATOM" at +4; the boot
 * message's offset at +16, the master data table's at +32); the master
 * data table lists the data tables (a 4-byte header, then 16-bit offsets:
 * the display objects at +48, dce_info at +58). The display object table
 * (formats 1.4 and 1.5): after its 4-byte header the supported devices,
 * the number of paths, then 16-byte paths: the connector's object ID and
 * its records' offset (from the table's start, as Linux's bios_parser2
 * reads it), the encoder's object ID, the device tag at +12 in both
 * formats. A path with connector 0 is one the board does not wire. An
 * object ID: type in bits 15-12 (3 a connector, 2 an encoder, 7 generic,
 * such as an MXM module's), the enum in 11-8,
 * the ID in 7-0 (a connector's kind: 12 HDMI, 19 DisplayPort, 20 eDP, 24
 * USB-C). Records: a type and a size byte each, 0xFF ends: 1 the DDC/AUX
 * line (i2c_id), 2 the hotplug pin (pin_id). dce_info 4.1: the display
 * clock at boot (+8) and the reference (+12), in 10 kHz.
 *
 * (66e-2a) Where a path's DDC/AUX line and hotplug pin are: the master
 * data table's gpio_pin_lut (+28; content revision 1, 8-byte entries: the
 * GPIO's register as a dword index, its bit, the ID the records name). A
 * DDC line's register is DC_GPIO_DDCn_A (n-1 is the AUX channel), a
 * hotplug pin's DC_GPIO_HPD_A with HPDn's bit. Then, for Cezanne only (the
 * register layout is DCN 2.1's; nothing on the card reports it), the
 * registers are read, none written: in BAR 5 at (DCN's segment 2, 0x34C0,
 * + the register) * 4; AUX channel k's block at 0x1F50 + 0x1C k (CONTROL,
 * SW_CONTROL, ARB_CONTROL, INTERRUPT_CONTROL, SW_STATUS), HPD n's at
 * 0x1F14 + 8 (n-1) (INT_STATUS: bit 1 the sense; CONTROL: bit 28 on).
 * The AUX arbiter (ARB_CONTROL bits 3-2) says who may use the engine:
 * 1 software, 2 the display microcontroller.
 *
 * (66e-2b) AUX transfers, in the order of Linux's dce_aux (one at a time,
 * from the layer's thread only): ask the arbiter (ARB_CONTROL bit 16; the
 * status must say software), acknowledge the last DONE (INTERRUPT_CONTROL
 * bit 1) and wait for it to clear, the byte count to send in SW_CONTROL
 * (bits 20-16: the 4-bit command and 20-bit address, a length byte when
 * any, the data written), the bytes into SW_DATA (bits 15-8; the first
 * write sets index 0 with bit 31, the rest increment), GO (SW_CONTROL bit
 * 0); wait for DONE (SW_STATUS bit 0; its error bits: 9 HPD low, 7-4 out
 * of time, 14/20/22/23 a bad reply; bits 28-24 the reply's bytes); read
 * the reply from SW_DATA (index 0, bit 31, then bit 0 read): its first
 * byte's upper half is the answer (0 ACK, 1 NACK, 2 DEFER; I2C over AUX
 * 4 NACK, 8 DEFER), then the data; give the engine back (bit 17 set, bit
 * 16 cleared). Each wait at most 5 ms (Linux's 2.4); DEFERs retried 7
 * times, 400 us apart, timeouts 3, bad replies 2. The EDID is read as
 * I2C over AUX at 0x50 (the offset written with "middle of transfer",
 * then 16-byte reads, then an address-only read to stop); the DPCD's
 * receiver capabilities (0x000-0x00F) and eDP's revision (0x700) as native
 * reads; an eDP 1.4 panel whose top rate (0x001) is 0 lists its rates at
 * 0x010-0x01F instead (16-bit, in 200 kHz; 0 ends the list). A hotplug
 * pin that senses nothing answers "no monitor" at once.
 *
 * (66e-3) The mode read back, only read. DCN 2.1 has 4 pipes: OTG k (the
 * timing generator, 0x1B2A + 0x80 k) counts each frame: totals - 1,
 * blanking start/end and sync end in pixels and lines from the sync's
 * start (so active = blank start - blank end), MASTER_EN in CONTROL, the
 * frame count (24 bits) and position (vertical 14-0, horizontal 30-16).
 * HUBP k (0x5E5 + 0xDC k) fetches the plane: pixel format (8 is 32-bit
 * ARGB), tiling (SW_MODE 0 linear), the viewport, the pitch register
 * (Linux writes the pitch - 1 there; the laptop's firmware wrote 1920
 * for its 1920-pixel stride: shown as read) and the surface's
 * 48-bit address in the display engine's space; DCN_VM_FB_LOCATION_BASE
 * (and _TOP, _OFFSET; units of 16 MiB) says which of it is the card's
 * memory, seen by the processor through BAR 0 from its start. DIG k (the
 * 5 encoders, 0x2068 + 0x100 k): FE_CNTL's source is an OTG, BE_CNTL's
 * mode (0 DisplayPort), BE_EN_CNTL on; DP k's lanes - 1, training done,
 * video stream on, pixel encoding and depth. The pixel clock is measured:
 * the pixels the OTG counts in 250 ms. The link as trained is the
 * panel's DPCD: 0x100 rate, 0x101 lanes, eDP's 0x115 rate index, 0x202-
 * 0x204 each lane's state (clock recovery, equalized, symbol locked) and
 * the alignment, read with the other DPCD bytes.
 *
 * (66f-1) The way to the vertical blank interrupt, only read. The card's
 * interrupts go through its interrupt handler (IH, in OSSSYS, segment 0
 * 0x10A0): it writes each event into a ring in memory and signals the
 * processor (MSI). Linux sets this family's ring up with physical
 * addresses (IH_RB_CNTL's MC_SPACE 1, IH_CHICKEN's MC_SPACE_GPA_ENABLE),
 * so no window of the card's own address space is needed. NBIO's
 * INTERRUPT_CNTL/_CNTL2 (NBIF0 segment 2, 0xD20) hold the IH's dummy-read
 * settings. The display engine's event for a vertical blank is OTG
 * VSTARTUP (OTG_GLOBAL_SYNC_STATUS: bit 0 its interrupt on, 2 occurred,
 * 3 its interrupt pending; VUPDATE's live status bit 11), at the line
 * VSTARTUP_PARAM sets before VUPDATE.
 *
 * (66f-2) The vertical blank interrupt, as Linux's vega10_ih and
 * nbio_v7_0 set it up: NBIO's dummy-read page and its override off (MSI);
 * IH_CHICKEN's physical addresses; the ring (a page, 1024 dwords: RB_SIZE
 * 10) and its write pointer's write-back given by physical address
 * (MC_SPACE 1), snooped, overflow detection, RPTR_REARM for MSI; both
 * pointers 0; MSI; then the ring and its interrupts on. Each entry is 8
 * dwords: client (7-0; 4 the display engine) and source (15-8; OTG k's
 * VSTARTUP 0x3C + k) in the first. The handler reads entries up to the
 * written-back pointer (bit 0: the ring overflowed), clears the OTG's
 * VSTARTUP event, counts, wakes the waiters and writes the read pointer.
 * Which output shows the picture: its path's encoder ID names its
 * transmitter (UNIPHY 0x1E A/B, 0x20 C/D, 0x21 E/F by enum), DIG n is
 * transmitter n; on when its back end is on and the OTG its front end
 * takes is running. The interrupt is switched on for that OTG. Should
 * the card raise more than STORM interrupts beyond the vertical blanks,
 * the handler switches the ring's interrupts off (`displays` says so).
 *
 * (66f-3) Page flipping. Buffer 0 is where the plane reads now (the
 * firmware's framebuffer), buffer 1 a BUFFER_SLOT after it, both in the
 * card's memory and reached by the processor through BAR 0 (mapped, then
 * write-combining). The VBIOS's vram_usagebyfirmware (master +26: start
 * and size in KiB) is the memory its firmware keeps: the buffers stay
 * clear of it. A flip as Linux's hubp1_program_surface_flip_and_addr:
 * DCSURF_FLIP_CONTROL's flip type "at the vertical sync" and stereo off,
 * then the address's high half, then its low half (which latches it);
 * shown once SURFACE_FLIP_PENDING is clear and SURFACE_EARLIEST_INUSE is
 * the new address. The pitch stays as the firmware set it.
 *
 * (66g-1) What a mode set will rewrite, read back first (only read). The
 * DCCG (DCN's segment 1, 0xC0): OTG k's pixel clock from its DP DTO
 * (PIXEL_RATE_CNTL 0x80 + 4 k, then the phase and modulo: Linux writes the
 * pixel clock and the DP reference in Hz), each pipe's DPP clock DTO, the
 * DENTIST's dividers. The clock block (CLK_BASE segment 1, 0x16E00) counts
 * the clocks running (CLKn_CURRENT_CNT: display, DPP, DP reference, DCF;
 * Linux reads count / 10 as MHz) and holds the VCO's multiplier (PLL_REQ:
 * 9.16 bits x the 48 MHz DFS reference). The OTG's blanking, double
 * buffering, variable refresh (V_TOTAL_MIN/MAX) and its OPTC's source. DP
 * stream d (in DIG d's block): the MSA, the timing the panel is told, and
 * M/N (pixel clock = the link's symbol clock, its rate / 10, x M / N).
 * HUBP k's request sizes and its DLG/TTU schedule (what Linux's display
 * mode library computes; REF_FREQ_TO_PIX_FREQ is the hub's reference over
 * the pixel clock x 2^19). DCHUBBUB's four watermark sets (A-D, one
 * selected), in cycles of the hub's reference: the DCCG's (the VBIOS's
 * reference clock), halved when GLOBAL_TIMER_CNTL's REFDIV is 2.
 *
 * (66g-2) Setting a mode, real timings only (the user's choice), with the
 * trained link kept: a timing from the output's EDID whose plane size and
 * line time are the firmware's (its DLG/TTU schedule is kept, as are its
 * VSTARTUP/VUPDATE/VREADY and watermarks), within the display clock and
 * the link's bandwidth (8b/10b: 8 of every 10 bits). The order is
 * Linux's: the DP stream off at the next vertical blank (DIS_DEFER 2,
 * waited for) and its steering FIFO reset (enc1_stream_encoder_dp_blank);
 * the OTG's data blanked, the OTG off at its vertical blank and the VTG
 * off, OTG_BUSY waited for (optc1_disable_crtc); the DTO off, its phase
 * the new pixel clock in Hz against the modulo the firmware chose, on
 * (dcn20_override_dp_pix_clk); the timing as optc1_program_timing writes
 * it (syncs from 0, blank start = total - front porch, blank end = start
 * - active, V_TOTAL_MIN/MAX = V_TOTAL, START_POINT_CNTL 1 for eDP, the
 * VTG's count from the blank's start and FP2); the MSA
 * (enc1_stream_encoder_dp_set_stream_attribute: start = sync + back
 * porch); the VTG, then the OTG on (optc2_enable_crtc), a frame, the data
 * shown; the stream on as enc2_stream_encoder_dp_unblank: M/N seeded (N
 * 0x8000, M = N x pixel / symbol clock) for the hardware's measuring, the
 * encoder's FIFO (DIG_START) and steering reset, 100 us, the video on.
 *
 * (37e-2a) The panel's backlight, only read. Both laptops' firmware hands
 * a brightness to the graphics driver rather than setting it (_BCM calls
 * ATIF's AFN7: the level kept, Notify 0x81 on the VGA device), so the
 * backlight is the driver's: the eDP panel's power sequencer (LVTMA_PWRSEQ_
 * CNTL: the target state, BLON and DIGON with their overrides; _STATE:
 * the state reached) and the backlight's PWM (BL_PWM_CNTL's duty cycle,
 * PERIOD_CNTL's period and its bit count), at the same place on Raven
 * (DCN 1.0: the VivoBook's Picasso, only these registers mapped) and on
 * Cezanne. ATIF (the method under the device's ACPI node, through
 * acpi_pci_method) is asked at the start: 0 its version, functions and
 * notifications, 1 how it notifies (flags 1: Notify 0x81), 0x10 the
 * backlight's characteristics for the first panel (the defaults on AC and
 * battery, the lowest and highest input signal of 255).
 *
 * (37e-2b) The backlight switched with the screen (the layer's backlight
 * operation): off is BLON held low by PWRSEQ_CNTL's override, BLON_OVRD
 * set and BLON 0 (Linux's is_panel_backlight_on reads it that way), the
 * register kept; on puts it back, so the sequencer drives BLON again. The
 * panel stays powered and the PWM keeps its level, so it comes back as
 * bright as it was. PWRSEQ_STATE's BLON bit still reads high meanwhile.
 *
 * (37e-2c) The brightness set (the layer dims to a third): the PWM's duty
 * as Linux's dce_driver_set_backlight writes it, no lower than ATIF's
 * lowest input signal.
 *
 * Facts from AMD's headers in Linux (atomfirmware.h, atombios.h,
 * displayobject.h, grph_object_id.h, amdgpu_bios.c's VFCT walk; for 66g-1
 * dcn_2_1_0_offset.h/_sh_mask.h, renoir_ip_offset.h, clk_10_0_2, rn_clk_mgr.c,
 * dcn20_hubbub.c, display_rq_dlg_calc_21.c; for 66g-2 dcn10_optc.c,
 * dcn20_optc.c, dcn10/dcn20_stream_encoder.c, dce_clock_source.c,
 * dcn20_hwseq.c; for 37e-2a dcn_1_0/dcn_2_1_0 offsets and masks,
 * vega10_ip_offset.h, dce_panel_cntl.c, amd_acpi.h, amdgpu_acpi.c).
 */
#include "driver.h"
#include "driver_gpu.h"
/* (37g-5a) the AtomBIOS interpreter, a part of this module (its own file:
 * scripts/atomhost.c builds it on the Mac too) */
#include "amdgpu_atom.c"
#include "amdgpu_regs.h"                /* (37g-5c-2) pipe 0's path, by name */

#define VFCT_IMAGE_HEADER 28

/* DCN 2.1's registers, dword indexes (segment 2 added). */
#define DCN_SEG2 0x34C0
#define GPIO_DDC1_A (DCN_SEG2 + 0x28D1)         /* DDC2..5 every 4 */
#define GPIO_DDCVGA_A (DCN_SEG2 + 0x28E9)
#define GPIO_HPD_A (DCN_SEG2 + 0x28F5)
#define AUX_CONTROL(k) (DCN_SEG2 + 0x1F50 + 0x1C * (k))
#define AUX_ARB_CONTROL(k) (AUX_CONTROL(k) + 2)
#define AUX_INTERRUPT_CONTROL(k) (AUX_CONTROL(k) + 3)
#define AUX_SW_STATUS(k) (AUX_CONTROL(k) + 4)
#define HPD_INT_STATUS(n) (DCN_SEG2 + 0x1F14 + 8 * ((n) - 1))
#define HPD_CONTROL(n) (HPD_INT_STATUS(n) + 2)
#define AUX_SW_CONTROL(k) (AUX_CONTROL(k) + 1)
#define AUX_SW_DATA(k) (AUX_CONTROL(k) + 6)

#define SW_DONE 0x1u
#define SW_HPD_DISCON 0x200u
#define SW_TIMEOUT 0xF0u
#define SW_BAD_REPLY 0xD04000u                  /* invalid stop, no detection, invalid H/L */
#define SW_DATA_FIELDS 0x801FFF01u              /* autoincrement off, index, data, read */

#define I2C_WRITE_MOT 0x40
#define I2C_READ 0x10
#define I2C_READ_MOT 0x50
#define DP_READ 0x90
#define EDID_ADDRESS 0x50
#define DCN21_LAST (GPIO_HPD_A + 1)

/* (37g-6) The display engines this driver knows: DCN 2.1 (Cezanne) and
 * DCN 1.0 (Raven, Picasso). Facts from Linux's dcn_2_1_0_offset.h and
 * dcn_1_0_offset.h: every register used here is at the same place on both
 * but the plane's (HUBP) block (its base, its stride, some places inside)
 * and the OTG's registers from its clock control on. The Cezanne's clock
 * block, DPP clock DTOs, interrupt destinations and plane schedule, mode
 * sets and the vertical blank are only used there (is_cezanne); the
 * snapshot of pipe 0's path and the panel's bring-up after a sleep on
 * both, each its own way (37g-6, `picasso`). */
struct dcn_layout {
    const char *name;
    uint32_t hubp, hubp_stride, viewport_start, hubp_cntl, address, flip_control,
             earliest_inuse;
    uint32_t otg_clock_control, otg_vstartup, otg_sync_status;
};
static const struct dcn_layout dcn21 = { "DCN 2.1", 0x5E5, 0xDC, 4, 0xE, 0x25, 0x36, 0x40,
                                         0x5C, 0x5D, 0x60 };
static const struct dcn_layout dcn10 = { "DCN 1.0", 0x559, 0xC4, 3, 0xD, 0x24, 0x35, 0x3E,
                                         0x5E, 0x5F, 0x62 };
static const struct dcn_layout *ly = &dcn21;
static bool is_cezanne, picasso;

#define PIPES 4
#define DIGS 5
#define OTG(k, r) (DCN_SEG2 + 0x1B2A + 0x80 * (k) + (r))   /* r from OTG_H_TOTAL */
#define OTG_H_TOTAL 0
#define OTG_H_BLANK 1
#define OTG_H_SYNC 2
#define OTG_V_TOTAL 5
#define OTG_V_BLANK 0xC
#define OTG_V_SYNC 0xD
#define OTG_CONTROL 0x17
#define OTG_POSITION 0x20
#define OTG_FRAMES 0x22
#define HUBP(k, r) (DCN_SEG2 + ly->hubp + ly->hubp_stride * (k) + (r))   /* r from DCSURF_SURFACE_CONFIG */
#define HUBP_CONFIG 0
#define HUBP_TILING 2
#define HUBP_VIEWPORT_START (ly->viewport_start)
#define HUBP_VIEWPORT_SIZE (ly->viewport_start + 1)
#define HUBP_CNTL (ly->hubp_cntl)
#define HUBP_PITCH 0x22
#define HUBP_ADDRESS (ly->address)
#define HUBP_ADDRESS_HIGH (ly->address + 1)
#define HUBP21(k, r) (DCN_SEG2 + 0x5E5 + 0xDC * (k) + (r))   /* (the Cezanne's: fixed tables) */
#define DIG(k, r) (DCN_SEG2 + 0x2068 + 0x100 * (k) + (r))  /* r from DIG_FE_CNTL */
#define DIG_FE 0
#define DIG_BE 0x47
#define DIG_BE_EN 0x48
#define DP_LINK 0xA0
#define DP_PIXEL_FORMAT 0xA1
#define DP_CONFIG 0xA3
#define DP_VIDEO 0xA4
#define VM_FB_BASE (DCN_SEG2 + 0x493)               /* _TOP, _OFFSET follow */
#define OTG_SYNC_STATUS (ly->otg_sync_status)
#define OTG_INTERRUPT_CONTROL 0x2F
#define OTG_VSTARTUP_PARAM (ly->otg_vstartup)
#define OTG_VUPDATE_PARAM (ly->otg_vstartup + 1)
#define OTG_VREADY_PARAM (ly->otg_vstartup + 2)
#define DCCG_INTERRUPT_DEST (DCN_SEG2 + 0x147)      /* DMU_, DCPG_ .. DCHUB_ follow */
#define OSSSYS 0x10A0
#define IH_RB_CNTL (OSSSYS + 0x80)                  /* BASE, BASE_HI, RPTR, WPTR, WPTR_ADDR_HI, _LO */
#define IH_DOORBELL_RPTR (OSSSYS + 0x87)
#define IH_CNTL (OSSSYS + 0xC0)
#define IH_STATUS (OSSSYS + 0xC2)
#define IH_CHICKEN (OSSSYS + 0x18C)
#define NBIF_SEG2 0xD20
#define INTERRUPT_CNTL (NBIF_SEG2 + 0xF1)           /* _CNTL2 follows */
#define IH_RB_BASE (IH_RB_CNTL + 1)
#define IH_RB_BASE_HI (IH_RB_CNTL + 2)
#define IH_RB_RPTR (IH_RB_CNTL + 3)
#define IH_RB_WPTR (IH_RB_CNTL + 4)
#define IH_RB_WPTR_ADDR_HI (IH_RB_CNTL + 5)
#define IH_RB_WPTR_ADDR_LO (IH_RB_CNTL + 6)
#define IH_RING_BYTES 4096u                         /* one page: 128 entries */
#define IH_ENTRY 32u
#define CLIENT_DCE 4
#define SRC_VSTARTUP 0x3C                           /* + the OTG */
#define VSTARTUP_INT_EN 0x1u
#define VSTARTUP_EVENT_CLEAR 0x10u
#define STORM 10000u
#define HUBP_FLIP_CONTROL (ly->flip_control)
#define HUBP_EARLIEST_INUSE (ly->earliest_inuse)    /* _HIGH follows */
#define FLIP_PENDING (1u << 8)
#define BUFFER_SLOT ((uint64_t)32 << 20)             /* the mapping reaches past it */
/* (66g-1) What a mode set changes, read back. */
#define DCN_SEG1 0xC0
#define DENTIST_DISPCLK_CNTL (DCN_SEG1 + 0x64)
#define PIXEL_RATE_CNTL(k) (DCN_SEG1 + 0x80 + 4 * (k))     /* DP_DTOk_PHASE, _MODULO follow */
#define DPPCLK_DTO_PARAM(k) (DCN_SEG1 + 0x99 + (k))
#define DPPCLK_DTO_CTRL (DCN_SEG1 + 0xB6)
#define CLK_SEG1 0x16E00                                    /* the clock block (CLK_BASE seg 1) */
#define CLK_PLL_REQ (CLK_SEG1 + 0xF)
#define CLK_BYPASS(n) (CLK_SEG1 + 0x49 + 0xA * (n))
#define CLK_CURRENT(n) (CLK_SEG1 + 0x8A + (n))             /* display, DPP, DP reference, DCF */
#define CLK_LAST (CLK_CURRENT(3) + 1)

/* (37e-2a) The panel's power sequencer and backlight (the LVTMA block of
 * the eDP panel), at the same place on Raven (DCN 1.0) and Renoir (DCN 2.1) */
#define PWRSEQ_CNTL (DCN_SEG2 + 0x2883)
#define PWRSEQ_STATE (DCN_SEG2 + 0x2884)
#define PWRSEQ_REF_DIV (DCN_SEG2 + 0x2885)
#define BL_PWM_CNTL (DCN_SEG2 + 0x2888)
#define BL_PWM_CNTL2 (DCN_SEG2 + 0x2889)
#define BL_PWM_PERIOD_CNTL (DCN_SEG2 + 0x288A)
#define BL_PWM_GRP1_REG_LOCK (DCN_SEG2 + 0x288B)
#define BL_LAST (BL_PWM_GRP1_REG_LOCK + 1)
#define PWRSEQ_TARGET_ON (1u << 4)                         /* PWRSEQ_CNTL */
#define PWRSEQ_DIGON (1u << 16)
#define PWRSEQ_DIGON_OVRD (1u << 17)
#define PWRSEQ_BLON (1u << 24)
#define PWRSEQ_BLON_OVRD (1u << 25)
#define PWRSEQ_STATE_TARGET_ON 1u                          /* PWRSEQ_STATE */
#define PWRSEQ_STATE_BLON (1u << 3)
#define PWRSEQ_STATE_DONE (1u << 4)
#define BL_PWM_EN (1u << 31)                               /* BL_PWM_CNTL */
#define BL_PWM_FRACTIONAL (1u << 30)
#define BL_LOCK 1u                                         /* BL_PWM_GRP1_REG_LOCK */
#define BL_UPDATE_PENDING (1u << 8)
#define BL_IGNORE_MASTER_LOCK (1u << 31)
#define BL_LOWEST_SIGNAL 12        /* (37e-2c) of 255, without ATIF's: Linux's default minimum */

/* (37e-2a) ATIF, the firmware's interface for AMD graphics (amd_acpi.h's
 * functions: 0 verify interface, 1 system parameters, 0x10 the backlight's
 * transfer characteristics) */
#define ATIF_VERIFY 0x0
#define ATIF_SYSTEM_PARAMS 0x1
#define ATIF_BACKLIGHT 0x10
#define ATIF_HAS_SYSTEM_PARAMS (1u << 0)                   /* verify's function bits */
#define ATIF_HAS_REQUESTS (1u << 1)
#define ATIF_HAS_BACKLIGHT (1u << 15)
#define ATIF_BRIGHTNESS_REQUESTS (1u << 7)                 /* verify's notifications */
#define ODM(k, r) (DCN_SEG2 + 0x1ACA + 0x10 * (k) + (r))   /* r from OPTC_INPUT_GLOBAL_CONTROL */
#define ODM_SOURCE 1
#define ODM_INPUT_CLOCK 5
#define OTG_H_TIMING 4
#define OTG_V_TOTAL_MIN 6
#define OTG_V_TOTAL_MAX 7
#define OTG_V_TOTAL_CONTROL 9
#define OTG_BLANK_CONTROL 0x18
#define OTG_DOUBLE_BUFFER 0x31
#define DP_MSA_MISC 0xA6
#define DP_VID_TIMING 0xA8
#define DP_VID_N 0xA9
#define DP_VID_M 0xAA
#define DP_MSA_TIMING 0xE4                                  /* PARAM1-4 */
#define HUBP_REQ_SIZE 0xC
#define HUBP_TTU_QOS_WM 0x48                                /* GLOBAL_TTU_CNTL, SURF0_TTU_CNTL0/1 */
#define HUBP_BLANK_OFFSET 0x61                              /* _0, _1 */
#define HUBP_DST_DIMENSIONS 0x63
#define HUBP_DST_AFTER_SCALER 0x64
#define HUBP_PREFETCH 0x65
#define HUBP_VBLANK_PARAMETERS 0x67                         /* 0-4 */
#define HUBP_NOM_PARAMETERS 0x6F                            /* 0-7 */
#define HUBP_PER_LINE_DELIVERY 0x77                         /* _PRE, then the line's */
#define HUBP_REF_TO_PIX 0x7A
#define HUBP_DRQ_LIMIT 0x7B
#define HUBBUB(r) (DCN_SEG2 + 0x505 + (r))                  /* r from DCHUBBUB_ARB_DF_REQ_OUTSTAND */
#define HUBBUB_SAT_LEVEL 1
#define HUBBUB_DRAM_STATE 3
#define HUBBUB_WM(set, r) HUBBUB(4 + 5 * (set) + (r))      /* urgency, trip, SR enter, exit, DRAM */
#define HUBBUB_WM_CHANGE 0x18
#define HUBBUB_GLOBAL_TIMER 0x1A
/* (66g-2) What the mode set writes. */
#define OTG_H_SYNC_CNTL 3
#define OTG_V_SYNC_CNTL 0xE
#define OTG_CLOCK_CONTROL (ly->otg_clock_control)
#define VTG(k) (DCN_SEG2 + 0x528 + (k))
#define DP_STEER_FIFO 0xA5
#define OTG_BUSY (1u << 16)
#define BLANK_DATA_EN (1u << 8)
#define BLANK_DE_MODE (1u << 16)
#define BLANK_DOUBLE_BUFFER (1u << 16)
#define VTG_ENABLE (1u << 31)
#define PLANE_BLANK_EN 1u                                   /* DCHUBP_CNTL's (37g-5d) */
#define PLANE_NO_REQUESTS (1u << 1)
#define PLANE_TTU_DISABLE (1u << 12)
#define STREAM_ENABLE 1u
#define STREAM_STATUS (1u << 16)
#define DIG_START (1u << 10)
#define MN_GEN_EN (1u << 8)

struct path {
    uint16_t connector, encoder, device_tag;
    uint8_t ddc, hpd;                   /* (0xFF: no record) */
    bool has_ddc, has_hpd;
};

static struct gpu_card cezanne;
static const uint8_t *rom;              /* the video BIOS image, in the VFCT table */
static uint32_t rom_size, vfct_size;
static int image_index, image_count;
static char boot_message[64];
static uint8_t data_format, data_content, objects_format, objects_content;
static uint32_t boot_dispclk_10khz, refclk_10khz;
static uint32_t boot_sclk_10khz, boot_mclk_10khz;   /* (37g-6) firmware_info's, for asic_init */
static uint32_t fw_start_kb, fw_kb, driver_kb;   /* (66f-3) vram_usagebyfirmware */
static bool has_usage;
static struct path paths[GPU_OUTPUTS];
static int path_count;
static int output_path[GPU_OUTPUTS];    /* the path each output came from */

struct pin {
    uint16_t reg;                       /* the GPIO's register, a dword index */
    uint8_t bit, id;
};
static struct pin pins[64];
static int pin_count;
static bool has_pins;
static volatile uint8_t *regs;          /* BAR 5 (Cezanne only) */
static volatile uint8_t *bl_regs;       /* (37e-2a) BAR 5 up to the backlight's (Raven too) */

/* (37e-2a) ATIF's answers, as the firmware gave them (its sizes kept). */
static int atif_found;                  /* 1 there, 0 not looked at, -1 none, -2 failed */
static uint16_t atif_version;
static uint32_t atif_notifications, atif_functions;
static int atif_params_got, atif_backlight_got;        /* the answers' lengths, or < 0 */
static uint32_t atif_params_valid, atif_params_flags;
static uint8_t atif_params_command;
static uint8_t atif_bl_error, atif_bl_ac, atif_bl_dc, atif_bl_min, atif_bl_max, atif_bl_points;

/* (37e-2b) The backlight switched off: PWRSEQ_CNTL as it was, to put back. */
static volatile bool bl_off;
static uint32_t bl_saved;
static uint64_t bl_offs, bl_ons;
static uint64_t bl_sets, bl_late;       /* (37e-2c) brightness set; its update not seen in 10 ms */

/* (66f-2) The interrupt ring and the vertical blanks it brings. */
static volatile uint32_t *ih_ring, *ih_wptr;
static uint32_t ih_rptr;
static int vblank_otg = -1;             /* the OTG whose VSTARTUP interrupts, or -1 */
static volatile uint64_t vblanks, interrupts, entries, others, overflows;
static volatile uint32_t last_other;    /* the first dword of the last entry not ours */
static volatile bool stormed;           /* the handler switched the interrupts off */

/* (66f-3) The screen buffers. */
static int plane = -1;                  /* the HUBP that shows them */
static uint64_t buffer0;                /* buffer 0's address in the display engine's space */
static uint64_t buffer0_phys;           /* ... and the processor's (BAR 0) */
static uint64_t buffer_bytes;           /* each; 0: not made for this mode */
static uint32_t buffer_pitch;           /* the pitch register (pixels) */
static uint64_t flips;
static int buffer_shown;
static uint32_t mode_sets, mode_ms, stream_ms;          /* (66g-2) mode sets, the last's times */
static uint32_t last_w, last_h, last_mhz;
static struct wait_queue vblank_waiters;

/* (66e-2b) How each output's AUX transfers went, and what its DPCD said. */
static struct aux_tally {
    uint64_t transfers, acks, defers, nacks, late, bad, hpd_low, busy, short_reads;
    uint64_t edid_reads, edid_whole;
    uint32_t last_status;               /* SW_STATUS at the last failure */
    uint8_t dpcd[16], edp_revision, rates[16];
    uint8_t link[2], rate_index, lanes[3];      /* (66e-3) 0x100-0x101, 0x115, 0x202-0x204 */
    bool has_dpcd, has_edp_revision, has_rates, has_link, has_rate_index, has_lanes;
    uint64_t edid_us;                   /* the last whole read of 128 bytes or more */
} tally[GPU_OUTPUTS];

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The video BIOS image for d in the VFCT table (vendor and device must
 * match; the bus may differ from the one the firmware saw). */
static bool find_rom(const struct pci_device *d)
{
    const struct acpi_header *h = acpi_find_table("VFCT");
    if (!h || h->length < 0x4C)
        return false;
    const uint8_t *t = (const uint8_t *)h;
    vfct_size = h->length;
    for (uint32_t at = u32(t + 0x34); at + VFCT_IMAGE_HEADER <= vfct_size;) {
        const uint8_t *v = t + at;
        uint32_t length = u32(v + 0x18);
        if (at + VFCT_IMAGE_HEADER + length > vfct_size)
            break;
        image_count++;
        if (length && u16(v + 12) == d->vendor_id && u16(v + 14) == d->device_id && !rom) {
            rom = v + VFCT_IMAGE_HEADER;
            rom_size = length;
            image_index = image_count;
        }
        at += VFCT_IMAGE_HEADER + length;
    }
    return rom != 0;
}

/* AtomBIOS: the boot message, dce_info's clocks and the display paths. */
static bool read_atom(void)
{
    if (rom_size < 0x4A || rom[0] != 0x55 || rom[1] != 0xAA)
        return false;
    uint32_t header = u16(rom + 0x48);
    if (header + 36 > rom_size || memcmp(rom + header + 4, "ATOM", 4))
        return false;
    uint32_t message = u16(rom + header + 16), master = u16(rom + header + 32);
    for (int i = 0; message && message + (uint32_t)i < rom_size && i < 63; i++) {
        char c = (char)rom[message + (uint32_t)i];
        if (c == 0 || c == '\r' || c == '\n')
            break;
        boot_message[i] = c >= 0x20 && c < 0x7F ? c : '?';
    }
    if (!master || master + 70 > rom_size)
        return false;
    data_format = rom[master + 2];
    data_content = rom[master + 3];
    uint32_t dce = u16(rom + master + 58), objects = u16(rom + master + 48);
    uint32_t usage = u16(rom + master + 26), lut = u16(rom + master + 28);
    if (usage && usage + 12 <= rom_size) {
        fw_start_kb = u32(rom + usage + 4);
        fw_kb = u16(rom + usage + 8);
        driver_kb = u16(rom + usage + 10);
        has_usage = true;
    }
    if (lut && lut + 4 <= rom_size && rom[lut + 3] == 1) {
        uint32_t size = u16(rom + lut);
        has_pins = true;
        for (uint32_t at = lut + 4; at + 8 <= lut + size && at + 8 <= rom_size &&
             pin_count < (int)(sizeof pins / sizeof pins[0]); at += 8) {
            pins[pin_count].reg = u16(rom + at);    /* (the low half, as bios_parser2) */
            pins[pin_count].bit = rom[at + 4];
            pins[pin_count].id = rom[at + 6];
            pin_count++;
        }
    }
    /* (37g-6) firmware_info (master +12) v3.1: the boot's engine and memory
     * clocks, asic_init's parameters (Linux's amdgpu_atom_asic_init) */
    uint32_t info = u16(rom + master + 12);
    if (info && info + 16 <= rom_size && rom[info + 2] == 3) {
        boot_sclk_10khz = u32(rom + info + 8);
        boot_mclk_10khz = u32(rom + info + 12);
    }
    if (dce && dce + 16 <= rom_size && rom[dce + 2] == 4) {
        boot_dispclk_10khz = u32(rom + dce + 8);
        refclk_10khz = u16(rom + dce + 12);
    }
    if (!objects || objects + 8 > rom_size)
        return false;
    objects_format = rom[objects + 2];
    objects_content = rom[objects + 3];
    if (objects_format != 1 || (objects_content != 4 && objects_content != 5))
        return false;                               /* (a layout this driver does not know) */
    uint32_t n = rom[objects + 6];
    for (uint32_t k = 0; k < n && path_count < GPU_OUTPUTS; k++) {
        const uint8_t *p = rom + objects + 8 + 16 * k;
        if (p + 16 > rom + rom_size)
            break;
        struct path *q = &paths[path_count++];
        q->connector = u16(p);
        q->encoder = u16(p + 4);
        q->device_tag = u16(p + 12);
        uint32_t r = u16(p + 2) ? objects + u16(p + 2) : 0;
        for (; r && r + 2 <= rom_size && rom[r] != 0xFF && rom[r + 1]; r += rom[r + 1]) {
            if (rom[r] == 1 && r + 4 <= rom_size) {
                q->ddc = rom[r + 2];
                q->has_ddc = true;
            } else if (rom[r] == 2 && r + 4 <= rom_size) {
                q->hpd = rom[r + 2];
                q->has_hpd = true;
            }
        }
    }
    return true;
}

static const char *connector_kind(uint16_t id, const char **where)
{
    if ((id >> 12 & 0xF) != 3) {
        *where = "";
        return 0;                                   /* not a connector */
    }
    switch (id & 0xFF) {
    case 20: *where = "the panel"; return "edp";
    case 12: *where = "HDMI"; return "hdmi";
    case 19: *where = "DisplayPort"; return "dp";
    case 24: *where = "USB-C"; return "dp";
    default: *where = ""; return 0;
    }
}

static const char *connector_name(uint16_t id)
{
    if (!id)
        return "none";
    if ((id >> 12 & 0xF) == 7)
        return "generic";
    if ((id >> 12 & 0xF) != 3)
        return "other";
    switch (id & 0xFF) {
    case 20: return "eDP";
    case 12: return "HDMI";
    case 19: return "DisplayPort";
    case 24: return "USB-C";
    case 5: return "VGA";
    case 1: case 2: case 3: case 4: return "DVI";
    default: return "other";
    }
}

static const struct pin *find_pin(uint8_t id)
{
    for (int i = 0; i < pin_count; i++)
        if (pins[i].id == id)
            return &pins[i];
    return 0;
}

/* The DDC line (1-5, 6 for VGA's; 0 unknown) a GPIO register is. */
static int ddc_line(const struct pin *p)
{
    if (!p)
        return 0;
    if (p->reg == GPIO_DDCVGA_A)
        return 6;
    if (p->reg >= GPIO_DDC1_A && p->reg <= GPIO_DDC1_A + 16 && (p->reg - GPIO_DDC1_A) % 4 == 0)
        return (p->reg - GPIO_DDC1_A) / 4 + 1;
    return 0;
}

/* The hotplug pin (HPD1-6; 0 unknown) a GPIO register and bit are. */
static int hpd_number(const struct pin *p)
{
    static const uint8_t bits[] = {0, 8, 16, 24, 26, 28};
    if (!p || p->reg != GPIO_HPD_A)
        return 0;
    for (int n = 0; n < 6; n++)
        if (p->bit == bits[n])
            return n + 1;
    return 0;
}

static uint32_t rd(uint32_t index)
{
    return mmio_read32(regs + 4 * index);
}

static void wr(uint32_t index, uint32_t v)
{
    mmio_write32(regs + 4 * index, v);
}

static void wait_us(uint32_t us)
{
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz * us / 1000000;
    while (arch_counter() < end)
        arch_cpu_relax();
}

/* Wait (at most 5 ms, looking every 10 us) until SW_STATUS's DONE is
 * `done`: false out of time. */
static bool wait_done(int k, uint32_t done)
{
    for (int i = 0; i <= 500; i++) {
        if ((rd(AUX_SW_STATUS(k)) & SW_DONE) == done)
            return true;
        wait_us(10);
    }
    return false;
}

enum { AUX_DONE, AUX_BUSY, AUX_LATE, AUX_BAD, AUX_HPD };

/* One AUX transaction on channel k: `action` (the command), `address`,
 * `length` bytes sent (out) or asked for (in, at most 16). The reply's
 * answer in *answer, the bytes it carried in *got. */
static int aux_once(int o, int k, uint8_t action, uint32_t address, const uint8_t *out,
                    uint32_t length, uint8_t *in, uint8_t *answer, uint32_t *got)
{
    *got = 0;
    uint32_t arb = rd(AUX_ARB_CONTROL(k));
    if ((arb >> 2 & 3) == 2 || !(rd(AUX_CONTROL(k)) & 1))
        return AUX_BUSY;                            /* the microcontroller's, or off */
    wr(AUX_ARB_CONTROL(k), arb | 1u << 16);
    int result = AUX_DONE;
    if ((rd(AUX_ARB_CONTROL(k)) >> 2 & 3) != 1) {
        result = AUX_BUSY;
        goto release;
    }
    wr(AUX_INTERRUPT_CONTROL(k), rd(AUX_INTERRUPT_CONTROL(k)) | 2);
    wait_done(k, 0);
    bool write = (action & 0x10) == 0;              /* I2C write or DP write */
    uint32_t bytes = (length ? 4 : 3) + (write ? length : 0);
    wr(AUX_SW_CONTROL(k), (rd(AUX_SW_CONTROL(k)) & ~0x1F00F0u) | bytes << 16);
    uint32_t v = (rd(AUX_SW_DATA(k)) & ~SW_DATA_FIELDS) | 1u << 31 |
                 (uint32_t)(action | (address >> 16 & 0xF)) << 8;
    wr(AUX_SW_DATA(k), v);
    v = (v & ~0x8000FF00u) | (address >> 8 & 0xFF) << 8;
    wr(AUX_SW_DATA(k), v);
    v = (v & ~0xFF00u) | (address & 0xFF) << 8;
    wr(AUX_SW_DATA(k), v);
    if (length) {
        v = (v & ~0xFF00u) | (length - 1) << 8;
        wr(AUX_SW_DATA(k), v);
    }
    for (uint32_t i = 0; write && i < length; i++) {
        v = (v & ~0xFF00u) | (uint32_t)out[i] << 8;
        wr(AUX_SW_DATA(k), v);
    }
    wr(AUX_SW_CONTROL(k), rd(AUX_SW_CONTROL(k)) | 1);
    tally[o].transfers++;
    bool done = wait_done(k, SW_DONE);
    uint32_t status = rd(AUX_SW_STATUS(k)), count = status >> 24 & 0x1F;
    if (status & SW_HPD_DISCON)
        result = AUX_HPD;
    else if (!done || (status & SW_TIMEOUT))
        result = AUX_LATE;
    else if ((status & SW_BAD_REPLY) || !count)
        result = AUX_BAD;
    if (result != AUX_DONE) {
        tally[o].last_status = status;
        goto release;
    }
    v = rd(AUX_SW_DATA(k)) & ~0x1F0000u;
    wr(AUX_SW_DATA(k), v);
    v |= 1u << 31;
    wr(AUX_SW_DATA(k), v);
    v |= 1;
    wr(AUX_SW_DATA(k), v);
    *answer = (uint8_t)(rd(AUX_SW_DATA(k)) >> 8 & 0xFF) >> 4;
    if (*answer == 0 && !write) {
        if (count - 1 > length) {
            result = AUX_BAD;
            tally[o].last_status = status;
            goto release;
        }
        for (uint32_t i = 0; i < count - 1; i++)
            in[i] = (uint8_t)(rd(AUX_SW_DATA(k)) >> 8);
        *got = count - 1;
    }
release:
    wr(AUX_ARB_CONTROL(k), (rd(AUX_ARB_CONTROL(k)) | 1u << 17) & ~(1u << 16));
    return result;
}

/* A transaction with DP's retries: the bytes read (a read may come back
 * short), or -1. */
static int aux_transfer(int o, int k, uint8_t action, uint32_t address, const uint8_t *out,
                        uint32_t length, uint8_t *in)
{
    int defers = 0, late = 0, bad = 0;
    for (;;) {
        uint8_t answer = 0xFF;
        uint32_t got;
        int r = aux_once(o, k, action, address, out, length, in, &answer, &got);
        if (r == AUX_BUSY) {
            tally[o].busy++;
            return -1;
        }
        if (r == AUX_HPD) {
            tally[o].hpd_low++;
            return -1;
        }
        if (r == AUX_LATE) {
            tally[o].late++;
            if (++late > 3)
                return -1;
            continue;
        }
        if (r == AUX_BAD) {
            tally[o].bad++;
            if (++bad > 2)
                return -1;
            continue;
        }
        if (answer == 0) {
            tally[o].acks++;
            if (got < length && !(action & 0x80) && (action & 0x10))
                tally[o].short_reads++;
            return (int)got;
        }
        if (answer == 2 || answer == 8) {
            tally[o].defers++;
            if (++defers > 7)
                return -1;
            wait_us(400);
            continue;
        }
        tally[o].nacks++;                           /* 1 AUX NACK, 4 I2C NACK */
        return -1;
    }
}

/* The AUX channel and hotplug pin of output o, from gpio_pin_lut: false
 * when either is unknown. */
static bool output_lines(int o, int *k, int *n)
{
    const struct path *q = &paths[output_path[o]];
    int line = q->has_ddc ? ddc_line(find_pin(q->ddc)) : 0;
    *n = q->has_hpd ? hpd_number(find_pin(q->hpd)) : 0;
    *k = line - 1;
    return line >= 1 && line <= 5 && *n;
}

static void read_dpcd(int o, int k)
{
    uint8_t b[16];
    if (aux_transfer(o, k, DP_READ, 0x000, 0, sizeof b, b) == (int)sizeof b) {
        memcpy(tally[o].dpcd, b, sizeof b);
        tally[o].has_dpcd = true;
    }
    if (!strcmp(cezanne.outputs[o].kind, "edp") && aux_transfer(o, k, DP_READ, 0x700, 0, 1, b) == 1) {
        tally[o].edp_revision = b[0];
        tally[o].has_edp_revision = true;
    }
    if (tally[o].has_dpcd && tally[o].dpcd[1] == 0 &&
        aux_transfer(o, k, DP_READ, 0x010, 0, sizeof b, b) == (int)sizeof b) {
        memcpy(tally[o].rates, b, sizeof b);
        tally[o].has_rates = true;
    }
    if (aux_transfer(o, k, DP_READ, 0x100, 0, 2, b) == 2) {
        memcpy(tally[o].link, b, 2);
        tally[o].has_link = true;
    }
    if (tally[o].has_rates && aux_transfer(o, k, DP_READ, 0x115, 0, 1, b) == 1) {
        tally[o].rate_index = b[0];
        tally[o].has_rate_index = true;
    }
    if (aux_transfer(o, k, DP_READ, 0x202, 0, 3, b) == 3) {
        memcpy(tally[o].lanes, b, 3);
        tally[o].has_lanes = true;
    }
}

static volatile bool bringing_up;           /* (37g-5c-3: the bring-up uses AUX) */

static bool read_edid(struct gpu_card *g, int o, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)g;
    if (bringing_up)
        return false;
    int k, n;
    if (!regs || len > 256 || offset > 255 || !output_lines(o, &k, &n))
        return false;
    if (!(rd(HPD_INT_STATUS(n)) >> 1 & 1))
        return false;                               /* nothing on the hotplug pin */
    tally[o].edid_reads++;
    uint64_t began = arch_counter();
    uint8_t start = (uint8_t)offset;
    bool whole = aux_transfer(o, k, I2C_WRITE_MOT, EDID_ADDRESS, &start, 1, 0) >= 0;
    for (uint32_t at = 0; whole && at < len;) {
        uint32_t ask = len - at < 16 ? len - at : 16;
        int got = aux_transfer(o, k, I2C_READ_MOT, EDID_ADDRESS, 0, ask, buf + at);
        if (got <= 0)
            whole = false;
        else
            at += (uint32_t)got;
    }
    aux_transfer(o, k, I2C_READ, EDID_ADDRESS, 0, 0, 0);    /* the stop */
    if (!whole)
        return false;
    tally[o].edid_whole++;
    if (len >= 128)
        tally[o].edid_us = (arch_counter() - began) * 1000000 / arch_counter_hz();
    if (offset == 0 && len >= 128)
        read_dpcd(o, k);                            /* with every whole read of a monitor */
    return true;
}

static const char *aux_holder(uint32_t arb)
{
    switch (arb >> 2 & 3) {
    case 0: return "nobody";
    case 1: return "software";
    case 2: return "the display microcontroller";
    default: return "?";
    }
}

/* (66e-2a) An output's lines through gpio_pin_lut, and the registers as
 * they are now (only read). */
static void report_output(int o)
{
    const struct path *q = &paths[output_path[o]];
    const struct pin *ddc = q->has_ddc ? find_pin(q->ddc) : 0;
    const struct pin *hpd = q->has_hpd ? find_pin(q->hpd) : 0;
    int line = ddc_line(ddc), n = hpd_number(hpd);
    const char *name = gpu_output_name(&cezanne, o);
    kprintf("  %-6s", name);
    if (!q->has_ddc)
        kprintf(" no DDC/AUX line;");
    else if (!ddc)
        kprintf(" line 0x%02x not in gpio_pin_lut;", q->ddc);
    else if (line == 6)
        kprintf(" line 0x%02x: VGA's DDC (register 0x%04x);", q->ddc, ddc->reg);
    else if (line)
        kprintf(" line 0x%02x: DDC%d (register 0x%04x), AUX channel %d;", q->ddc, line, ddc->reg,
                line - 1);
    else
        kprintf(" line 0x%02x: register 0x%04x bit %u, not a DDC line;", q->ddc, ddc->reg,
                ddc->bit);
    if (!q->has_hpd)
        kprintf(" no hotplug pin\n");
    else if (!hpd)
        kprintf(" pin %u not in gpio_pin_lut\n", q->hpd);
    else if (n)
        kprintf(" pin %u: HPD%d (register 0x%04x bit %u)\n", q->hpd, n, hpd->reg, hpd->bit);
    else
        kprintf(" pin %u: register 0x%04x bit %u, not a hotplug pin\n", q->hpd, hpd->reg,
                hpd->bit);
    if (!regs)
        return;
    if (n) {
        uint32_t status = rd(HPD_INT_STATUS(n)), control = rd(HPD_CONTROL(n));
        kprintf("         HPD%d: sense %u, on %u (DC_HPD_INT_STATUS 0x%08x, DC_HPD_CONTROL 0x%08x)\n",
                n, status >> 1 & 1, control >> 28 & 1, status, control);
    }
    if (line >= 1 && line <= 5) {
        int k = line - 1;
        uint32_t control = rd(AUX_CONTROL(k)), arb = rd(AUX_ARB_CONTROL(k));
        uint32_t status = rd(AUX_SW_STATUS(k)), interrupt = rd(AUX_INTERRUPT_CONTROL(k));
        uint32_t pad = rd(ddc->reg - 1);                /* DC_GPIO_DDCn_MASK */
        kprintf("         AUX%d: on %u, follows HPD select %u; held by %s; pad in AUX mode %u "
                "(AUX_CONTROL 0x%08x, ARB_CONTROL 0x%08x, SW_STATUS 0x%08x, INTERRUPT_CONTROL "
                "0x%08x, DC_GPIO_DDC%d_MASK 0x%08x)\n", k, control & 1, control >> 20 & 7,
                aux_holder(arb), pad >> 16 & 1, control, arb, status, interrupt, line, pad);
        const struct aux_tally *t = &tally[o];
        kprintf("         AUX%d: %lu transfers: %lu ACK, %lu DEFER, %lu NACK, %lu out of time, "
                "%lu bad replies, %lu with HPD low, %lu engine busy, %lu short reads; EDID: "
                "%lu reads, %lu whole", k, t->transfers, t->acks, t->defers, t->nacks, t->late,
                t->bad, t->hpd_low, t->busy, t->short_reads, t->edid_reads, t->edid_whole);
        if (t->edid_us)
            kprintf(", the last of 128 bytes or more in %lu us", t->edid_us);
        if (t->late + t->bad + t->hpd_low)
            kprintf(" (the last failure: SW_STATUS 0x%08x)", t->last_status);
        kprintf("\n");
    }
    const struct aux_tally *t = &tally[o];
    if (t->has_dpcd) {
        static const char *const edp[] = {"1.1", "1.2", "1.3", "1.4", "1.4a", "1.4b", "1.5"};
        uint32_t rate = t->dpcd[1] * 27u;           /* code x 0.27 Gbit/s */
        kprintf("         DPCD: DisplayPort %u.%u, ", t->dpcd[0] >> 4, t->dpcd[0] & 0xF);
        if (t->dpcd[1])
            kprintf("up to %u.%02u Gbit/s a lane (0x%02x)", rate / 100, rate % 100, t->dpcd[1]);
        else if (t->has_rates) {
            kprintf("link rates (its table) ");
            for (int i = 0; i < 16; i += 2) {
                uint32_t r = (uint32_t)(t->rates[i] | t->rates[i + 1] << 8);    /* 200 kHz */
                if (!r)
                    break;
                kprintf("%s%u.%02u", i ? " " : "", r / 5000, r % 5000 / 50);
            }
            kprintf(" Gbit/s a lane");
        } else
            kprintf("no top rate (0x00)");
        kprintf(", %u lanes%s", t->dpcd[2] & 0x1F, t->dpcd[2] & 0x80 ? ", enhanced framing" : "");
        if (t->has_edp_revision)
            kprintf("; eDP %s (0x%02x)", t->edp_revision < 7 ? edp[t->edp_revision] : "?",
                    t->edp_revision);
        kprintf(";");
        for (int i = 0; i < 16; i++)
            kprintf(" %02x", t->dpcd[i]);
        kprintf("\n");
    }
}

/* A lane's rate from the panel's table (entry i), in 10 Mbit/s; 0 if none. */
static uint32_t table_rate(const struct aux_tally *t, int i)
{
    if (!t->has_rates || i < 0 || i > 7)
        return 0;
    uint32_t r = (uint32_t)(t->rates[2 * i] | t->rates[2 * i + 1] << 8);   /* 200 kHz */
    return r / 50;
}

/* (66e-3) The link as the panel says it was trained. */
static void report_link(int o)
{
    const struct aux_tally *t = &tally[o];
    if (!t->has_link)
        return;
    uint32_t rate = t->link[0] ? t->link[0] * 27u : table_rate(t, t->rate_index);
    kprintf("         link (the panel's DPCD): %u.%02u Gbit/s a lane", rate / 100, rate % 100);
    if (!t->link[0] && t->has_rate_index)
        kprintf(" (its table's rate %u)", t->rate_index);
    else
        kprintf(" (0x%02x)", t->link[0]);
    kprintf(", %u lanes%s", t->link[1] & 0x1F, t->link[1] & 0x80 ? ", enhanced framing" : "");
    if (t->has_lanes) {
        int lanes = t->link[1] & 0x1F;
        kprintf("; lanes:");
        for (int l = 0; l < lanes && l < 4; l++) {
            uint8_t v = t->lanes[l / 2] >> (4 * (l & 1)) & 0xF;
            kprintf(" %d %s%s%s", l, v & 1 ? "C" : "-", v & 2 ? "E" : "-", v & 4 ? "S" : "-");
        }
        kprintf(" (C clock recovered, E equalized, S symbols locked)%s",
                t->lanes[2] & 1 ? ", aligned" : ", not aligned");
    }
    kprintf("\n");
}

/* (66e-3) The pixels OTG k counts in about 250 ms: its pixel clock in kHz
 * and frames a second (x 1000); 0 if it did not move. */
static uint64_t measure_otg(int k, uint32_t htotal, uint32_t vtotal, uint64_t *frames_mhz)
{
    uint64_t when[2], count[2];
    for (int n = 0; n < 2; n++) {
        if (n)
            timer_sleep_ms(250);
        uint32_t f, pos, again;
        do {                                        /* frame and position of one frame */
            f = rd(OTG(k, OTG_FRAMES)) & 0xFFFFFF;
            pos = rd(OTG(k, OTG_POSITION));
            when[n] = arch_counter();
            again = rd(OTG(k, OTG_FRAMES)) & 0xFFFFFF;
        } while (again != f);
        count[n] = ((uint64_t)f * vtotal + (pos & 0x7FFF)) * htotal + (pos >> 16 & 0x7FFF);
    }
    uint64_t frame = (uint64_t)htotal * vtotal, span = (uint64_t)0x1000000 * frame;
    uint64_t pixels = (count[1] + span - count[0]) % span;  /* (the frame count wraps) */
    uint64_t ticks = when[1] - when[0];
    if (!pixels || !ticks || !frame)
        return 0;
    uint64_t hz = arch_counter_hz();
    *frames_mhz = pixels * hz / ticks * 1000 / frame;
    return pixels * hz / ticks / 1000;
}

static const char *pixel_format(uint32_t f)
{
    switch (f) {
    case 1: return "16-bit ARGB1555";
    case 3: return "16-bit RGB565";
    case 8: return "32-bit ARGB8888";
    case 9: return "32-bit ABGR8888";
    case 10: return "32-bit ARGB2101010";
    case 11: return "32-bit ABGR2101010";
    default: return "?";
    }
}

/* (66g-1) x * num / den as "%lu.%03lu": whole and thousandths. */
static void milli(uint64_t x, uint64_t num, uint64_t den, uint64_t *whole, uint64_t *frac)
{
    uint64_t v = den ? x * num / den : 0;
    *whole = v / 1000;
    *frac = v % 1000;
}

static int output_otg(int o);

/* The trained link's rate (10 Mbit/s units) of the output OTG k drives, 0
 * if not known: the link's symbol clock in MHz. */
static uint32_t link_rate_of(int k)
{
    for (int o = 0; o < cezanne.output_count; o++) {
        const struct aux_tally *t = &tally[o];
        if (output_otg(o) == k && t->has_link)
            return t->link[0] ? t->link[0] * 27u : table_rate(t, t->rate_index);
    }
    return 0;
}

/* (66g-1) OTG k's settings a mode set rewrites: where its pixel clock
 * comes from (the DP DTO: phase and modulo in Hz of the DP reference), the
 * DPP clock of its pipe, its blanking, double buffering, variable refresh
 * and data source. measured: its pixel clock in kHz. */
static void report_otg_setting(int k, uint64_t measured)
{
    uint32_t rate = rd(PIXEL_RATE_CNTL(k));
    uint32_t phase = rd(PIXEL_RATE_CNTL(k) + 1), modulo = rd(PIXEL_RATE_CNTL(k) + 2);
    uint64_t ref_khz = is_cezanne ? (uint64_t)rd(CLK_CURRENT(2)) * 100 : 0, w, f;
    kprintf("        its pixel clock: source %u, DP DTO%d on %u, phase %u / modulo %u", rate & 3, k,
            rate >> 4 & 1, phase, modulo);
    if (modulo && ref_khz) {
        milli(ref_khz, phase, modulo, &w, &f);
        kprintf(" of the DP reference: %lu.%03lu MHz", w, f);
    }
    if (measured && phase)
        kprintf("; the measured clock makes the reference %lu.%03lu MHz",
                measured * modulo / phase / 1000, measured * modulo / phase % 1000);
    kprintf(" (PIXEL_RATE_CNTL 0x%08x)\n", rate);
    if (!is_cezanne)
        return;                                     /* (DCN 1.0 has no DPP clock DTOs) */
    uint32_t dto = rd(DPPCLK_DTO_CTRL) >> (4 * k), param = rd(DPPCLK_DTO_PARAM(k));
    uint64_t dpp_khz = (uint64_t)rd(CLK_CURRENT(1)) * 100;
    kprintf("        pipe %d's DPP clock: DTO on %u, phase %u / modulo %u", k, dto & 1,
            param & 0xFF, param >> 16 & 0xFF);
    if (dto & 1 && param >> 16 & 0xFF) {
        milli(dpp_khz, param & 0xFF, param >> 16 & 0xFF, &w, &f);
        kprintf(" of the DPP clock: %lu.%03lu MHz", w, f);
    }
    kprintf("\n");
    uint32_t blank = rd(OTG(k, OTG_BLANK_CONTROL)), vtc = rd(OTG(k, OTG_V_TOTAL_CONTROL));
    uint32_t source = rd(ODM(k, ODM_SOURCE));
    kprintf("        blanked %u (BLANK_CONTROL 0x%08x), double buffering 0x%08x, H timing 0x%08x; "
            "variable refresh %s (V total min %u, max %u, control 0x%08x); data from OPP %u, %u "
            "segment(s) (DATA_SOURCE_SELECT 0x%08x, INPUT_CLOCK 0x%08x)\n", blank & 1, blank,
            rd(OTG(k, OTG_DOUBLE_BUFFER)), rd(OTG(k, OTG_H_TIMING)), vtc & 3 ? "on" : "off",
            rd(OTG(k, OTG_V_TOTAL_MIN)) & 0x7FFF, rd(OTG(k, OTG_V_TOTAL_MAX)) & 0x7FFF, vtc,
            source >> 8 & 0xF, (source & 1) + 1, source, rd(ODM(k, ODM_INPUT_CLOCK)));
    uint32_t vtg = rd(VTG(k));
    kprintf("        VTG%d: on %u, count from line %u, FP2 %u; sync polarity H %u V %u (1: negative); "
            "OTG_CONTROL 0x%08x\n", k, vtg >> 31, vtg >> 16 & 0x7FFF, vtg & 0x7FFF,
            rd(OTG(k, OTG_H_SYNC_CNTL)) & 1, rd(OTG(k, OTG_V_SYNC_CNTL)) & 1, rd(OTG(k, OTG_CONTROL)));
}

/* (66g-1) DP stream d's timing as the panel is told (the MSA) and its M/N:
 * the pixel clock is the link's symbol clock (its rate / 10) times M / N. */
static void report_stream(int d, int k)
{
    uint32_t p1 = rd(DIG(d, DP_MSA_TIMING)), p2 = rd(DIG(d, DP_MSA_TIMING + 1));
    uint32_t p3 = rd(DIG(d, DP_MSA_TIMING + 2)), p4 = rd(DIG(d, DP_MSA_TIMING + 3));
    uint32_t timing = rd(DIG(d, DP_VID_TIMING));
    uint32_t n = rd(DIG(d, DP_VID_N)) & 0xFFFFFF, m = rd(DIG(d, DP_VID_M)) & 0xFFFFFF;
    kprintf("        DP%d's MSA: total %ux%u, active %ux%u from (%u,%u), sync %u%c x %u%c, MISC 0x%08x"
            "\n", d, p1 >> 16, p1 & 0xFFFF, p4 >> 16, p4 & 0xFFFF, p2 >> 16, p2 & 0xFFFF,
            p3 >> 16 & 0x7FFF, p3 >> 31 ? '-' : '+', p3 & 0x7FFF, p3 >> 15 & 1 ? '-' : '+',
            rd(DIG(d, DP_MSA_MISC)));
    kprintf("        DP%d's M/N: %u / %u (made by the hardware %u, N x%u; VID_TIMING 0x%08x)", d, m, n,
            timing >> 8 & 1, 1u << (timing >> 10 & 3), timing);
    uint32_t rate = link_rate_of(k);
    if (rate && n) {
        uint64_t w, f;
        milli((uint64_t)rate * 1000, m, n, &w, &f);
        kprintf(": the link's %u MHz x M / N = %lu.%03lu MHz", rate, w, f);
    }
    kprintf("\n");
}

/* (66g-1) What HUBP k asks the memory for, and when (its request sizes,
 * and the delivery and prefetch schedule Linux's display mode library
 * computes: DLG and TTU). REF_FREQ_TO_PIX_FREQ is the hub's reference
 * clock over the pixel clock, x 2^19: with the measured clock, the
 * reference the schedule was made for. */
static void report_requests(int k, uint64_t measured)
{
    uint32_t req = rd(HUBP(k, HUBP_REQ_SIZE)), r2p = rd(HUBP(k, HUBP_REF_TO_PIX)) & 0x1FFFFF;
    kprintf("        requests: swath height %u, chunk %u, min chunk %u, PTE row height %u "
            "(REQ_SIZE_CONFIG 0x%08x); reference / pixel clock 0x%x", 1u << (req & 7),
            req >> 8 & 7, req >> 11 & 3, req >> 4 & 7, req, r2p);
    if (measured)
        kprintf(" (the measured clock makes the reference %lu.%03lu MHz)",
                (measured * r2p >> 19) / 1000, (measured * r2p >> 19) % 1000);
    kprintf(", per line total 0x%x\n", rd(HUBP(k, HUBP_DST_DIMENSIONS)) & 0x1FFFFF);
    kprintf("        DLG: blank offset 0x%08x 0x%08x, after scaler 0x%08x, prefetch 0x%08x, vblank",
            rd(HUBP(k, HUBP_BLANK_OFFSET)), rd(HUBP(k, HUBP_BLANK_OFFSET + 1)),
            rd(HUBP(k, HUBP_DST_AFTER_SCALER)), rd(HUBP(k, HUBP_PREFETCH)));
    for (int i = 0; i < 5; i++)
        kprintf(" 0x%08x", rd(HUBP(k, HUBP_VBLANK_PARAMETERS + i)));
    kprintf(", nominal");
    for (int i = 0; i < 8; i++)
        kprintf(" 0x%08x", rd(HUBP(k, HUBP_NOM_PARAMETERS + i)));
    kprintf(", per line 0x%08x 0x%08x, DRQ limit 0x%08x\n", rd(HUBP(k, HUBP_PER_LINE_DELIVERY)),
            rd(HUBP(k, HUBP_PER_LINE_DELIVERY + 1)), rd(HUBP(k, HUBP_DRQ_LIMIT)));
    kprintf("        TTU: QoS watermark 0x%08x, global 0x%08x, surface 0x%08x 0x%08x\n",
            rd(HUBP(k, HUBP_TTU_QOS_WM)), rd(HUBP(k, HUBP_TTU_QOS_WM + 1)),
            rd(HUBP(k, HUBP_TTU_QOS_WM + 2)), rd(HUBP(k, HUBP_TTU_QOS_WM + 3)));
}

/* (66g-1) The clocks as the clock block counts them (Linux reads a count
 * / 10 as MHz), its VCO (FbMult, 9.16 bits, x the 48 MHz DFS reference),
 * the dividers the DENTIST holds; the hub's watermarks: four sets (A-D,
 * the one in use selected), in cycles of the hub's reference clock (the
 * DCCG's, the VBIOS's reference, halved when REFDIV is 2). */
static void report_clocks(void)
{
    static const char *const names[4] = {"display", "DPP", "DP reference", "DCF"};
    kprintf("  clocks (the clock block's counts):");
    for (int n = 0; n < 4; n++) {
        uint32_t c = rd(CLK_CURRENT(n));
        kprintf("%s %s %u.%u MHz (bypass %u)", n ? "," : "", names[n], c / 10, c % 10,
                rd(CLK_BYPASS(n)) & 7);
    }
    uint32_t pll = rd(CLK_PLL_REQ), dentist = rd(DENTIST_DISPCLK_CNTL);
    uint64_t vco_khz = ((uint64_t)(pll & 0x1FF) * 65536 + (pll >> 16)) * 48000 / 65536;
    kprintf("; VCO %lu.%03lu MHz (PLL_REQ 0x%08x); DENTIST dividers display 0x%02x, DPP 0x%02x "
            "(DISPCLK_CNTL 0x%08x)\n", vco_khz / 1000, vco_khz % 1000, pll, dentist & 0x7F,
            dentist >> 24 & 0x7F, dentist);
    uint32_t timer = rd(HUBBUB(HUBBUB_GLOBAL_TIMER)), change = rd(HUBBUB(HUBBUB_WM_CHANGE));
    uint32_t ref_khz = refclk_10khz * 10 / ((timer & 0xF) == 2 ? 2 : 1);
    kprintf("  watermarks: set %c in use (CHANGE_CNTL 0x%08x); the hub's reference %u.%03u MHz "
            "(GLOBAL_TIMER_CNTL 0x%08x: on %u, REFDIV %u); SAT level %u, DRAM state 0x%08x\n",
            'A' + (change & 3), change, ref_khz / 1000, ref_khz % 1000, timer, timer >> 12 & 1,
            timer & 0xF, rd(HUBBUB(HUBBUB_SAT_LEVEL)), rd(HUBBUB(HUBBUB_DRAM_STATE)));
    static const char *const kinds[5] = {"urgent", "trip to memory", "self refresh enter",
                                         "self refresh exit", "DRAM clock change"};
    static const uint32_t masks[5] = {0x3FFF, 0x1FFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    for (int set = 0; set < 4; set++) {
        kprintf("        %c:", 'A' + set);
        for (int r = 0; r < 5; r++) {
            uint32_t v = rd(HUBBUB_WM(set, r)) & masks[r];
            uint32_t ns = ref_khz ? (uint32_t)((uint64_t)v * 1000000 / ref_khz) : 0;
            kprintf("%s %s %u (%u.%02u us)", r ? "," : "", kinds[r], v, ns / 1000, ns % 1000 / 10);
        }
        kprintf("\n");
    }
}

/* (66e-3) The mode as the hardware has it: timing generators, encoders,
 * planes, where a plane reads from, the pixel clock measured. */
static void report_mode(void)
{
    uint64_t fb_base = (uint64_t)(rd(VM_FB_BASE) & 0xFFFFFF) << 24;
    uint64_t fb_top = (uint64_t)(rd(VM_FB_BASE + 1) & 0xFFFFFF) << 24 | 0xFFFFFF;
    uint64_t fb_offset = (uint64_t)(rd(VM_FB_BASE + 2) & 0xFFFFFF) << 24;
    static const uint8_t depth[] = {6, 8, 10, 12, 14, 16};
    uint64_t measured_khz[PIPES] = {0};
    for (int k = 0; k < PIPES; k++) {
        uint32_t control = rd(OTG(k, OTG_CONTROL));
        if (!(control & 1)) {
            kprintf("  OTG%d: off\n", k);
            continue;
        }
        uint32_t ht = (rd(OTG(k, OTG_H_TOTAL)) & 0x7FFF) + 1;
        uint32_t vt = (rd(OTG(k, OTG_V_TOTAL)) & 0x7FFF) + 1;
        uint32_t hb = rd(OTG(k, OTG_H_BLANK)), vb = rd(OTG(k, OTG_V_BLANK));
        uint32_t hs = rd(OTG(k, OTG_H_SYNC)), vs = rd(OTG(k, OTG_V_SYNC));
        uint32_t hstart = hb & 0x7FFF, hend = hb >> 16 & 0x7FFF;
        uint32_t vstart = vb & 0x7FFF, vend = vb >> 16 & 0x7FFF;
        uint32_t hsync = (hs >> 16 & 0x7FFF) - (hs & 0x7FFF);
        uint32_t vsync = (vs >> 16 & 0x7FFF) - (vs & 0x7FFF);
        kprintf("  OTG%d: on, %ux%u, total %ux%u; H: sync %u, back porch %u, front porch %u; "
                "V: sync %u, back porch %u, front porch %u\n", k, hstart - hend, vstart - vend,
                ht, vt, hsync, hend - hsync, ht - hstart, vsync, vend - vsync, vt - vstart);
        for (int o = 0; o < cezanne.output_count; o++) {
            const struct edid_info *monitor = gpu_output_monitor(&cezanne, o);
            const struct edid_detailed *e = monitor ? &monitor->detailed[0] : 0;
            if (!e || !e->htotal)
                continue;
            kprintf("        %s's EDID, its first timing: %ux%u, total %ux%u; H: sync %u, back "
                    "porch %u, front porch %u; V: sync %u, back porch %u, front porch %u; %u.%03u "
                    "MHz\n", gpu_output_name(&cezanne, o), e->hactive, e->vactive, e->htotal, e->vtotal,
                    e->hsync_end - e->hsync_start, e->htotal - e->hsync_end,
                    e->hsync_start - e->hactive, e->vsync_end - e->vsync_start,
                    e->vtotal - e->vsync_end, e->vsync_start - e->vactive, e->clock_khz / 1000,
                    e->clock_khz % 1000);
        }
        uint64_t frames_mhz = 0, khz = measure_otg(k, ht, vt, &frames_mhz);
        if (khz)
            kprintf("        measured over 250 ms: pixel clock %lu.%03lu MHz, %lu.%03lu frames "
                    "a second\n", khz / 1000, khz % 1000, frames_mhz / 1000, frames_mhz % 1000);
        else
            kprintf("        measured over 250 ms: its counters did not move\n");
        measured_khz[k] = khz;
        report_otg_setting(k, khz);
        for (int d = 0; d < DIGS; d++) {
            uint32_t fe = rd(DIG(d, DIG_FE)), be = rd(DIG(d, DIG_BE));
            if ((fe & 7) != (uint32_t)k || !(rd(DIG(d, DIG_BE_EN)) & 1))
                continue;
            uint32_t mode = be >> 16 & 7, config = rd(DIG(d, DP_CONFIG));
            uint32_t format = rd(DIG(d, DP_PIXEL_FORMAT)), c = format >> 24 & 7;
            kprintf("        DIG%d: on, %s; its back end takes front ends 0x%02x", d,
                    mode == 0 ? "DisplayPort" : mode == 2 ? "DVI" : mode == 3 ? "HDMI" :
                    "another mode", be >> 8 & 0x7F);
            if (mode == 0)
                kprintf("; %u lanes, link trained %u, video stream on %u, %s, %u bits a "
                        "component", (config & 3) + 1, rd(DIG(d, DP_LINK)) >> 4 & 1,
                        rd(DIG(d, DP_VIDEO)) & 1, (format & 7) == 0 ? "RGB" : "YCbCr",
                        c < 6 ? depth[c] : 0);
            kprintf(" (DIG_FE_CNTL 0x%08x, DIG_BE_CNTL 0x%08x)\n", fe, be);
            if (mode == 0)
                report_stream(d, k);
        }
    }
    const struct pci_bar *vram = &cezanne.pci->bars[0];
    for (int k = 0; k < PIPES; k++) {
        uint32_t cntl = rd(HUBP(k, HUBP_CNTL));
        if (cntl & 4) {
            kprintf("  HUBP%d: off\n", k);
            continue;
        }
        if ((cntl & 1) && !rd(HUBP(k, HUBP_ADDRESS)) && !rd(HUBP(k, HUBP_ADDRESS_HIGH))) {
            kprintf("  HUBP%d: blanked, no surface\n", k);
            continue;
        }
        uint32_t size = rd(HUBP(k, HUBP_VIEWPORT_SIZE)), start = rd(HUBP(k, HUBP_VIEWPORT_START));
        uint32_t format = rd(HUBP(k, HUBP_CONFIG)) & 0x7F;
        uint64_t address = (uint64_t)(rd(HUBP(k, HUBP_ADDRESS_HIGH)) & 0xFFFF) << 32 |
                           rd(HUBP(k, HUBP_ADDRESS));
        uint32_t tiling = rd(HUBP(k, HUBP_TILING));
        kprintf("  HUBP%d: on%s, format %u (%s), %ux%u from (%u,%u), pitch register %u, %s "
                "(tiling 0x%08x); surface at 0x%lx", k, cntl & 1 ? " (blanked)" : "", format,
                pixel_format(format), size & 0x3FFF, size >> 16 & 0x3FFF, start & 0x3FFF,
                start >> 16 & 0x3FFF, rd(HUBP(k, HUBP_PITCH)) & 0x3FFF,
                tiling & 0x1F ? "tiled" : "linear", tiling, address);
        if (fb_top > fb_base && address >= fb_base && address <= fb_top)
            kprintf(", the card's memory + 0x%lx (memory 0x%lx; through BAR 0 0x%lx)",
                    address - fb_base, address - fb_base + fb_offset,
                    vram->address + address - fb_base);
        kprintf("\n");
        if (is_cezanne)
            report_requests(k, measured_khz[k]);
    }
    kprintf("  the card's memory in the display engine's space: 0x%lx - 0x%lx, at memory 0x%lx "
            "(DCN_VM_FB_LOCATION, _OFFSET); BAR 0 0x%lx, %lu MiB\n", fb_base, fb_top, fb_offset,
            vram->address, vram->size >> 20);
    if (is_cezanne)
        report_clocks();
}

/* (66f-1) The interrupt path as the firmware left it, and OTG k's VUPDATE
 * watched for 50 ms (its live status bit's rises and their lines). */
static void report_interrupts(int k)
{
    struct pci_device *d = cezanne.pci;
    uint8_t msi = pci_find_capability(d, 0x05), msix = pci_find_capability(d, 0x11);
    kprintf("  interrupts: MSI ");
    if (msi) {
        uint16_t c = pci_read16(d, msi + 2);
        kprintf("at 0x%02x (control 0x%04x: on %u, %u vectors offered, 64-bit %u)", msi, c, c & 1,
                1u << (c >> 1 & 7), c >> 7 & 1);
    } else
        kprintf("none");
    kprintf(", MSI-X ");
    if (msix)
        kprintf("at 0x%02x (control 0x%04x)\n", msix, pci_read16(d, msix + 2));
    else
        kprintf("none\n");
    uint32_t cntl = rd(IH_RB_CNTL);
    kprintf("  IH: RB_CNTL 0x%08x (ring on %u, interrupts on %u, 2^%u dwords, write-back %u, "
            "snoop %u, MC_SPACE %u); ring at 0x%08x%08x (x 256), read 0x%x, write 0x%x, "
            "write-back to 0x%08x%08x; IH_CNTL 0x%08x, STATUS 0x%08x (idle %u), CHICKEN 0x%08x "
            "(physical addresses %u), DOORBELL_RPTR 0x%08x\n", cntl, cntl & 1, cntl >> 17 & 1,
            cntl >> 1 & 0x3F, cntl >> 8 & 1, cntl >> 20 & 1, cntl >> 28 & 7, rd(IH_RB_CNTL + 2),
            rd(IH_RB_CNTL + 1), rd(IH_RB_CNTL + 3), rd(IH_RB_CNTL + 4), rd(IH_RB_CNTL + 5),
            rd(IH_RB_CNTL + 6), rd(IH_CNTL), rd(IH_STATUS), rd(IH_STATUS) & 1, rd(IH_CHICKEN),
            rd(IH_CHICKEN) >> 4 & 1, rd(IH_DOORBELL_RPTR));
    kprintf("  NBIO: INTERRUPT_CNTL 0x%08x, INTERRUPT_CNTL2 0x%08x; the display engine's "
            "interrupt destinations: DCCG 0x%08x, DMU 0x%08x, DCHUB 0x%08x\n", rd(INTERRUPT_CNTL),
            rd(INTERRUPT_CNTL + 1), rd(DCCG_INTERRUPT_DEST), rd(DCCG_INTERRUPT_DEST + 1),
            rd(DCCG_INTERRUPT_DEST + 6));
    uint32_t sync = rd(OTG(k, OTG_SYNC_STATUS)), vt = (rd(OTG(k, OTG_V_TOTAL)) & 0x7FFF) + 1;
    kprintf("  OTG%d's events: GLOBAL_SYNC_STATUS 0x%08x (VSTARTUP: interrupt on %u, occurred %u, "
            "pending %u), VSTARTUP_PARAM 0x%08x, VUPDATE_PARAM 0x%08x, VREADY_PARAM 0x%08x, "
            "INTERRUPT_CONTROL 0x%08x\n", k, sync, sync & 1, sync >> 2 & 1, sync >> 3 & 1,
            rd(OTG(k, OTG_VSTARTUP_PARAM)), rd(OTG(k, OTG_VUPDATE_PARAM)),
            rd(OTG(k, OTG_VREADY_PARAM)), rd(OTG(k, OTG_INTERRUPT_CONTROL)));
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz / 20;
    uint32_t rises = 0, lines[4] = {0}, was = sync >> 11 & 1;
    while (arch_counter() < end) {                  /* (50 ms: three frames) */
        uint32_t now = rd(OTG(k, OTG_SYNC_STATUS)) >> 11 & 1;
        if (now && !was) {
            if (rises < 4)
                lines[rises] = rd(OTG(k, OTG_POSITION)) & 0x7FFF;
            rises++;
        }
        was = now;
    }
    kprintf("        VUPDATE (live bit) rose %u times in 50 ms", rises);
    for (uint32_t i = 0; i < rises && i < 4; i++)
        kprintf("%s%u", i ? ", " : ", at lines ", lines[i]);
    kprintf(" (of %u)\n", vt);
}

/* The transmitter (DIG n) an encoder object ID names, or -1. */
static int transmitter(uint16_t encoder)
{
    int pair;
    switch (encoder & 0xFF) {
    case 0x1E: pair = 0; break;                 /* UNIPHY: A, B */
    case 0x20: pair = 1; break;                 /* UNIPHY1: C, D */
    case 0x21: pair = 2; break;                 /* UNIPHY2: E, F */
    default: return -1;
    }
    int en = encoder >> 8 & 7;
    int n = 2 * pair + en - 1;
    return en >= 1 && en <= 2 && n < DIGS ? n : -1;
}

/* The OTG output o's picture comes from, or -1 when it shows none. */
static int output_otg(int o)
{
    int n = transmitter(paths[output_path[o]].encoder);
    if (!regs || n < 0 || !(rd(DIG(n, DIG_BE_EN)) & 1))
        return -1;
    uint32_t fronts = rd(DIG(n, DIG_BE)) >> 8 & 0x7F;
    for (int f = 0; f < DIGS; f++)
        if (fronts & 1u << f) {
            int k = (int)(rd(DIG(f, DIG_FE)) & 7);
            return k < PIPES && (rd(OTG(k, OTG_CONTROL)) & 1) ? k : -1;
        }
    return -1;
}

static bool showing(struct gpu_card *g, int o)
{
    (void)g;
    return output_otg(o) >= 0;
}

/* Wait at most ms for (rd(reg) & mask) == want; whether it came. */
static bool wait_reg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz * ms / 1000;
    while ((rd(reg) & mask) != want)
        if (arch_counter() >= end)
            return (rd(reg) & mask) == want;
        else
            arch_cpu_relax();
    return true;
}

/* (66g-2) Timing t on output o, the trained link kept: refused (nothing
 * touched) unless the plane's schedule the firmware made still fits. */
static bool mode_fits(int o, const struct edid_detailed *t, int *k_out, int *d_out, char *why,
                      size_t why_size)
{
    int k = output_otg(o), d = -1;
    for (int f = 0; k >= 0 && f < DIGS; f++)
        if ((rd(DIG(f, DIG_FE)) & 7) == (uint32_t)k && (rd(DIG(f, DIG_BE_EN)) & 1))
            d = f;
    if (k < 0 || d < 0) {
        ksnprintf(why, why_size, "the output shows nothing");
        return false;
    }
    if ((rd(DIG(d, DIG_BE)) >> 16 & 7) != 0) {
        ksnprintf(why, why_size, "only DisplayPort outputs so far");
        return false;
    }
    uint32_t cntl = rd(HUBP(k, HUBP_CNTL)), size = rd(HUBP(k, HUBP_VIEWPORT_SIZE));
    uint32_t w = size & 0x3FFF, h = size >> 16 & 0x3FFF;
    if (cntl & 5) {
        ksnprintf(why, why_size, "pipe %d's plane is off or blanked", k);
        return false;
    }
    if (buffer_shown) {
        ksnprintf(why, why_size, "screen buffer %d is shown", buffer_shown);
        return false;
    }
    if (t->hactive != w || t->vactive != h) {
        ksnprintf(why, why_size, "the plane's schedule (the firmware's) is for %ux%u", w, h);
        return false;
    }
    if (!t->clock_khz || t->htotal < t->hsync_end || t->vtotal < t->vsync_end ||   /* (porch 0: the panel's) */
        t->hsync_start < t->hactive || t->vsync_start < t->vactive) {
        ksnprintf(why, why_size, "the timing is not one a monitor could take");
        return false;
    }
    /* The schedule paces each line: the line's time must stay the
     * firmware's (to 0.5%); a different refresh comes from more or fewer
     * lines a frame. */
    uint32_t pr = rd(PIXEL_RATE_CNTL(k)), phase = rd(PIXEL_RATE_CNTL(k) + 1);
    uint64_t old_line = (uint64_t)((rd(OTG(k, OTG_H_TOTAL)) & 0x7FFF) + 1) * t->clock_khz * 1000;
    uint64_t new_line = (uint64_t)t->htotal * phase;
    if (!(pr & 0x10) || !phase) {
        ksnprintf(why, why_size, "its pixel clock is not from a DP DTO");
        return false;
    }
    if ((old_line > new_line ? old_line - new_line : new_line - old_line) * 200 > old_line) {
        ksnprintf(why, why_size, "a line of %u pixels at %u.%03u MHz takes another time than the "
                  "plane's schedule (the firmware's) is for", t->htotal, t->clock_khz / 1000,
                  t->clock_khz % 1000);
        return false;
    }
    uint32_t display_khz = rd(CLK_CURRENT(0)) * 100;
    if (t->clock_khz > display_khz) {
        ksnprintf(why, why_size, "needs %u.%03u MHz, more than the display clock's %u MHz",
                  t->clock_khz / 1000, t->clock_khz % 1000, display_khz / 1000);
        return false;
    }
    static const uint8_t depth[] = {6, 8, 10, 12, 14, 16};
    uint32_t c = rd(DIG(d, DP_PIXEL_FORMAT)) >> 24 & 7, bpc = c < 6 ? depth[c] : 16;
    uint32_t rate = link_rate_of(k), lanes = (rd(DIG(d, DP_CONFIG)) & 3) + 1;
    uint64_t need = (uint64_t)t->clock_khz * 3 * bpc, carry = (uint64_t)rate * 10000 * 8 / 10 * lanes;
    if (!rate) {
        ksnprintf(why, why_size, "the link's rate is not known (its DPCD not read)");
        return false;
    }
    if (need > carry) {
        ksnprintf(why, why_size, "needs %lu Mbit/s, the link carries %lu (%u lanes at %u.%02u "
                  "Gbit/s)", need / 1000, carry / 1000, lanes, rate / 100, rate % 100);
        return false;
    }
    uint32_t vstartup = rd(OTG(k, OTG_VSTARTUP_PARAM)) & 0x3FF;
    if (t->vtotal - t->vactive <= vstartup) {
        ksnprintf(why, why_size, "its vertical blank of %u lines is not longer than the %u lines "
                  "the plane starts fetching before it", t->vtotal - t->vactive, vstartup);
        return false;
    }
    *k_out = k;
    *d_out = d;
    return true;
}

/* (66g-2) The mode set, in the order of Linux's DCN code with the trained
 * link kept (see the header): the stream off, the OTG off, its DTO and
 * timing, the MSA, the OTG on, the stream on. */
static bool set_mode(struct gpu_card *g, int o, const struct edid_detailed *t,
                     struct gpu_screen *screen, char *why, size_t why_size)
{
    (void)g;
    int k, d;
    if (!mode_fits(o, t, &k, &d, why, why_size))
        return false;
    uint64_t started = arch_counter(), hz = arch_counter_hz();
    uint32_t hsync = t->hsync_end - t->hsync_start, vsync = t->vsync_end - t->vsync_start;
    uint32_t hfront = t->hsync_start - t->hactive, vfront = t->vsync_start - t->vactive;
    if (vfront < 1)
        vfront = 1;                                 /* (Linux's front porch work-around) */
    uint32_t hback = t->htotal - t->hactive - hfront - hsync;
    uint32_t hb_start = t->htotal - hfront, hb_end = hb_start - t->hactive;
    uint32_t vb_start = t->vtotal - vfront, vb_end = vb_start - t->vactive;
    uint32_t vstartup = rd(OTG(k, OTG_VSTARTUP_PARAM)) & 0x3FF;
    uint32_t rate = link_rate_of(k), v;
    /* 1. The stream off at the next vertical blank; the encoder leaves the OTG's data. */
    v = rd(DIG(d, DP_VIDEO));
    wr(DIG(d, DP_VIDEO), (v & ~0x300u) | 2u << 8);
    wr(DIG(d, DP_VIDEO), rd(DIG(d, DP_VIDEO)) & ~STREAM_ENABLE);
    bool stopped = wait_reg(DIG(d, DP_VIDEO), STREAM_STATUS, 0, 120);
    wr(DIG(d, DP_STEER_FIFO), rd(DIG(d, DP_STEER_FIFO)) | 1);
    /* 2. The OTG's data blanked, the OTG stopped at its next vertical blank. */
    wr(OTG(k, OTG_BLANK_CONTROL), (rd(OTG(k, OTG_BLANK_CONTROL)) | BLANK_DATA_EN) & ~BLANK_DE_MODE);
    wr(OTG(k, OTG_DOUBLE_BUFFER), rd(OTG(k, OTG_DOUBLE_BUFFER)) & ~BLANK_DOUBLE_BUFFER);
    wr(OTG(k, OTG_CONTROL), (rd(OTG(k, OTG_CONTROL)) & ~0x301u) | 3u << 8);
    wr(VTG(k), rd(VTG(k)) & ~VTG_ENABLE);
    bool idle = wait_reg(OTG(k, OTG_CLOCK_CONTROL), OTG_BUSY, 0, 100);
    /* 3. The pixel clock: phase the new clock in Hz, modulo the reference as kept. */
    wr(PIXEL_RATE_CNTL(k), rd(PIXEL_RATE_CNTL(k)) & ~0x10u);
    wr(PIXEL_RATE_CNTL(k) + 1, t->clock_khz * 1000);
    wr(PIXEL_RATE_CNTL(k), rd(PIXEL_RATE_CNTL(k)) | 0x10u);
    /* 4. The timing (optc1_program_timing's formulas, sync starting at 0). */
    wr(OTG(k, OTG_H_TOTAL), t->htotal - 1);
    wr(OTG(k, OTG_H_SYNC), hsync << 16);
    wr(OTG(k, OTG_H_BLANK), hb_start | hb_end << 16);
    wr(OTG(k, OTG_H_SYNC_CNTL), (rd(OTG(k, OTG_H_SYNC_CNTL)) & ~1u) | !t->hsync_high);
    wr(OTG(k, OTG_V_TOTAL), t->vtotal - 1);
    wr(OTG(k, OTG_V_TOTAL_MAX), t->vtotal - 1);
    wr(OTG(k, OTG_V_TOTAL_MIN), t->vtotal - 1);
    wr(OTG(k, OTG_V_SYNC), vsync << 16);
    wr(OTG(k, OTG_V_BLANK), vb_start | vb_end << 16);
    wr(OTG(k, OTG_V_SYNC_CNTL), (rd(OTG(k, OTG_V_SYNC_CNTL)) & ~1u) | !t->vsync_high);
    wr(OTG(k, OTG_CONTROL), (rd(OTG(k, OTG_CONTROL)) | 1u << 12) & ~(1u << 13));
    int fp2 = (int)vb_end - (int)vstartup + 1;
    wr(VTG(k), (rd(VTG(k)) & VTG_ENABLE) | vb_start << 16 | (uint32_t)(fp2 < 0 ? -fp2 : 0));
    /* 5. What the panel is told. */
    wr(DIG(d, DP_MSA_TIMING), t->htotal << 16 | t->vtotal);
    wr(DIG(d, DP_MSA_TIMING + 1), (hsync + hback) << 16 | (t->vtotal - t->vactive - vfront));
    wr(DIG(d, DP_MSA_TIMING + 2), hsync << 16 | (uint32_t)!t->hsync_high << 31 | vsync |
                                  (uint32_t)!t->vsync_high << 15);
    wr(DIG(d, DP_MSA_TIMING + 3), t->hactive << 16 | t->vactive);
    /* 6. The OTG on (its VTG first), a frame, its data shown again. */
    wr(ODM(k, ODM_SOURCE), (rd(ODM(k, ODM_SOURCE)) & ~0xF00u) | (uint32_t)k << 8);
    wr(VTG(k), rd(VTG(k)) | VTG_ENABLE);
    wr(OTG(k, OTG_CONTROL), (rd(OTG(k, OTG_CONTROL)) & ~0x300u) | 3u << 8 | 1);
    timer_sleep_ms((uint32_t)((uint64_t)t->htotal * t->vtotal / t->clock_khz) + 2);
    wr(OTG(k, OTG_BLANK_CONTROL), rd(OTG(k, OTG_BLANK_CONTROL)) & ~(BLANK_DATA_EN | BLANK_DE_MODE));
    mode_ms = (uint32_t)((arch_counter() - started) * 1000 / hz);
    /* 7. The stream on: M/N seeded (N 0x8000) for the hardware's measuring,
     * the encoder's FIFO and its steering reset, then the video. */
    wr(DIG(d, DP_VID_TIMING), rd(DIG(d, DP_VID_TIMING)) & ~MN_GEN_EN);
    wr(DIG(d, DP_VID_N), 0x8000);
    wr(DIG(d, DP_VID_M), (uint32_t)((uint64_t)0x8000 * t->clock_khz / ((uint64_t)rate * 1000)));
    wr(DIG(d, DP_VID_TIMING), (rd(DIG(d, DP_VID_TIMING)) & ~0xC00u) | MN_GEN_EN);
    wait_reg(DIG(d, DP_VIDEO), STREAM_STATUS, 0, 50);
    wr(DIG(d, DIG_FE), rd(DIG(d, DIG_FE)) | DIG_START);
    wait_us(1);
    wr(DIG(d, DIG_FE), rd(DIG(d, DIG_FE)) & ~DIG_START);
    wr(DIG(d, DP_STEER_FIFO), rd(DIG(d, DP_STEER_FIFO)) | 1);
    wait_us(10);
    wr(DIG(d, DP_STEER_FIFO), rd(DIG(d, DP_STEER_FIFO)) & ~1u);
    wait_us(100);
    wr(DIG(d, DP_VIDEO), rd(DIG(d, DP_VIDEO)) | STREAM_ENABLE);
    bool streaming = wait_reg(DIG(d, DP_VIDEO), STREAM_STATUS, STREAM_STATUS, 120);
    stream_ms = (uint32_t)((arch_counter() - started) * 1000 / hz);
    mode_sets++;
    last_w = t->hactive, last_h = t->vactive, last_mhz = t->refresh_mhz;
    if (!stopped || !idle || !streaming) {
        ksnprintf(why, why_size, "%s", !stopped ? "the stream did not stop" : !idle ?
                  "the OTG stayed busy" : "the stream did not start again");
        return false;
    }
    /* The screen: the plane's surface as before (its size kept), through BAR 0. */
    uint64_t fb_base = (uint64_t)(rd(VM_FB_BASE) & 0xFFFFFF) << 24;
    uint64_t address = (uint64_t)(rd(HUBP(k, HUBP_ADDRESS_HIGH)) & 0xFFFF) << 32 |
                       rd(HUBP(k, HUBP_ADDRESS));
    uint64_t phys = cezanne.pci->bars[0].address + address - fb_base;
    uint32_t pitch = rd(HUBP(k, HUBP_PITCH)) & 0x3FFF;
    if (!paging_write_combining(phys, (uint64_t)pitch * 4 * t->vactive)) {
        ksnprintf(why, why_size, "the screen cannot be mapped");
        return false;
    }
    *screen = (struct gpu_screen){ (volatile uint32_t *)phys_to_virt(phys), t->hactive, t->vactive,
                                   pitch, 16, 8, 0 };
    buffer_bytes = 0;                               /* (66f-3: made again at the next flip) */
    return true;
}

/* The ring's entries since the last look; interrupt context. */
static void ih_interrupt(void *context)
{
    (void)context;
    interrupts++;
    for (;;) {
        uint32_t w = *ih_wptr;
        if (w & 1) {                                /* it overflowed: go on past the oldest */
            w = rd(IH_RB_WPTR);
            if (w & 1) {
                overflows++;
                ih_rptr = ((w & ~1u) + IH_ENTRY) & (IH_RING_BYTES - 1);
                uint32_t c = rd(IH_RB_CNTL);
                wr(IH_RB_CNTL, c | 1u << 31);       /* WPTR_OVERFLOW_CLEAR, then off again */
                wr(IH_RB_CNTL, c & ~(1u << 31));
            }
        }
        w &= (IH_RING_BYTES - 1) & ~3u;
        if (w == ih_rptr)
            break;
        arch_read_barrier();                        /* the pointer before the entries */
        bool vblank = false;
        while (ih_rptr != w) {
            uint32_t dw0 = ih_ring[ih_rptr / 4];
            entries++;
            if ((dw0 & 0xFF) == CLIENT_DCE && (int)(dw0 >> 8 & 0xFF) == SRC_VSTARTUP + vblank_otg)
                vblank = true;
            else {
                others++;
                last_other = dw0;
            }
            ih_rptr = (ih_rptr + IH_ENTRY) & (IH_RING_BYTES - 1);
        }
        if (vblank) {
            wr(OTG(vblank_otg, OTG_SYNC_STATUS),
               rd(OTG(vblank_otg, OTG_SYNC_STATUS)) | VSTARTUP_EVENT_CLEAR);
            vblanks++;
            wait_queue_wake_all(&vblank_waiters);
        }
        wr(IH_RB_RPTR, ih_rptr);
    }
    if (interrupts > vblanks + STORM && !stormed) {
        stormed = true;
        wr(IH_RB_CNTL, rd(IH_RB_CNTL) & ~(1u << 17));
        wr(OTG(vblank_otg, OTG_SYNC_STATUS), rd(OTG(vblank_otg, OTG_SYNC_STATUS)) & ~VSTARTUP_INT_EN);
    }
}

/* The IH ring set up and on, then OTG k's VSTARTUP interrupt. False
 * (with the reason) when it cannot be. */
/* (37g-5c-3) The IH's registers for the ring (its pages made once):
 * at the start, and again after a sleep (the IH block is reset in S3). */
static uint64_t ih_ring_phys, ih_wb_phys, ih_dummy_phys;

static void ih_program(void)
{
    uint64_t ring = ih_ring_phys, wb = ih_wb_phys, dummy = ih_dummy_phys;
    wr(IH_RB_CNTL, rd(IH_RB_CNTL) & ~(1u | 1u << 17));     /* ring and interrupts off */
    wr(IH_RB_RPTR, 0);
    wr(IH_RB_WPTR, 0);
    wr(INTERRUPT_CNTL + 1, (uint32_t)(dummy >> 8));
    wr(INTERRUPT_CNTL, rd(INTERRUPT_CNTL) & ~(0x1u | 0x8u));  /* dummy-read override, non-snoop off */
    wr(IH_CHICKEN, rd(IH_CHICKEN) | 1u << 4);       /* MC_SPACE_GPA_ENABLE */
    wr(IH_RB_BASE, (uint32_t)(ring >> 8));
    wr(IH_RB_BASE_HI, (uint32_t)(ring >> 40) & 0xFF);
    uint32_t c = rd(IH_RB_CNTL) & ~(0x70000000u | 0x0F000000u | 0x3Eu | 1u << 22);
    c |= 1u << 28;                                  /* MC_SPACE: physical addresses */
    c |= 1u << 31 | 1u << 16;                       /* overflow: cleared, detected */
    c |= 10u << 1;                                  /* RB_SIZE: 2^10 dwords */
    c |= 1u << 8 | 1u << 20 | 1u << 21;             /* write-back, snooped, RPTR_REARM */
    wr(IH_RB_CNTL, c);
    wr(IH_RB_WPTR_ADDR_LO, (uint32_t)wb);
    wr(IH_RB_WPTR_ADDR_HI, (uint32_t)(wb >> 32) & 0xFFFF);
    wr(IH_RB_WPTR, 0);
    wr(IH_RB_RPTR, 0);
    wr(IH_DOORBELL_RPTR, 0);
    memset((void *)ih_ring, 0, 4096);
    *ih_wptr = 0;
    ih_rptr = 0;
}

/* The ring on, OTG k's VSTARTUP its interrupt (MSI already on). */
static void ih_enable(int k)
{
    wr(IH_RB_CNTL, rd(IH_RB_CNTL) | 1u | 1u << 7 | 1u << 17);  /* ring, time stamps, interrupts */
    wr(OTG(k, OTG_SYNC_STATUS), rd(OTG(k, OTG_SYNC_STATUS)) | VSTARTUP_EVENT_CLEAR);
    wr(OTG(k, OTG_SYNC_STATUS), rd(OTG(k, OTG_SYNC_STATUS)) | VSTARTUP_INT_EN);
}

static bool vblank_on(int k, const char **why)
{
    struct pci_device *d = cezanne.pci;
    uint64_t ring = pmm_alloc_page(), wb = pmm_alloc_page(), dummy = pmm_alloc_page();
    if (!ring || !wb || !dummy) {
        *why = "no memory";
        return false;
    }
    memset(phys_to_virt(ring), 0, 4096);
    memset(phys_to_virt(wb), 0, 4096);
    memset(phys_to_virt(dummy), 0, 4096);
    ih_ring = phys_to_virt(ring);
    ih_wptr = phys_to_virt(wb);
    ih_ring_phys = ring, ih_wb_phys = wb, ih_dummy_phys = dummy;
    pci_enable_dma(d);                              /* (the ring and a message are its writes) */
    ih_program();
    vblank_otg = k;
    uint64_t address;
    uint32_t data;
    if (!pci_msi_enable(d, ih_interrupt, 0, &address, &data)) {
        vblank_otg = -1;
        *why = "no MSI";
        return false;
    }
    ih_enable(k);
    return true;
}

static bool wait_vblank(struct gpu_card *g, int o, uint64_t *frame, uint32_t timeout_ms)
{
    (void)g;
    int k = output_otg(o);
    if (vblank_otg < 0 || k != vblank_otg)
        return false;
    uint64_t seen = vblanks;
    uint64_t deadline = timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000;
    bool came = wait_event_until(&vblank_waiters, vblanks != seen ||
                                 thread_stop_requested(), deadline) && vblanks != seen;
    *frame = rd(OTG(k, OTG_FRAMES)) & 0xFFFFFF;
    return came;
}

/* The buffers made for the plane's present size (buffer 1 cleared to
 * black): false when they cannot be (the reason in *why). */
static bool buffers_ready(const char **why)
{
    *why = "";
    if (plane < 0) {
        for (int k = 0; k < PIPES && plane < 0; k++) {
            uint32_t c = rd(HUBP(k, HUBP_CNTL));
            if (!(c & 4) && !(c & 1))
                plane = k;
        }
        if (plane < 0) {
            *why = "no plane shows";
            return false;
        }
        buffer0 = (uint64_t)(rd(HUBP(plane, HUBP_ADDRESS_HIGH)) & 0xFFFF) << 32 |
                  rd(HUBP(plane, HUBP_ADDRESS));
    }
    uint32_t pitch = rd(HUBP(plane, HUBP_PITCH)) & 0x3FFF;
    uint32_t height = rd(HUBP(plane, HUBP_VIEWPORT_SIZE)) >> 16 & 0x3FFF;
    uint64_t bytes = (uint64_t)pitch * 4 * height;
    if (buffer_bytes == bytes)
        return true;
    uint64_t fb_base = (uint64_t)(rd(VM_FB_BASE) & 0xFFFFFF) << 24;
    uint64_t fb_top = (uint64_t)(rd(VM_FB_BASE + 1) & 0xFFFFFF) << 24 | 0xFFFFFF;
    const struct pci_bar *vram = &cezanne.pci->bars[0];
    uint64_t at = buffer0 - fb_base;                /* in the card's memory */
    if (buffer0 < fb_base || buffer0 + 2 * BUFFER_SLOT - 1 > fb_top)
        *why = "the card's memory does not hold two buffers there";
    else if (at + 2 * BUFFER_SLOT > vram->size)
        *why = "BAR 0 does not reach the second buffer";
    else if (bytes > BUFFER_SLOT || !bytes)
        *why = "the picture is larger than a buffer";
    else if (has_usage && fw_kb && (uint64_t)fw_start_kb * 1024 < at + 2 * BUFFER_SLOT &&
             (uint64_t)fw_start_kb * 1024 + (uint64_t)fw_kb * 1024 > at)
        *why = "the firmware keeps that memory (vram_usagebyfirmware)";
    if (**why)
        return false;
    buffer0_phys = vram->address + at;
    if (!paging_write_combining(buffer0_phys, 2 * BUFFER_SLOT)) {   /* (maps what is not) */
        *why = "no memory for its page tables";
        return false;
    }
    memset(phys_to_virt(buffer0_phys + BUFFER_SLOT), 0, bytes);   /* black */
    buffer_pitch = pitch;
    buffer_bytes = bytes;
    return true;
}

static bool buffers(struct gpu_card *g, int o, uint64_t *phys, uint64_t *slot, int *count,
                    uint32_t *stride)
{
    (void)g;
    const char *why;
    if (vblank_otg < 0 || output_otg(o) != vblank_otg || !buffers_ready(&why))
        return false;
    *phys = buffer0_phys;
    *slot = BUFFER_SLOT;
    *count = 2;
    *stride = buffer_pitch * 4;
    return true;
}

static bool flip(struct gpu_card *g, int o, int buffer, uint64_t *frame)
{
    const char *why;
    if (vblank_otg < 0 || output_otg(o) != vblank_otg || !buffers_ready(&why))
        return false;
    uint64_t address = buffer0 + (uint64_t)buffer * BUFFER_SLOT;
    uint32_t c = rd(HUBP(plane, HUBP_FLIP_CONTROL)) & ~(1u << 1 | 1u << 12 | 1u << 16);
    wr(HUBP(plane, HUBP_FLIP_CONTROL), c);          /* at the vertical sync, no stereo */
    wr(HUBP(plane, HUBP_ADDRESS_HIGH), (uint32_t)(address >> 32) & 0xFFFF);
    wr(HUBP(plane, HUBP_ADDRESS), (uint32_t)address);       /* (latches the address) */
    for (int n = 0; n < 6; n++) {                   /* (a 60 Hz frame is 16.7 ms: ~100 ms) */
        uint64_t inuse = (uint64_t)(rd(HUBP(plane, HUBP_EARLIEST_INUSE + 1)) & 0xFFFF) << 32 |
                         rd(HUBP(plane, HUBP_EARLIEST_INUSE));
        if (!(rd(HUBP(plane, HUBP_FLIP_CONTROL)) & FLIP_PENDING) && inuse == address) {
            *frame = rd(OTG(vblank_otg, OTG_FRAMES)) & 0xFFFFFF;
            flips++;
            buffer_shown = buffer;
            return true;
        }
        wait_vblank(g, o, frame, 20);
    }
    return false;
}

/* (37e-2a) The backlight, only read: BL_PWM_CNTL's duty cycle (its low 16
 * bits; without fractional mode only the top BITCNT bits count) over
 * BL_PWM_PERIOD_CNTL's period (BITCNT bits, 0 meaning 16) as a 16-bit
 * level, Linux's dce_get_16_bit_backlight_from_pwm; on as Linux's
 * dce_is_panel_backlight_on (BLON if overridden, else the power
 * sequencer's target), the panel powered as dce_is_panel_powered_on. */
static uint32_t bl_rd(uint32_t index)
{
    return mmio_read32(bl_regs + 4 * index);
}

/* The PWM's period (BITCNT bits, 0 meaning 16) and its bit count. */
static uint32_t bl_period(uint32_t *bits)
{
    uint32_t period_cntl = bl_rd(BL_PWM_PERIOD_CNTL);
    *bits = period_cntl >> 16 & 0xF;
    if (!*bits)
        *bits = 16;
    return period_cntl & ((1u << *bits) - 1);
}

/* The level now, of 0xffff, and its duty cycle and period. */
static uint32_t bl_level(uint32_t *duty_out, uint32_t *period_out, uint32_t *bits_out)
{
    uint32_t cntl = bl_rd(BL_PWM_CNTL), bits, period = bl_period(&bits);
    uint32_t duty = cntl & 0xFFFF;
    if (!(cntl & BL_PWM_FRACTIONAL))
        duty &= ((1u << bits) - 1) << (16 - bits);
    uint64_t level = ((uint64_t)duty << (1 + bits)) / (period ? period : 0xFFFF);
    level = (level + 1) >> 1;
    if (level > 0xFFFF)
        level = 0xFFFF;                 /* (a whole period rounds up to 0x10000) */
    if (duty_out)
        *duty_out = duty, *period_out = period, *bits_out = bits;
    return (uint32_t)level;
}

/* (37e-2c) The lowest level the panel takes: ATIF's lowest input signal
 * (of 255), else Linux's default. */
static uint32_t bl_lowest(void)
{
    uint32_t signal = atif_backlight_got >= 10 && atif_bl_min ? atif_bl_min : BL_LOWEST_SIGNAL;
    return signal * 0x101;
}

static void report_backlight(void)
{
    uint32_t cntl = bl_rd(BL_PWM_CNTL), cntl2 = bl_rd(BL_PWM_CNTL2);
    uint32_t period_cntl = bl_rd(BL_PWM_PERIOD_CNTL), lock = bl_rd(BL_PWM_GRP1_REG_LOCK);
    uint32_t seq = bl_rd(PWRSEQ_CNTL), state = bl_rd(PWRSEQ_STATE), div = bl_rd(PWRSEQ_REF_DIV);
    uint32_t duty, period, bits;
    uint64_t level = bl_level(&duty, &period, &bits);
    bool on = seq & PWRSEQ_BLON_OVRD ? (seq & PWRSEQ_BLON) != 0 : (seq & PWRSEQ_TARGET_ON) != 0;
    bool powered = (state & PWRSEQ_STATE_TARGET_ON) ||
                   ((seq & PWRSEQ_DIGON) && (seq & PWRSEQ_DIGON_OVRD));
    kprintf("  backlight: %s (%s; the power sequencer %s, state %u, BLON %s), the panel %s; PWM "
            "%s, level 0x%04x of 0xffff (%u.%u%%: duty 0x%04x of a period of 0x%04x, %u bits%s)\n",
            on ? "on" : "off", seq & PWRSEQ_BLON_OVRD ? "BLON overridden" : "as the power sequencer",
            seq & PWRSEQ_TARGET_ON ? "on" : "off", state >> 8 & 0xF,
            state & PWRSEQ_STATE_BLON ? "high" : "low", powered ? "powered" : "not powered",
            cntl & BL_PWM_EN ? "on" : "off", (uint32_t)level, (uint32_t)(level * 100 / 0xFFFF),
            (uint32_t)(level * 1000 / 0xFFFF % 10), duty, period, bits,
            cntl & BL_PWM_FRACTIONAL ? ", fractional" : "");
    kprintf("  backlight registers: BL_PWM_CNTL 0x%08x, _CNTL2 0x%08x, _PERIOD_CNTL 0x%08x, "
            "_GRP1_REG_LOCK 0x%08x; LVTMA_PWRSEQ_CNTL 0x%08x, _STATE 0x%08x, _REF_DIV 0x%08x\n",
            cntl, cntl2, period_cntl, lock, seq, state, div);
    if (bl_off)
        kprintf("  backlight switched off %lu times, on %lu times with the screen; off now "
                "(LVTMA_PWRSEQ_CNTL was 0x%08x)\n", bl_offs, bl_ons, bl_saved);
    else
        kprintf("  backlight switched off %lu times, on %lu times with the screen\n", bl_offs,
                bl_ons);
    kprintf("  brightness set %lu times (%lu not seen updated within 10 ms); the panel's lowest "
            "level 0x%04x (%s)\n", bl_sets, bl_late, bl_lowest(),
            atif_backlight_got >= 10 && atif_bl_min ? "ATIF's lowest signal" : "Linux's default");
}

/* (37e-2b) The panel's backlight off: its enable line (BLON) held low
 * through the power sequencer's override, the rest of PWRSEQ_CNTL kept
 * (the panel stays powered, the PWM's brightness untouched); on: the
 * register as it was, so the sequencer drives BLON again. Register writes
 * only (a crash calls it, with no lock). */
static bool backlight(struct gpu_card *g, int i, bool on)
{
    if (!bl_regs || i < 0 || i >= g->output_count || strcmp(g->outputs[i].kind, "edp"))
        return false;
    if (!on && !bl_off) {
        bl_saved = bl_rd(PWRSEQ_CNTL);
        mmio_write32(bl_regs + 4 * PWRSEQ_CNTL, (bl_saved | PWRSEQ_BLON_OVRD) & ~PWRSEQ_BLON);
        bl_off = true;
        bl_offs++;
    } else if (on && bl_off) {
        mmio_write32(bl_regs + 4 * PWRSEQ_CNTL, bl_saved);
        bl_off = false;
        bl_ons++;
    }
    return true;
}

/* (37e-2c) The brightness, as Linux's dce_driver_set_backlight: the duty
 * cycle for level (of 0xffff, raised to the panel's lowest) over the
 * period, its 16 bits rounded; the group's registers locked (ignoring the
 * master lock), the duty written, unlocked, then UPDATE_PENDING waited for
 * (Linux's 10 ms). The rest of BL_PWM_CNTL is kept. */
static bool brightness(struct gpu_card *g, int i, uint32_t level)
{
    if (!bl_regs || i < 0 || i >= g->output_count || strcmp(g->outputs[i].kind, "edp"))
        return false;
    if (level < bl_lowest())
        level = bl_lowest();
    if (level > 0xFFFF)
        level = 0xFFFF;
    uint32_t bits, period = bl_period(&bits);
    uint64_t active = (uint64_t)level * period;
    uint32_t duty = (uint32_t)(active >> bits) & 0xFFFF;
    duty += (uint32_t)(active >> (bits - 1)) & 1;
    if (duty > 0xFFFF)
        duty = 0xFFFF;
    uint32_t lock = bl_rd(BL_PWM_GRP1_REG_LOCK);
    mmio_write32(bl_regs + 4 * BL_PWM_GRP1_REG_LOCK, lock | BL_IGNORE_MASTER_LOCK | BL_LOCK);
    mmio_write32(bl_regs + 4 * BL_PWM_CNTL, (bl_rd(BL_PWM_CNTL) & ~0xFFFFu) | duty);
    mmio_write32(bl_regs + 4 * BL_PWM_GRP1_REG_LOCK,
                 (lock | BL_IGNORE_MASTER_LOCK) & ~BL_LOCK);
    uint64_t hz = arch_counter_hz(), end = arch_counter() + hz / 100;
    while (bl_rd(BL_PWM_GRP1_REG_LOCK) & BL_UPDATE_PENDING && arch_counter() < end)
        arch_cpu_relax();
    if (bl_rd(BL_PWM_GRP1_REG_LOCK) & BL_UPDATE_PENDING)
        bl_late++;
    bl_sets++;
    return true;
}

static bool brightness_now(struct gpu_card *g, int i, uint32_t *level)
{
    if (!bl_regs || i < 0 || i >= g->output_count || strcmp(g->outputs[i].kind, "edp"))
        return false;
    *level = bl_level(0, 0, 0);
    return true;
}

/* (37e-2a) ATIF's answers at the start (each a buffer beginning with its
 * size, which the firmware's may make shorter or longer than Linux's
 * structures: only the bytes it gave are read). */
static void ask_atif(struct pci_device *d)
{
    uint8_t out[256];
    int got = acpi_pci_method(d, "ATIF", ATIF_VERIFY, 0, 0, out, sizeof(out));
    if (got < 0) {
        atif_found = got == -1 ? -1 : -2;
        return;
    }
    uint32_t size = got >= 2 ? u16(out) : 0;
    if (size > (uint32_t)got)
        size = (uint32_t)got;
    if (size < 12) {
        atif_found = -2;
        return;
    }
    atif_found = 1;
    atif_version = u16(out + 2);
    atif_notifications = u32(out + 4);
    atif_functions = u32(out + 8);
    atif_params_got = atif_backlight_got = -3;          /* (not asked: not supported) */
    if (atif_functions & ATIF_HAS_SYSTEM_PARAMS) {
        got = acpi_pci_method(d, "ATIF", ATIF_SYSTEM_PARAMS, 0, 0, out, sizeof(out));
        size = got >= 2 ? u16(out) : 0;
        atif_params_got = got < 0 ? got : (int)(size < (uint32_t)got ? size : (uint32_t)got);
        if (atif_params_got >= 10) {
            atif_params_valid = u32(out + 2);
            atif_params_flags = u32(out + 6);
        }
        if (atif_params_got >= 11)
            atif_params_command = out[10];
    }
    if (atif_functions & ATIF_HAS_BACKLIGHT) {
        uint8_t in[3] = { 3, 0, 0 };                    /* its size; the first panel (LCD1) */
        got = acpi_pci_method(d, "ATIF", ATIF_BACKLIGHT, in, sizeof(in), out, sizeof(out));
        size = got >= 2 ? u16(out) : 0;
        atif_backlight_got = got < 0 ? got : (int)(size < (uint32_t)got ? size : (uint32_t)got);
        if (atif_backlight_got >= 10) {
            atif_bl_error = out[4];
            atif_bl_ac = out[5];
            atif_bl_dc = out[6];
            atif_bl_min = out[7];
            atif_bl_max = out[8];
            atif_bl_points = out[9];
        }
    }
}

static const char *atif_answer(int got)
{
    return got == -1 ? "no ATIF" : got == -2 ? "failed" : got == -3 ? "not supported" :
           "too short";
}

static void report_atif(void)
{
    if (atif_found <= 0) {
        kprintf("  ATIF: %s\n", atif_found == -1 ? "none under this device's ACPI node" :
                "its verify interface failed or was too short");
        return;
    }
    kprintf("  ATIF: version %u, functions 0x%08x, notifications 0x%08x (brightness change "
            "requests: %s)\n", atif_version, atif_functions, atif_notifications,
            atif_notifications & ATIF_BRIGHTNESS_REQUESTS ? "yes" : "no");
    if (atif_params_got >= 10) {
        uint32_t notify = atif_params_flags & 3;
        kprintf("  ATIF's system parameters: valid 0x%08x, flags 0x%08x: requests by %s",
                atif_params_valid, atif_params_flags, notify == 1 ? "Notify 0x81" :
                notify == 2 ? "Notify of its command" : "no notification");
        if (notify == 2 && atif_params_got >= 11)
            kprintf(" 0x%02x", atif_params_command);
        kprintf("\n");
    } else
        kprintf("  ATIF's system parameters: %s\n", atif_answer(atif_params_got));
    if (atif_backlight_got >= 10)
        kprintf("  ATIF's backlight: %s; the default %u%% on AC, %u%% on battery; the input "
                "signal %u-%u (of 255); %u data points\n", atif_bl_error == 0 ? "ok" :
                atif_bl_error == 2 ? "this panel not supported" : "failed", atif_bl_ac * 100 / 255,
                atif_bl_dc * 100 / 255, atif_bl_min, atif_bl_max, atif_bl_points);
    else
        kprintf("  ATIF's backlight: %s\n", atif_answer(atif_backlight_got));
}

/* (37g-5b) AtomBIOS: the video BIOS's command tables (the chip's own
 * procedures) run by our interpreter (amdgpu_atom.c) on all of BAR 5, the
 * register space they address by dword index (one past it is refused:
 * Linux reaches those through the PCIe index and data registers). At the
 * start two that only ask are run (the user's choice): getsmuclockinfo,
 * as Linux's display code calls it on these chips (system PLL 0's VCO,
 * get_smu_clock_info_v3_1), and processauxchanneltransaction reading the
 * panel's first 16 DPCD bytes, compared with our own AUX read where there
 * is one (Cezanne). Nothing is set. Facts from Linux's atomfirmware.h
 * (the master list's names, the parameters) and atombios_dp.c (the AUX
 * request in the scratch at byte 4, its reply at 20). */
#define ATOM_SCRATCH (20 * 1024)            /* bytes: Linux's when the BIOS says none */
#define T_SMU_CLOCK 41
#define T_AUX 78
#define AUX_REQUEST 4                       /* the request's byte in the scratch */
#define AUX_REPLY 20                        /* the reply's */
#define DP_NATIVE_READ 0x9

static volatile uint8_t *atom_bar;
static uint32_t atom_dwords;
static struct atom atom;
static bool atom_ready;
static char atom_failed[96];
static uint32_t *atom_scratch;
static int atom_tables;
static uint8_t atom_master_format, atom_master_content;
static struct atom_ran {
    bool ran;
    int result;
    uint32_t us;
    char why[96];
} ran_smu, ran_aux;
static uint32_t smu_vco_10khz;
static int aux_output = -1;
static uint8_t aux_status, aux_got, aux_dpcd[16];
static int aux_same = -1;                   /* 1 the same as ours, 0 not, -1 not compared */

/* The master list's functions (v2.1), by index; the rest are "internal". */
static const char *const table_names[] = {
    [0] = "asic_init", [4] = "digxencodercontrol", [10] = "setengineclock",
    [11] = "setmemoryclock", [12] = "setpixelclock", [13] = "enabledisppowergating",
    [34] = "blankcrtc", [35] = "enablecrtc", [41] = "getsmuclockinfo",
    [42] = "selectcrtc_source", [46] = "setdceclock", [47] = "getmemoryclock",
    [48] = "getengineclock", [49] = "setcrtc_usingdtdtiming", [50] = "externalencodercontrol",
    [54] = "processi2cchanneltransaction", [60] = "computegpuclockparam",
    [63] = "dynamicmemorysettings", [64] = "memorytraining", [67] = "setvoltage",
    [69] = "readefusevalue", [76] = "dig1transmittercontrol",
    [78] = "processauxchanneltransaction", [80] = "getvoltageinfo",
};

static const char *table_name(int index)
{
    if (atom_master_format != 2 || atom_master_content != 1 || index < 0 ||
        index >= (int)(sizeof table_names / sizeof table_names[0]))
        return 0;
    return table_names[index];
}

static uint32_t atom_rd(void *ctx, uint32_t index)
{
    (void)ctx;
    if (index >= atom_dwords) {
        atom.refused = "a register past BAR 5";
        return 0;
    }
    return mmio_read32(atom_bar + 4 * (uint64_t)index);
}

static void atom_wr(void *ctx, uint32_t index, uint32_t value)
{
    (void)ctx;
    if (index >= atom_dwords) {
        atom.refused = "a register past BAR 5";
        return;
    }
    mmio_write32(atom_bar + 4 * (uint64_t)index, value);
}

static void atom_wait(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us >= 2000)
        timer_sleep_ms((us + 999) / 1000);
    else
        wait_us(us);
}

static uint64_t atom_now(void *ctx)
{
    (void)ctx;
    return arch_counter() / (arch_counter_hz() / 1000);
}

/* Table index run with ps, timed; what happened in *out. */
static int atom_call(int index, uint32_t *ps, struct atom_ran *out)
{
    uint64_t t0 = arch_counter();
    int r = atom_run(&atom, index, ps);
    out->ran = true;
    out->result = r;
    out->us = (uint32_t)((arch_counter() - t0) * 1000000 / arch_counter_hz());
    memcpy(out->why, atom.why, sizeof out->why);
    return r;
}

static void log_run(const char *what, const struct atom_ran *x)
{
    if (x->result == -2)
        kprintf("  amdgpu:        AtomBIOS: %s: no such table\n", what);
    else if (x->result < 0)
        kprintf("  amdgpu:        AtomBIOS: %s: stopped: %s (%u us)\n", what, x->why, x->us);
}

static void atom_start(const struct pci_device *d)
{
    const struct pci_bar *bar = &d->bars[5];
    if (bar->is_io || !bar->address || bar->size < 0x40000 || bar->size > 0x100000) {
        ksnprintf(atom_failed, sizeof atom_failed, "BAR 5 is not a register BAR (%lu bytes)",
                  bar->size);
        return;
    }
    atom_bar = paging_map_mmio(bar->address, bar->size);
    atom_scratch = kzalloc(ATOM_SCRATCH);
    if (!atom_bar || !atom_scratch) {
        ksnprintf(atom_failed, sizeof atom_failed, "no memory for it");
        return;
    }
    atom_dwords = (uint32_t)(bar->size / 4);
    struct atom_machine m = { atom_rd, atom_wr, atom_wait, atom_now, 0 };
    if (!atom_init(&atom, rom, rom_size, &m, atom_scratch, ATOM_SCRATCH)) {
        ksnprintf(atom_failed, sizeof atom_failed, "%s", atom.why);
        return;
    }
    atom_ready = true;
    atom_master_format = rom[atom.cmd_table + 2];
    atom_master_content = rom[atom.cmd_table + 3];
    for (int i = 0; i < atom.tables; i++) {
        uint16_t size;
        uint8_t frev, crev, ws, ps;
        if (atom_table(&atom, i, &size, &frev, &crev, &ws, &ps))
            atom_tables++;
    }
    /* getsmuclockinfo: syspllid 0, clk_id 0, command GET_PLLVCO_FREQ (1);
     * the answer, in 10 kHz, over the parameters' first dword */
    uint32_t ps[ATOM_PARAMS];
    memset(ps, 0, sizeof ps);
    ps[0] = 1u << 16;
    if (atom_call(T_SMU_CLOCK, ps, &ran_smu) == 0)
        smu_vco_10khz = ps[0];
    log_run("getsmuclockinfo", &ran_smu);
    if (ran_smu.result == 0)
        kprintf("  amdgpu:        AtomBIOS: getsmuclockinfo: system PLL 0's VCO %u.%02u MHz "
                "(%u us)\n", smu_vco_10khz / 100, smu_vco_10khz % 100, ran_smu.us);
    /* processauxchanneltransaction on the eDP panel's line: a native read
     * of DPCD 0x000, 16 bytes (the request as Linux's: the address's low
     * and middle bytes, command << 4 | its top bits, then length - 1 | the
     * bytes to send (4, the header) << 4: the table sends that many) */
    for (int o = 0; o < cezanne.output_count && aux_output < 0; o++)
        if (!strcmp(cezanne.outputs[o].kind, "edp") && paths[output_path[o]].has_ddc)
            aux_output = o;
    if (aux_output < 0)
        return;
    const struct path *q = &paths[output_path[aux_output]];
    int hpd = q->has_hpd ? hpd_number(find_pin(q->hpd)) : 0;
    memset(atom_scratch, 0, 64);
    atom_scratch[AUX_REQUEST / 4] = (uint32_t)DP_NATIVE_READ << 20 | (15u | 4u << 4) << 24;
    memset(ps, 0, sizeof ps);
    ps[0] = AUX_REQUEST | (uint32_t)AUX_REPLY << 16;
    ps[1] = q->ddc | (uint32_t)(hpd ? hpd - 1 : 0) << 24;
    if (atom_call(T_AUX, ps, &ran_aux) == 0) {
        aux_status = (uint8_t)(ps[1] >> 8);
        aux_got = (uint8_t)(ps[1] >> 16);
        memcpy(aux_dpcd, (uint8_t *)atom_scratch + AUX_REPLY, sizeof aux_dpcd);
        int k, n;
        uint8_t ours[16];
        if (regs && aux_status == 0 && aux_got == sizeof ours && output_lines(aux_output, &k, &n) &&
            aux_transfer(aux_output, k, DP_READ, 0x000, 0, sizeof ours, ours) == (int)sizeof ours)
            aux_same = !memcmp(ours, aux_dpcd, sizeof ours);
    }
    log_run("processauxchanneltransaction", &ran_aux);
    if (ran_aux.result == 0) {
        char bytes[16 * 3 + 1];                     /* (one line: one kprintf) */
        size_t n = 0;
        for (unsigned i = 0; i < sizeof aux_dpcd && i < aux_got; i++)
            n += (size_t)ksnprintf(bytes + n, sizeof bytes - n, " %02x", aux_dpcd[i]);
        bytes[n] = 0;
        kprintf("  amdgpu:        AtomBIOS: processauxchanneltransaction: DPCD 0x000 on line "
                "0x%02x, HPD%d: reply 0x%02x, %u bytes:%s%s (%u us)\n", q->ddc, hpd ? hpd : 1,
                aux_status, aux_got, bytes, aux_same == 1 ? "; the same as our AUX read" :
                aux_same == 0 ? "; NOT the same as our AUX read" : "", ran_aux.us);
    }
}

static void report_ran(const char *what, int index, const struct atom_ran *x)
{
    kprintf("    %s (%d): ", what, index);
    if (!x->ran)
        kprintf("not run\n");
    else if (x->result == -2)
        kprintf("no such table\n");
    else if (x->result < 0)
        kprintf("stopped: %s (%u us)\n", x->why, x->us);
    else if (index == T_SMU_CLOCK)
        kprintf("system PLL 0's VCO %u.%02u MHz (%u us)\n", smu_vco_10khz / 100,
                smu_vco_10khz % 100, x->us);
    else {
        kprintf("DPCD 0x000: reply 0x%02x, %u bytes:", aux_status, aux_got);
        for (unsigned i = 0; i < sizeof aux_dpcd && i < aux_got; i++)
            kprintf(" %02x", aux_dpcd[i]);
        kprintf("%s (%u us)\n", aux_same == 1 ? "; the same as our AUX read" :
                aux_same == 0 ? "; NOT the same as our AUX read" : "", x->us);
    }
}

static void report_atom(void)
{
    if (!atom_ready) {
        kprintf("  AtomBIOS: not running tables (%s)\n", atom_failed[0] ? atom_failed : "not started");
        return;
    }
    kprintf("  AtomBIOS: %d command tables (master list %u.%u), BAR 5's %u registers; run: %lu "
            "instructions, %lu reads, %lu writes, %lu us waited, %lu calls\n", atom_tables,
            atom_master_format, atom_master_content, atom_dwords, atom.ops, atom.reads,
            atom.writes, atom.delays_us, atom.calls);
    report_ran("getsmuclockinfo", T_SMU_CLOCK, &ran_smu);
    report_ran("processauxchanneltransaction", T_AUX, &ran_aux);
    kprintf("    tables (index, bytes, revision):");
    int shown = 0;
    for (int i = 0; i < atom.tables; i++) {
        uint16_t size;
        uint8_t frev, crev, ws, ps;
        if (!atom_table(&atom, i, &size, &frev, &crev, &ws, &ps))
            continue;
        const char *name = table_name(i);
        if (shown++ % 6 == 0)
            kprintf("\n     ");
        kprintf(" %d%s%s %u %u.%u%s", i, name ? " " : "", name ? name : "", size, frev, crev,
                shown < atom_tables ? "," : "");
    }
    kprintf("\n");
}

static void report(struct gpu_card *g)
{
    (void)g;
    kprintf("  VBIOS: image %d of %d in the firmware's VFCT table (%u bytes), %u bytes; '%s'; "
            "AtomBIOS data tables %u.%u, display objects %u.%u\n", image_index, image_count,
            vfct_size, rom_size, boot_message, data_format, data_content, objects_format,
            objects_content);
    if (boot_dispclk_10khz)
        kprintf("  display clock at boot %u.%02u MHz, reference %u.%02u MHz (dce_info)\n",
                boot_dispclk_10khz / 100, boot_dispclk_10khz % 100, refclk_10khz / 100,
                refclk_10khz % 100);
    kprintf("  display paths: %d:", path_count);
    for (int k = 0; k < path_count; k++) {
        const struct path *q = &paths[k];
        kprintf(" [%s 0x%04x, encoder 0x%04x, device 0x%04x", connector_name(q->connector),
                q->connector, q->encoder, q->device_tag);
        if (q->has_ddc)
            kprintf(", DDC/AUX line 0x%02x", q->ddc);
        if (q->has_hpd)
            kprintf(", hotplug pin %u", q->hpd);
        kprintf("]");
    }
    kprintf("\n");
    if (!has_pins)
        kprintf("  gpio_pin_lut: none in the VBIOS\n");
    else {
        kprintf("  gpio_pin_lut: %d pins (ID: register.bit):", pin_count);
        for (int i = 0; i < pin_count; i++)
            kprintf(" 0x%02x: 0x%04x.%u", pins[i].id, pins[i].reg, pins[i].bit);
        kprintf("\n");
    }
    for (int o = 0; o < cezanne.output_count; o++) {
        report_output(o);
        report_link(o);
    }
    if (regs) {
        report_mode();
        if (mode_sets)
            kprintf("  mode sets: %u; the last %ux%u at %u.%03u Hz: the OTG back after %u ms, the "
                    "stream after %u ms\n", mode_sets, last_w, last_h, last_mhz / 1000,
                    last_mhz % 1000, mode_ms, stream_ms);
        if (is_cezanne)
            report_interrupts(vblank_otg >= 0 ? vblank_otg : 0);
        if (vblank_otg >= 0)
            kprintf("  vertical blanks: %lu (OTG%d's VSTARTUP; %lu interrupts, %lu ring entries, %lu "
                    "others (the last 0x%08x), %lu overflows); OTG%d's frame %u%s\n", vblanks,
                    vblank_otg, interrupts, entries, others, last_other, overflows, vblank_otg,
                    rd(OTG(vblank_otg, OTG_FRAMES)) & 0xFFFFFF, stormed ?
                    "; switched off: too many interrupts that were not vertical blanks" : "");
        if (has_usage)
            kprintf("  the firmware's own memory (vram_usagebyfirmware): from %u KiB, %u KiB for "
                    "the firmware, %u KiB for the driver\n", fw_start_kb, fw_kb, driver_kb);
        if (buffer_bytes)
            kprintf("  screen buffers: 2 of %lu KB (HUBP%d; the display engine's 0x%lx and 0x%lx, "
                    "through BAR 0 0x%lx and 0x%lx); pitch register %u; showing buffer %d; %lu "
                    "flips\n", buffer_bytes / 1000, plane, buffer0, buffer0 + BUFFER_SLOT,
                    buffer0_phys, buffer0_phys + BUFFER_SLOT, buffer_pitch, buffer_shown, flips);
    }
    if (bl_regs)
        report_backlight();
    report_atif();
    report_atom();
    if (!regs)
        kprintf("  %sregisters: not read (%s)\n", bl_regs ? "the display engine's " : "",
                cezanne.pci->device_id == 0x1638 ? "BAR 5 is not the registers" :
                "the layout is known for Cezanne only");
}

/* (37g-4) Sleep, only looked at (the user's choice): in S3 the display
 * engine loses its state and the firmware does not set it up again on
 * these laptops (Linux does it with the video BIOS's command tables and
 * the display firmware). Before the sleep the registers that show the
 * picture are kept; after it they are read again and each one that
 * changed is logged, the facts for deciding how the panel comes back.
 * Nothing is written. */
static const struct { const char *name; uint32_t reg; bool cezanne; } watched[] = {
    { "OTG0_CONTROL", OTG(0, OTG_CONTROL), true },
    { "OTG0_H_TOTAL", OTG(0, OTG_H_TOTAL), true },
    { "OTG0_V_TOTAL", OTG(0, OTG_V_TOTAL), true },
    { "OTG0_BLANK_CONTROL", OTG(0, OTG_BLANK_CONTROL), true },
    { "HUBP0_CNTL", HUBP21(0, 0xE), true },
    { "HUBP0_SURFACE_CONFIG", HUBP21(0, 0), true },
    { "HUBP0_PITCH", HUBP21(0, 0x22), true },
    { "HUBP0_ADDRESS", HUBP21(0, 0x25), true },
    { "HUBP0_ADDRESS_HIGH", HUBP21(0, 0x26), true },
    { "DIG0_FE_CNTL", DIG(0, DIG_FE), true },
    { "DIG0_BE_CNTL", DIG(0, DIG_BE), true },
    { "DIG0_BE_EN_CNTL", DIG(0, DIG_BE_EN), true },
    { "DP0_LINK_CNTL", DIG(0, DP_LINK), true },
    { "DP0_VID_STREAM_CNTL", DIG(0, DP_VIDEO), true },
    { "DCCG_PIXEL_RATE_CNTL0", PIXEL_RATE_CNTL(0), true },
    { "DCN_VM_FB_LOCATION_BASE", VM_FB_BASE, true },
    { "IH_RB_CNTL", IH_RB_CNTL, true },
    { "DC_HPD1_INT_STATUS", HPD_INT_STATUS(1), true },
    { "LVTMA_PWRSEQ_CNTL", PWRSEQ_CNTL, false },
    { "LVTMA_PWRSEQ_STATE", PWRSEQ_STATE, false },
    { "BL_PWM_CNTL", BL_PWM_CNTL, false },
    { "BL_PWM_PERIOD_CNTL", BL_PWM_PERIOD_CNTL, false },
};
#define WATCHED (sizeof(watched) / sizeof(watched[0]))
static uint32_t before_sleep[WATCHED];

static uint32_t watched_read(size_t i)
{
    return watched[i].cezanne ? rd(watched[i].reg) : bl_rd(watched[i].reg);
}

static bool watched_there(size_t i)
{
    return watched[i].cezanne ? regs != 0 && is_cezanne : bl_regs != 0;
}

/* (37g-5c-2) The whole of pipe 0's path (amdgpu_regs.h's 662 registers)
 * before the sleep and after it, read only, on Cezanne (all of BAR 5
 * mapped for the interpreter): the facts for putting it back. The log
 * counts what changed by block; `displays sleep` lists each register. */
/* (37g-6) The chip's list: DCN 2.1's or DCN 1.0's (chosen at the start). */
static const struct dcn_reg *dcn_pipe0 = dcn21_pipe0;
static const struct dcn_block *dcn_blocks = dcn21_blocks;
static uint32_t snap_count = DCN21_REGS, dcn_block_count = DCN21_BLOCKS;
#define SNAP snap_count
#define SNAP_MAX (DCN21_REGS > DCN10_REGS ? DCN21_REGS : DCN10_REGS)
static uint32_t snap_before[SNAP_MAX], snap_after[SNAP_MAX];
static bool snap_taken;                 /* before and after of the last sleep */
static uint32_t snap_sleeps;
static char bring_step[256] = "not tried";  /* (37g-5c-3) how the last bring-up went */
static uint32_t bring_ms;
static bool bring_address;                  /* (37g-5d) the plane's address in use as before */                /* (37g-5c-3: 1:1, its coefficients lost) */
static char asic_init_said[192] = "asic_init: no sleep yet";     /* (37g-6) */

static void snap_read(uint32_t *into)
{
    for (size_t i = 0; i < SNAP; i++)
        into[i] = dcn_pipe0[i].reg < atom_dwords ?
                  mmio_read32(atom_bar + 4 * (uint64_t)dcn_pipe0[i].reg) : 0;
}

static bool snap_there(void)
{
    return atom_bar && regs && (is_cezanne || picasso);     /* (37g-6: both lists) */
}

static void snap_log(void)
{
    char line[512];
    size_t n = 0;
    uint32_t changed = 0, at = 0;
    for (size_t b = 0; b < dcn_block_count; b++) {
        uint32_t c = 0;
        for (uint32_t k = 0; k < dcn_blocks[b].count && at + k < SNAP; k++)
            c += snap_before[at + k] != snap_after[at + k];
        at += dcn_blocks[b].count;
        changed += c;
        if (c && n < sizeof(line) - 24)
            n += (size_t)ksnprintf(line + n, sizeof(line) - n, " %s %u/%u", dcn_blocks[b].name, c,
                                   dcn_blocks[b].count);
    }
    line[n] = 0;
    kprintf("  amdgpu:        after the sleep %u of pipe 0's %u registers changed (displays sleep "
            "lists them):%s\n", changed, (uint32_t)SNAP, n ? line : " none");
}

static void report_not_taken(void);
static int snap_named(const char *name);
static bool power_processor_ready(char *why, size_t size);

/* A register of the snapshot's list read now (~0 if it is not there). */
static uint32_t reg_now(const char *name)
{
    int i = snap_named(name);
    return i >= 0 ? rd(dcn_pipe0[i].reg) : ~0u;
}

static void report_sleep(struct gpu_card *g)
{
    (void)g;
    if (!snap_there()) {
        kprintf("  pipe 0's registers: not kept (%s)\n",
                regs ? "BAR 5 not mapped" : "the layout is known for Cezanne only");
        return;
    }
    if (is_cezanne) {                               /* (SMU 12's places) */
        char mp1[160];                              /* (37g-5d) as the bring-up looks at it */
        bool ready = power_processor_ready(mp1, sizeof mp1);
        kprintf("  now: %s (%s)\n", mp1, ready ? "ready" : "NOT READY");
    }
    if (!snap_taken) {
        kprintf("  pipe 0's registers: no sleep since the start (%u registers kept at each)\n",
                (uint32_t)SNAP);
        return;
    }
    uint32_t changed = 0;
    for (size_t i = 0; i < SNAP; i++)
        changed += snap_before[i] != snap_after[i];
    kprintf("  pipe 0's registers over sleep %u: %u of %u changed (before -> after)\n",
            snap_sleeps, changed, (uint32_t)SNAP);
    kprintf("  the panel after it: %s (%u ms)\n", bring_step, bring_ms);
    /* (the display engine's memory requests, now: their faults and replies;
     * DCN 1.0 has no DCN_VM fault registers) */
    if (is_cezanne)
        kprintf("  memory requests now: DCN_VM_FAULT_STATUS 0x%08x, the address 0x%08x%08x, "
                "DCHUBBUB_SDPIF_CFG0 0x%08x, ODM0_OPTC_INPUT_GLOBAL_CONTROL 0x%08x\n",
                reg_now("DCN_VM_FAULT_STATUS"), reg_now("DCN_VM_FAULT_ADDR_MSB"),
                reg_now("DCN_VM_FAULT_ADDR_LSB"), reg_now("DCHUBBUB_SDPIF_CFG0"),
                reg_now("ODM0_OPTC_INPUT_GLOBAL_CONTROL"));
    else
        kprintf("  memory requests now: DCHUBBUB_SDPIF_CFG0 0x%08x; %s\n",
                reg_now("DCHUBBUB_SDPIF_CFG0"), asic_init_said);
    report_not_taken();
    uint32_t at = 0;
    for (size_t b = 0; b < dcn_block_count; b++) {
        uint32_t c = 0;
        for (uint32_t k = 0; k < dcn_blocks[b].count && at + k < SNAP; k++)
            c += snap_before[at + k] != snap_after[at + k];
        kprintf("  %s: %u of %u changed\n", dcn_blocks[b].name, c, dcn_blocks[b].count);
        for (uint32_t k = 0; k < dcn_blocks[b].count && at + k < SNAP; k++)
            if (snap_before[at + k] != snap_after[at + k])
                kprintf("    %s 0x%08x -> 0x%08x\n", dcn_pipe0[at + k].name, snap_before[at + k],
                        snap_after[at + k]);
        at += dcn_blocks[b].count;
    }
}

/* (37g-5c-3) The panel back after a sleep, on Cezanne, the way Linux's
 * DCN 2.1 code resumes (dcn10_init_hw, then the link and the stream), the
 * video BIOS's tables doing what they do there and the rest put back from
 * the snapshot taken before the sleep:
 *   1. enabledisppowergating INIT: the display engine's golden init;
 *   2. setdceclock: the display and DPP clocks at 700 MHz (both at 100 MHz
 *      in bypass after S3: too slow for the panel's 133 MHz);
 *   3. dig1transmittercontrol INIT, then POWER_ON (the panel's supply),
 *      HPD awaited; AUX and HPD as before; the panel's own link settings
 *      (DPCD 0x100-0x10A, 0x115, 0x600, kept before the sleep) written back;
 *   4. ENABLE: the PHY at the link's rate, all lanes;
 *   5. link training (DisplayPort's: clock recovery with TPS1, channel
 *      equalization with TPS3, the drive levels through the table's
 *      SETUP_VSEMPH, one for all lanes as Linux's default), then the
 *      encoder back in video mode as before;
 *   6. the pipe's registers as before the sleep, block by block (status,
 *      counters and what the tables or the steps here set left out), the
 *      enables last in set_mode's order: the OTG, a frame, the plane, the
 *      stream with M/N seeded and the DIG started;
 *   7. the backlight's PWM as before, then LCD_BLON; the IH ring again.
 * Each step is logged; a failure stops there (the panel stays dark).
 * (37g-6) On DCN 1.0 (the VivoBook's Picasso) as Linux's DCN 1.0 code
 * (dcn10_init_hw, rv1_clk_mgr) instead in steps 0-2: no readiness check
 * of the power-management processor (SMU 10), the golden init at every
 * init (no time base condition), no host VM, the display clock as before
 * the sleep (its DENTIST divider, Linux's dentist_get_divider_from_did,
 * off the VCO table 41 gave) and the reference clock at 600 MHz by the
 * display mailbox's messages 4 and 5 (rv1_vbios_smu_*: the DPP clock is
 * the display clock there), no SetDisplayCount (Linux sends it through
 * the power driver's own mailbox, which this driver does not have), the
 * deep sleep and DP DTOs' registers put back with the pipe's. */
#define DP_WRITE 0x80
#define T_POWER_GATING 13
#define T_DCE_CLOCK 46
#define T_TRANSMITTER 76
#define TX_ENABLE 1
#define TX_LCD_BLON 3
#define TX_INIT 7
#define TX_SETUP_VSEMPH 11
#define TX_POWER_ON 12
#define CLOCK_700MHZ 70000u                 /* (10 kHz) the boot's display clock (dce_info) */
#define DCE_DISPCLK 0
#define DCE_DPPCLK 3                        /* (this BIOS's: SMU message 7, SetDppclkFreq) */
#define TRAINING_COMPLETE 0x10u             /* DP_LINK_CNTL */
#define DPHY_BYPASS (1u << 16)              /* DP_DPHY_CNTL */
#define DPHY_PRBS_EN 1u                     /* DP_DPHY_PRBS_CNTL */
#define HPD_SENSE 2u                        /* DC_HPD_INT_STATUS */
#define AUX_PAD_MODE (1u << 16)             /* DC_GPIO_DDCn_MASK */

static uint8_t dpcd_kept[11], dpcd_rate_kept, dpcd_power_kept;
static bool dpcd_saved;
static uint32_t bring_cr_tries, bring_eq_tries, bring_symclk;
static uint8_t bring_level;

/* (37g-5c-3) Each step logged before it runs. */
static void bring_mark(const char *what)
{
    kprintf("  amdgpu:        after the sleep, now: %s\n", what);
}

/* (37g-5d) The power-management processor (MP1), as Linux looks at it
 * before its display code sends it anything: its firmware running
 * (smu_v12_0_check_fw_status: MP1_FIRMWARE_FLAGS' INTERRUPTS_ENABLED,
 * read through the SMN window as amdgpu_device_indirect_rreg) and the
 * display's mailbox not busy (rn_smu_wait_for_response: C2PMSG_91 not 0,
 * up to 2 s 10 us apart; nothing sent while it reads 0). Facts: nbio_v7_0's
 * PCIE_INDEX2/DATA2, smu_v12_0.h's MP1_Public, MP0_BASE segment 0 +
 * mp_12_0_0's mmMP1_SMN_C2PMSG_91. */
#define SMN_INDEX 0xE
#define SMN_DATA 0xF
#define MP1_FIRMWARE_FLAGS (0x03B00000u | 0x3010024u)
#define MP1_INTERRUPTS_ENABLED 1u
#define MP1_MAILBOX_RESPONSE (0x16000 + 0x29B)

static uint32_t smn_read(uint32_t address)
{
    wr(SMN_INDEX, address);
    (void)rd(SMN_INDEX);
    return rd(SMN_DATA);
}

/* (37g-5d) A message to MP1 through the display's mailbox, as Linux's
 * rn_vbios_smu_send_msg_with_param: wait for the mailbox (not 0, up to
 * 2 s), the answer cleared, the parameter, then the message, its answer
 * awaited the same way (1 OK, 0xFF failed, 0xFE unknown, 0xFD/0xFC
 * refused). The answer, or 0 when it never came (then, as Linux, nothing
 * sent when the mailbox was busy before). */
#define MP1_MAILBOX_MESSAGE (0x16000 + 0x283)               /* mmMP1_SMN_C2PMSG_67 */
#define MP1_MAILBOX_PARAMETER (0x16000 + 0x293)             /* mmMP1_SMN_C2PMSG_83 */
#define DISPLAY_MSG_SET_DISPLAY_COUNT 0xC                   /* VBIOSSMC_MSG_SetDisplayCount */
#define DCHUBBUB_SDPIF_CFG0_REG (DCN_SEG2 + 0x48F)          /* (as in amdgpu_regs.h) */
#define RV_MSG_SET_DISPCLK 0x4                              /* (37g-6) rv1's VBIOSSMC messages, MHz */
#define RV_MSG_SET_DPREFCLK 0x5
#define RV_DPREFCLK_MHZ 600u                                /* rv1_clk_mgr's dprefclk_khz */
#define BIOS_SCRATCH_7 0x53                                 /* (37g-6) nbio's, table 0 tests it */
#define ASIC_INIT_COMPLETE 0x200u                           /* ATOM_S7_ASIC_INIT_COMPLETE_MASK */
#define T_ASIC_INIT 0

static uint32_t mailbox_wait(void)
{
    uint32_t answer = 0;
    for (uint32_t waited = 0; waited < 2000000 && !(answer = rd(MP1_MAILBOX_RESPONSE));
         waited += 10)
        wait_us(10);
    return answer;
}

static uint32_t display_message(uint32_t message, uint32_t parameter, uint32_t *returned)
{
    /* (37g-6) rv1_vbios_smu_send_msg_with_param does not wait before */
    if (is_cezanne && !mailbox_wait())
        return 0;
    wr(MP1_MAILBOX_RESPONSE, 0);
    wr(MP1_MAILBOX_PARAMETER, parameter);
    wr(MP1_MAILBOX_MESSAGE, message);
    uint32_t answer = mailbox_wait();
    *returned = rd(MP1_MAILBOX_PARAMETER);
    return answer;
}

static bool power_processor_ready(char *why, size_t size)
{
    uint32_t flags = smn_read(MP1_FIRMWARE_FLAGS), response = 0, waited = 0;
    if (flags & MP1_INTERRUPTS_ENABLED)
        for (; waited < 2000000 && !(response = rd(MP1_MAILBOX_RESPONSE)); waited += 10)
            wait_us(10);
    bool ready = (flags & MP1_INTERRUPTS_ENABLED) && response;
    ksnprintf(why, size, "the power-management processor's firmware flags 0x%08x, its display "
              "mailbox's answer 0x%08x after %u us", flags, response, waited);
    return ready;
}

/* (37g-5d) The display's host VM (DCHVM: its memory reads through the
 * IOMMU), lost in S3, set up as Linux's dcn21_dchvm_init at every
 * power-up: the init requested, the rIOMMU active within 100 x 5 us, then
 * its power status and prefetch, dynamic clock gating, the prefetch done
 * (100 x 5 us). Facts: dcn_2_1_0's offsets (segment 3 at 0x9000) and bits. */
#define DCN_PIPES 4
#define GOLDEN_DIV_RESET 0x00120464u
#define DCHVM_CTRL0 (0x9000 + 0x16B)
#define DCHVM_CLK_CTRL (0x9000 + 0x16D)
#define DCHVM_RIOMMU_CTRL0 (0x9000 + 0x16F)
#define DCHVM_RIOMMU_STAT0 (0x9000 + 0x170)
#define HOSTVM_INIT_REQ 1u
#define HOSTVM_PREFETCH_REQ 1u
#define HOSTVM_POWERSTATUS 2u
#define RIOMMU_ACTIVE 1u
#define HOSTVM_PREFETCH_DONE 2u
#define HVM_GATE_DIS 0x33u                  /* HVM_DISPCLK_R/G, HVM_DCFCLK_R/G_GATE_DIS */

static bool dchvm_init(char *said, size_t size)
{
    wr(DCHVM_CTRL0, rd(DCHVM_CTRL0) | HOSTVM_INIT_REQ);
    uint32_t tries = 0;
    bool active;
    while (!(active = rd(DCHVM_RIOMMU_STAT0) & RIOMMU_ACTIVE) && ++tries < 100)
        wait_us(5);
    bool done = false;
    if (active) {
        wr(DCHVM_RIOMMU_CTRL0, rd(DCHVM_RIOMMU_CTRL0) | HOSTVM_POWERSTATUS);
        wr(DCHVM_RIOMMU_CTRL0, rd(DCHVM_RIOMMU_CTRL0) | HOSTVM_PREFETCH_REQ);
        wr(DCHVM_CLK_CTRL, rd(DCHVM_CLK_CTRL) & ~HVM_GATE_DIS);
        for (int i = 0; i < 100 && !(done = rd(DCHVM_RIOMMU_STAT0) & HOSTVM_PREFETCH_DONE); i++)
            wait_us(5);
    }
    ksnprintf(said, size, "the display's host VM: the rIOMMU %s, the prefetch %s (STAT0 0x%08x, "
              "CLK_CTRL 0x%08x)", active ? "active" : "NOT ACTIVE", done ? "done" : "NOT DONE",
              rd(DCHVM_RIOMMU_STAT0), rd(DCHVM_CLK_CTRL));
    return active && done;
}

/* A register of the snapshot by name: its index in dcn_pipe0, or -1. */
static int snap_named(const char *name)
{
    for (size_t i = 0; i < SNAP; i++)
        if (!strcmp(dcn_pipe0[i].name, name))
            return (int)i;
    return -1;
}

static void snap_put(const char *name)
{
    int i = snap_named(name);
    if (i >= 0)
        wr(dcn_pipe0[i].reg, snap_before[i]);
}

static uint32_t snap_was(const char *name)
{
    int i = snap_named(name);
    return i >= 0 ? snap_before[i] : 0;
}

/* dig1transmittercontrol (v1.6) on the panel's PHY: action, its mode or
 * drive levels (byte 2), the symbol clock (10 kHz). */
static int transmitter_call(uint8_t action, uint8_t mode, uint32_t symclk_10khz)
{
    const struct path *q = &paths[output_path[aux_output]];
    int phy = transmitter(q->encoder), hpd = q->has_hpd ? hpd_number(find_pin(q->hpd)) : 0;
    uint32_t ps[ATOM_PARAMS];
    memset(ps, 0, sizeof ps);
    uint32_t lanes = dpcd_kept[1] & 0x1F;                  /* (37g-6) the panel's (0x101), 2 here */
    if (lanes < 1 || lanes > 4)
        lanes = 4;
    ps[0] = (uint32_t)(phy < 0 ? 0 : phy) | (uint32_t)action << 8 | (uint32_t)mode << 16 | lanes << 24;
    ps[1] = symclk_10khz;
    ps[2] = (uint32_t)hpd | (uint32_t)(q->connector & 0xFF) << 16;
    return atom_run(&atom, T_TRANSMITTER, ps);
}

static bool dpcd_write(int o, int k, uint32_t address, uint8_t value)
{
    return aux_transfer(o, k, DP_WRITE, address, &value, 1, 0) >= 0;
}

/* The source's training pattern (0 TPS1 ... 3 TPS4), as Linux's
 * dcn10_link_encoder_set_dp_phy_pattern_training_pattern. */
static void source_pattern(uint32_t index)
{
    wr(DIG(0, 0xB0), index);                                /* DP_DPHY_TRAINING_PATTERN_SEL */
    wr(DIG(0, DP_LINK), rd(DIG(0, DP_LINK)) & ~TRAINING_COMPLETE);
    wr(DIG(0, 0xAF), rd(DIG(0, 0xAF)) & ~DPHY_BYPASS);      /* DP_DPHY_CNTL */
    wr(DIG(0, 0xB5), rd(DIG(0, 0xB5)) & ~DPHY_PRBS_EN);     /* DP_DPHY_PRBS_CNTL */
}

/* The drive level every lane asks for (the highest), in TRAINING_LANEx_SET
 * form: swing bits 1-0, pre-emphasis 4-3, their "max reached" bits 2, 5. */
static uint8_t level_asked(const uint8_t *status, int lanes)
{
    uint8_t swing = 0, pre = 0;
    for (int l = 0; l < lanes; l++) {
        uint8_t b = status[4 + l / 2] >> (4 * (l & 1));
        if ((b & 3) > swing)
            swing = b & 3;
        if ((b >> 2 & 3) > pre)
            pre = b >> 2 & 3;
    }
    if (swing + pre > 3)
        pre = (uint8_t)(3 - swing);
    return (uint8_t)(swing | (swing == 3) << 2 | pre << 3 | (pre == 3 || swing + pre == 3) << 5);
}

static bool lanes_ok(const uint8_t *status, int lanes, uint8_t bits)
{
    for (int l = 0; l < lanes; l++)
        if ((status[l / 2] >> (4 * (l & 1)) & bits) != bits)
            return false;
    return true;
}

static bool set_level(int o, int k, uint8_t level)
{
    if (transmitter_call(TX_SETUP_VSEMPH, level, bring_symclk) != 0)
        return false;
    for (uint32_t l = 0; l < 4; l++)
        if (!dpcd_write(o, k, 0x103 + l, level))
            return false;
    bring_level = level;
    return true;
}

/* DisplayPort link training (8b/10b): false with the step in bring_step. */
static bool link_train(int o, int k)
{
    int lanes = dpcd_kept[1] & 0x1F;
    if (lanes < 1 || lanes > 4)
        lanes = 4;
    uint8_t st[6], level = 0;
    /* clock recovery: TPS1, scrambling off */
    source_pattern(0);
    if (!dpcd_write(o, k, 0x102, 0x21) || !set_level(o, k, level)) {
        ksnprintf(bring_step, sizeof bring_step, "training: TPS1 not set");
        return false;
    }
    int same = 0;
    for (bring_cr_tries = 1;; bring_cr_tries++) {
        wait_us(100);
        if (aux_transfer(o, k, DP_READ, 0x202, 0, 6, st) != 6) {
            ksnprintf(bring_step, sizeof bring_step, "training: lane status not read");
            return false;
        }
        if (lanes_ok(st, lanes, 1))
            break;
        uint8_t want = level_asked(st, lanes);
        same = (want & 0x1B) == (level & 0x1B) ? same + 1 : 0;
        if (same >= 5 || bring_cr_tries >= 32 || (level & 4)) {
            ksnprintf(bring_step, sizeof bring_step, "training: no clock recovery (lanes 0x%02x "
                      "0x%02x, level 0x%02x)", st[0], st[1], level);
            return false;
        }
        level = want;
        if (!set_level(o, k, level))
            return false;
    }
    /* channel equalization, scrambling off: TPS3 where the panel lists it
     * (DPCD 0x002 bit 6), else TPS2, as Linux's decide_eq_training_pattern
     * (37g-6: the VivoBook's DisplayPort 1.1 panel has no TPS3; TPS4 is
     * not used) */
    bool tps3 = tally[o].has_dpcd && (tally[o].dpcd[2] & 0x40);
    source_pattern(tps3 ? 2 : 1);
    if (!dpcd_write(o, k, 0x102, tps3 ? 0x23 : 0x22)) {
        ksnprintf(bring_step, sizeof bring_step, "training: TPS%d not set", tps3 ? 3 : 2);
        return false;
    }
    for (bring_eq_tries = 1;; bring_eq_tries++) {
        wait_us(400);
        if (aux_transfer(o, k, DP_READ, 0x202, 0, 6, st) != 6) {
            ksnprintf(bring_step, sizeof bring_step, "training: lane status not read");
            return false;
        }
        if (!lanes_ok(st, lanes, 1)) {
            ksnprintf(bring_step, sizeof bring_step, "training: clock recovery lost in "
                      "equalization with TPS%d (lanes 0x%02x 0x%02x)", tps3 ? 3 : 2, st[0], st[1]);
            return false;
        }
        if (lanes_ok(st, lanes, 7) && (st[2] & 1))
            break;
        if (bring_eq_tries >= 6) {
            ksnprintf(bring_step, sizeof bring_step, "training: not equalized with TPS%d (lanes "
                      "0x%02x 0x%02x, align 0x%02x, level 0x%02x)", tps3 ? 3 : 2, st[0], st[1],
                      st[2], level);
            return false;
        }
        level = level_asked(st, lanes);
        if (!set_level(o, k, level))
            return false;
    }
    if (!dpcd_write(o, k, 0x102, 0)) {
        ksnprintf(bring_step, sizeof bring_step, "training: pattern not ended");
        return false;
    }
    return true;
}

/* Is name one the pipe's restore leaves alone (status, counters, what the
 * tables and the steps set themselves), or one of the enables written last? */
static bool restore_skips(const char *name)
{
    static const char *const parts[] = { "STATUS", "_COUNT", "POSITION", "PENDING", "READ_LINE",
                                         "LB_V_COUNTER", "EARLIEST_INUSE", "READBACK" };
    static const char *const names[] = {
        "HUBPRET0_HUBPRET_INTERRUPT", "HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT",
        "OTG0_OTG_CONTROL", "OTG0_OTG_MASTER_EN", "HUBP0_DCHUBP_CNTL", "DIG0_DIG_FE_CNTL",
        "DIG0_DIG_BE_EN_CNTL", "DIG0_DIG_LANE_ENABLE", "DP0_DP_VID_STREAM_CNTL",
        "DP0_DP_VID_TIMING", "DP0_DP_VID_N", "DP0_DP_VID_M", "DP0_DP_STEER_FIFO",
        "DP0_DP_LINK_CNTL", "DP0_DP_DPHY_TRAINING_PATTERN_SEL", "DP0_DP_DPHY_CNTL",
        "DP0_DP_DPHY_PRBS_CNTL", "DP0_DP_CONFIG",
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++)
        for (const char *p = name; *p; p++) {
            size_t n = 0;
            while (parts[i][n] && p[n] == parts[i][n])
                n++;
            if (!parts[i][n])
                return true;
        }
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i]))
            return true;
    return false;
}

/* The blocks put back as they were, in order; the DCCG's only for these
 * (the DTO's phase and modulo before its enable; the clocks are the SMU's). */
static const char *const restored_blocks[] = { "MMHUB", "VM", "HUBBUB", "HUBP0", "HUBPREQ0",
                                               "HUBPRET0", "DPP0", "CNVC0", "DSCL0", "CM0", "MPCC0", "OGAM0", "MPC",
                                               "OPP0", "OPP", "OPPBUF0", "DPG0", "FMT0", "OTG0",
                                               "OPTC", "ODM0", "VTG0", "DIG0", "DP0", "DIO" };
static const char *const restored_dccg[] = { "SYMCLKA_CLOCK_ENABLE", "DPPCLK0_DTO_PARAM",
                                             "DPPCLK_DTO_CTRL", "DP_DTO0_PHASE", "DP_DTO0_MODULO",
                                             "OTG0_PHYPLL_PIXEL_RATE_CNTL", "OTG0_PIXEL_RATE_CNTL" };

/* (37g-6) DCN 1.0's too: the deep sleep DTO and the other DP DTOs' moduli
 * (S3 zeroes them; the firmware's table 46 had set them at the boot). */
static const char *const restored_dccg10[] = { "DCCG_DS_DTO_INCR", "DCCG_DS_DTO_MODULO",
                                               "DCCG_DS_CNTL", "DP_DTO1_MODULO", "DP_DTO2_MODULO",
                                               "DP_DTO3_MODULO" };

/* A block's registers take writes only while its clock runs: these first. */
static const char *const restored_clocks[] = { "HUBP0_HUBP_CLK_CNTL", "DPP_TOP0_DPP_CONTROL",
                                               "OPP_TOP_CLK_CONTROL", "OPP_PIPE0_OPP_PIPE_CONTROL",
                                               "OTG0_OTG_CLOCK_CONTROL",
                                               "ODM0_OPTC_INPUT_CLOCK_CONTROL",
                                               "HUBPRET0_HUBPRET_MEM_PWR_CTRL",
                                               "MPCC0_MPCC_MEM_PWR_CTRL", "DIO_MEM_PWR_CTRL" };

static uint32_t restore_named(const char *const *names, size_t count)
{
    uint32_t written = 0;
    for (size_t i = 0; i < count; i++) {
        int r = snap_named(names[i]);
        if (r >= 0 && rd(dcn_pipe0[r].reg) != snap_before[r]) {
            wr(dcn_pipe0[r].reg, snap_before[r]);
            written++;
        }
    }
    return written;
}

/* Block name's registers that differ from before written back (with
 * check: only counted): how many. */
static uint32_t restore_block(const char *name, bool check)
{
    uint32_t n = 0, at = 0;
    for (size_t x = 0; x < dcn_block_count; x++) {
        if (!strcmp(dcn_blocks[x].name, name)) {
            for (uint32_t r = at; r < at + dcn_blocks[x].count && r < SNAP; r++)
                if (!restore_skips(dcn_pipe0[r].name) && rd(dcn_pipe0[r].reg) != snap_before[r]) {
                    if (!check)
                        wr(dcn_pipe0[r].reg, snap_before[r]);
                    n++;
                }
            break;
        }
        at += dcn_blocks[x].count;
    }
    return n;
}

/* The pipe's blocks the same way, in order. */
static uint32_t restore_blocks(bool check)
{
    uint32_t n = 0;
    for (size_t b = 0; b < sizeof restored_blocks / sizeof restored_blocks[0]; b++)
        n += restore_block(restored_blocks[b], check);
    return n;
}

static uint32_t pipe_missed, hub_written, hub_missed;

/* (37g-5c-3) Now: the registers of the restored blocks still not as
 * before the sleep (restore_skips' left out), the value wanted and now. */
static void report_not_taken(void)
{
    uint32_t n = 0;
    for (size_t b = 0; b < sizeof restored_blocks / sizeof restored_blocks[0]; b++) {
        uint32_t at = 0;
        for (size_t x = 0; x < dcn_block_count; x++) {
            if (!strcmp(dcn_blocks[x].name, restored_blocks[b])) {
                for (uint32_t r = at; r < at + dcn_blocks[x].count && r < SNAP; r++) {
                    uint32_t now = rd(dcn_pipe0[r].reg);
                    if (!restore_skips(dcn_pipe0[r].name) && now != snap_before[r]) {
                        if (!n++)
                            kprintf("  not as before the sleep now (wanted -> now):\n");
                        kprintf("    %s 0x%08x -> 0x%08x\n", dcn_pipe0[r].name, snap_before[r], now);
                    }
                }
                break;
            }
            at += dcn_blocks[x].count;
        }
    }
    if (!n)
        kprintf("  every restored register as before the sleep now\n");
}

static uint32_t pipe_restore(void)
{
    uint32_t written = restore_named(restored_dccg, sizeof restored_dccg / sizeof restored_dccg[0]);
    if (picasso)
        written += restore_named(restored_dccg10, sizeof restored_dccg10 / sizeof restored_dccg10[0]);
    written += restore_named(restored_clocks, sizeof restored_clocks / sizeof restored_clocks[0]);
    wait_us(10);
    written += restore_blocks(false);
    restore_blocks(false);                                  /* (once more: an order inside) */
    pipe_missed = restore_blocks(true);
    return written;
}

/* Steps 0b-2b on Cezanne (DCN 2.1). */
static bool cezanne_start(void)
{
    uint32_t ps[ATOM_PARAMS];
    /* (0: the memory hub, put back in amdgpu_resume itself) */
    /* 0b (37g-5d): nothing for the power-management processor (table 46
     * sends it messages) before it is ready, as Linux */
    bring_mark("0b the power-management processor ready");
    char mp1[160];
    bool ready = power_processor_ready(mp1, sizeof mp1);
    kprintf("  amdgpu:        after the sleep: %s\n", mp1);
    if (!ready) {
        ksnprintf(bring_step, sizeof bring_step, "stopped before the golden init, nothing sent: "
                  "%s", mp1);
        return false;
    }
    /* 1, 2: the golden init, the clocks. (37g-5d) As Linux's
     * dcn10_bios_golden_init: only while MICROSECOND_TIME_BASE_DIV is at
     * its reset value (dcn21_s0i3_golden_init_wa), INIT, then each pipe
     * disabled (ATOM_DISABLE: nothing on the Acer's BIOS) */
    int div = snap_named("MICROSECOND_TIME_BASE_DIV");
    if (div >= 0 && rd(dcn_pipe0[div].reg) == GOLDEN_DIV_RESET) {
        bring_mark("1 the golden init (table 13)");
        memset(ps, 0, sizeof ps);
        ps[0] = (uint32_t)TX_INIT << 8;                     /* pipe 0, ATOM_INIT */
        if (atom_run(&atom, T_POWER_GATING, ps) != 0) {
            ksnprintf(bring_step, sizeof bring_step, "the golden init stopped: %s", atom.why);
            return false;
        }
        for (uint32_t p = 0; p < DCN_PIPES; p++) {
            memset(ps, 0, sizeof ps);
            ps[0] = p;                                      /* pipe p, ATOM_DISABLE */
            if (atom_run(&atom, T_POWER_GATING, ps) != 0) {
                ksnprintf(bring_step, sizeof bring_step, "pipe %u's disable stopped: %s", p,
                          atom.why);
                return false;
            }
        }
    } else {
        bring_mark("1 no golden init (the time base kept: Linux's condition)");
    }
    /* 1b (37g-5d): the display's host VM, as Linux at every power-up */
    bring_mark("1b the display's host VM (DCHVM)");
    char hvm[128];
    bool hvm_up = dchvm_init(hvm, sizeof hvm);
    kprintf("  amdgpu:        after the sleep: %s\n", hvm);
    if (!hvm_up)
        kprintf("  amdgpu:        after the sleep: (the display's memory reads may fail)\n");
    bring_mark("2 the display and DPP clocks (table 46)");
    for (int c = 0; c < 2; c++) {
        memset(ps, 0, sizeof ps);
        ps[0] = CLOCK_700MHZ;
        ps[1] = c ? DCE_DPPCLK : DCE_DISPCLK;
        if (atom_run(&atom, T_DCE_CLOCK, ps) != 0) {
            ksnprintf(bring_step, sizeof bring_step, "the %s clock not set: %s", c ? "DPP" : "display",
                      atom.why);
            return false;
        }
    }
    /* 2b (37g-5d): MP1 told a display is on, as Linux's rn_update_clocks
     * before a stream (DCN_PWR_STATE_MISSION_MODE: SetDisplayCount(1)); the
     * memory replies' status before and after it */
    bring_mark("2b MP1 told a display is on (SetDisplayCount 1)");
    uint32_t sdp_was = rd(DCHUBBUB_SDPIF_CFG0_REG), returned = 0;
    uint32_t answer = display_message(DISPLAY_MSG_SET_DISPLAY_COUNT, 1, &returned);
    kprintf("  amdgpu:        after the sleep: SetDisplayCount(1): the answer 0x%02x, the parameter "
            "back 0x%x; DCHUBBUB_SDPIF_CFG0 0x%08x before, 0x%08x after\n", answer, returned, sdp_was,
            rd(DCHUBBUB_SDPIF_CFG0_REG));
    return true;
}

/* (37g-6) Steps 0b-2 on DCN 1.0, as Linux's dcn10_init_hw and rv1_clk_mgr. */
static uint32_t dentist_divider(uint32_t did)       /* x4: Linux's dentist_get_divider_from_did */
{
    if (did < 0x08)
        did = 0x08;
    if (did > 0x7F)
        did = 0x7F;
    if (did < 0x40)
        return 8 + (did - 0x08);
    if (did < 0x60)
        return 64 + 2 * (did - 0x40);
    if (did < 0x7E)
        return 128 + 4 * (did - 0x60);
    return 248 + 264 * (did - 0x7E);
}

static bool dcn10_start(void)
{
    uint32_t ps[ATOM_PARAMS];
    /* 0b: the display mailbox as it is (Linux looks at nothing before) */
    kprintf("  amdgpu:        after the sleep: the display mailbox's answer register 0x%08x\n",
            rd(MP1_MAILBOX_RESPONSE));
    /* 1: the golden init (dcn10_bios_golden_init: INIT, each pipe's DISABLE) */
    bring_mark("1 the golden init (table 13), each pipe disabled");
    memset(ps, 0, sizeof ps);
    ps[0] = (uint32_t)TX_INIT << 8;
    if (atom_run(&atom, T_POWER_GATING, ps) != 0) {
        ksnprintf(bring_step, sizeof bring_step, "the golden init stopped: %s", atom.why);
        return false;
    }
    for (uint32_t p = 0; p < DCN_PIPES; p++) {
        memset(ps, 0, sizeof ps);
        ps[0] = p;
        if (atom_run(&atom, T_POWER_GATING, ps) != 0) {
            ksnprintf(bring_step, sizeof bring_step, "pipe %u's disable stopped: %s", p, atom.why);
            return false;
        }
    }
    /* 2: the display clock as before the sleep, then the reference clock
     * (ramp_up_dispclk_with_dpp: set_dispclk, set_dprefclk) */
    uint32_t vco_khz = smu_vco_10khz ? smu_vco_10khz * 10 : 3600000;
    uint32_t div = dentist_divider(snap_was("DENTIST_DISPCLK_CNTL") & 0x7F);
    uint32_t want_mhz = (uint32_t)(((uint64_t)vco_khz * 4 / div + 999) / 1000);
    bring_mark("2 the display clock as before, the reference clock (messages 4, 5)");
    uint32_t got_disp = 0, got_ref = 0;
    uint32_t a1 = display_message(RV_MSG_SET_DISPCLK, want_mhz, &got_disp);
    uint32_t a2 = display_message(RV_MSG_SET_DPREFCLK, RV_DPREFCLK_MHZ, &got_ref);
    kprintf("  amdgpu:        after the sleep: the display clock asked %u MHz (DENTIST divider "
            "0x%02x before, VCO %u MHz): the answer 0x%02x, %u MHz; the reference clock %u MHz: "
            "the answer 0x%02x, %u MHz; DENTIST_DISPCLK_CNTL now 0x%08x\n", want_mhz,
            snap_was("DENTIST_DISPCLK_CNTL") & 0x7F, vco_khz / 1000, a1, got_disp,
            RV_DPREFCLK_MHZ, a2, got_ref, reg_now("DENTIST_DISPCLK_CNTL"));
    if (a1 != 1 || a2 != 1) {
        ksnprintf(bring_step, sizeof bring_step, "the clocks not set (answers 0x%02x, 0x%02x)", a1,
                  a2);
        return false;
    }
    return true;
}

static bool bring_up(void)
{
    int o = aux_output, k = 0, n = 0;
    if (o < 0 || !atom_ready || !dpcd_saved || !output_lines(o, &k, &n) ||
        transmitter(paths[output_path[o]].encoder) != 0) {
        ksnprintf(bring_step, sizeof bring_step, "not tried (%s)", o < 0 ? "no panel" : !atom_ready ?
                  "no AtomBIOS" : !dpcd_saved ? "the panel's settings not kept" :
                  transmitter(paths[output_path[o]].encoder) ? "the panel not on DIG0" : "no AUX line");
        return false;
    }
    if (!(picasso ? dcn10_start() : cezanne_start()))
        return false;
    /* 3: the PHY's init, the panel's supply, its HPD, AUX, its settings */
    bring_mark("3 the panel's power (table 76 INIT, POWER_ON), its HPD");
    if (transmitter_call(TX_INIT, 0, 0) != 0 || transmitter_call(TX_POWER_ON, 0, 0) != 0) {
        ksnprintf(bring_step, sizeof bring_step, "the panel's power: %s", atom.why);
        return false;
    }
    bool sensed = false;
    for (int t = 0; t < 100 && !sensed; t++) {
        sensed = rd(HPD_INT_STATUS(n)) & HPD_SENSE;
        if (!sensed)
            timer_sleep_ms(5);
    }
    if (!sensed) {
        ksnprintf(bring_step, sizeof bring_step, "the panel's HPD did not come in 500 ms");
        return false;
    }
    bring_mark("3b AUX and HPD as before, the panel's DPCD");
    /* the line's pads back in AUX mode (DC_GPIO_DDCn_MASK's AUX_PAD_MODE:
     * S3 leaves them in I2C mode, and every AUX transfer runs out of time;
     * table 78 and Linux's AUX engine set it before using the line) */
    const struct pin *ddc = find_pin(paths[output_path[o]].ddc);
    if (ddc)
        wr(ddc->reg - 1u, rd(ddc->reg - 1u) | AUX_PAD_MODE);
    snap_put("HPD0_DC_HPD_INT_CONTROL");
    snap_put("HPD0_DC_HPD_CONTROL");
    snap_put("DP_AUX0_AUX_DPHY_TX_REF_CONTROL");
    snap_put("DP_AUX0_AUX_DPHY_TX_CONTROL");
    snap_put("DP_AUX0_AUX_DPHY_RX_CONTROL0");
    snap_put("DP_AUX0_AUX_DPHY_RX_CONTROL1");
    snap_put("DP_AUX0_AUX_INTERRUPT_CONTROL");
    snap_put("DP_AUX0_AUX_CONTROL");
    timer_sleep_ms(2);
    if (!dpcd_write(o, k, 0x600, dpcd_power_kept ? dpcd_power_kept : 1)) {
        ksnprintf(bring_step, sizeof bring_step, "the panel did not answer over AUX");
        return false;
    }
    timer_sleep_ms(1);
    static const uint8_t kept_at[] = { 0, 1, 7, 8, 10 };    /* 0x100 rate, 0x101 lanes, 0x107-8, 0x10A */
    for (size_t i = 0; i < sizeof kept_at; i++)
        dpcd_write(o, k, 0x100 + kept_at[i], dpcd_kept[kept_at[i]]);
    if (dpcd_rate_kept || !dpcd_kept[0])
        dpcd_write(o, k, 0x115, dpcd_rate_kept);
    /* 4: the PHY on at the link's rate: the encoder's lanes, DP SST, its
     * panel mode as before, then the table */
    bring_mark("4 the PHY on (table 76 ENABLE)");
    snap_put("DP0_DP_CONFIG");
    snap_put("DP0_DP_DPHY_INTERNAL_CTRL");
    snap_put("DP0_DP_DPHY_SCRAM_CNTL");
    snap_put("DIG0_DIG_BE_CNTL");
    /* the symbol clock (10 kHz) from the panel's rate: 10 Mbit/s units x 100 */
    const struct aux_tally *t = &tally[o];
    uint32_t rate = t->link[0] ? t->link[0] * 27u : table_rate(t, t->rate_index);
    bring_symclk = rate * 100;
    if (!t->has_link || !rate) {
        ksnprintf(bring_step, sizeof bring_step, "the link's rate not known");
        return false;
    }
    if (transmitter_call(TX_ENABLE, 0, bring_symclk) != 0) {
        ksnprintf(bring_step, sizeof bring_step, "the PHY not enabled: %s", atom.why);
        return false;
    }
    /* 5: link training, then video mode as before */
    bring_mark("5 link training");
    if (!link_train(o, k))
        return false;
    snap_put("DP0_DP_LINK_FRAMING_CNTL");
    snap_put("DP0_DP_DPHY_SCRAM_CNTL");
    snap_put("DP0_DP_DPHY_INTERNAL_CTRL");
    snap_put("DP0_DP_DPHY_TRAINING_PATTERN_SEL");
    snap_put("DP0_DP_DPHY_PRBS_CNTL");
    snap_put("DP0_DP_DPHY_CNTL");
    snap_put("DP0_DP_LINK_CNTL");
    /* 6: the pipe, then the enables in set_mode's order */
    bring_mark("6 the pipe's registers as before");
    uint32_t written = pipe_restore();
    bring_mark("6b the OTG, the plane, the stream on");
    snap_put("OTG0_OTG_MASTER_EN");
    snap_put("OTG0_OTG_CONTROL");
    uint32_t htotal = (snap_was("OTG0_OTG_H_TOTAL") & 0x7FFF) + 1;
    uint32_t vtotal = (snap_was("OTG0_OTG_V_TOTAL") & 0x7FFF) + 1;
    uint32_t phase = snap_was("DP_DTO0_PHASE");             /* (the pixel clock in Hz) */
    timer_sleep_ms(phase ? (uint32_t)((uint64_t)htotal * vtotal * 1000 / phase) + 2 : 20);
    snap_put("HUBP0_DCHUBP_CNTL");
    /* (37g-5d) The plane's surface address loaded as Linux's
     * hubp2_program_surface_flip_and_addr writes it: the flip control, the
     * high part, then the low part, whose write starts the update; then
     * the address in use (EARLIEST_INUSE) awaited, as our flip. Put back in
     * another order, the plane kept the address it had after the reset:
     * the Acer's gray screen. */
    uint64_t surface = (uint64_t)(snap_was("HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH") & 0xFFFF)
                       << 32 | snap_was("HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS");
    wr(HUBP(0, HUBP_FLIP_CONTROL), rd(HUBP(0, HUBP_FLIP_CONTROL)) & ~(1u << 1 | 1u << 12 | 1u << 16));
    wr(HUBP(0, HUBP_ADDRESS_HIGH), (uint32_t)(surface >> 32) & 0xFFFF);
    wr(HUBP(0, HUBP_ADDRESS), (uint32_t)surface);
    uint64_t inuse = 0;
    for (int n = 0; n < 10 && inuse != surface; n++) {      /* (6 frames at 60 Hz: ~100 ms) */
        timer_sleep_ms(17);
        inuse = (uint64_t)(rd(HUBP(0, HUBP_EARLIEST_INUSE + 1)) & 0xFFFF) << 32 |
                rd(HUBP(0, HUBP_EARLIEST_INUSE));
    }
    bring_address = inuse == surface;
    wr(DIG(0, DP_VID_TIMING), snap_was("DP0_DP_VID_TIMING") & ~MN_GEN_EN);
    snap_put("DP0_DP_VID_N");
    snap_put("DP0_DP_VID_M");
    snap_put("DP0_DP_VID_TIMING");
    wr(DIG(0, DIG_FE), snap_was("DIG0_DIG_FE_CNTL") | DIG_START);
    wait_us(1);
    wr(DIG(0, DIG_FE), snap_was("DIG0_DIG_FE_CNTL") & ~DIG_START);
    wr(DIG(0, DP_STEER_FIFO), rd(DIG(0, DP_STEER_FIFO)) | 1);
    wait_us(10);
    wr(DIG(0, DP_STEER_FIFO), rd(DIG(0, DP_STEER_FIFO)) & ~1u);
    wait_us(100);
    snap_put("DP0_DP_VID_STREAM_CNTL");
    bool streaming = wait_reg(DIG(0, DP_VIDEO), STREAM_STATUS, STREAM_STATUS, 120);
    /* 7: the backlight, the IH ring */
    bring_mark("7 the backlight (table 76 LCD_BLON), the IH ring");
    snap_put("LVTMA_PWRSEQ_REF_DIV");
    uint32_t lock = snap_was("BL_PWM_GRP1_REG_LOCK");
    wr(BL_PWM_GRP1_REG_LOCK, lock | BL_IGNORE_MASTER_LOCK | BL_LOCK);
    snap_put("BL_PWM_PERIOD_CNTL");
    snap_put("BL_PWM_CNTL");
    wr(BL_PWM_GRP1_REG_LOCK, (lock | BL_IGNORE_MASTER_LOCK) & ~BL_LOCK);
    if (transmitter_call(TX_LCD_BLON, 0, 0) != 0) {
        ksnprintf(bring_step, sizeof bring_step, "the backlight not on: %s", atom.why);
        return false;
    }
    if (vblank_otg >= 0) {
        ih_program();
        ih_enable(vblank_otg);
    }
    ksnprintf(bring_step, sizeof bring_step, "the panel back: the memory hub's %u registers "
              "put back (%u did not take); trained (clock recovery in %u, equalization in %u, "
              "level 0x%02x), %u registers put back (%u did not take), the plane's address %s, "
              "the stream %s", hub_written, hub_missed, bring_cr_tries, bring_eq_tries, bring_level,
              written, pipe_missed, bring_address ? "in use" : "NOT IN USE",
              streaming ? "on" : "NOT on");
    return streaming && bring_address;
}

static void bring_thread(void *arg)
{
    (void)arg;
    timer_sleep_ms(2000);                       /* (the other drivers and the network first) */
    uint64_t started = arch_counter();
    bring_cr_tries = bring_eq_tries = 0;
    bring_up();
    bringing_up = false;
    bring_ms = (uint32_t)((arch_counter() - started) * 1000 / arch_counter_hz());
    kprintf("  amdgpu:        after the sleep: %s (%u ms)\n", bring_step, bring_ms);
}

/* (37g-5d) Before the sleep, as Linux's display code: the stream off,
 * the OTG stopped, the plane blanked until it has no memory request
 * outstanding (hubp2_set_blank), so nothing of the display engine reads
 * memory while the machine goes to sleep (running, the Acer now and then
 * hung on the way into S3, its screen dark). */
static void pipe_stop(void)
{
    int k = aux_output >= 0 && (is_cezanne || picasso) ? output_otg(aux_output) : -1, d = -1;
    for (int f = 0; k >= 0 && f < DIGS; f++)
        if ((rd(DIG(f, DIG_FE)) & 7) == (uint32_t)k && (rd(DIG(f, DIG_BE_EN)) & 1))
            d = f;
    if (k < 0 || d < 0) {
        kprintf("  amdgpu:        for the sleep: the panel shows nothing, no pipe to stop\n");
        return;
    }
    uint32_t v = rd(DIG(d, DP_VIDEO));
    wr(DIG(d, DP_VIDEO), (v & ~0x300u) | 2u << 8);
    wr(DIG(d, DP_VIDEO), rd(DIG(d, DP_VIDEO)) & ~STREAM_ENABLE);
    bool stopped = wait_reg(DIG(d, DP_VIDEO), STREAM_STATUS, 0, 120);
    wr(OTG(k, OTG_BLANK_CONTROL), (rd(OTG(k, OTG_BLANK_CONTROL)) | BLANK_DATA_EN) & ~BLANK_DE_MODE);
    wr(OTG(k, OTG_DOUBLE_BUFFER), rd(OTG(k, OTG_DOUBLE_BUFFER)) & ~BLANK_DOUBLE_BUFFER);
    wr(OTG(k, OTG_CONTROL), (rd(OTG(k, OTG_CONTROL)) & ~0x301u) | 3u << 8);
    wr(VTG(k), rd(VTG(k)) & ~VTG_ENABLE);
    bool idle = wait_reg(OTG(k, OTG_CLOCK_CONTROL), OTG_BUSY, 0, 100);
    wr(HUBP(k, HUBP_CNTL), rd(HUBP(k, HUBP_CNTL)) | PLANE_BLANK_EN | PLANE_TTU_DISABLE);
    bool quiet = wait_reg(HUBP(k, HUBP_CNTL), PLANE_NO_REQUESTS, PLANE_NO_REQUESTS, 1);
    kprintf("  amdgpu:        for the sleep: the stream %s, OTG%d %s, its plane %s\n",
            stopped ? "off" : "NOT off", k, idle ? "stopped" : "STILL BUSY",
            quiet ? "blanked with no memory request outstanding" : "blanked, REQUESTS OUTSTANDING");
}

static bool amdgpu_suspend(struct pci_device *pci)
{
    (void)pci;
    if (bringing_up) {
        kprintf("  amdgpu:        no sleep now: the panel is still being brought back\n");
        return false;                           /* (the sleep is called off) */
    }
    for (size_t i = 0; i < WATCHED; i++)
        before_sleep[i] = watched_there(i) ? watched_read(i) : 0;
    /* (37g-5c-3) the snapshot only of a pipe that shows the picture (after a
     * bring-up that failed, the one before is kept) */
    bool shows = aux_output >= 0 && showing(&cezanne, aux_output) &&
                 (rd(DIG(0, DP_VIDEO)) & STREAM_STATUS);
    if (snap_there() && !shows && snap_taken)
        kprintf("  amdgpu:        the panel shows nothing: the registers kept from before the "
                "last sleep stay\n");
    if (snap_there() && (shows || !snap_taken)) {
        snap_read(snap_before);
        /* (37g-5c-3) the panel's own link settings, for after the sleep */
        int k, n;
        dpcd_saved = aux_output >= 0 && output_lines(aux_output, &k, &n) &&
                     aux_transfer(aux_output, k, DP_READ, 0x100, 0, sizeof dpcd_kept,
                                  dpcd_kept) == (int)sizeof dpcd_kept &&
                     aux_transfer(aux_output, k, DP_READ, 0x115, 0, 1, &dpcd_rate_kept) == 1 &&
                     aux_transfer(aux_output, k, DP_READ, 0x600, 0, 1, &dpcd_power_kept) == 1;
    }
    pipe_stop();
    return true;
}

static bool amdgpu_resume(struct pci_device *pci)
{
    (void)pci;
    int kept = 0, looked = 0;
    for (size_t i = 0; i < WATCHED; i++) {
        if (!watched_there(i))
            continue;
        uint32_t now = watched_read(i);
        looked++;
        if (now == before_sleep[i])
            kept++;
        else
            kprintf("  amdgpu:        after the sleep %s 0x%08x (before it 0x%08x)\n",
                    watched[i].name, now, before_sleep[i]);
    }
    kprintf("  amdgpu:        after the sleep %d of %d display registers as before (nothing "
            "set up again yet: the panel stays dark)\n", kept, looked);
    if (snap_there()) {
        snap_read(snap_after);
        snap_taken = true;
        snap_sleeps++;
        snap_log();
        /* (37g-6) asic_init (table 0) first where the video BIOS's own mark
         * says the chip lost its set-up: Linux's amdgpu_device_resume posts
         * the card when BIOS_SCRATCH_7's ASIC_INIT_COMPLETE is clear
         * (amdgpu_atombios_scratch_need_asic_init), with the boot's engine
         * and memory clocks (amdgpu_atom_asic_init). Only on DCN 1.0: the
         * Acer's path is the one it was proven with. */
        uint32_t scratch7 = atom_ready ? rd(BIOS_SCRATCH_7) : 0;
        if (!atom_ready || !picasso) {
            ksnprintf(asic_init_said, sizeof asic_init_said, "asic_init: not looked at");
        } else if (scratch7 & ASIC_INIT_COMPLETE) {
            ksnprintf(asic_init_said, sizeof asic_init_said, "asic_init: not needed "
                      "(BIOS_SCRATCH_7 0x%08x)", scratch7);
        } else if (!boot_sclk_10khz || !boot_mclk_10khz) {
            ksnprintf(asic_init_said, sizeof asic_init_said, "asic_init: needed (BIOS_SCRATCH_7 "
                      "0x%08x) but firmware_info gives no boot clocks", scratch7);
        } else {
            uint32_t ps[ATOM_PARAMS];
            memset(ps, 0, sizeof ps);
            ps[0] = boot_sclk_10khz;
            ps[1] = boot_mclk_10khz;
            uint64_t t0 = arch_counter();
            int r = atom_run(&atom, T_ASIC_INIT, ps);
            uint32_t us = (uint32_t)((arch_counter() - t0) * 1000000 / arch_counter_hz());
            ksnprintf(asic_init_said, sizeof asic_init_said, "asic_init: run (BIOS_SCRATCH_7 "
                      "0x%08x before, 0x%08x after; %u us): %s", scratch7, rd(BIOS_SCRATCH_7), us,
                      r == 0 ? "done" : atom.why);
        }
        kprintf("  amdgpu:        after the sleep: %s\n", asic_init_said);
        /* (37g-5d) the GPU's memory hub as before the sleep, at once: Linux's
         * resume sets it up first (the GMC block, mmhub_v1_0's gart_enable),
         * and the console's writes through BAR 0 come right after the
         * drivers are back; the golden init copies its memory window from it */
        hub_written = restore_block("MMHUB", false);
        hub_missed = restore_block("MMHUB", true);
        /* (37g-5c-3) on a thread of its own, once every driver is back */
        bringing_up = true;
        if (!thread_create("amdgpu wake", bring_thread, 0)) {
            bringing_up = false;
            kprintf("  amdgpu:        after the sleep: no thread to bring the panel back\n");
        }
    }
    return true;
}

/* (67f) The module's start, for AMD's devices: a display controller only
 * (class 03; the vendor's audio, IOMMU and the rest are not ours). */
static bool start(struct pci_device *d)
{
    if (d->class_code != 0x03 || cezanne.pci)
        return false;                               /* (one chip: its state is static) */
    if (!find_rom(d)) {
        kprintf("  amdgpu:        %02x:%02x.%u: no video BIOS for it in the firmware's tables\n",
                d->bus, d->device, d->function);
        return false;
    }
    if (!read_atom()) {
        kprintf("  amdgpu:        %02x:%02x.%u: its video BIOS is not AtomBIOS this driver "
                "knows (%u bytes)\n", d->bus, d->device, d->function, rom_size);
        return false;
    }
    is_cezanne = d->device_id == 0x1638;
    picasso = d->device_id == 0x15D8;               /* (37g-6) DCN 1.0 */
    cezanne.model = is_cezanne ? "AMD Radeon Graphics (Cezanne)" :
                    picasso ? "AMD Radeon Graphics (Picasso)" : "AMD Radeon Graphics";
    cezanne.pci = d;
    cezanne.report = report;
    const struct pci_bar *bar = &d->bars[5];
    uint64_t span = is_cezanne ? 4 * (uint64_t)CLK_LAST     /* (66g-1: the clock block too) */
                               : 4 * (uint64_t)DCN21_LAST;  /* (the display engine's, as far as HPD) */
    if (picasso) {
        ly = &dcn10;
        dcn_pipe0 = dcn10_pipe0;                    /* (37g-6) */
        dcn_blocks = dcn10_blocks;
        snap_count = DCN10_REGS;
        dcn_block_count = DCN10_BLOCKS;
    }
    if ((is_cezanne || picasso) && !bar->is_io && bar->address && bar->size >= span)
        regs = paging_map_mmio(bar->address, span);
    /* (37e-2a) the backlight's registers: Cezanne's mapping, or on Raven
     * (15dd; 15d8 Picasso) only as far as them */
    bool raven = d->device_id == 0x15DD || d->device_id == 0x15D8;
    if (regs)
        bl_regs = regs;
    else if (raven && !bar->is_io && bar->address && bar->size >= 4 * (uint64_t)BL_LAST)
        bl_regs = paging_map_mmio(bar->address, 4 * (uint64_t)BL_LAST);
    ask_atif(d);
    if (bl_regs) {
        cezanne.backlight = backlight;
        cezanne.brightness = brightness;            /* (37e-2c) */
        cezanne.brightness_now = brightness_now;
    }
    if (regs) {
        cezanne.read_edid = read_edid;
        cezanne.showing = showing;
        if (is_cezanne)
            cezanne.set_mode = set_mode;
    }
    for (int k = 0; k < path_count && cezanne.output_count < GPU_OUTPUTS; k++) {
        const char *where, *kind = connector_kind(paths[k].connector, &where);
        if (!kind)
            continue;
        cezanne.outputs[cezanne.output_count].kind = kind;
        cezanne.outputs[cezanne.output_count].where = where;
        output_path[cezanne.output_count] = k;
        cezanne.output_count++;
    }
    for (int o = 0; regs && is_cezanne && o < cezanne.output_count && vblank_otg < 0; o++) {
        int k = output_otg(o);
        const char *why = "";
        if (k < 0)
            continue;
        if (vblank_on(k, &why)) {
            cezanne.wait_vblank = wait_vblank;
            cezanne.flip = flip;
            cezanne.buffers = buffers;
            kprintf("  amdgpu:        the vertical blank: OTG%d's VSTARTUP through the IH ring, by "
                    "MSI\n", k);
        } else
            kprintf("  amdgpu:        no vertical blank interrupt: %s\n", why);
    }
    atom_start(d);                                  /* (37g-5b) */
    cezanne.report_sleep = report_sleep;            /* (37g-5c-2) */
    gpu_register(&cezanne);
    return true;
}

/* AMD's devices: start keeps the display controller. */
static const struct module_pci ids[] = {
    { 0x1002, MODULE_ANY },
};

MODULE(amdgpu, start, amdgpu_suspend, amdgpu_resume, ids)
