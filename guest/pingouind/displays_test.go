package main

import (
	"bytes"
	"net"
	"testing"
	"time"
)

func TestPutStringPads(t *testing.T) {
	// "wl_output": 9 bytes + NUL = 10, padded to 12; length field counts the NUL.
	got := putString(nil, "wl_output")
	want := []byte{10, 0, 0, 0, 'w', 'l', '_', 'o', 'u', 't', 'p', 'u', 't', 0, 0, 0}
	if !bytes.Equal(got, want) {
		t.Fatalf("putString = %v, want %v", got, want)
	}
	// 3 bytes + NUL = 4: no padding.
	if got := putString(nil, "abc"); !bytes.Equal(got, []byte{4, 0, 0, 0, 'a', 'b', 'c', 0}) {
		t.Fatalf("putString(abc) = %v", got)
	}
}

func TestMessageHeader(t *testing.T) {
	// wl_display.get_registry(new_id 2): object 1, opcode 1, size 12.
	m := message(1, 1, putUint(nil, 2))
	want := []byte{1, 0, 0, 0, 1, 0, 12, 0, 2, 0, 0, 0}
	if !bytes.Equal(m, want) {
		t.Fatalf("message = %v, want %v", m, want)
	}
	obj, op, size, ok := header(m)
	if !ok || obj != 1 || op != 1 || size != 12 {
		t.Fatalf("header = %d %d %d %v", obj, op, size, ok)
	}
	if _, _, _, ok := header(m[:7]); ok {
		t.Fatal("a 7-byte header parsed")
	}
}

func TestArgs(t *testing.T) {
	// wl_registry.global(name 5, "wl_output", version 4)
	b := putUint(nil, 5)
	b = putString(b, "wl_output")
	b = putUint(b, 4)
	a := &args{b: b}
	if n, s, v := a.uint(), a.string(), a.uint(); n != 5 || s != "wl_output" || v != 4 || a.err != nil {
		t.Fatalf("got %d %q %d %v", n, s, v, a.err)
	}
	// negative int, null string, then short
	a = &args{b: append(putUint(nil, uint32(0xffffffff)), 0, 0, 0, 0)}
	if i, s := a.int(), a.string(); i != -1 || s != "" || a.err != nil {
		t.Fatalf("got %d %q %v", i, s, a.err)
	}
	a.uint()
	if a.err == nil {
		t.Fatal("reading past the end didn't fail")
	}
	// a string longer than what's there
	a = &args{b: []byte{20, 0, 0, 0, 'x'}}
	a.string()
	if a.err == nil {
		t.Fatal("truncated string didn't fail")
	}
}

