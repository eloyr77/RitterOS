/*
 * (37e-1b, the user's rule) The settings files' default texts: every file
 * of /system/config that holds settings is written with these when it is
 * missing, by the one that reads it (the kernel at the start, after the
 * boot's check of the disk; a program when it looks): every key at its
 * default and comments saying what it can be, so nobody writes a file from
 * memory, and a deleted file comes back. A key whose default is "not set"
 * is a commented line to remove the # from. One text per file, here only:
 * the kernel and the programs include this header, the tests read it
 * (scripts/settingstext.py).
 */
#ifndef SETTINGS_DEFAULTS_H
#define SETTINGS_DEFAULTS_H

/* /system/config/cpu: written by the kernel (cpu_clock_load) */
#define SETTINGS_CPU "/system/config/cpu"
#define SETTINGS_CPU_TEXT \
    "# The processor's clock (AMD processors with CPPC; read at the start,\n" \
    "# so a change counts from the next start):\n" \
    "#\n" \
    "#   clock = performance  the processor chooses its clock itself,\n" \
    "#                        favouring speed (the default)\n" \
    "#   clock = balanced     it chooses, balancing speed and power\n" \
    "#   clock = powersave    it chooses, favouring low power\n" \
    "#   clock = top          always its highest clock while it runs\n" \
    "#                        (fastest; more power while idle)\n" \
    "#   clock = firmware     the firmware's own settings (as before 64t)\n" \
    "#\n" \
    "# Measured on a Ryzen 7 5800H streaming from a PlutoSDR (short bursts of\n" \
    "# work): about 3,200 MHz with performance, balanced or firmware, 3,540 MHz\n" \
    "# with top; idle, top stays near 3,200 MHz, the others drop to about\n" \
    "# 1,400 MHz. `threads` shows the clock while running.\n" \
    "#\n" \
    "# Elsewhere (Intel processors, ARM, QEMU) the setting is not used.\n" \
    "clock = performance\n"

/* /system/config/power: written by the kernel (acpi_events_settings) */
#define SETTINGS_POWER "/system/config/power"
#define SETTINGS_POWER_TEXT \
    "# Power management, read by the kernel at the start (a change counts from\n" \
    "# the next start; `power` shows the settings in use):\n" \
    "#\n" \
    "#   power_button = off      a press of the power button switches the\n" \
    "#                           machine off, as typing poweroff does (the default)\n" \
    "#   power_button = nothing  a press is only logged\n" \
    "#   power_button = sleep    a press makes the machine sleep (power sleep);\n" \
    "#                           the next press wakes it\n" \
    "#\n" \
    "#   lid = nothing           closing the lid is only logged (the default)\n" \
    "#   lid = sleep             closing the lid makes the machine sleep; the\n" \
    "#                           power button (or on many laptops opening the\n" \
    "#                           lid) wakes it\n" \
    "#\n" \
    "#   screen_off = 5          minutes without a key, a click or a mouse move\n" \
    "#                           until the screen goes off (0: it stays on); any\n" \
    "#                           key or mouse move brings it back\n" \
    "#   screen_dim = 2          minutes until a panel dims to a third, when that\n" \
    "#                           comes before screen_off (0: never)\n" \
    "#\n" \
    "#   sleep_after = 0         minutes without a key, a click, a mouse move or\n" \
    "#                           typing over SSH until the machine sleeps (0:\n" \
    "#                           never); the power button wakes it\n" \
    "power_button = off\n" \
    "lid = nothing\n" \
    "screen_off = 5\n" \
    "screen_dim = 2\n" \
    "sleep_after = 0\n"

/* /system/config/displays: written by the kernel (gpu_load_settings) */
#define SETTINGS_DISPLAYS "/system/config/displays"
#define SETTINGS_DISPLAYS_TEXT \
    "# The screens' modes, set at the start: one line per output,\n" \
    "#\n" \
    "#   OUTPUT = WIDTHxHEIGHT       e.g. hdmi0 = 1920x1080\n" \
    "#   OUTPUT = WIDTHxHEIGHT@HZ    e.g. hdmi0 = 1920x1080@60\n" \
    "#\n" \
    "# `displays` lists the outputs (hdmi0, vga0, dp0, edp0, ...) and the modes\n" \
    "# their monitors offer; `displays set OUTPUT WxH` tries one at once. An\n" \
    "# output without a line keeps the mode the firmware chose. Remove the # to\n" \
    "# use a line:\n" \
    "# hdmi0 = 1920x1080@60\n"

