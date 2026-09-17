package main

import (
	"fmt"
	"sync"
)

func main() {
	const n = 8
	ch := make(chan int, n)
	var wg sync.WaitGroup
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			ch <- id * id
		}(i)
	}
	go func() { wg.Wait(); close(ch) }()
	sum := 0
	for v := range ch {
		sum += v
	}
	// sum of squares 0..7 = 140
	fmt.Println("goroutine-sum", sum)
}
