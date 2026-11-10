package main

// The guest copies the Mac's display arrangement and scales: QEMU sends
// {"layout":[{"output":"Virtual-1","x":0,"y":0,"width":1512,"height":982,"scale":2},...],
//  "builtin":"Virtual-1",...} (Mac points; the guest's mode is width*scale x
// height*scale). macOS moves the pointer by its arrangement, so the guest's
// must be the same or the pointer jumps between screens; and a desktop meets a
// new set of monitors with default scales, which this sets back.
// Backends: GNOME (Mutter over D-Bus, arrange_gnome.go), KDE (kscreen-doctor,
// arrange_kde.go) and wlroots-style compositors (wlr-output-management,
// arrange_wlr.go); elsewhere the layout is only logged as unsupported.

import (
	"encoding/json"
	"log"
	"sort"
	"sync"
	"time"
)

// MacOutput is one entry of QEMU's layout.
type MacOutput struct {
	Output string  `json:"output"`
	X      int     `json:"x"`
	Y      int     `json:"y"`
	Width  int     `json:"width"`
	Height int     `json:"height"`
	Scale  float64 `json:"scale"`
}

// MacLayout is QEMU's layout line.
type MacLayout struct {
	Layout  []MacOutput `json:"layout"`
	Builtin string      `json:"builtin"`
}

// parseLayout reads a line from QEMU; ok only for a layout with outputs.
func parseLayout(line []byte) (MacLayout, bool) {
	var raw map[string]json.RawMessage
	if json.Unmarshal(line, &raw) != nil {
		return MacLayout{}, false
	}
	if _, ok := raw["layout"]; !ok {
		return MacLayout{}, false
	}
	var l MacLayout
	if json.Unmarshal(line, &l) != nil {
		return MacLayout{}, false
	}
	out := l.Layout[:0]
	for _, o := range l.Layout {
		if o.Output != "" && o.Width > 0 && o.Height > 0 {
			if o.Scale <= 0 {
				o.Scale = 1
			}
			out = append(out, o)
		}
	}
	l.Layout = out
	return l, len(l.Layout) > 0
}

// Placement is where one output goes in the guest.
type Placement struct {
	Name    string
	X, Y    int // logical pixels, the top-left output at 0,0
	Width   int // logical size (Mac points)
	Height  int
	Scale   float64 // the Mac display's backing scale
	Primary bool
}

// placements: the layout shifted so its top-left is 0,0, sorted by name, with
// the primary output (builtin, else Virtual-1, else the first).
func placements(l MacLayout) []Placement {
	if len(l.Layout) == 0 {
		return nil
	}
	minX, minY := l.Layout[0].X, l.Layout[0].Y
	for _, o := range l.Layout {
		minX, minY = min(minX, o.X), min(minY, o.Y)
	}
	primary := l.Builtin
	has := func(n string) bool {
		for _, o := range l.Layout {
			if o.Output == n {
				return true
			}
		}
		return false
	}
	if !has(primary) {
		primary = "Virtual-1"
		if !has(primary) {
			primary = ""
		}
	}
	ps := make([]Placement, 0, len(l.Layout))
	for _, o := range l.Layout {
		ps = append(ps, Placement{Name: o.Output, X: o.X - minX, Y: o.Y - minY,
			Width: o.Width, Height: o.Height, Scale: o.Scale})
	}
	sort.Slice(ps, func(i, j int) bool { return ps[i].Name < ps[j].Name })
	if primary == "" {
		ps[0].Primary = true
	}
	for i := range ps {
		if ps[i].Name == primary {
			ps[i].Primary = true
		}
	}
	return ps
}

