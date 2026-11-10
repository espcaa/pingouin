package main

// wlroots-style compositors (Hyprland, Sway, niri, river, labwc, Wayfire):
// wlr-output-management-unstable-v1 over our own Wayland connection.
//
//   zwlr_output_manager_v1 (v4)  req 0 create_configuration(new_id, serial)
//                                ev  0 head(new_id head)  1 done(serial)  2 finished
//   zwlr_output_head_v1          ev  0 name(s) 1 description(s) 2 physical_size(i,i)
//                                    3 mode(new_id) 4 enabled(i) 5 current_mode(o)
//                                    6 position(i,i) 7 transform(i) 8 scale(fixed)
//                                    9 finished (v2: 10 make 11 model 12 serial_number,
//                                    v4: 13 adaptive_sync)
//   zwlr_output_mode_v1          ev  0 size(i,i) 1 refresh(i) 2 preferred 3 finished
//   zwlr_output_configuration_v1 req 0 enable_head(new_id, o head) 1 disable_head(o head)
//                                    2 apply 3 test 4 destroy
//                                ev  0 succeeded 1 failed 2 cancelled
//   zwlr_output_configuration_head_v1
//                                req 0 set_mode(o) 1 set_custom_mode(i,i,i)
//                                    2 set_position(i,i) 3 set_transform(i) 4 set_scale(fixed)
// A configuration names every head: those not in the Mac's layout are enabled
// again as they are (or disabled if they were off), so they stay unchanged.

import (
	"errors"
	"fmt"
	"math"
	"net"
	"sort"
	"sync"
	"time"
)

const (
	wlrManagerCreateConfiguration = 0
	wlrManagerHead                = 0
	wlrManagerDone                = 1
	wlrManagerFinished            = 2

	wlrHeadName        = 0
	wlrHeadMode        = 3
	wlrHeadEnabled     = 4
	wlrHeadCurrentMode = 5
	wlrHeadPosition    = 6
	wlrHeadScale       = 8
	wlrHeadFinished    = 9

	wlrModeSize      = 0
	wlrModeRefresh   = 1
	wlrModePreferred = 2
	wlrModeFinished  = 3

	wlrConfigEnableHead  = 0
	wlrConfigDisableHead = 1
	wlrConfigApply       = 2
	wlrConfigDestroy     = 4

	wlrConfigSucceeded = 0
	wlrConfigFailed    = 1
	wlrConfigCancelled = 2

	wlrCHSetMode       = 0
	wlrCHSetCustomMode = 1
	wlrCHSetPosition   = 2
	wlrCHSetScale      = 4

	wlCallbackDone = 0
)

// toFixed is Wayland's 24.8 fixed point.
func toFixed(f float64) int32   { return int32(math.Round(f * 256)) }
func fromFixed(v int32) float64 { return float64(v) / 256 }

func setPositionMsg(ch uint32, x, y int32) []byte {
	return message(ch, wlrCHSetPosition, putUint(putUint(nil, uint32(x)), uint32(y)))
}

func setScaleMsg(ch uint32, scale float64) []byte {
	return message(ch, wlrCHSetScale, putUint(nil, uint32(toFixed(scale))))
}

type wlrMode struct {
	id        uint32
	w, h      int32
	refresh   int32 // mHz
	preferred bool
}

type wlrHead struct {
	id      uint32
	name    string
	enabled bool
	modes   []*wlrMode
	current uint32 // mode id, 0: none
	x, y    int32
	scale   float64
}

type wlrClient struct {
	conn     net.Conn
	mu       sync.Mutex
	next     uint32
	registry uint32
	manager  uint32
	sync     uint32 // the first wl_display.sync's callback
	synced   chan struct{}
	serial   uint32
	haveDone bool
	heads    map[uint32]*wlrHead
	modes    map[uint32]*wlrMode
	modeHead map[uint32]*wlrHead
	config   uint32
	result   chan uint16 // succeeded / failed / cancelled
	changed  func()
	runErr   chan error
}

