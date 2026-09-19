package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sort"
	"strings"

	"example.com/greet/mathx"
)

func main() {
	nums := []int{5, 3, 8, 1, 9, 2}
	sort.Ints(nums)
	sum := mathx.Sum(nums...)
	h := sha256.Sum256([]byte(fmt.Sprint(nums)))
	rec := map[string]any{"sorted": nums, "sum": sum, "sha": hex.EncodeToString(h[:8])}
	b, _ := json.Marshal(rec)
	fmt.Println(strings.ToUpper("greetmod ok:"), string(b))
}
