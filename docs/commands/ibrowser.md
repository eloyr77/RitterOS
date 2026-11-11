# ibrowser: iBrowser: show web pages (http, https, HTML files) in a window

```
ibrowser [-t] [URL]
```

**iBrowser** is RitterOS's web browser, a windowed app (since step 27a).
Run it under the window system (`windowd &` first), from a terminal or
from `owl`, which finds it in `/system/<arch>/apps`. URL is an `http://` or
`https://` address (`http://` is assumed for `host/path`; an IPv6 address
in brackets, `http://[2001:db8::5]/`, since 64d, and names with both
kinds of address IPv6 first on a network with IPv6), or an HTML file on
the disk (a path such as `/home/anna/page.html`); without one it shows
its own start page (`about:home`). The window is 900 by 640 pixels, smaller
on a small screen, and may be resized.

It shows text with headings, lists, links, tables and forms, PNG, JPEG and
SVG pictures, styled by the page's CSS (selectors, the box model, floats,
positioning, flexible boxes, grids, `@media`, custom properties and much
more), in the Adobe faces of `/system/fonts` (Times, Helvetica, Courier).
Pages run their JavaScript on RitterOS's own engine (the one `js` uses)
with the document's objects, events, timers, Promises, `fetch` and
`XMLHttpRequest`. Cookies and `localStorage` are kept while iBrowser runs,
never on the disk. Addresses are fetched as `fetch` fetches them:
redirections followed (up to 5), `https://` checked against the system's
root certificates, answers packed with gzip or deflate unpacked. The
manual's section "The web browser: iBrowser" lists everything it shows and
what it does not do yet.

Pages load in the background, with their style sheets and script files:
the page shown stays usable meanwhile and is replaced when the new one is
there; pictures are put in as they arrive. A server that does not answer is
given up after 30 seconds, for the connection and for each wait for data.

The window:

- **At the top:** `<` goes back to the page before; `x` (while a page
  loads) stops the load; the address line shows the page's address. It is
  a text field like every app's (select, edit, copy and paste in it):
  click it or press **Ctrl-L** (all of it selected, so typing replaces it),
  **Enter** goes there.
- **At the bottom:** where the link under the pointer leads, or the page's
  title, or why a page could not be shown.
- **Links** are blue and underlined; a click follows one when the button is
  let go without the pointer moving (a drag selects instead).
- **File menu:** **Open File...** (a page from the disk, with the Open
  dialog), **Reload**, **Back**, **Quit**.
- **Forms:** a click or **Tab** gives a control the keyboard; text fields,
  passwords and text areas take typing; a click or **Space** ticks a check
  box or radio button; a list opens on a click (**Up**/**Down** change it);
  a submit button or **Enter** in a text field sends the form (GET or
  POST), a reset button puts it back.

| Key | Does (when the address line and no form control has the keyboard) |
|---|---|
| Up, Down | scroll a line |
| Page Up, Page Down, Space | scroll a window's height (Space: down) |
| Home, End | the top, the end of the page |
| Backspace | back to the page before |
| Ctrl-L | to the address line, all selected |
| Ctrl-C | copy the selected text (drag across the page; a double click selects a word) |
| Tab | the next form control |
| Escape | stop the page that is loading |
| `q` | quit |

The wheel and the scroll bar at the right scroll too. A page's script may
take a key or a click for itself (`preventDefault`), as in other browsers.

## Options

| Option | Meaning |
|---|---|
| `-t` | also print each page's laid-out text, links, form controls and script errors on the standard output (for tests) |

## Exit status and messages

On its standard output, always:

- `ibrowser: loaded URL (STATUS, TYPE, N bytes)` for each page shown
  (`..., N bytes, gzip from M)` for a packed one, M the bytes that came);
  `ibrowser: URL: REASON` for one that cannot be had (the window shows the
  reason too: `HOST: the server did not answer`, `the page's gzip data is
  damaged`, ...).
- `ibrowser: following URL` when a link is followed.
- `ibrowser: console: TEXT` for the page's `console.log` (and the other
  console methods), `ibrowser: console: alert: TEXT` for `alert`.

With `-t`, also: `ibrowser| ...` lines with the page's text as it is laid
out, `ibrowser: page URL: N lines, N links, N pixels high`, `ibrowser: link
N at X,Y: URL`, `ibrowser: control N at X,Y WxH: [text: q=VALUE]` (and
`[password: pw=N characters]`, `[checkbox: NAME=VALUE on|off]`, `[select:
NAME=OPTION]`, `[submit: LABEL]`, ...), `ibrowser: now [...]` for a
control's change, `ibrowser: sending GET URL` and `ibrowser: sending POST
URL (N bytes): DATA` for a form sent, `ibrowser: scrolled to y N for #part`, `ibrowser: #part
is not on the page`, `ibrowser: scrolled by hand to y N`, `ibrowser:
copied N bytes`, `ibrowser: stopped`, and `ibrowser: script error: NAME:
MESSAGE at URL:LINE` for an error that ends a script, a listener or a
timer (`ibrowser: script error: URL: REASON` for a script file that could
not be had; without `-t` nothing is shown, as in other browsers). The page's report comes once its pictures are in.

- `0`: the window was closed (`q`, **File > Quit** or the close box).
- `1`: `ibrowser: cannot open a window: is windowd running?`, or `...: the
  screen is another user's`.
- `2`: an unknown option: `usage: ibrowser [-t] [URL]`.

## Examples

```
ritteros:/$ windowd &
ritteros:/$ ibrowser https://example.com &
ibrowser: loaded https://example.com/ (200, text/html, 1256 bytes)
ritteros:/$ ibrowser /home/root/notes.html &
ritteros:/$ ibrowser -t http://10.0.2.2:8000/form.html
```

## See also

[`fetch`](fetch.md), [`js`](js.md), [`windowd`](windowd.md),
[`owl`](owl.md), [`term`](term.md), [`imageview`](imageview.md)
