package main

// GNOME: org.gnome.Mutter.DisplayConfig on the session bus.
//   GetCurrentState() -> (u serial, a((ssss)a(siiddada{sv})a{sv}) monitors,
//                         a(iiduba(ssss)a{sv}) logical_monitors, a{sv} properties)
//   ApplyMonitorsConfig(u serial, u method, a(iiduba(ssa{sv})) logical_monitors, a{sv} properties)
//   signal MonitorsChanged()
// method 1 is "temporary": applied without GNOME's "Keep these settings?"
// dialog and not saved; the Mac's arrangement is sent again on every change.

import (
	"errors"
	"fmt"
	"log"

	"github.com/godbus/dbus/v5"
)

const (
	mutterName  = "org.gnome.Mutter.DisplayConfig"
	mutterPath  = "/org/gnome/Mutter/DisplayConfig"
	mutterIface = "org.gnome.Mutter.DisplayConfig"
)

type mutterSpec struct{ Connector, Vendor, Product, Serial string }

type mutterMode struct {
	ID              string
	Width, Height   int32
	Refresh         float64
	PreferredScale  float64
	SupportedScales []float64
	Props           map[string]dbus.Variant
}

type mutterMonitor struct {
	Spec  mutterSpec
	Modes []mutterMode
	Props map[string]dbus.Variant
}

type mutterLogical struct {
	X, Y      int32
	Scale     float64
	Transform uint32
	Primary   bool
	Monitors  []mutterSpec
	Props     map[string]dbus.Variant
}

type mutterState struct {
	Serial   uint32
	Monitors []mutterMonitor
	Logical  []mutterLogical
	Props    map[string]dbus.Variant
}

type applyMonitor struct {
	Connector string
	ModeID    string
	Props     map[string]dbus.Variant
}

type applyLogical struct {
	X, Y      int32
	Scale     float64
	Transform uint32
	Primary   bool
	Monitors  []applyMonitor
}

func boolProp(p map[string]dbus.Variant, k string) bool {
	v, ok := p[k]
	if !ok {
		return false
	}
	b, _ := v.Value().(bool)
	return b
}

// physicalLayout: Mutter's "layout-mode" 2 (positions in physical pixels);
// 1 (logical) is the default on Wayland.
func (s mutterState) physicalLayout() bool {
	v, ok := s.Props["layout-mode"]
	if !ok {
		return false
	}
	m, _ := v.Value().(uint32)
	return m == 2
}

// pickMode: the current mode if it is the wanted size, else the preferred
// one, else one of the wanted size, else the current, else the first.
func pickMode(modes []mutterMode, w, h int32) (mutterMode, bool) {
	var cur, pref, match *mutterMode
	for i := range modes {
		m := &modes[i]
		if boolProp(m.Props, "is-current") && cur == nil {
			cur = m
		}
		if boolProp(m.Props, "is-preferred") && pref == nil {
			pref = m
		}
		if m.Width == w && m.Height == h && match == nil {
			match = m
		}
	}
	switch {
	case cur != nil && cur.Width == w && cur.Height == h:
		return *cur, true
	case pref != nil:
		return *pref, true
	case match != nil:
		return *match, true
	case cur != nil:
		return *cur, true
	case len(modes) > 0:
		return modes[0], true
	}
	return mutterMode{}, false
}

