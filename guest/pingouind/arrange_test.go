package main

import (
	"reflect"
	"testing"

	"github.com/godbus/dbus/v5"
)

const twoScreens = `{"layout":[{"output":"Virtual-1","x":0,"y":0,"width":1512,"height":982,"scale":2},` +
	`{"output":"Virtual-2","x":-1920,"y":-98,"width":1920,"height":1080,"scale":1}],` +
	`"builtin":"Virtual-1","external":true,"fullscreen":true}`

func TestParseLayout(t *testing.T) {
	l, ok := parseLayout([]byte(twoScreens))
	if !ok || len(l.Layout) != 2 || l.Builtin != "Virtual-1" || l.Layout[1].Scale != 1 {
		t.Fatalf("parseLayout = %+v %v", l, ok)
	}
	if _, ok := parseLayout([]byte(`{"monitors":[]}`)); ok {
		t.Fatal("a non-layout line parsed")
	}
	if _, ok := parseLayout([]byte(`{"layout":[]}`)); ok {
		t.Fatal("an empty layout parsed")
	}
	l, _ = parseLayout([]byte(`{"layout":[{"output":"Virtual-1","width":10,"height":10}]}`))
	if l.Layout[0].Scale != 1 {
		t.Fatal("a missing scale isn't 1")
	}
}

func TestPlacementsNormalised(t *testing.T) {
	l, _ := parseLayout([]byte(twoScreens))
	ps := placements(l)
	want := []Placement{
		{Name: "Virtual-1", X: 1920, Y: 98, Width: 1512, Height: 982, Scale: 2, Primary: true},
		{Name: "Virtual-2", X: 0, Y: 0, Width: 1920, Height: 1080, Scale: 1},
	}
	if !reflect.DeepEqual(ps, want) {
		t.Fatalf("placements = %+v", ps)
	}
	// no builtin: Virtual-1 is primary
	l.Builtin = "Virtual-9"
	if ps := placements(l); !ps[0].Primary || ps[1].Primary {
		t.Fatalf("primary without builtin: %+v", ps)
	}
}

func TestPhysicalPositions(t *testing.T) {
	// Virtual-1 (2x, 1512 points wide) at 0,0; Virtual-2 (1x) right of it.
	ps := []Placement{
		{Name: "Virtual-1", X: 0, Y: 0, Width: 1512, Height: 982, Scale: 2},
		{Name: "Virtual-2", X: 1512, Y: 0, Width: 1920, Height: 1080, Scale: 1},
	}
	got := physicalPositions(ps)
	if got["Virtual-1"] != [2]int{0, 0} || got["Virtual-2"] != [2]int{3024, 0} {
		t.Fatalf("physicalPositions = %v", got)
	}
}

func TestNearestScale(t *testing.T) {
	if s := nearestScale([]float64{1, 1.25, 1.5, 1.75, 2, 2.25}, 2); s != 2 {
		t.Fatalf("nearest to 2 = %v", s)
	}
	if s := nearestScale([]float64{1, 1.3333333}, 2); s != 1.3333333 {
		t.Fatalf("nearest to 2 = %v", s)
	}
	if s := nearestScale(nil, 2); s != 2 {
		t.Fatal("no supported scales")
	}
}

func mode(id string, w, h int32, current, preferred bool, scales ...float64) mutterMode {
	p := map[string]dbus.Variant{}
	if current {
		p["is-current"] = dbus.MakeVariant(true)
	}
	if preferred {
		p["is-preferred"] = dbus.MakeVariant(true)
	}
	return mutterMode{ID: id, Width: w, Height: h, Refresh: 60, PreferredScale: 1, SupportedScales: scales, Props: p}
}

// GNOME after the second screen appeared: Virtual-1 back at 1.333 (the
// default), Virtual-2 to its right at 1.
func fakeGnome() mutterState {
	return mutterState{
		Serial: 7,
		Monitors: []mutterMonitor{
			{Spec: mutterSpec{"Virtual-1", "RHT", "QEMU Monitor", "0"}, Modes: []mutterMode{
				mode("3024x1964@60", 3024, 1964, true, true, 1, 1.333, 2),
				mode("1920x1080@60", 1920, 1080, false, false, 1),
			}},
			{Spec: mutterSpec{"Virtual-2", "RHT", "QEMU Monitor", "0"}, Modes: []mutterMode{
				mode("1920x1080@60", 1920, 1080, true, true, 1, 1.25),
			}},
		},
		Logical: []mutterLogical{
			{X: 0, Y: 0, Scale: 1.333, Primary: true, Monitors: []mutterSpec{{"Virtual-1", "RHT", "QEMU Monitor", "0"}}},
			{X: 2268, Y: 0, Scale: 1, Monitors: []mutterSpec{{"Virtual-2", "RHT", "QEMU Monitor", "0"}}},
		},
		Props: map[string]dbus.Variant{"layout-mode": dbus.MakeVariant(uint32(1))},
	}
}

