# imageview: show an image (PPM, BMP, PNG, JPEG) in a window

```
imageview [FILE]
```

Shows a picture in a window of the window system: PPM, BMP, PNG or JPEG
(the libraries' decoders, `libimage.so.1`). It is a windowed app (it lives
in `/system/<arch>/apps`), so the window server must be running first
(`windowd &`, or the window system started at login). `owl` opens pictures
(`.ppm`, `.bmp`, `.png`, `.jpg`, `.jpeg`) in it.

With FILE the window opens at the picture's size, made smaller if the
picture does not fit the screen (40 pixels kept free on each side), with the
file's name as its title. The picture is scaled to fit the window with its
proportions kept, centred, the rest of the window dark grey, so resizing the
window rescales it. Photos are turned the way their camera recorded (EXIF
orientation: a portrait photo, which cameras store lying on its side, shows
upright). PNG files of every kind are read (not animated ones); JPEG files
grey or colour, baseline or progressive, as cameras and web pages make them
(not the rare arithmetic-coded, 12-bit, lossless or CMYK ones).

Without FILE it opens an empty window of 480 x 320 pixels that says `File >
Open... to show an image`. **File > Open...** brings up the shared **Open**
dialog (starting in the folder of the last picture, or the current
directory); the window then takes the new picture's size. A file it cannot
read there is reported and the old picture stays. `q`, **File > Close** or
the window's close box ends it.

`imageview` came with the media suite (PNG since step 25a, JPEG since 25b,
progressive JPEG 25c, EXIF orientation 25d). It takes no options.

## Exit status and messages

Its messages go to the output (the terminal it was started from):

- `imageview: showing FILE (WIDTHxHEIGHT)`: a picture was loaded.
- `imageview: started, no image`: started without FILE.
- `0`: the window was closed.
- `1`: it could not start:
  - `imageview: FILE: No such file or directory`, `imageview: FILE:
    Invalid argument` (not a picture it can read).
  - `imageview: cannot open a window (is windowd running?)`
  - `imageview: cannot open a window: the screen is another user's`: the
    window server running is another user's.
- `2`: more than one FILE: `usage: imageview [FILE]`.

## Examples

```
ritteros:/$ windowd &
ritteros:/$ imageview /home/root/photo.jpg &
imageview: showing /home/root/photo.jpg (4032x3024)
ritteros:/$ imageview /readme.txt
imageview: /readme.txt: Invalid argument
imageview: exit code 1
ritteros:/$ imageview
imageview: cannot open a window (is windowd running?)
imageview: exit code 1
```

## See also

[`windowd`](windowd.md), [`owl`](owl.md), [`reel`](reel.md),
[`tune`](tune.md), [`term`](term.md), [`ibrowser`](ibrowser.md)