/* /system/config/swap: written by the kernel (swap_settings); swap on/off change its line */
#define SETTINGS_SWAP "/system/config/swap"
#define SETTINGS_SWAP_TEXT \
    "# Swap: the file /system/swap holds programs' memory pages, encrypted, when\n" \
    "# memory runs short (`swap make SIZE` makes it and turns swap on, e.g.\n" \
    "# swap make 4G).\n" \
    "#\n" \
    "#   on = no    swap is not used (the default)\n" \
    "#   on = yes   swap is turned on at every start\n" \
    "#\n" \
    "# `swap on` and `swap off` turn it on or off at once and change this line.\n" \
    "on = no\n"

/* ~/.config/windows, a user's own (the user's rule: a user's settings go
 * in the user's folder): written by login at a console login and by
 * windowd, when missing. Every key is a comment showing its default: a
 * line without # wins over /system/config/windows, the optional file
 * for everyone (an administrator's; nothing writes it). */
#define SETTINGS_USER_WINDOWS ".config/windows"
#define SETTINGS_USER_WINDOWS_TEXT \
    "# The window system's settings for you (they win over\n" \
    "# /system/config/windows). Remove the # from a line to change it.\n" \
    "#\n" \
    "# What windowd opens when it starts: a program and its arguments;\n" \
    "# owl (the file browser) if not set, nothing if empty.\n" \
    "# startup = owl\n" \
    "#\n" \
    "# A double click: two presses within this many milliseconds\n" \
    "# (100-5000; 500 if not set).\n" \
    "# double_click = 500\n" \
    "#\n" \
    "# Clicks where you aimed on a touchpad (the pointer's slow creep as\n" \
    "# the finger presses and lifts is held); on if not set.\n" \
    "# click_steady = on\n" \
    "#\n" \
    "# Frames a second windowd draws at most: auto, the screen's own rate\n" \
    "# (from the monitor), or a number (10-1000); auto if not set.\n" \
    "# refresh = auto\n" \
    "#\n" \
    "# The size of text in programs that draw it with scalable fonts at\n" \
    "# your size (FreeSans...): pixels an em, 8-64; 15 if not set.\n" \
    "# text_size = 15\n" \
    "#\n" \
    "# The colours of windows, menus and programs: dark or light; dark if\n" \
    "# not set. Programs read it when they start (windowd too).\n" \
    "# theme = dark\n" \
    "#\n" \
    "# The colour of what is in front and chosen (the window in front's\n" \
    "# title bar, menus, selections, focus), as RRGGBB; the theme's if not\n" \
    "# set (5E81AC, a blue).\n" \
    "# accent = 5E81AC\n"

/* (42a) ~/.config/filetypes, a user's own: which program a double click in
 * owl (and `open FILE`) starts for a file, by its ending. Written by login
 * at a login and by libwindow's window_app_for, when missing; unlike the
 * other files its lines are in use (it is the list itself). A line here
 * wins over /system/config/filetypes (optional, an administrator's). */
#define SETTINGS_USER_FILETYPES ".config/filetypes"
#define SETTINGS_USER_FILETYPES_TEXT \
    "# The program a double click in owl (and open FILE) starts for a file,\n" \
    "# by the file's ending, in small or capital letters: ENDING = PROGRAM.\n" \
    "# An app (in /system/<arch>/apps) opens it in its window, any other\n" \
    "# program in a terminal window; an ending not here opens in the editor.\n" \
    "# Apps open other files too, from their own menus.\n" \
    "ppm = imageview\n" \
    "bmp = imageview\n" \
    "png = imageview\n" \
    "jpg = imageview\n" \
    "jpeg = imageview\n" \
    "rvid = reel\n" \
    "html = ibrowser\n" \
    "htm = ibrowser\n" \
    "wav = tune\n" \
    "txt = edit\n" \
    "md = edit\n" \
    "c = edit\n" \
    "h = edit\n"

