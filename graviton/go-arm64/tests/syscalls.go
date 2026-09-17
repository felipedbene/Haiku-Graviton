package main

import (
	"fmt"
	"os"
	"time"
)

func main() {
	start := time.Now()
	time.Sleep(200 * time.Millisecond)
	elapsed := time.Since(start)
	pid := os.Getpid()
	// direct write(2) to stdout via os.Stdout.Write
	os.Stdout.Write([]byte("syscall-write-ok\n"))
	fmt.Printf("pid=%d slept=%dms\n", pid, elapsed.Milliseconds())
}
