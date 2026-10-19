// Regression test for DeBeOS issue 843 (haiku/arm64): a SIGURG async
// preemption that lands while libroot C code has repurposed x28 (Go's g
// register) must not livelock the signal handler.
//
// Goroutines that lock their OS thread and return make the runtime retire
// that thread, so new ones keep being created through pthread_create, which
// runs libroot's malloc. Meanwhile call-free spin loops and allocation keep
// the GC asking sysmon to preempt running goroutines with SIGURG. Before the
// fix one thread ends up spinning in runtime.sigtrampgo within seconds and
// the heartbeat stops. Prints PASS after the given number of seconds.
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

var spins, threads atomic.Uint64

//go:noinline
func spin(n int) int {
	x := 0
	for i := 0; i < n; i++ {
		x += i ^ (x >> 3)
	}
	return x
}

func main() {
	dur := 60
	if len(os.Args) > 1 {
		dur, _ = strconv.Atoi(os.Args[1])
	}
	for i := 0; i < 4; i++ {
		go func() {
			for {
				spin(20_000_000)
				spins.Add(1)
			}
		}()
	}
	go func() {
		var keep [][]byte
		for {
			keep = append(keep, make([]byte, 32<<10))
			if len(keep) > 256 {
				keep = keep[:0]
			}
		}
	}()
	go func() {
		for {
			var wg sync.WaitGroup
			for i := 0; i < 8; i++ {
				wg.Add(1)
				go func() {
					defer wg.Done()
					// Exiting while locked retires this M.
					runtime.LockOSThread()
					spin(100_000)
				}()
			}
			wg.Wait()
			threads.Add(8)
		}
	}()
	var lastS, lastT uint64
	stuck := 0
	for sec := 1; sec <= dur; sec++ {
		time.Sleep(time.Second)
		s, t := spins.Load(), threads.Load()
		if s == lastS || t == lastT {
			stuck++
			fmt.Printf("t=%d no progress (spins=%d threads=%d)\n", sec, s, t)
		}
		lastS, lastT = s, t
		if stuck >= 5 {
			fmt.Println("FAIL: stalled")
			os.Exit(1)
		}
	}
	fmt.Printf("PASS: %ds, spins=%d threads=%d\n", dur, lastS, lastT)
}
