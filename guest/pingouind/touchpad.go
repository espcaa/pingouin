package main

import "time"

// emitter is a uinput device (or a recorder in tests).
type emitter interface {
	Emit(typ, code uint16, value int32)
	Sync() error
}

// Geometry of the virtual touchpad, in units of 1/100 mm (resolution 100/mm,
// so libinput sees real sizes). The Mac's trackpad maps onto the middle of a
// larger pad; "O" from the Mac sets its real size.
const (
	padRes   = 100
	padW     = 100000
	padH     = 100000
	maxSlots = 10
	// libinput turns movement into speed by time: frames microseconds apart
	// look like jumps, so the same fingers' frames closer than this coalesce.
	minGap = 3 * time.Millisecond
)

var tools = [5]uint16{btnToolFinger, btnToolDoubletap, btnToolTripletap, btnToolQuadtap, btnToolQuinttap}

type contact struct {
	id   int
	x, y int32
}

type Touchpad struct {
	dev       emitter
	w, h      int32         // the Mac trackpad's size
	slots     map[int]int32 // mac contact id -> slot
	nextTID   int32
	tool      uint16
	lastWrite time.Time
	pending   []contact
	now       func() time.Time
}

func newTouchpad(dev emitter) *Touchpad {
	return &Touchpad{dev: dev, w: 15600, h: 9600, slots: map[int]int32{}, nextTID: 1, now: time.Now}
}

// SetSize takes the Mac trackpad's size in 1/100 mm, if plausible.
func (t *Touchpad) SetSize(w, h int) bool {
	if w > 2000 && w < 40000 && h > 2000 && h < 30000 {
		t.w, t.h = int32(w), int32(h)
		return true
	}
	return false
}

// Point maps Mac coordinates (0..1, y down) onto the pad.
func (t *Touchpad) Point(x, y float64) (int32, int32) {
	clamp := func(v float64) float64 { return min(max(v, 0), 1) }
	return (padW-t.w)/2 + int32(clamp(x)*float64(t.w)+0.5), (padH-t.h)/2 + int32(clamp(y)*float64(t.h)+0.5)
}

func (t *Touchpad) sameFingers(cs []contact) bool {
	if len(cs) != len(t.slots) {
		return false
	}
	for _, c := range cs {
		if _, ok := t.slots[c.id]; !ok {
			return false
		}
	}
	return true
}

// Frame takes one finger frame from the Mac.
func (t *Touchpad) Frame(cs []contact) error {
	if len(cs) > 0 && t.sameFingers(cs) && t.now().Sub(t.lastWrite) < minGap {
		t.pending = cs
		return nil
	}
	t.pending = nil
	// libinput rejects a lift and a new touch in one frame: lifts go first.
	lifted, added := false, false
	ids := map[int]bool{}
	for _, c := range cs {
		ids[c.id] = true
		if _, ok := t.slots[c.id]; !ok {
			added = true
		}
	}
	for id := range t.slots {
		if !ids[id] {
			lifted = true
		}
	}
	if len(cs) > 0 && lifted && added {
		var kept []contact
		for _, c := range cs {
			if _, ok := t.slots[c.id]; ok {
				kept = append(kept, c)
			}
		}
		if err := t.write(kept); err != nil {
			return err
		}
	}
	return t.write(cs)
}

// Flush writes a coalesced frame once its gap has passed. Pending reports
// whether one still waits.
func (t *Touchpad) Flush() error {
	if t.pending != nil && t.now().Sub(t.lastWrite) >= minGap {
		cs := t.pending
		t.pending = nil
		return t.write(cs)
	}
	return nil
}

func (t *Touchpad) Pending() bool { return t.pending != nil }

// Release lifts every finger.
func (t *Touchpad) Release() error {
	t.pending = nil
	if len(t.slots) > 0 || t.tool != 0 {
		return t.write(nil)
	}
	return nil
}

func (t *Touchpad) write(cs []contact) error {
	d := t.dev
	ids := map[int]bool{}
	for _, c := range cs {
		ids[c.id] = true
	}
	for id, slot := range t.slots {
		if !ids[id] {
			d.Emit(evAbs, absMtSlot, slot)
			d.Emit(evAbs, absMtTrackingID, -1)
			delete(t.slots, id)
		}
	}
	for _, c := range cs {
		slot, ok := t.slots[c.id]
		if !ok {
			if slot = t.freeSlot(); slot < 0 {
				continue
			}
			t.slots[c.id] = slot
			d.Emit(evAbs, absMtSlot, slot)
			d.Emit(evAbs, absMtTrackingID, t.nextTID)
			t.nextTID = t.nextTID%65535 + 1
		}
		d.Emit(evAbs, absMtSlot, slot)
		d.Emit(evAbs, absMtPositionX, c.x)
		d.Emit(evAbs, absMtPositionY, c.y)
	}
	var tool uint16
	if n := len(t.slots); n > 0 {
		tool = tools[min(n, 5)-1]
	}
	if tool != t.tool {
		if t.tool != 0 {
			d.Emit(evKey, t.tool, 0)
		}
		if tool != 0 {
			d.Emit(evKey, tool, 1)
		}
		if (t.tool == 0) != (tool == 0) {
			d.Emit(evKey, btnTouch, b2i(tool != 0))
		}
		t.tool = tool
	}
	if len(cs) > 0 {
		d.Emit(evAbs, absX, cs[0].x)
		d.Emit(evAbs, absY, cs[0].y)
	}
	t.lastWrite = t.now()
	return d.Sync()
}

func (t *Touchpad) freeSlot() int32 {
	used := map[int32]bool{}
	for _, s := range t.slots {
		used[s] = true
	}
	for s := int32(0); s < maxSlots; s++ {
		if !used[s] {
			return s
		}
	}
	return -1
}

func b2i(b bool) int32 {
	if b {
		return 1
	}
	return 0
}

// Keys is the virtual keyboard for the Mac's Cmd shortcuts (sent as Super).
type Keys struct {
	dev  emitter
	down map[uint16]bool
}

func newKeys(dev emitter) *Keys { return &Keys{dev: dev, down: map[uint16]bool{}} }

func (k *Keys) Key(code int, value int) error {
	if code <= 0 || code >= 249 {
		return nil
	}
	c := uint16(code)
	k.dev.Emit(evKey, c, int32(value))
	if value != 0 {
		k.down[c] = true
	} else {
		delete(k.down, c)
	}
	return k.dev.Sync()
}

func (k *Keys) Release() error {
	for c := range k.down {
		if err := k.Key(int(c), 0); err != nil {
			return err
		}
	}
	return nil
}