// physicalPositions: the same arrangement in physical pixels (a desktop in
// physical layout mode): each output starts where the output it touches on
// the left (or above) ends, in that output's pixels; else at its own scale.
func physicalPositions(ps []Placement) map[string][2]int {
	out := make(map[string][2]int, len(ps))
	byX := append([]Placement(nil), ps...)
	sort.Slice(byX, func(i, j int) bool { return byX[i].X < byX[j].X })
	for _, p := range byX {
		x := int(float64(p.X) * p.Scale)
		for _, q := range byX {
			if q.Name != p.Name && q.X+q.Width == p.X {
				x = out[q.Name][0] + int(float64(q.Width)*q.Scale)
			}
		}
		out[p.Name] = [2]int{x, 0}
	}
	byY := append([]Placement(nil), ps...)
	sort.Slice(byY, func(i, j int) bool { return byY[i].Y < byY[j].Y })
	for _, p := range byY {
		y := int(float64(p.Y) * p.Scale)
		for _, q := range byY {
			if q.Name != p.Name && q.Y+q.Height == p.Y {
				y = out[q.Name][1] + int(float64(q.Height)*q.Scale)
			}
		}
		out[p.Name] = [2]int{out[p.Name][0], y}
	}
	return out
}

// nearestScale picks the supported scale closest to want (want if none listed).
func nearestScale(supported []float64, want float64) float64 {
	if len(supported) == 0 {
		return want
	}
	best := supported[0]
	for _, s := range supported {
		if abs(s-want) < abs(best-want) {
			best = s
		}
	}
	return best
}

func abs(f float64) float64 {
	if f < 0 {
		return -f
	}
	return f
}

// arranger applies a layout through one desktop's API.
type arranger interface {
	name() string
	// apply makes the guest's arrangement the layout's; it does nothing if
	// it already is. changed: it asked the desktop for a change.
	apply(l MacLayout) (changed bool, err error)
	// watch calls f when the desktop's monitors change (may never).
	watch(f func())
}

// arrangeLoop applies the latest layout ~300 ms after the last layout or
// monitor change. A layout the desktop refuses is logged once.
type arrangeLoop struct {
	mu      sync.Mutex
	layout  MacLayout
	has     bool
	timer   *time.Timer
	a       arranger
	failed  string // the layout (JSON) the last failure was for
	lastKey string
}

func newArrangeLoop(a arranger) *arrangeLoop {
	l := &arrangeLoop{a: a}
	if a != nil {
		log.Printf("displays: arranging screens like the Mac's (%s)", a.name())
		a.watch(l.poke)
	}
	return l
}

// set: QEMU sent a layout.
func (l *arrangeLoop) set(m MacLayout) {
	l.mu.Lock()
	l.layout, l.has = m, true
	l.mu.Unlock()
	l.poke()
}

func (l *arrangeLoop) poke() {
	if l.a == nil {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.timer != nil {
		l.timer.Stop()
	}
	l.timer = time.AfterFunc(300*time.Millisecond, l.run)
}

func (l *arrangeLoop) run() {
	l.mu.Lock()
	m, has := l.layout, l.has
	l.mu.Unlock()
	if !has {
		return
	}
	key, _ := json.Marshal(m)
	changed, err := l.a.apply(m)
	l.mu.Lock()
	defer l.mu.Unlock()
	if err != nil {
		if l.failed != string(key) {
			log.Printf("displays: %s didn't take the Mac's arrangement: %v", l.a.name(), err)
			l.failed = string(key)
		}
		return
	}
	l.failed = ""
	if changed && l.lastKey != string(key) {
		log.Printf("displays: screens arranged like the Mac's (%d)", len(m.Layout))
	}
	l.lastKey = string(key)
}

// pickArranger: GNOME (Mutter's D-Bus name), KDE (kscreen-doctor), the
// wlr-output-management global, else none (logged once).
func pickArranger() arranger {
	if a := newGnomeArranger(); a != nil {
		return a
	}
	if a := newKdeArranger(); a != nil {
		return a
	}
	if a := newWlrArranger(); a != nil {
		return a
	}
	log.Printf("displays: this desktop isn't supported for arranging screens like the Mac's; arrange them in its display settings")
	return nil
}
