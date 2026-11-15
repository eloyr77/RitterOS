# openssl: OpenSSL's tool: TLS connections, keys, certificates, digests and ciphers

```
openssl COMMAND [OPTION...]
```

The command line tool of OpenSSL 3.5 (the long-term support series),
built from OpenSSL's own sources with RitterOS's system libraries
`libssl.so.3` and `libcrypto.so.3` (since step 46b). Its commands are
OpenSSL's, documented by OpenSSL (https://docs.openssl.org/3.5/man1/):
`openssl help` lists them, `openssl COMMAND -help` gives a command's
options. The ones most used:

- `openssl s_client -connect HOST:PORT [-servername NAME] [-alpn h2]
  [-brief]`: a TLS connection to a server (TLS 1.2 and 1.3), the
  certificate checked against the system's roots; what is typed is sent.
  `-verify_return_error` ends the connection when the check fails,
  `-verify_hostname NAME` checks the name, `-CAfile FILE` trusts other
  roots, `-sess_out FILE` / `-sess_in FILE` save and resume a session,
  `-status` asks for the server's OCSP answer.
- `openssl s_server -accept PORT -cert FILE -key FILE`: a TLS server.
- `openssl genpkey`, `openssl req`, `openssl x509`, `openssl verify`: keys,
  certificate requests, certificates and their checks.
- `openssl dgst -sha256 FILE`, `openssl enc`, `openssl rand -hex 16`:
  digests, ciphers, random bytes.
- `openssl version -a`, `openssl info -cpusettings`: the version, the
  build, the processor features its assembly uses.

The default roots are `/system/config/openssl/cert.pem`, a link to
`/system/certs/cacert.pem` (Mozilla's list, the same `fetch` and `libtls`
use). OpenSSL's folder `/system/config/openssl` needs no `openssl.cnf`;
one put there is read. There are no loadable modules: no legacy provider
(MD4, RC4, Blowfish and the other old algorithms it holds are not offered)
and no engines.

Licence: the Apache License 2.0, `/system/doc/licences/openssl.txt`.
