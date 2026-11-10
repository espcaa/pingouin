package main

import (
	"bufio"
	"net"
	"strings"
	"testing"
	"time"
)

type ev struct {
	typ, code uint16
	val       int32
}

type recorder struct {
	frames [][]ev
	cur    []ev
}

func (r *recorder) Emit(typ, code uint16, v int32) { r.cur = append(r.cur, ev{typ, code, v}) }
func (r *recorder) Sync() error                    { r.frames = append(r.frames, r.cur); r.cur = nil; return nil }

func has(frame []ev, e ev) bool {
	for _, x := range frame {
		if x == e {
			return true
		}
	}
	return false
}

func newTestPad() (*Touchpad, *recorder, *time.Time) {
	rec := &recorder{}
	pad := newTouchpad(rec)
	now := time.Unix(1000, 0)
	pad.now = func() time.Time { return now }
	return pad, rec, &now
}

func TestThreeFingerToolAndLift(t *testing.T) {
	pad, rec, _ := newTestPad()
	pad.Frame([]contact{{1, 10, 10}, {2, 20, 20}, {3, 30, 30}})
	f := rec.frames[0]
	if !has(f, ev{evKey, btnToolTripletap, 1}) || !has(f, ev{evKey, btnTouch, 1}) {
		t.Fatalf("3 fingers down: want TRIPLETAP + BTN_TOUCH, got %v", f)
	}
	pad.Release()
	f = rec.frames[len(rec.frames)-1]
	if !has(f, ev{evKey, btnToolTripletap, 0}) || !has(f, ev{evKey, btnTouch, 0}) {
		t.Fatalf("lift: want TRIPLETAP up + BTN_TOUCH up, got %v", f)
	}
	if n := countTracking(f, -1); n != 3 {
		t.Fatalf("lift: %d slots ended, want 3", n)
	}
}

func countTracking(f []ev, v int32) int {
	n := 0
	for _, e := range f {
		if e.typ == evAbs && e.code == absMtTrackingID && e.val == v {
			n++
		}
	}
	return n
}

// A finger lifting and another landing in one Mac frame become two frames.
func TestLiftBeforeNewTouch(t *testing.T) {
	pad, rec, now := newTestPad()
	pad.Frame([]contact{{1, 10, 10}, {2, 20, 20}})
	*now = now.Add(10 * time.Millisecond)
	pad.Frame([]contact{{2, 21, 21}, {3, 30, 30}})
	if len(rec.frames) != 3 {
		t.Fatalf("want 3 frames (down, lift, new touch), got %d", len(rec.frames))
	}
	if countTracking(rec.frames[1], -1) != 1 {
		t.Fatalf("second frame should only lift finger 1: %v", rec.frames[1])
	}
}

func TestCoalesceCloseFrames(t *testing.T) {
	pad, rec, now := newTestPad()
	pad.Frame([]contact{{1, 10, 10}, {2, 20, 20}, {3, 30, 30}})
	*now = now.Add(time.Millisecond)
	pad.Frame([]contact{{1, 11, 11}, {2, 21, 21}, {3, 31, 31}})
	if len(rec.frames) != 1 || !pad.Pending() {
		t.Fatal("a frame 1 ms later should wait")
	}
	pad.Flush()
	if len(rec.frames) != 1 {
		t.Fatal("flushed before the gap")
	}
	*now = now.Add(3 * time.Millisecond)
	pad.Flush()
	if len(rec.frames) != 2 || pad.Pending() {
		t.Fatal("pending frame not written after the gap")
	}
}

func TestPointMapsIntoMiddle(t *testing.T) {
	pad, _, _ := newTestPad()
	pad.SetSize(12400, 7600) // 124 x 76 mm, the user's MacBook
	x, y := pad.Point(0.5, 0.5)
	if x != padW/2 || y != padH/2 {
		t.Fatalf("centre maps to %d,%d", x, y)
	}
	x, _ = pad.Point(-1, 0)
	if x != (padW-12400)/2 {
		t.Fatalf("x not clamped: %d", x)
	}
}

func TestHandleLines(t *testing.T) {
	pad, rec, _ := newTestPad()
	keys := newKeys(rec)
	logf := func(string, ...any) {}
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	handle(strings.Fields("F 3 1 0.1 0.1 5 2 0.2 0.2 5 3 0.3 0.3 5"), a, pad, keys, logf)
	if len(pad.slots) != 3 {
		t.Fatalf("F: %d fingers down", len(pad.slots))
	}
	handle(strings.Fields("K 125 1"), a, pad, keys, logf) // KEY_LEFTMETA
	handle(strings.Fields("S off"), a, pad, keys, logf)
	if len(pad.slots) != 0 || len(keys.down) != 0 {
		t.Fatal("S off should release fingers and keys")
	}
	handle(strings.Fields("F 2 1 x 0.1"), a, pad, keys, logf) // malformed: skipped, no panic
	go handle([]string{"Q"}, a, pad, keys, logf)
	r := bufio.NewReader(b)
	v, _ := r.ReadString('\n')
	line, _ := r.ReadString('\n')
	if v != "V "+version+"\n" || line != "D -\n" {
		t.Fatalf("Q answered %q %q", v, line)
	}
}
