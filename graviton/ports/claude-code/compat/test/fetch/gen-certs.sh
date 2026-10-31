#!/bin/bash
# gen-certs.sh <dir>: throwaway PKI for the fetch diff test, valid for 2 days.
#   ca.pem      test CA; signs srv.pem (SAN localhost + 127.0.0.1) and srvn.pem (SAN localhost only)
#   other.pem   an unrelated CA (the "wrong CA" cases)
#   cca.pem     client CA; signs cli.pem (clientAuth), key cli.key, and cli-enc.key (passphrase s3cret)
set -euo pipefail
d=$1; mkdir -p "$d"; cd "$d"
q() { "$@" > /dev/null 2>&1; }
q openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj "/CN=cc-fetch test CA"
q openssl req -x509 -newkey rsa:2048 -nodes -keyout other.key -out other.pem -days 2 -subj "/CN=cc-fetch other CA"
q openssl req -x509 -newkey rsa:2048 -nodes -keyout cca.key -out cca.pem -days 2 -subj "/CN=cc-fetch client CA"
leaf() {	# leaf <name> <ca> <cn> <ext>
	q openssl req -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=$3"
	printf '%s\n' "$4" > "$1.ext"
	q openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" -CAcreateserial -out "$1.pem" -days 2 -extfile "$1.ext"
}
leaf srv ca localhost "subjectAltName=DNS:localhost,IP:127.0.0.1"
leaf srvn ca localhost "subjectAltName=DNS:localhost"
leaf cli cca cc-fetch-client "extendedKeyUsage=clientAuth"
q openssl pkey -in cli.key -aes256 -passout pass:s3cret -out cli-enc.key
ls
