#!/bin/sh
# testcerts.sh DIR: the TLS test's certificates (step 23d), made with the
# Mac's /usr/bin/openssl (LibreSSL) into DIR: a root (P-384, root.crt, the
# one the guest trusts), an intermediate (RSA 2048, int.crt) signed by it,
# server certificates signed by the intermediate for test.local and
# *.example.test (leaf: P-256; leafrsa: RSA 2048; expired: P-256, valid only
# in January 2020), and a second root (other.crt), which signed only
# stranger (P-256, the same names).
set -e
O=/usr/bin/openssl
D=$1
mkdir -p "$D"
cd "$D"
printf '[v3]\nbasicConstraints=critical,CA:true\nkeyUsage=critical,keyCertSign,cRLSign\n' > ca.cnf
printf '[v3]\nbasicConstraints=CA:false\nsubjectAltName=DNS:test.local,DNS:*.example.test\nextendedKeyUsage=serverAuth\n' > leaf.cnf
root() {        # root NAME CN
    $O ecparam -name secp384r1 -genkey -noout -out "$1.key"
    $O req -new -key "$1.key" -subj "/CN=$2" -out "$1.csr"
    $O x509 -req -in "$1.csr" -signkey "$1.key" -days 3650 -sha384 \
        -extfile ca.cnf -extensions v3 -out "$1.crt" 2>/dev/null
}
signed() {      # signed NAME BY DAYS CNF HASH
    $O req -new -key "$1.key" -subj "/CN=$1" -out "$1.csr"
    $O x509 -req -in "$1.csr" -CA "$2.crt" -CAkey "$2.key" -CAcreateserial -days "$3" \
        -"$5" -extfile "$4" -extensions v3 -out "$1.crt" 2>/dev/null
}
root root "RitterOS Test Root"
root other "Another Test Root"
$O genrsa -out int.key 2048 2>/dev/null
signed int root 3650 ca.cnf sha384
$O ecparam -name prime256v1 -genkey -noout -out leaf.key
signed leaf int 365 leaf.cnf sha256
$O genrsa -out leafrsa.key 2048 2>/dev/null
signed leafrsa int 365 leaf.cnf sha256
# The expired one through openssl ca, which takes explicit dates.
$O ecparam -name prime256v1 -genkey -noout -out expired.key
$O req -new -key expired.key -subj /CN=expired -out expired.csr
: > index.txt
echo 01 > serial
cat > expired.cnf <<CNF
[ca]
default_ca = test
[test]
database = index.txt
new_certs_dir = .
serial = serial
default_md = sha256
policy = any
[any]
commonName = supplied
[v3]
basicConstraints=CA:false
subjectAltName=DNS:test.local,DNS:*.example.test
extendedKeyUsage=serverAuth
CNF
$O ca -config expired.cnf -batch -notext -cert int.crt -keyfile int.key -in expired.csr \
    -startdate 20200101000000Z -enddate 20200201000000Z -extensions v3 -out expired.crt 2>/dev/null
# A server certificate from the second root (no root the guest trusts).
$O ecparam -name prime256v1 -genkey -noout -out stranger.key
signed stranger other 365 leaf.cnf sha256
