# curl: transfer data from or to a URL: HTTP/1.1 and HTTP/2, HTTPS, WebSockets, proxies

```
curl [OPTION...] URL...
```

The command line tool of curl 8.23 (since step 46d-2), on its library
`libcurl.so.4`, which ported programs (the WebKit port first) use too. Its
options are curl's, documented by curl (https://curl.se/docs/manpage.html;
`curl --help all` lists them). What it was built with: TLS 1.2 and 1.3 by
OpenSSL (`libssl.so.3`), HTTP/2 by nghttp2 (chosen by ALPN on https://, or
`--http2-prior-knowledge`), gzip and brotli answers (`--compressed`),
cookies (`-b`, `-c`; the Public Suffix List of libpsl refuses a cookie for
a suffix such as `co.uk`), proxies (`-x`, CONNECT tunnels for https://),
WebSockets (`ws://`, `wss://`, through libcurl), IPv6 (`-g` for an address
in brackets), names resolved by the threaded resolver through `dnsd`. No
LDAP, SSH (SCP, SFTP), HTTP/3, international domain names or zstd.

Some uses:

- `curl https://example.com/`: the page to the output; `-o FILE` into a
  file, `-O` named as on the server, `-L` follows redirections.
- `curl -sS -v https://example.com/ -o /dev/null`: the connection (TLS
  version, the certificate's check, ALPN, the headers) on the error output.
- `curl --compressed URL`, `curl -I URL` (the headers only), `curl -d
  DATA URL` (a POST).

The certificate is checked against `/system/certs/cacert.pem` (Mozilla's
list, the same as `fetch` and OpenSSL use); `--cacert FILE` trusts other
roots, `-k` none at all. Exit codes are curl's: 0 done, 6 the name not
found, 7 no connection, 28 out of time, 60 the certificate refused.

Licence: curl's (MIT-like), `/system/doc/licences/curl.txt`.
