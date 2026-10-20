// Regression test for DeBeOS issue 843 (haiku/arm64): a SIGURG async
// preemption that lands while libroot C code has repurposed x28 (Go's g
// register) must not livelock the signal handler.
//
// Goroutines that lock their OS thread and return make the runtime retire
// that thread, so new ones keep being created through pthread_create, which
// runs libroot's malloc. Meanwhile call-free spin loops and allocation keep
// the GC asking sysmon to preempt running goroutines with SIGURG. Before the
// fix one thread ends up spinning in runtime.sigtrampgo within seconds and
// the heartbeat stops.
//
// Usage: preemptlibroot [duration]. The duration is plain seconds ("60") or
// a Go duration ("30s", "2m"), at least one second; the default is 60 s. A
// malformed or non-positive duration exits 2 rather than running nothing
// and printing PASS. Prints PASS once the duration has elapsed.
//
// The livelock this hunts wedges stop-the-world and libroot's malloc lock,
// so the in-process stall check below may never get scheduled to report
// it. A watchdog therefore runs as a separate process (this binary,
// re-executed with preemptlibrootWatchdogEnv set) whose runtime is not under
// the stress load. The test writes it a heartbeat byte every second; after
// watchdogLimit without one, the watchdog prints FAIL and SIGKILLs the test
// (a shell reports 137). Any exit without a PASS line is a failure.
package main

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	preemptlibrootWatchdogEnv = "PREEMPTLIBROOT_WATCHDOG"
	// Well past the five-second in-process stall window, so a runtime
	// that can still schedule main reports its own stall first.
	watchdogLimit = 20 * time.Second
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

// parseDuration accepts whole seconds or a Go duration and returns the
// run length in whole seconds, rounded up.
func parseDuration(arg string) (int, error) {
	if n, err := strconv.Atoi(arg); err == nil {
		if n < 1 {
			return 0, fmt.Errorf("duration %q must be at least 1s", arg)
		}
		return n, nil
	}
	d, err := time.ParseDuration(arg)
	if err != nil {
		return 0, fmt.Errorf("bad duration %q: want seconds or a Go duration like 30s", arg)
	}
	if d < time.Second {
		return 0, fmt.Errorf("duration %q must be at least 1s", arg)
	}
	return int((d + time.Second - 1) / time.Second), nil
}

func usage(msg string) {
	fmt.Fprintf(os.Stderr, "preemptlibroot: %s\nusage: preemptlibroot [duration]\n", msg)
	fmt.Println("FAIL: bad arguments")
	os.Exit(2)
}

// startWatchdog re-executes this binary as the watchdog and returns the
// write end of its heartbeat pipe.
func startWatchdog() (*os.File, error) {
	exe, err := os.Executable()
	if err != nil {
		return nil, err
	}
	r, w, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	cmd := exec.Command(exe)
	cmd.Env = append(os.Environ(), preemptlibrootWatchdogEnv+"=1")
	cmd.Stdin, cmd.Stdout, cmd.Stderr = r, os.Stdout, os.Stderr
	if err := cmd.Start(); err != nil {
		r.Close()
		w.Close()
		return nil, err
	}
	r.Close()
	// Reap the watchdog if it exits first, so it does not linger as a
	// zombie; the FAIL it prints goes straight to the shared stdout.
	go cmd.Wait()
	return w, nil
}

// watchdog runs in the child. EOF on stdin means the test exited on its
// own (PASS or FAIL), so the watchdog exits quietly.
func watchdog() {
	parent := os.Getppid()
	var last atomic.Int64
	last.Store(time.Now().UnixNano())
	done := make(chan struct{})
	go func() {
		buf := make([]byte, 64)
		for {
			if _, err := os.Stdin.Read(buf); err != nil {
				if err != io.EOF {
					fmt.Fprintf(os.Stderr, "watchdog: read: %v\n", err)
				}
				close(done)
				return
			}
			last.Store(time.Now().UnixNano())
		}
	}()
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case <-done:
			os.Exit(0)
		case <-tick.C:
		}
		quiet := time.Since(time.Unix(0, last.Load()))
		if quiet < watchdogLimit {
			continue
		}
		fmt.Printf("FAIL: watchdog: no heartbeat from pid %d for %v, runtime wedged; killing it\n",
			parent, quiet.Round(time.Second))
		syscall.Kill(parent, syscall.SIGKILL)
		os.Exit(1)
	}
}

func main() {
	if os.Getenv(preemptlibrootWatchdogEnv) != "" {
		watchdog()
		return
	}
	dur := 60
	if len(os.Args) > 2 {
		usage("too many arguments")
	}
	if len(os.Args) > 1 {
		var err error
		if dur, err = parseDuration(os.Args[1]); err != nil {
			usage(err.Error())
		}
	}
	heartbeat, err := startWatchdog()
	if err != nil {
		fmt.Printf("FAIL: cannot start watchdog: %v\n", err)
		os.Exit(1)
	}
	beat := func() {
		if _, err := heartbeat.Write([]byte{0}); err != nil {
			fmt.Printf("FAIL: watchdog gone: %v\n", err)
			os.Exit(1)
		}
	}
	beat()
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
		beat()
		s, t := spins.Load(), threads.Load()
		// A stall is five consecutive seconds without progress. Scattered
		// slow seconds on a small, busy instance are not a livelock, so
		// any second in which both counters move resets the count.
		if s == lastS || t == lastT {
			stuck++
			fmt.Printf("t=%d no progress (spins=%d threads=%d)\n", sec, s, t)
		} else {
			stuck = 0
		}
		lastS, lastT = s, t
		if stuck >= 5 {
			fmt.Printf("FAIL: stalled for %d consecutive seconds\n", stuck)
			os.Exit(1)
		}
	}
	fmt.Printf("PASS: %ds, spins=%d threads=%d\n", dur, lastS, lastT)
}