/* /system/config/wifi/radio: written by wifid; wifi country changes its line */
#define SETTINGS_WIFI_RADIO "/system/config/wifi/radio"
#define SETTINGS_WIFI_RADIO_TEXT \
    "# The Wi-Fi radio (wifid reads it when it starts; the saved networks are\n" \
    "# in networks/, written by `wifi join`):\n" \
    "#\n" \
    "#   country = 00     the country whose rules the card's firmware follows for\n" \
    "#                    channels and transmit power: a two-letter code such as\n" \
    "#                    US or DE (`wifi country CC` sets it); 00 is the\n" \
    "#                    world-wide safe choice (the default; the Intel 8265's\n" \
    "#                    firmware chooses the country itself)\n" \
    "#   powersave = off  on: the card dozes between beacons once joined (less\n" \
    "#                    power; an idle machine answers a ping only at its next\n" \
    "#                    wake); off: always awake (the default)\n" \
    "country = 00\n" \
    "powersave = off\n"

/* /system/config/routing: written by net up */
#define SETTINGS_ROUTING "/system/config/routing"
#define SETTINGS_ROUTING_TEXT \
    "# Routing between the network cards (net up applies it). Remove a # to use\n" \
    "# a line:\n" \
    "#\n" \
    "# Pass packets on between the cards (the machine as a router); off when not\n" \
    "# set. `net forward on` switches it at once.\n" \
    "# forward = on\n" \
    "#\n" \
    "# The private cards, whose packets leave through the others with this\n" \
    "# machine's address (masquerade, source NAT; IPv4); none when not set.\n" \
    "# masquerade = eth1\n" \
    "#\n" \
    "# The cards the default route prefers, in order: the first of them with a\n" \
    "# link, an address and a gateway carries it; when not set (or none can),\n" \
    "# the first card with a gateway and a link. `net default` shows it.\n" \
    "# default = eth0 wlan0\n"

/* /system/config/network/virtual (38c): written by net (net up, and net
 * tap / net bridge, which change their own lines) */
#define SETTINGS_VIRTUAL "/system/config/network/virtual"
#define SETTINGS_VIRTUAL_TEXT \
    "# Virtual network cards and bridges, made again by net up at every start\n" \
    "# (taps first, then bridges; then every card's own settings). `net tap add`,\n" \
    "# `net tap remove`, `net bridge add` and `net bridge remove` change their\n" \
    "# lines. Remove a # to use a line:\n" \
    "#\n" \
    "# A virtual card tapN whose /dev/tapN the user (and root) may open.\n" \
    "# tap0 = user root\n" \
    "#\n" \
    "# A bridge brN of these cards (its MAC address the first one's there).\n" \
    "# br0 = eth0 tap0\n"

/* /system/config/time: written by login (the library reads it) */
#define SETTINGS_TIME "/system/config/time"
#define SETTINGS_TIME_TEXT \
    "# The local time zone, for programs' local time (date, localtime,\n" \
    "# strftime): a fixed offset from UTC, +HH:MM or -HH:MM, e.g.\n" \
    "#   zone = -05:00\n" \
    "# 0 is UTC (the default). There are no daylight saving rules: change the\n" \
    "# offset when the clocks change.\n" \
    "zone = 0\n"

/* /system/config/services: written by login (the library reads it) */
#define SETTINGS_SERVICES "/system/config/services"
#define SETTINGS_SERVICES_TEXT \
    "# Service names for programs (getaddrinfo, getservbyname): name = port,\n" \
    "# the same port for TCP and UDP. Add your own; # starts a comment.\n" \
    "ftp = 21\n" \
    "ssh = 22\n" \
    "telnet = 23\n" \
    "smtp = 25\n" \
    "domain = 53\n" \
    "http = 80\n" \
    "pop3 = 110\n" \
    "ntp = 123\n" \
    "imap = 143\n" \
    "snmp = 161\n" \
    "ldap = 389\n" \
    "https = 443\n" \
    "submission = 587\n" \
    "imaps = 993\n" \
    "pop3s = 995\n"

/* /system/config/sudo: written by login (sudo reads it) */
#define SETTINGS_SUDO "/system/config/sudo"
#define SETTINGS_SUDO_TEXT \
    "# Who may run commands as root with sudo (sudo COMMAND), each with their\n" \
    "# own password (asked again after 5 minutes): user names, separated by\n" \
    "# commas, e.g.\n" \
    "#   users = anna, bob\n" \
    "# Empty: only root. Root's to change (the file is root's, 0644).\n" \
    "users =\n" \
    "#\n" \
    "# Those of them sudo never asks for a password (a program deploying\n" \
    "# without a person, an agent: whoever has their login has root); none if\n" \
    "# not set.\n" \
    "# nopassword = anna\n"

#endif
