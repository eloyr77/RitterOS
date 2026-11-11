# wifi: the wireless link (saves networks; wifid joins them)

```
wifi scan | join SSID [PASSPHRASE [open|wpa2|wpa3]] | forget SSID | country CC | status
wifi scan
wifi join SSID [PASSPHRASE [open|wpa2|wpa3]]
wifi forget SSID
wifi country CC
wifi status
```

A wireless card is `wlan0`, an ordinary network card once it has joined a
network: `net up`, DHCP, DNS, routing and the firewall treat it like any
other. What is special -- finding networks, the passphrase, the keys -- is
the elf `wifid`'s, and `wifi` tells it what to do: it saves networks,
which `wifid` then joins (and rejoins after a reboot or a dropped link),
and it shows what the card hears and how the link is. `wifi` always works
on `wlan0`. It came in step 24e (the MediaTek MT7921 driver in 24f; the
Intel Dual Band Wireless-AC 8265's, `intelwifi`, since roadmap item 36:
scans since 36c-2, joins since 36d, at full speed since 36e and 36f).

## Commands

| Command | Does |
|---|---|
| `scan` | lists the networks in range, strongest first: name, security (`open`, `wpa2` or `wpa3`), channel and, when the card reports it, the signal in dBm (-40 is very strong, -90 barely there); a network heard on several channels or from several access points is listed once, at its strongest; hidden networks (no name in their beacons) are not listed. Not while joined |
| `join SSID [PASSPHRASE [SEC]]` | saves the network in `/system/config/wifi/networks/SSID` (`ssid`, `security`, `automatic = yes`, `passphrase`), makes `wifid`'s elf file `/system/config/elves/wifid` if it is missing, and starts `elvesd`, which runs `wifid`. Without PASSPHRASE the network is open -- unless it is saved already: then it keeps its saved settings (security, passphrase) and `wifid` joins it with them (since 36f-6; before, it was saved again as open and its passphrase lost). With a PASSPHRASE it is `wpa2` unless SEC says `wpa3` (or `open`), and saving an SSID again so replaces it. To make a saved network open, `forget` it first |
| `forget SSID` | removes the saved network (`wifid` no longer joins it) |
| `country CC` | sets the country, a two-letter code such as `US` (small letters are made capitals), in `/system/config/wifi/radio`; the card's firmware chooses the allowed channels and transmit power by it. Applied at once when the card is up, else when `wifid` starts |
| `status` | whether the link is up: the access point's address, the channel, its signal (when known), and the card's IP address |

The settings live under `/system/config/wifi/`, which only root may change:
`join`, `forget` and `country` are for root. A card that scans in its own
firmware (the MT7921, the Intel 8265) sweeps the channels itself, about 5
seconds (at most 10); for others `wifi` tunes each channel and listens 200 ms
there.
`country` changes only the radio file's `country` line (or adds one): other
lines there, such as `powersave = on`, stay (since 50k). The Intel 8265's firmware
chooses the country itself from the networks it hears (Intel's LAR, as it
does under Linux): on that card `country` is saved but not applied.

## Exit status and messages

- `0`: done. `scan` ends with `wifi: N network(s)`; while joined it scans
  nothing and says `wifi: joined; not scanning (wifi status shows the
  link)`. `join` says `wifi: saved SSID (SEC); wifid will join it`,
  `forget` `wifi: forgot SSID`, `country` `wifi: country US (applied)` or
  `wifi: country US (saved; takes effect when wifid restarts)`, `status`
  `wifi: not joined` or `wifi: joined BSSID on channel N, -48 dBm` and
  `wifi: address A.B.C.D`.
- `1`: `wifi: no wireless card` (`scan`, `status`); a file could not be
  written or removed: `wifi: /system/config/wifi/networks: Permission
  denied`, `wifi: SSID: No such file or directory` (`forget` of a network
  not saved), `wifi: /system/config/wifi/radio: ...`.
- `2`: a wrong command line: `usage: wifi scan | join SSID ... | forget
  SSID | country CC | status`, `wifi: unknown command NAME`, `usage: wifi
  join SSID [PASSPHRASE [wpa2|wpa3]]`, `wifi: security must be open, wpa2
  or wpa3`, `usage: wifi forget SSID`, `usage: wifi country CC (a
  two-letter code, e.g. US)`.

What happens next is `wifid`'s, on the console and in its log: `wifid:
ExampleNet found on channel 6 (2.4 GHz); joining`, `wifid: joined
ExampleNet (02:11:22:33:44:c9) on channel 6, wpa2`, or the step that
failed (`authentication failed`, `handshake failed`; `wifid: handshake: no
message 3` is most often a wrong passphrase). See [`wifid`](wifid.md).

## Examples

```
ritteros:/$ wifi scan
wifi: ExampleNet                       wpa2 channel 6   -39 dBm
wifi: Amazon                           wpa2 channel 149 -57 dBm
wifi: xfinitywifi                      open channel 157 -66 dBm
wifi: 3 network(s)
ritteros:/$ wifi country US
wifi: country US (applied)
ritteros:/$ wifi join ExampleNet s3cret
wifi: saved ExampleNet (wpa2); wifid will join it
wifid: ExampleNet found on channel 6 (2.4 GHz); joining
wifid: ExampleNet: authenticated
wifid: ExampleNet: associated (AID 2)
wifid: ExampleNet: handshake done
wifid: joined ExampleNet (02:11:22:33:44:c9) on channel 6, wpa2
ritteros:/$ wifi status
wifi: joined 02:11:22:33:44:c9 on channel 6, -41 dBm
wifi: address 192.168.1.20
ritteros:/$ wifi join HomeNet s3cret wpa3
wifi: saved HomeNet (wpa3); wifid will join it
ritteros:/$ wifi forget HomeNet
wifi: forgot HomeNet
```

## See also

[`wifid`](wifid.md), [`net`](net.md), [`dhclient`](dhclient.md),
[`elf`](elf.md), [`ping`](ping.md)