func dialWlr(path string) (*wlrClient, error) {
	conn, err := net.Dial("unix", path)
	if err != nil {
		return nil, err
	}
	c := &wlrClient{conn: conn, next: 2, synced: make(chan struct{}),
		heads: map[uint32]*wlrHead{}, modes: map[uint32]*wlrMode{}, modeHead: map[uint32]*wlrHead{},
		runErr: make(chan error, 1)}
	c.registry = c.newID()
	c.sync = c.newID()
	m := message(wlDisplayID, wlDisplayGetRegistry, putUint(nil, c.registry))
	m = append(m, message(wlDisplayID, wlDisplaySync, putUint(nil, c.sync))...)
	if _, err := conn.Write(m); err != nil {
		conn.Close()
		return nil, err
	}
	go func() { c.runErr <- c.run() }()
	return c, nil
}

func (c *wlrClient) newID() uint32 { id := c.next; c.next++; return id }

func (c *wlrClient) send(m []byte) error { _, err := c.conn.Write(m); return err }

func (c *wlrClient) run() error {
	buf := make([]byte, 0, 64*1024)
	chunk := make([]byte, 16*1024)
	for {
		n, err := c.conn.Read(chunk)
		if n > 0 {
			buf = append(buf, chunk[:n]...)
			for {
				obj, op, size, ok := header(buf)
				if !ok || len(buf) < size {
					break
				}
				if size < 8 {
					return fmt.Errorf("wayland: bad message size %d", size)
				}
				if err := c.event(obj, op, buf[8:size]); err != nil {
					return err
				}
				buf = buf[size:]
			}
			buf = append(buf[:0:0], buf...)
		}
		if err != nil {
			return err
		}
	}
}

func (c *wlrClient) event(obj uint32, op uint16, body []byte) error {
	a := &args{b: body}
	c.mu.Lock()
	notify := false
	defer func() {
		f := c.changed
		c.mu.Unlock()
		if notify && f != nil {
			go f()
		}
	}()
	switch {
	case obj == wlDisplayID:
		if op == wlDisplayError {
			a.uint()
			code := a.uint()
			return fmt.Errorf("wayland: protocol error %d: %s", code, a.string())
		}
	case obj == c.sync:
		if op == wlCallbackDone {
			close(c.synced)
			c.sync = 0
		}
	case obj == c.registry:
		if op == wlRegistryGlobal {
			name, iface, version := a.uint(), a.string(), a.uint()
			if iface == "zwlr_output_manager_v1" && c.manager == 0 {
				id := c.newID()
				b := putUint(nil, name)
				b = putString(b, iface)
				b = putUint(b, min(version, 4))
				b = putUint(b, id)
				if err := c.send(message(c.registry, wlRegistryBind, b)); err != nil {
					return err
				}
				c.manager = id
			}
		}
	case obj == c.manager:
		switch op {
		case wlrManagerHead:
			id := a.uint()
			c.heads[id] = &wlrHead{id: id, scale: 1}
		case wlrManagerDone:
			c.serial = a.uint()
			c.haveDone = true
			notify = true
		case wlrManagerFinished:
			return errors.New("wayland: the output manager finished")
		}
	case obj == c.config && c.config != 0:
		if op <= wlrConfigCancelled && c.result != nil {
			c.result <- op
		}
	default:
		if h, ok := c.heads[obj]; ok {
			switch op {
			case wlrHeadName:
				h.name = a.string()
			case wlrHeadMode:
				id := a.uint()
				m := &wlrMode{id: id}
				c.modes[id] = m
				c.modeHead[id] = h
				h.modes = append(h.modes, m)
			case wlrHeadEnabled:
				h.enabled = a.int() != 0
			case wlrHeadCurrentMode:
				h.current = a.uint()
			case wlrHeadPosition:
				h.x, h.y = a.int(), a.int()
			case wlrHeadScale:
				h.scale = fromFixed(a.int())
			case wlrHeadFinished:
				delete(c.heads, obj)
			}
		} else if m, ok := c.modes[obj]; ok {
			switch op {
			case wlrModeSize:
				m.w, m.h = a.int(), a.int()
			case wlrModeRefresh:
				m.refresh = a.int()
			case wlrModePreferred:
				m.preferred = true
			case wlrModeFinished:
				if h := c.modeHead[obj]; h != nil {
					for i, hm := range h.modes {
						if hm == m {
							h.modes = append(h.modes[:i], h.modes[i+1:]...)
							break
						}
					}
				}
				delete(c.modes, obj)
				delete(c.modeHead, obj)
			}
		}
	}
	return a.err
}