// A fake compositor: two outputs with xdg_output, then one unplugged.
func TestWaylandClientLayout(t *testing.T) {
	srv, cli := net.Pipe()
	defer srv.Close()
	changed := make(chan struct{}, 16)
	c := &wlClient{conn: cli, next: 3, registry: 2, outputs: map[uint32]*wlOutput{},
		byXdg: map[uint32]*wlOutput{}, changed: func() { changed <- struct{}{} }}
	runErr := make(chan error, 1)
	go func() { runErr <- c.Run() }()

	// Drain the client's requests in the background (net.Pipe is unbuffered).
	go func() {
		buf := make([]byte, 4096)
		for {
			if _, err := srv.Read(buf); err != nil {
				return
			}
		}
	}()
	ev := func(obj uint32, op uint16, a []byte) {
		if _, err := srv.Write(message(obj, op, a)); err != nil {
			t.Fatal(err)
		}
	}
	global := func(name uint32, iface string, v uint32) []byte {
		b := putUint(nil, name)
		b = putString(b, iface)
		return putUint(b, v)
	}
	two := func(x, y int32) []byte { return putUint(putUint(nil, uint32(x)), uint32(y)) }

	ev(2, wlRegistryGlobal, global(10, "wl_output", 4))              // -> id 3
	ev(2, wlRegistryGlobal, global(11, "wl_output", 4))              // -> id 4
	ev(2, wlRegistryGlobal, global(12, "zxdg_output_manager_v1", 3)) // -> id 5; xdg 6 (for 3), 7 (for 4)
	time.Sleep(50 * time.Millisecond)
	c.mu.Lock()
	o3, o4 := c.outputs[3], c.outputs[4]
	c.mu.Unlock()
	if o3 == nil || o4 == nil || o3.xdg == 0 || o4.xdg == 0 {
		t.Fatalf("outputs not bound with xdg outputs: %+v %+v", o3, o4)
	}
	ev(o3.xdg, xdgOutputLogicalPosition, two(0, 0))
	ev(o3.xdg, xdgOutputLogicalSize, two(1512, 982))
	ev(o3.xdg, xdgOutputName, putString(nil, "Virtual-1"))
	ev(3, wlOutputDone, nil)
	ev(o4.xdg, xdgOutputLogicalPosition, two(1512, -100))
	ev(o4.xdg, xdgOutputLogicalSize, two(2560, 1440))
	ev(4, wlOutputName, putString(nil, "Virtual-2")) // wl_output's name only
	ev(4, wlOutputDone, nil)
	<-changed
	<-changed
	got := c.Monitors()
	want := []Monitor{{"Virtual-1", 0, 0, 1512, 982}, {"Virtual-2", 1512, -100, 2560, 1440}}
	if len(got) != 2 || got[0] != want[0] || got[1] != want[1] {
		t.Fatalf("Monitors = %+v, want %+v", got, want)
	}

	ev(2, wlRegistryGlobalRemove, putUint(nil, 11))
	<-changed
	if got := c.Monitors(); len(got) != 1 || got[0].Name != "Virtual-1" {
		t.Fatalf("after unplug: %+v", got)
	}

	srv.Close()
	if err := <-runErr; err == nil {
		t.Fatal("Run didn't end with the connection")
	}
}

func TestMonitorsLine(t *testing.T) {
	got := string(monitorsLine([]Monitor{{"Virtual-1", 0, 0, 1512, 982}}))
	want := `{"monitors":[{"name":"Virtual-1","x":0,"y":0,"width":1512,"height":982}]}` + "\n"
	if got != want {
		t.Fatalf("got %s want %s", got, want)
	}
	if got := string(monitorsLine(nil)); got != "{\"monitors\":[]}\n" {
		t.Fatalf("empty: %s", got)
	}
}

func TestParseXrandr(t *testing.T) {
	out := `Screen 0: minimum 320 x 200, current 4072 x 1440, maximum 16384 x 16384
Virtual-1 connected primary 1512x982+0+229 (normal left inverted right x axis y axis) 0mm x 0mm
   1512x982      60.00*+
Virtual-2 connected 2560x1440+1512+0 (normal left inverted right x axis y axis) 597mm x 336mm
   2560x1440     59.95*+
Virtual-3 disconnected (normal left inverted right x axis y axis)
Virtual-4 connected (normal left inverted right x axis y axis)
`
	got := parseXrandr(out)
	want := []Monitor{{"Virtual-1", 0, 229, 1512, 982}, {"Virtual-2", 1512, 0, 2560, 1440}}
	if len(got) != 2 || got[0] != want[0] || got[1] != want[1] {
		t.Fatalf("parseXrandr = %+v", got)
	}
}

func TestLayoutSourceOnlyChanges(t *testing.T) {
	s := newLayoutSource()
	s.set([]Monitor{{"Virtual-1", 0, 0, 10, 10}})
	<-s.updates
	s.set([]Monitor{{"Virtual-1", 0, 0, 10, 10}})
	select {
	case <-s.updates:
		t.Fatal("same layout signalled again")
	default:
	}
	if ms, ok := s.get(); !ok || len(ms) != 1 {
		t.Fatalf("get = %v %v", ms, ok)
	}
}
