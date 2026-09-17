// M2 netpoller/HTTP/TLS stress harness for haiku/arm64, combined into a single
// binary so it can be pushed to a bare Graviton Haiku guest (base64 + shell
// only) in one transfer. Dispatches on argv[1]:
//
//	m2 httpsget   [url ...]                 test (a): real HTTPS TLS GET
//	m2 concurrent [url] [nG] [nR]           test (b): N goroutines, many conns
//	m2 serverload [nG] [nR]                 test (c): in-proc server + clients
//	m2 all                                  run a, b, c in sequence
//
// Each subtest prints a PASS/FAIL line and, in "all" mode, the process exits 0
// only if every subtest passed.
package main

import (
	"crypto/sha256"
	"crypto/tls"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
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

func trunc(b []byte, n int) []byte {
	if len(b) > n {
		return b[:n]
	}
	return b
}

// ---- test (a): HTTPS GET -------------------------------------------------

func httpsget(args []string) bool {
	urls := args
	if len(urls) == 0 {
		urls = []string{
			"https://checkip.amazonaws.com",
			"https://sources.debene.dev/go-arm64/go-1.26.1-haiku-arm64-bootstrap.tbz",
			"https://www.amazon.com/",
		}
	}
	for _, p := range caPaths {
		if fi, err := os.Stat(p); err == nil {
			fmt.Printf("ca-found: %s (%d bytes, dir=%v)\n", p, fi.Size(), fi.IsDir())
		} else {
			fmt.Printf("ca-missing: %s (%v)\n", p, err)
		}
	}
	client := &http.Client{Timeout: 20 * time.Second}
	for _, u := range urls {
		t0 := time.Now()
		resp, err := client.Get(u)
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
			fmt.Println("PASS(httpsget): verified TLS handshake + 2xx GET completed")
			return true
		}
	}
	fmt.Println("FAIL(httpsget): no verified 2xx HTTPS GET completed")
	return false
}

// ---- test (b): concurrent external HTTPS client --------------------------

func concurrent(args []string) bool {
	url := "https://checkip.amazonaws.com"
	if len(args) > 0 {
		url = args[0]
	}
	nG := 50
	if len(args) > 1 {
		nG, _ = strconv.Atoi(args[1])
	}
	nR := 4
	if len(args) > 2 {
		nR, _ = strconv.Atoi(args[2])
	}
	total := nG * nR
	tr := &http.Transport{
		MaxIdleConns:        nG * 2,
		MaxIdleConnsPerHost: nG * 2,
		TLSClientConfig:     &tls.Config{MinVersion: tls.VersionTLS12},
	}
	client := &http.Client{Transport: tr, Timeout: 30 * time.Second}

	var ok, fail int64
	var firstErr atomic.Value
	var wg sync.WaitGroup
	done := make(chan struct{})
	deadline := 90 * time.Second
	go func() {
		select {
		case <-done:
		case <-time.After(deadline):
			fmt.Printf("FAIL(concurrent): watchdog fired after %v — %d/%d done (ok=%d fail=%d). Likely netpoll hang.\n",
				deadline, atomic.LoadInt64(&ok)+atomic.LoadInt64(&fail), total,
				atomic.LoadInt64(&ok), atomic.LoadInt64(&fail))
			os.Exit(2)
		}
	}()
	start := make(chan struct{})
	t0 := time.Now()
	for g := 0; g < nG; g++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			<-start
			for r := 0; r < nR; r++ {
				resp, err := client.Get(url)
				if err != nil {
					atomic.AddInt64(&fail, 1)
					firstErr.CompareAndSwap(nil, err.Error())
					continue
				}
				io.Copy(io.Discard, io.LimitReader(resp.Body, 1<<16))
				resp.Body.Close()
				if resp.StatusCode >= 200 && resp.StatusCode < 300 {
					atomic.AddInt64(&ok, 1)
				} else {
					atomic.AddInt64(&fail, 1)
					firstErr.CompareAndSwap(nil, fmt.Sprintf("status %d", resp.StatusCode))
				}
			}
		}(g)
	}
	close(start)
	wg.Wait()
	close(done)
	elapsed := time.Since(t0)
	fmt.Printf("concurrent: url=%s goroutines=%d reqs/goroutine=%d total=%d\n", url, nG, nR, total)
	fmt.Printf("result: ok=%d fail=%d elapsed=%v (%.0f req/s)\n",
		ok, fail, elapsed, float64(total)/elapsed.Seconds())
	if e := firstErr.Load(); e != nil {
		fmt.Printf("first-error: %v\n", e)
	}
	if ok == int64(total) {
		fmt.Println("PASS(concurrent): all concurrent HTTPS requests completed 2xx, no hang")
		return true
	}
	fmt.Println("FAIL(concurrent): not all concurrent requests succeeded")
	return false
}

