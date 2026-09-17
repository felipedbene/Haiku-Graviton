// M2 test (a): a real HTTPS client — TLS handshake + GET to an external
// endpoint. Validates crypto/tls (the pure-Go TLS that replaces the mbedTLS
// hack) and the poll(2) netpoller on a single connection.
//
// Usage: httpsget [url ...]   (defaults to a small, stable, valid-cert list)
// Prints the negotiated TLS version + cipher suite + HTTP status, and whether
// the Haiku system CA bundle was found. Exit 0 only if a verified 2xx GET
// completes over a real TLS handshake.
package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net/http"
	"os"
	"time"
)

var caPaths = []string{
	"/system/data/ssl/CARootCertificates.pem",
	"/system/data/ssl/certs",
}

func tlsVersionName(v uint16) string {
	switch v {
	case tls.VersionTLS13:
		return "TLS1.3"
	case tls.VersionTLS12:
		return "TLS1.2"
	case tls.VersionTLS11:
		return "TLS1.1"
	case tls.VersionTLS10:
		return "TLS1.0"
	}
	return fmt.Sprintf("0x%04x", v)
}

func main() {
	urls := os.Args[1:]
	if len(urls) == 0 {
		urls = []string{
			"https://checkip.amazonaws.com",
			"https://sources.debene.dev/go-arm64/go-1.26.1-haiku-arm64-bootstrap.tbz",
			"https://www.amazon.com/",
		}
	}

	// Report CA discovery — a missing bundle is the classic first TLS failure
	// on a fresh GOOS.
	for _, p := range caPaths {
		if fi, err := os.Stat(p); err == nil {
			fmt.Printf("ca-found: %s (%d bytes, dir=%v)\n", p, fi.Size(), fi.IsDir())
		} else {
			fmt.Printf("ca-missing: %s (%v)\n", p, err)
		}
	}

	client := &http.Client{Timeout: 20 * time.Second}

	ok := false
	for _, u := range urls {
		t0 := time.Now()
		req, _ := http.NewRequest("GET", u, nil)
		// Use HEAD-like small read but keep GET semantics; some endpoints
		// reject HEAD.
		resp, err := client.Do(req)
		if err != nil {
			fmt.Printf("GET %s -> ERROR after %v: %v\n", u, time.Since(t0), err)
			continue
		}
		var tlsInfo string
		if resp.TLS != nil {
			tlsInfo = fmt.Sprintf("%s cipher=0x%04x alpn=%q peerCerts=%d",
				tlsVersionName(resp.TLS.Version), resp.TLS.CipherSuite,
				resp.TLS.NegotiatedProtocol, len(resp.TLS.PeerCertificates))
		} else {
			tlsInfo = "NO-TLS"
		}
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 256))
		resp.Body.Close()
		fmt.Printf("GET %s -> %d in %v [%s] body[:64]=%q\n",
			u, resp.StatusCode, time.Since(t0), tlsInfo, trunc(body, 64))
		if resp.StatusCode >= 200 && resp.StatusCode < 300 && resp.TLS != nil {
			ok = true
			break
		}
	}

	if ok {
		fmt.Println("PASS: verified TLS handshake + 2xx GET completed")
		os.Exit(0)
	}
	fmt.Println("FAIL: no verified 2xx HTTPS GET completed")
	os.Exit(1)
}

func trunc(b []byte, n int) []byte {
	if len(b) > n {
		return b[:n]
	}
	return b
}
