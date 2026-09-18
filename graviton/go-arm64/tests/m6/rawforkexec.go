package main

import (
	"fmt"
	"os"
	"syscall"
)

func main() {
	fmt.Println("rawforkexec: start")
	pid, err := syscall.ForkExec("/bin/true", []string{"/bin/true"}, &syscall.ProcAttr{
		Files: []uintptr{0, 1, 2},
	})
	fmt.Printf("rawforkexec: ForkExec pid=%d err=%v\n", pid, err)
	if err != nil {
		os.Exit(1)
	}
	var ws syscall.WaitStatus
	wpid, werr := syscall.Wait4(pid, &ws, 0, nil)
	fmt.Printf("rawforkexec: Wait4 wpid=%d ws=%v werr=%v\n", wpid, ws, werr)
	fmt.Println("rawforkexec: OK")
}
