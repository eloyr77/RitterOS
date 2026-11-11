# wifid: join and hold the wireless link (the elf; see the wifi command)

```
wifid
elf start wifid
elf stop wifid
elf restart wifid
```

`wifid` is an elf: a program `elvesd` keeps running, not one you type. It
owns the wireless card `wlan0`: it scans for a saved network, joins it, and
holds the link. Joining is authentication (open, or SAE for WPA3),
association, and for WPA2 and WPA3 the 4-way key handshake, after which it
gives the kernel the keys and the card's link comes up. The address then
comes as for any card (`dhclient`, or a fixed one in
`/system/config/network/wlan0`). While joined it answers the access
point's key renewals, roams to a stronger access point of the same network,
and joins again when the link drops. `wifid` came with step 24e; the
MediaTek MT7921 (the laptop's card) with 24f, the rest of the list (BIP,
hidden networks, roaming, wider channels, power saving) with 24g.

The command `wifi` tells it what to do: `wifi join SSID ...` saves a network
and makes the elf's file, `/system/config/elves/wifid`, when it is missing
(then starts `elvesd`):

```
command = wifid
start = on
```

Its settings, read every few seconds while it is not joined:

- `/system/config/wifi/networks/SSID`, one file per saved network (root
  only; `wifi join` and `wifi forget` write them): `ssid`, `passphrase`,
  `security` (`open`, `wpa2` or `wpa3`; `open` when missing) and
  `automatic` (`no` leaves the network out; anything else or missing:
  joined). At most 16 networks; the first one found in range is joined.
- `/system/config/wifi/radio`: `country = US` (a two-letter code; without
  it the world-wide safe default `00`), sent to the card's firmware when
  `wifid` starts, which then chooses the channels and transmit power
  allowed there (`wifi country CC` also applies it at once when the card
  takes it; else `elf restart wifid`). `powersave = on`
  lets the card doze between beacons once joined (off by default), applied
  at the next join.

A network on both bands is joined on 5 GHz when it is heard there at -75
dBm or better, else where it is strongest. A hidden network (no name in
its beacons) is asked for by name when the scan does not see it. Every 10
seconds while joined it looks at the access point's signal; below -75 dBm
it scans, and an access point of the same network and security at least 10
dB stronger is joined instead.

## Exit status and messages

`wifid` ends only when there is no wireless card: `wifid: no wireless card`
(exit code 1; `elvesd` tries again later, waiting longer each time). Its
lines appear on the console and in `/system/logs/wifid.log`:

- Joining: `wifid: SSID found on channel 6 (2.4 GHz); joining`, `wifid:
  SSID: authenticated`, `wifid: SSID: associated (AID 2)`, `wifid: SSID:
  handshake done`, `wifid: joined SSID (02:11:22:33:44:c9) on channel 6,
  wpa2`. The AID is the number the access point gave the machine.
- A step that failed (it tries again every few seconds): `wifid: SSID:
  authentication failed` (open and WPA2; a failed WPA3 exchange prints no
  line of its own), `wifid: SSID: association failed`, `wifid: SSID: the
  card did not take the association`, `wifid: SSID: handshake failed`
  after a line with the reason: `wifid: handshake: no message 1`, `message
  1 malformed`, `message 2 not sent`, `no message 3` (most often a wrong
  passphrase), `message 3 key data not readable`, `no group key in message
  3`, `message 4 not sent`; `wifid: handshake: a message that is not
  message 3 ignored`.
- `wifid: SSID needs a passphrase (wpa2): wifi join SSID PASSPHRASE`: a
  network saved open whose beacons say WPA2 or WPA3 (with ` wpa3` at the end
  for WPA3); said once.
- `wifid: SSID: link lost; joining again`: the kernel's line before it says
  why.
- `wifid: SSID: roaming to 02:11:22:33:44:d1 on channel 36 (-52 dBm; the
  old one -81 dBm)`.
- `wifid: group key renewed (index 2)`, `wifid: group key message with a
  wrong MIC ignored`, `wifid: group key message without a readable key
  ignored`.
- `wifid: pairwise key renewed`, or `wifid: the renewed key handshake
  failed; the keys stay` (a forged first message must not end the link; an
  access point that really renewed the keys and got no answer sends the
  machine away, and `wifid` joins again).

## Examples

```
ritteros:/$ wifi join ExampleNet s3cret
wifi: saved ExampleNet (wpa2); wifid will join it
elvesd: wifid started (process 8)
wifid: ExampleNet found on channel 6 (2.4 GHz); joining
wifid: ExampleNet: authenticated
wifid: ExampleNet: associated (AID 2)
wifid: ExampleNet: handshake done
wifid: joined ExampleNet (02:11:22:33:44:c9) on channel 6, wpa2
ritteros:/$ wifi country US
wifi: country US (saved; takes effect when wifid restarts)
ritteros:/$ elf restart wifid
elf: wifid: process 8 ended (elvesd starts it again)
ritteros:/$ elf stop wifid
elf: wifid off (elvesd stops it within 2 s)
```

## See also

[`wifi`](wifi.md), [`elf`](elf.md), [`net`](net.md),
[`dhclient`](dhclient.md), [`logd`](logd.md)