// wlrPlanHead is what the configuration says about one head.
type wlrPlanHead struct {
	head    uint32
	enable  bool
	mode    uint32 // 0: custom
	w, h    int32  // the custom mode
	refresh int32
	x, y    int32
	scale   float64
}

// wlrPickMode: the current mode if it's the wanted size, else the preferred
// one of that size, else any of that size (the highest refresh); 0: none.
func wlrPickMode(h *wlrHead, w, ht int32) uint32 {
	var best *wlrMode
	for _, m := range h.modes {
		if m.w != w || m.h != ht {
			continue
		}
		if m.id == h.current {
			return m.id
		}
		if best == nil || (m.preferred && !best.preferred) || (m.preferred == best.preferred && m.refresh > best.refresh) {
			best = m
		}
	}
	if best != nil {
		return best.id
	}
	return 0
}

// planWlr: every head, the layout's ones placed like the Mac's, the others as
// they are. ok false: none of the layout's outputs is a head here.
func planWlr(heads []*wlrHead, l MacLayout) ([]wlrPlanHead, bool) {
	want := map[string]Placement{}
	for _, p := range placements(l) {
		want[p.Name] = p
	}
	var plan []wlrPlanHead
	found := false
	for _, h := range heads {
		p, ok := want[h.name]
		if !ok {
			ph := wlrPlanHead{head: h.id, enable: h.enabled, mode: h.current, x: h.x, y: h.y, scale: h.scale}
			plan = append(plan, ph)
			continue
		}
		found = true
		w, ht := int32(float64(p.Width)*p.Scale+0.5), int32(float64(p.Height)*p.Scale+0.5)
		ph := wlrPlanHead{head: h.id, enable: true, x: int32(p.X), y: int32(p.Y), scale: p.Scale}
		if ph.mode = wlrPickMode(h, w, ht); ph.mode == 0 {
			ph.w, ph.h = w, ht
		}
		plan = append(plan, ph)
	}
	sort.Slice(plan, func(i, j int) bool { return plan[i].head < plan[j].head })
	return plan, found
}

// wlrDiffers: is a head of the layout off, elsewhere, at another scale or mode?
func wlrDiffers(heads []*wlrHead, plan []wlrPlanHead) bool {
	byID := map[uint32]*wlrHead{}
	for _, h := range heads {
		byID[h.id] = h
	}
	for _, p := range plan {
		h := byID[p.head]
		if h == nil {
			return true
		}
		if h.enabled != p.enable {
			return true
		}
		if !p.enable {
			continue
		}
		if h.x != p.x || h.y != p.y || abs(h.scale-p.scale) > 0.01 {
			return true
		}
		if p.mode != 0 && p.mode != h.current {
			return true
		}
		if p.mode == 0 {
			if cur := findMode(h, h.current); cur == nil || cur.w != p.w || cur.h != p.h {
				return true
			}
		}
	}
	return false
}

func findMode(h *wlrHead, id uint32) *wlrMode {
	for _, m := range h.modes {
		if m.id == id {
			return m
		}
	}
	return nil
}