// planGnome: the logical monitors that make the guest's arrangement the
// layout's. Only outputs both in the layout and connected in the guest.
func planGnome(s mutterState, l MacLayout) ([]applyLogical, error) {
	ps := placements(l)
	phys := s.physicalLayout()
	var physPos map[string][2]int
	if phys {
		physPos = physicalPositions(ps)
	}
	var out []applyLogical
	primary := false
	for _, p := range ps {
		var mon *mutterMonitor
		for i := range s.Monitors {
			if s.Monitors[i].Spec.Connector == p.Name {
				mon = &s.Monitors[i]
			}
		}
		if mon == nil {
			continue // not connected in the guest (yet)
		}
		w, h := int32(float64(p.Width)*p.Scale+0.5), int32(float64(p.Height)*p.Scale+0.5)
		mode, ok := pickMode(mon.Modes, w, h)
		if !ok {
			continue
		}
		x, y := int32(p.X), int32(p.Y)
		if phys {
			x, y = int32(physPos[p.Name][0]), int32(physPos[p.Name][1])
		}
		out = append(out, applyLogical{
			X: x, Y: y,
			Scale:    nearestScale(mode.SupportedScales, p.Scale),
			Primary:  p.Primary,
			Monitors: []applyMonitor{{Connector: p.Name, ModeID: mode.ID, Props: map[string]dbus.Variant{}}},
		})
		primary = primary || p.Primary
	}
	if len(out) == 0 {
		return nil, errors.New("none of the Mac's screens is connected in the guest yet")
	}
	if !primary {
		out[0].Primary = true
	}
	return out, nil
}

// gnomeDiffers: is the guest's arrangement other than the plan (outputs on,
// positions, scales, primary, modes)?
func gnomeDiffers(s mutterState, plan []applyLogical) bool {
	type cur struct {
		x, y    int32
		scale   float64
		primary bool
	}
	now := map[string]cur{}
	for _, lm := range s.Logical {
		for _, m := range lm.Monitors {
			now[m.Connector] = cur{lm.X, lm.Y, lm.Scale, lm.Primary}
		}
	}
	if len(now) != len(plan) {
		return true
	}
	for _, p := range plan {
		c, ok := now[p.Monitors[0].Connector]
		if !ok || c.x != p.X || c.y != p.Y || abs(c.scale-p.Scale) > 0.01 || c.primary != p.Primary {
			return true
		}
		for _, m := range s.Monitors {
			if m.Spec.Connector != p.Monitors[0].Connector {
				continue
			}
			for _, md := range m.Modes {
				if boolProp(md.Props, "is-current") && md.ID != p.Monitors[0].ModeID {
					return true
				}
			}
		}
	}
	return false
}

type gnomeArranger struct {
	conn *dbus.Conn
	obj  dbus.BusObject
}

// newGnomeArranger: only when Mutter's DisplayConfig is on the session bus.
func newGnomeArranger() arranger {
	conn, err := dbus.ConnectSessionBus()
	if err != nil {
		return nil
	}
	var has bool
	if err := conn.BusObject().Call("org.freedesktop.DBus.NameHasOwner", 0, mutterName).Store(&has); err != nil || !has {
		conn.Close()
		return nil
	}
	return &gnomeArranger{conn: conn, obj: conn.Object(mutterName, mutterPath)}
}

func (g *gnomeArranger) name() string { return "GNOME" }

func (g *gnomeArranger) state() (mutterState, error) {
	var s mutterState
	err := g.obj.Call(mutterIface+".GetCurrentState", 0).Store(&s.Serial, &s.Monitors, &s.Logical, &s.Props)
	return s, err
}

func (g *gnomeArranger) apply(l MacLayout) (bool, error) {
	s, err := g.state()
	if err != nil {
		return false, fmt.Errorf("GetCurrentState: %w", err)
	}
	plan, err := planGnome(s, l)
	if err != nil {
		return false, err
	}
	if !gnomeDiffers(s, plan) {
		return false, nil
	}
	call := g.obj.Call(mutterIface+".ApplyMonitorsConfig", 0, s.Serial, uint32(1), plan, map[string]dbus.Variant{})
	return true, call.Err
}

func (g *gnomeArranger) watch(f func()) {
	if err := g.conn.AddMatchSignal(dbus.WithMatchObjectPath(mutterPath),
		dbus.WithMatchInterface(mutterIface), dbus.WithMatchMember("MonitorsChanged")); err != nil {
		log.Printf("displays: GNOME's MonitorsChanged: %v", err)
		return
	}
	ch := make(chan *dbus.Signal, 8)
	g.conn.Signal(ch)
	go func() {
		for sig := range ch {
			if sig.Name == mutterIface+".MonitorsChanged" {
				f()
			}
		}
	}()
}