func TestPickMode(t *testing.T) {
	ms := fakeGnome().Monitors[0].Modes
	if m, _ := pickMode(ms, 3024, 1964); m.ID != "3024x1964@60" {
		t.Fatalf("current of the wanted size: %s", m.ID)
	}
	// wanted size not current: the preferred one
	if m, _ := pickMode(ms, 1920, 1080); m.ID != "3024x1964@60" {
		t.Fatalf("preferred first: %s", m.ID)
	}
	// no preferred: the one of the wanted size
	noPref := []mutterMode{mode("a", 800, 600, true, false), mode("b", 1920, 1080, false, false)}
	if m, _ := pickMode(noPref, 1920, 1080); m.ID != "b" {
		t.Fatalf("matching: %s", m.ID)
	}
	if _, ok := pickMode(nil, 1, 1); ok {
		t.Fatal("a mode from nothing")
	}
}

func TestPlanGnome(t *testing.T) {
	s := fakeGnome()
	l, _ := parseLayout([]byte(twoScreens))
	plan, err := planGnome(s, l)
	if err != nil {
		t.Fatal(err)
	}
	want := []applyLogical{
		{X: 1920, Y: 98, Scale: 2, Primary: true,
			Monitors: []applyMonitor{{Connector: "Virtual-1", ModeID: "3024x1964@60", Props: map[string]dbus.Variant{}}}},
		{X: 0, Y: 0, Scale: 1,
			Monitors: []applyMonitor{{Connector: "Virtual-2", ModeID: "1920x1080@60", Props: map[string]dbus.Variant{}}}},
	}
	if !reflect.DeepEqual(plan, want) {
		t.Fatalf("plan = %+v", plan)
	}
	if !gnomeDiffers(s, plan) {
		t.Fatal("GNOME's arrangement isn't the Mac's, but no difference seen")
	}
	// GNOME after applying it: no difference, so no loop
	s.Logical = []mutterLogical{
		{X: 1920, Y: 98, Scale: 2, Primary: true, Monitors: []mutterSpec{{Connector: "Virtual-1"}}},
		{X: 0, Y: 0, Scale: 1, Monitors: []mutterSpec{{Connector: "Virtual-2"}}},
	}
	if gnomeDiffers(s, plan) {
		t.Fatal("the applied arrangement still differs")
	}
	// a screen of the layout not connected in the guest yet: left out
	s.Monitors = s.Monitors[:1]
	plan, err = planGnome(s, l)
	if err != nil || len(plan) != 1 || plan[0].Monitors[0].Connector != "Virtual-1" {
		t.Fatalf("plan with one connected = %+v %v", plan, err)
	}
	// none connected: an error, never an empty config (it would turn everything off)
	s.Monitors = nil
	if _, err := planGnome(s, l); err == nil {
		t.Fatal("a plan with no screens")
	}
}

func TestPlanGnomePhysical(t *testing.T) {
	s := fakeGnome()
	s.Props["layout-mode"] = dbus.MakeVariant(uint32(2))
	l, _ := parseLayout([]byte(`{"layout":[{"output":"Virtual-1","x":0,"y":0,"width":1512,"height":982,"scale":2},` +
		`{"output":"Virtual-2","x":1512,"y":0,"width":1920,"height":1080,"scale":1}]}`))
	plan, err := planGnome(s, l)
	if err != nil || plan[1].X != 3024 {
		t.Fatalf("physical plan = %+v %v", plan, err)
	}
}

func TestKscreen(t *testing.T) {
	outs, err := parseKscreen([]byte(`{"outputs":[` +
		`{"name":"Virtual-1","connected":true,"enabled":true,"pos":{"x":0,"y":0},"scale":1.5,"priority":1},` +
		`{"name":"Virtual-2","connected":true,"enabled":true,"pos":{"x":2016,"y":0},"scale":1,"priority":2},` +
		`{"name":"Virtual-3","connected":false,"enabled":false,"pos":{"x":0,"y":0},"scale":1}]}`))
	if err != nil || len(outs) != 3 {
		t.Fatalf("parseKscreen = %v %v", outs, err)
	}
	l, _ := parseLayout([]byte(twoScreens))
	if !kdeDiffers(outs, l) {
		t.Fatal("KDE's arrangement isn't the Mac's, but no difference seen")
	}
	args := kscreenArgs(outs, l, false)
	want := []string{
		"output.Virtual-1.enable", "output.Virtual-1.position.1920,98", "output.Virtual-1.scale.2", "output.Virtual-1.priority.1",
		"output.Virtual-2.enable", "output.Virtual-2.position.0,0", "output.Virtual-2.scale.1",
	}
	if !reflect.DeepEqual(args, want) {
		t.Fatalf("kscreenArgs = %q", args)
	}
	if a := kscreenArgs(outs, l, true); a[3] != "output.Virtual-1.primary" {
		t.Fatalf("plasma 5 primary: %q", a)
	}
	outs[0].Pos.X, outs[0].Pos.Y, outs[0].Scale = 1920, 98, 2
	outs[1].Pos.X = 0
	if kdeDiffers(outs, l) {
		t.Fatal("the applied arrangement still differs")
	}
}

