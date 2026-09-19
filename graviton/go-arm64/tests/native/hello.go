package main

import (
	"fmt"
	"runtime"
)

func main() {
	fmt.Printf("hello from native go build on %s/%s\n", runtime.GOOS, runtime.GOARCH)
}