// configMessages: create_configuration and the requests that fill and apply it.
func configMessages(manager, serial uint32, plan []wlrPlanHead, newID func() uint32) (config uint32, msgs []byte) {
	config = newID()
	msgs = message(manager, wlrManagerCreateConfiguration, putUint(putUint(nil, config), serial))
	for _, p := range plan {
		if !p.enable {
			msgs = append(msgs, message(config, wlrConfigDisableHead, putUint(nil, p.head))...)
			continue
		}
		ch := newID()
		msgs = append(msgs, message(config, wlrConfigEnableHead, putUint(putUint(nil, ch), p.head))...)
		if p.mode != 0 {
			msgs = append(msgs, message(ch, wlrCHSetMode, putUint(nil, p.mode))...)
		} else if p.w > 0 && p.h > 0 {
			msgs = append(msgs, message(ch, wlrCHSetCustomMode,
				putUint(putUint(putUint(nil, uint32(p.w)), uint32(p.h)), uint32(p.refresh)))...)
		}
		msgs = append(msgs, setPositionMsg(ch, p.x, p.y)...)
		msgs = append(msgs, setScaleMsg(ch, p.scale)...)
	}
	msgs = append(msgs, message(config, wlrConfigApply, nil)...)
	return config, msgs
}

type wlrArranger struct{ c *wlrClient }

// newWlrArranger: only when the compositor has zwlr_output_manager_v1.
func newWlrArranger() arranger {
	path, ok := waylandSocket()
	if !ok {
		return nil
	}
	c, err := dialWlr(path)
	if err != nil {
		return nil
	}
	select {
	case <-c.synced:
	case <-time.After(2 * time.Second):
	}
	c.mu.Lock()
	has := c.manager != 0
	c.mu.Unlock()
	if !has {
		c.conn.Close()
		return nil
	}
	// the heads come with the manager's first done
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		c.mu.Lock()
		done := c.haveDone
		c.mu.Unlock()
		if done {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	return &wlrArranger{c: c}
}

func (w *wlrArranger) name() string { return "wlr-output-management" }

func (w *wlrArranger) watch(f func()) {
	w.c.mu.Lock()
	w.c.changed = f
	w.c.mu.Unlock()
}

func (w *wlrArranger) heads() []*wlrHead {
	var hs []*wlrHead
	for _, h := range w.c.heads {
		if h.name != "" {
			hs = append(hs, h)
		}
	}
	sort.Slice(hs, func(i, j int) bool { return hs[i].id < hs[j].id })
	return hs
}

func (w *wlrArranger) apply(l MacLayout) (bool, error) {
	for try := 0; try < 2; try++ {
		c := w.c
		c.mu.Lock()
		if !c.haveDone {
			c.mu.Unlock()
			return false, errors.New("the compositor hasn't listed its outputs yet")
		}
		hs := w.heads()
		plan, ok := planWlr(hs, l)
		if !ok {
			c.mu.Unlock()
			return false, errors.New("none of the Mac's screens is an output here yet")
		}
		if !wlrDiffers(hs, plan) {
			c.mu.Unlock()
			return false, nil
		}
		config, msgs := configMessages(c.manager, c.serial, plan, c.newID)
		c.config = config
		c.result = make(chan uint16, 1)
		res := c.result
		err := c.send(msgs)
		c.mu.Unlock()
		if err != nil {
			return false, err
		}
		var r uint16
		select {
		case r = <-res:
		case err := <-c.runErr:
			return false, fmt.Errorf("wayland: %v", err)
		case <-time.After(3 * time.Second):
			return true, errors.New("the compositor didn't answer the configuration")
		}
		c.mu.Lock()
		c.send(message(config, wlrConfigDestroy, nil))
		c.config, c.result = 0, nil
		c.mu.Unlock()
		switch r {
		case wlrConfigSucceeded:
			return true, nil
		case wlrConfigFailed:
			return true, errors.New("the compositor refused the configuration")
		}
		// cancelled: the outputs changed meanwhile; once more with the new serial
		time.Sleep(200 * time.Millisecond)
	}
	return true, errors.New("the configuration was cancelled twice")
}
