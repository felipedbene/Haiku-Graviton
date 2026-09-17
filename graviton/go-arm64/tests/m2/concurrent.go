// M2 test (b): a concurrent HTTPS client — N goroutines doing many
// simultaneous connections to an external endpoint. Exercises the poll(2)
// netpoll readiness path under load (the epoll/kqueue-less path): many fds
// armed for read/write at once, many goroutines parked on netpoll and woken.
//
// Usage: concurrent [url] [goroutines] [reqsPerGoroutine]
// Defaults: https://checkip.amazonaws.com, 50 goroutines, 4 reqs each = 200.
// Exit 0 only if every request returns 2xx with no error/hang within the
// deadline. A hang here (goroutines never waking) is the key netpoller finding.
package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	url := "https://checkip.amazonaws.com"
	if len(os.Args) > 1 {
		url = os.Args[1]
	}
	nG := 50
	if len(os.Args) > 2 {
		nG, _ = strconv.Atoi(os.Args[2])
	}
	nR := 4
	if len(os.Args) > 3 {
		nR, _ = strconv.Atoi(os.Args[3])
	}
	total := nG * nR

	// A transport that actually opens many concurrent conns rather than
	// serializing on one keep-alive socket — this is what stresses netpoll.
	tr := &http.Transport{
		MaxIdleConns:        nG * 2,
		MaxIdleConnsPerHost: nG * 2,
		MaxConnsPerHost:     0,
		TLSClientConfig:     &tls.Config{MinVersion: tls.VersionTLS12},
		DisableKeepAlives:   false,
	}
	client := &http.Client{Transport: tr, Timeout: 30 * time.Second}

	var ok, fail int64
	var firstErr atomic.Value
	var wg sync.WaitGroup

	// Global watchdog: if the whole run does not finish in time, the netpoller
	// likely wedged. Print a diagnostic and exit non-zero rather than hang.
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

	// Barrier so all goroutines fire as simultaneously as possible.
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
		fmt.Println("PASS: all concurrent HTTPS requests completed 2xx, no hang")
		os.Exit(0)
	}
	fmt.Println("FAIL: not all concurrent requests succeeded")
	os.Exit(1)
}
