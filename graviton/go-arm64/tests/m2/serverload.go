// M2 test (c): an in-process http.Server + many concurrent client goroutines
// hammering it. This is the most direct stress of the poll(2) netpoll
// readiness machinery: the listener accept fd, and both ends of every
// connection, are armed and woken through netpoll — accept/read/write
// readiness, all in one process, with no external network dependency (so it is
// deterministic and cannot be blamed on an external server).
//
// Usage: serverload [goroutines] [reqsPerGoroutine]
// Defaults: 100 goroutines, 20 reqs each = 2000 requests. The handler echoes a
// per-request checksum the client verifies, so correctness (not just liveness)
// is checked. Exit 0 only if every request returns the correct body with no
// error/hang. A hang is the key netpoller finding.
package main

import (
	"crypto/sha256"
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

func expected(id string) string {
	h := sha256.Sum256([]byte("m2-" + id))
	return hex.EncodeToString(h[:])
}

func main() {
	nG := 100
	if len(os.Args) > 1 {
		nG, _ = strconv.Atoi(os.Args[1])
	}
	nR := 20
	if len(os.Args) > 2 {
		nR, _ = strconv.Atoi(os.Args[2])
	}
	total := nG * nR

	mux := http.NewServeMux()
	mux.HandleFunc("/echo", func(w http.ResponseWriter, r *http.Request) {
		id := r.URL.Query().Get("id")
		// Force a read of the request body too, to exercise read-readiness.
		io.Copy(io.Discard, r.Body)
		fmt.Fprint(w, expected(id))
	})

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Printf("FAIL: listen: %v\n", err)
		os.Exit(1)
	}
	addr := ln.Addr().String()
	srv := &http.Server{Handler: mux}
	go srv.Serve(ln)
	fmt.Printf("server listening on %s\n", addr)

	tr := &http.Transport{
		MaxIdleConns:        nG * 2,
		MaxIdleConnsPerHost: nG * 2,
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
			fmt.Printf("FAIL: watchdog fired after %v — %d/%d done (ok=%d fail=%d). Likely netpoll hang.\n",
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
		fmt.Println("PASS: all in-process client/server requests completed with correct bodies, no hang")
		os.Exit(0)
	}
	fmt.Println("FAIL: not all requests succeeded")
	os.Exit(1)
}
