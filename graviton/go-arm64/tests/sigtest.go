package main

import "fmt"

//go:noinline
func deref(p *int) int { return *p } // nil deref -> SIGSEGV

func main() {
	defer func() {
		if r := recover(); r != nil {
			fmt.Println("recovered-from-signal:", r)
			return
		}
		fmt.Println("NO-PANIC-BUG")
	}()
	var p *int
	fmt.Println("before-deref")
	_ = deref(p)
	fmt.Println("NOT-REACHED")
}
