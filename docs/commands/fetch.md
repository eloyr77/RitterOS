# fetch: get a web page or file over HTTP or HTTPS (-i: with the headers)

```
fetch [-i] [-o FILE] URL
```

Gets the page or file at URL and writes it to the output, byte for byte, or
with `-o` into FILE (then it says on the error output how much it saved).
URL is `http://HOST[:PORT][/PATH]` or `https://HOST[:PORT][/PATH]`; one
without a `://` is taken as `http://` (`fetch example.com/`). HOST is a
name (asked of the elf `dnsd`) or an address, an IPv6 one in brackets
(`http://[2001:db8::5]:8080/`, since 64d); the port is 80 for http and
443 for https unless given. A name with addresses of both kinds is tried
IPv6 first on a network where the machine has an IPv6 address beyond the
link (RFC 6724), else IPv4 first; if one address does not answer, the
next is tried.

`fetch` sends one HTTP/1.1 `GET` (with `Connection: close` and the
`User-Agent` `RitterOS-fetch/1`) and reads the answer: as long as its
`Content-Length` says, in chunks (`Transfer-Encoding: chunked`), or until
the server closes the connection. Redirections (status 301, 302, 303, 307
or 308 with a `Location`) are followed, up to 5, each one reported, also
from http:// to https:// and back. Any other status outside 200-299 is an
error, and nothing is written.

**HTTPS** (since step 23e) is HTTP over TLS 1.3 (`libtls.so.1`): the
connection is encrypted, and the server must prove it is HOST. Its
certificate has to name HOST (a `*.` name covers one label), be within its
dates (the machine's clock must be right), and chain to one of the root
certificates in `/system/certs/cacert.pem`, Mozilla's list of trusted
certificate authorities as browsers use it. Servers that speak only TLS 1.2
or older are refused. Whether a page came whole is HTTP's to say (its
length or its last chunk), so a server that closes without TLS's
`close_notify` is accepted, as other HTTP clients accept it.

## Options

| Option | Meaning |
|---|---|
| `-i` | write the answer's status line and headers first, then an empty line, then the body (for every answer, redirections too) |
| `-o FILE` | write the body into FILE (replaced, or made with mode 0644) instead of the output |

The options may come before or after URL.

## Exit status and messages

- `0`: the whole body was written. With `-o`: `fetch: 200 OK, 1256 bytes
  saved to FILE`.
- `1`: it failed; the message names the URL, the host or the file:
  - `fetch: URL: not an http:// or https:// address`, `no host name in it`,
    `not a port number`, `the path is too long`.
  - `fetch: HOST: Name not found`, `Connection refused`, `Timed out` (from
    the network); `not an HTTP answer`.
  - `fetch: URL: 404 Not Found` (or whatever other status the server gave).
  - `fetch: URL: too many redirections` (more than 5).
  - `fetch: URL: the connection ended early (N of M bytes)`.
  - `fetch: FILE: Permission denied` (FILE could not be made or written).
  - From HTTPS, after the host: `the certificate is not for this host name`,
    `a certificate is expired or not yet valid`, `no trusted root
    certificate signed the chain`, `the server does not speak TLS 1.3`, `the
    server refused the handshake` (it found nothing in common with us), `a
    record failed its check (bad_record_mac)` (the data was changed on the
    way).
- `2`: no URL, two of them, `-o` without FILE or an unknown option: `usage:
  fetch [-i] [-o FILE] URL`.

Each redirection followed is reported on the error output: `fetch: 301
Moved Permanently: redirected to https://example.com/`.

## Examples

```
ritteros:/$ fetch https://example.com/
<!doctype html>
...
ritteros:/$ fetch -o /page.html https://example.com/
fetch: 200 OK, 1256 bytes saved to /page.html
ritteros:/$ fetch -i http://10.0.2.2:8080/hello.txt
HTTP/1.0 200 OK
Server: SimpleHTTP/0.6 Python/3.9.6
Content-Length: 6

hello
ritteros:/$ fetch https://example.com/nope
fetch: https://example.com/nope: 404 Not Found
fetch: exit code 1
```

## See also

[`net`](net.md), [`ping`](ping.md), [`dnsd`](dnsd.md),
[`ibrowser`](ibrowser.md), [`ssh`](ssh.md), [`guard`](guard.md)