// ---- test (c): in-process server + concurrent clients --------------------

func expected(id string) string {
	h := sha256.Sum256([]byte("m2-" + id))
	return hex.EncodeToString(h[:])
}

func serverload(args []string) bool {
	nG := 100
	if len(args) > 0 {
		nG, _ = strconv.Atoi(args[0])
	}
	nR := 20
	if len(args) > 1 {
		nR, _ = strconv.Atoi(args[1])
	}
	total := nG * nR
	mux := http.NewServeMux()
	mux.HandleFunc("/echo", func(w http.ResponseWriter, r *http.Request) {
		id := r.URL.Query().Get("id")
		io.Copy(io.Discard, r.Body)
		fmt.Fprint(w, expected(id))
	})
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Printf("FAIL(serverload): listen: %v\n", err)
		return false
	}
	addr := ln.Addr().String()
	srv := &http.Server{Handler: mux}
	go srv.Serve(ln)
	fmt.Printf("server listening on %s\n", addr)

	tr := &http.Transport{MaxIdleConns: nG * 2, MaxIdleConnsPerHost: nG * 2}
	client := &http.Client{Transport: tr, Timeout: 30 * time.Second}

	var ok, fail int64
	var firstErr atomic.Value
	var wg sync.WaitGroup
	done := make(chan struct{})
	deadline := 90 * time.Second
	go func() {
		select {
		case <-done:
		case <-time.After(deadline):
			fmt.Printf("FAIL(serverload): watchdog fired after %v — %d/%d done (ok=%d fail=%d). Likely netpoll hang.\n",
				deadline, atomic.LoadInt64(&ok)+atomic.LoadInt64(&fail), total,
				atomic.LoadInt64(&ok), atomic.LoadInt64(&fail))
			os.Exit(2)
		}
	}()
	start := make(chan struct{})
	t0 := time.Now()
	for g := 0; g < nG; g++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			<-start
			for r := 0; r < nR; r++ {
				reqID := fmt.Sprintf("%d-%d", id, r)
				resp, err := client.Get("http://" + addr + "/echo?id=" + reqID)
				if err != nil {
					atomic.AddInt64(&fail, 1)
					firstErr.CompareAndSwap(nil, err.Error())
					continue
				}
				body, _ := io.ReadAll(resp.Body)
				resp.Body.Close()
				if resp.StatusCode == 200 && string(body) == expected(reqID) {
					atomic.AddInt64(&ok, 1)
				} else {
					atomic.AddInt64(&fail, 1)
					firstErr.CompareAndSwap(nil, fmt.Sprintf("bad resp id=%s status=%d bodylen=%d", reqID, resp.StatusCode, len(body)))
				}
			}
		}(g)
	}
	close(start)
	wg.Wait()
	close(done)
	elapsed := time.Since(t0)
	fmt.Printf("serverload: goroutines=%d reqs/goroutine=%d total=%d\n", nG, nR, total)
	fmt.Printf("result: ok=%d fail=%d elapsed=%v (%.0f req/s)\n",
		ok, fail, elapsed, float64(total)/elapsed.Seconds())
	if e := firstErr.Load(); e != nil {
		fmt.Printf("first-error: %v\n", e)
	}
	if ok == int64(total) {
		fmt.Println("PASS(serverload): all in-process client/server requests completed with correct bodies, no hang")
		return true
	}
	fmt.Println("FAIL(serverload): not all requests succeeded")
	return false
}

func main() {
	if len(os.Args) < 2 {
		fmt.Println("usage: m2 {httpsget|concurrent|serverload|all} [args...]")
		os.Exit(2)
	}
	cmd := os.Args[1]
	rest := os.Args[2:]
	var pass bool
	switch cmd {
	case "httpsget":
		pass = httpsget(rest)
	case "concurrent":
		pass = concurrent(rest)
	case "serverload":
		pass = serverload(rest)
	case "all":
		fmt.Println("===== (a) httpsget =====")
		a := httpsget(nil)
		fmt.Println("===== (b) concurrent =====")
		b := concurrent(nil)
		fmt.Println("===== (c) serverload =====")
		c := serverload(nil)
		fmt.Printf("===== SUMMARY: httpsget=%v concurrent=%v serverload=%v =====\n", a, b, c)
		pass = a && b && c
	default:
		fmt.Printf("unknown subcommand %q\n", cmd)
		os.Exit(2)
	}
	if pass {
		os.Exit(0)
	}
	os.Exit(1)
}
