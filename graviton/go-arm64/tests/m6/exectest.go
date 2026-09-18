package main

import (
	"fmt"
	"os"
	"os/exec"
)

func main() {
	fmt.Println("exectest: start pid=", os.Getpid())
	out, err := exec.Command("/bin/true").CombinedOutput()
	fmt.Printf("exectest: /bin/true done err=%v out=%q\n", err, string(out))
	out, err = exec.Command("/bin/echo", "hello-from-child").CombinedOutput()
	fmt.Printf("exectest: /bin/echo done err=%v out=%q\n", err, string(out))
	if err != nil {
		os.Exit(1)
	}
	fmt.Println("exectest: OK")
}