type fakeArranger struct{ applied []MacLayout }

func (f *fakeArranger) name() string { return "fake" }
func (f *fakeArranger) watch(func()) {}
func (f *fakeArranger) apply(l MacLayout) (bool, error) {
	f.applied = append(f.applied, l)
	return true, nil
}

func TestArrangeLoopDebounces(t *testing.T) {
	f := &fakeArranger{}
	loop := newArrangeLoop(f)
	l, _ := parseLayout([]byte(twoScreens))
	loop.set(l)
	loop.set(l)
	loop.poke()
	loop.mu.Lock()
	timer := loop.timer
	loop.mu.Unlock()
	timer.Stop()
	loop.run() // what the timer would do, once
	if len(f.applied) != 1 {
		t.Fatalf("applied %d times", len(f.applied))
	}
}

func TestWlrMessages(t *testing.T) {
	// set_position(x=-1920, y=98) on config head 9: object 9, opcode 2, size 16.
	got := setPositionMsg(9, -1920, 98)
	want := []byte{9, 0, 0, 0, 2, 0, 16, 0, 0x80, 0xf8, 0xff, 0xff, 98, 0, 0, 0}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("set_position = %v, want %v", got, want)
	}
	// set_scale(2.0) = fixed 512; set_scale(1.25) = 320.
	if got, want := setScaleMsg(9, 2), []byte{9, 0, 0, 0, 4, 0, 12, 0, 0, 2, 0, 0}; !reflect.DeepEqual(got, want) {
		t.Fatalf("set_scale(2) = %v, want %v", got, want)
	}
	if got, want := setScaleMsg(9, 1.25), []byte{9, 0, 0, 0, 4, 0, 12, 0, 0x40, 1, 0, 0}; !reflect.DeepEqual(got, want) {
		t.Fatalf("set_scale(1.25) = %v, want %v", got, want)
	}
	if fromFixed(toFixed(1.5)) != 1.5 {
		t.Fatal("fixed round trip")
	}
}

func wlrHeads() []*wlrHead {
	return []*wlrHead{
		{id: 0xff000001, name: "Virtual-1", enabled: true, current: 0xff000010, x: 0, y: 0, scale: 1,
			modes: []*wlrMode{{id: 0xff000010, w: 3024, h: 1964, refresh: 60000, preferred: true}}},
		{id: 0xff000002, name: "Virtual-2", enabled: true, current: 0xff000020, x: 3024, y: 0, scale: 1,
			modes: []*wlrMode{{id: 0xff000020, w: 1920, h: 1080, refresh: 60000, preferred: true}}},
		{id: 0xff000003, name: "Virtual-3", enabled: false, scale: 1},
	}
}

func TestPlanWlr(t *testing.T) {
	l, _ := parseLayout([]byte(twoScreens))
	hs := wlrHeads()
	plan, ok := planWlr(hs, l)
	if !ok || len(plan) != 3 {
		t.Fatalf("planWlr = %+v %v", plan, ok)
	}
	if p := plan[0]; !p.enable || p.mode != 0xff000010 || p.x != 1920 || p.y != 98 || p.scale != 2 {
		t.Fatalf("Virtual-1 = %+v", p)
	}
	if p := plan[1]; p.mode != 0xff000020 || p.x != 0 || p.y != 0 || p.scale != 1 {
		t.Fatalf("Virtual-2 = %+v", p)
	}
	if p := plan[2]; p.enable {
		t.Fatalf("a head off and not in the layout got enabled: %+v", p)
	}
	if !wlrDiffers(hs, plan) {
		t.Fatal("no difference seen")
	}
	hs[0].x, hs[0].y, hs[0].scale = 1920, 98, 2
	hs[1].x = 0
	if wlrDiffers(hs, plan) {
		t.Fatal("the applied arrangement still differs")
	}
	// a size no mode has: a custom mode
	hs[1].modes = nil
	plan, _ = planWlr(hs, l)
	if p := plan[1]; p.mode != 0 || p.w != 1920 || p.h != 1080 {
		t.Fatalf("custom mode = %+v", p)
	}
	// the whole configuration: create, enable/disable each head, apply
	next := uint32(100)
	config, msgs := configMessages(5, 42, plan, func() uint32 { next++; return next })
	if config != 101 {
		t.Fatalf("config id %d", config)
	}
	obj, op, _, _ := header(msgs)
	if obj != 5 || op != wlrManagerCreateConfiguration {
		t.Fatalf("first message %d.%d", obj, op)
	}
	// the last message is apply on the configuration
	last := msgs[len(msgs)-8:]
	if obj, op, size, _ := header(last); obj != config || op != wlrConfigApply || size != 8 {
		t.Fatalf("last message %d.%d size %d", obj, op, size)
	}
}
