// SPDX-License-Identifier: GPL-2.0-only
// Stock Linux/amd64 Go target for n_native_pager. Only stdin/stdout carry
// the test protocol. The buffer is on the Go heap and GC runs concurrently.
package main

import (
	"encoding/binary"
	"io"
	"os"
	"runtime"
	"sync/atomic"
	"syscall"
	"unsafe"
)

func answer(v uint64) {
	var b [8]byte
	binary.LittleEndian.PutUint64(b[:], v)
	if _, err := os.Stdout.Write(b[:]); err != nil {
		panic(err)
	}
}

func main() {
	runtime.GOMAXPROCS(2)
	runtime.LockOSThread()
	size := os.Getpagesize()
	heap := make([]byte, 3*size)
	var pin runtime.Pinner
	pin.Pin(&heap[0])
	defer pin.Unpin()
	offset := int(-uintptr(unsafe.Pointer(&heap[0])) & uintptr(size-1))
	page := heap[offset : offset+size]
	for i := range page {
		page[i] = 0x2a
	}
	var cycles atomic.Uint64
	stop, done := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(done)
		for {
			select {
			case <-stop:
				return
			default:
				runtime.GC()
				cycles.Add(1)
			}
		}
	}()
	answer(uint64(uintptr(unsafe.Pointer(&page[0]))))
	answer(uint64(syscall.Gettid()))
	for {
		var b [8]byte
		if _, err := io.ReadFull(os.Stdin, b[:]); err != nil {
			panic(err)
		}
		switch binary.LittleEndian.Uint64(b[:]) {
		case 0:
			close(stop)
			<-done
			runtime.KeepAlive(heap)
			return
		case 1:
			answer(uint64(page[0]))
		case 2:
			page[0]++
			answer(uint64(page[0]))
		case 3:
			if _, err := io.ReadFull(os.Stdin, page[:1]); err != nil {
				panic(err)
			}
			answer(uint64(page[0]))
		case 4:
			answer(cycles.Load())
		default:
			panic("unknown command")
		}
	}
}
