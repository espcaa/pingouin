package main

// A minimal Wayland client (pure Go, no libwayland): just enough to learn
// where the compositor put each output, in logical pixels.
//
// Wire format: every message is a header of two little-endian uint32s,
// the object id and (size<<16 | opcode), size counting the header; then
// the arguments, each 4-byte aligned. A string is its length including the
// NUL, then the bytes, the NUL and padding to 4 (length 0: null).
//
// Bound: wl_registry; wl_output up to v4 (v2: done, v4: name); and
// zxdg_output_manager_v1 up to v3 (logical position and size per output;
// v2: name; v3: no xdg done, wl_output.done ends a change instead).

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sort"
	"sync"
)

// ---- wire helpers ----

func putUint(b []byte, v uint32) []byte {
	return binary.LittleEndian.AppendUint32(b, v)
}

func putString(b []byte, s string) []byte {
	b = putUint(b, uint32(len(s)+1))
	b = append(b, s...)
	b = append(b, 0)
	for len(b)%4 != 0 {
		b = append(b, 0)
	}
	return b
}

// message builds one request: header plus the already encoded arguments.
func message(object uint32, opcode uint16, args []byte) []byte {
	size := 8 + len(args)
	b := make([]byte, 0, size)
	b = putUint(b, object)
	b = putUint(b, uint32(size)<<16|uint32(opcode))
	return append(b, args...)
}

// header splits a message header; size includes the 8 header bytes.
func header(b []byte) (object uint32, opcode uint16, size int, ok bool) {
	if len(b) < 8 {
		return 0, 0, 0, false
	}
	object = binary.LittleEndian.Uint32(b)
	w := binary.LittleEndian.Uint32(b[4:])
	return object, uint16(w & 0xffff), int(w >> 16), true
}

// args reads a message's arguments in order.
type args struct {
	b   []byte
	err error
}

func (a *args) uint() uint32 {
	if a.err != nil || len(a.b) < 4 {
		a.err = errShort
		return 0
	}
	v := binary.LittleEndian.Uint32(a.b)
	a.b = a.b[4:]
	return v
}

func (a *args) int() int32 { return int32(a.uint()) }

func (a *args) string() string {
	n := int(a.uint())
	if a.err != nil || n == 0 {
		return ""
	}
	padded := (n + 3) &^ 3
	if len(a.b) < padded {
		a.err = errShort
		return ""
	}
	s := string(a.b[:n-1]) // without the NUL
	a.b = a.b[padded:]
	return s
}

var errShort = errors.New("wayland: message shorter than its arguments")

// ---- the client ----

const (
	wlDisplayID = 1

	// wl_display
	wlDisplaySync        = 0
	wlDisplayGetRegistry = 1
	wlDisplayError       = 0

	// wl_registry
	wlRegistryBind         = 0
	wlRegistryGlobal       = 0
	wlRegistryGlobalRemove = 1

	// wl_output events
	wlOutputGeometry = 0
	wlOutputMode     = 1
	wlOutputDone     = 2
	wlOutputScale    = 3
	wlOutputName     = 4

	// zxdg_output_manager_v1 requests
	xdgManagerGetOutput = 1

	// zxdg_output_v1 events
	xdgOutputLogicalPosition = 0
	xdgOutputLogicalSize     = 1
	xdgOutputDone            = 2
	xdgOutputName            = 3

	wlOutputModeCurrent = 1
)

type wlOutput struct {
	id, xdg              uint32
	global               uint32 // registry name
	wlName, xdgName      string
	x, y, w, h           int32 // logical (xdg_output)
	logical              bool
	gx, gy, modeW, modeH int32 // wl_output geometry and current mode (fallback)
	scale                int32
}

type wlClient struct {
	conn     net.Conn
	mu       sync.Mutex
	next     uint32
	registry uint32
	manager  uint32               // zxdg_output_manager_v1, 0 until bound
	outputs  map[uint32]*wlOutput // by wl_output id
	byXdg    map[uint32]*wlOutput // by zxdg_output_v1 id
	changed  func()               // a change is complete (from the read loop)
}

// waylandSocket is the compositor's socket, from $WAYLAND_DISPLAY.
func waylandSocket() (string, bool) {
	d := os.Getenv("WAYLAND_DISPLAY")
	if d == "" {
		return "", false
	}
	if filepath.IsAbs(d) {
		return d, true
	}
	run := os.Getenv("XDG_RUNTIME_DIR")
	if run == "" {
		return "", false
	}
	return filepath.Join(run, d), true
}

func dialWayland(path string, changed func()) (*wlClient, error) {
	conn, err := net.Dial("unix", path)
	if err != nil {
		return nil, err
	}
	c := &wlClient{conn: conn, next: 2, outputs: map[uint32]*wlOutput{},
		byXdg: map[uint32]*wlOutput{}, changed: changed}
	c.registry = c.newID()
	if err := c.send(message(wlDisplayID, wlDisplayGetRegistry, putUint(nil, c.registry))); err != nil {
		conn.Close()
		return nil, err
	}
	return c, nil
}

func (c *wlClient) newID() uint32 { id := c.next; c.next++; return id }

func (c *wlClient) send(m []byte) error {
	_, err := c.conn.Write(m)
	return err
}

func (c *wlClient) Close() error { return c.conn.Close() }

// bind asks the registry for global `name` as a new object.
func (c *wlClient) bind(name uint32, iface string, version uint32) (uint32, error) {
	id := c.newID()
	a := putUint(nil, name)
	a = putString(a, iface)
	a = putUint(a, version)
	a = putUint(a, id)
	return id, c.send(message(c.registry, wlRegistryBind, a))
}

func (c *wlClient) getXdgOutput(o *wlOutput) error {
	o.xdg = c.newID()
	c.byXdg[o.xdg] = o
	a := putUint(nil, o.xdg)
	a = putUint(a, o.id)
	return c.send(message(c.manager, xdgManagerGetOutput, a))
}

// Run reads events until the connection ends.
func (c *wlClient) Run() error {
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
			// keep the remainder at the start
			buf = append(buf[:0:0], buf...)
		}
		if err != nil {
			if errors.Is(err, io.EOF) {
				return errors.New("wayland: the compositor closed the connection")
			}
			return err
		}
	}
}

func (c *wlClient) event(obj uint32, op uint16, body []byte) error {
	a := &args{b: body}
	c.mu.Lock()
	defer c.mu.Unlock()
	done := false
	switch {
	case obj == wlDisplayID:
		if op == wlDisplayError {
			a.uint()
			code := a.uint()
			msg := a.string()
			return fmt.Errorf("wayland: protocol error %d: %s", code, msg)
		}
	case obj == c.registry:
		switch op {
		case wlRegistryGlobal:
			name, iface, version := a.uint(), a.string(), a.uint()
			if a.err != nil {
				return a.err
			}
			switch iface {
			case "wl_output":
				id, err := c.bind(name, iface, min(version, 4))
				if err != nil {
					return err
				}
				o := &wlOutput{id: id, global: name, scale: 1}
				c.outputs[id] = o
				if c.manager != 0 {
					if err := c.getXdgOutput(o); err != nil {
						return err
					}
				}
			case "zxdg_output_manager_v1":
				if c.manager == 0 {
					id, err := c.bind(name, iface, min(version, 3))
					if err != nil {
						return err
					}
					c.manager = id
					for _, o := range c.outputs {
						if err := c.getXdgOutput(o); err != nil {
							return err
						}
					}
				}
			}
		case wlRegistryGlobalRemove:
			name := a.uint()
			for id, o := range c.outputs {
				if o.global == name {
					delete(c.outputs, id)
					delete(c.byXdg, o.xdg)
					done = true
				}
			}
		}
	default:
		if o, ok := c.outputs[obj]; ok {
			switch op {
			case wlOutputGeometry:
				o.gx, o.gy = a.int(), a.int()
			case wlOutputMode:
				flags, w, h := a.uint(), a.int(), a.int()
				if flags&wlOutputModeCurrent != 0 {
					o.modeW, o.modeH = w, h
				}
			case wlOutputScale:
				if s := a.int(); s > 0 {
					o.scale = s
				}
			case wlOutputName:
				o.wlName = a.string()
			case wlOutputDone:
				done = true
			}
		} else if o, ok := c.byXdg[obj]; ok {
			switch op {
			case xdgOutputLogicalPosition:
				o.x, o.y = a.int(), a.int()
				o.logical = true
			case xdgOutputLogicalSize:
				o.w, o.h = a.int(), a.int()
				o.logical = true
			case xdgOutputName:
				o.xdgName = a.string()
			case xdgOutputDone:
				done = true
			}
		}
	}
	if a.err != nil {
		return a.err
	}
	if done && c.changed != nil {
		go c.changed() // not under the lock
	}
	return nil
}

// Monitors is the compositor's layout now: outputs whose size is known.
func (c *wlClient) Monitors() []Monitor {
	c.mu.Lock()
	defer c.mu.Unlock()
	var ms []Monitor
	for _, o := range c.outputs {
		name := o.xdgName
		if name == "" {
			name = o.wlName
		}
		m := Monitor{Name: name}
		switch {
		case o.logical && o.w > 0 && o.h > 0:
			m.X, m.Y, m.Width, m.Height = int(o.x), int(o.y), int(o.w), int(o.h)
		case o.modeW > 0 && o.modeH > 0:
			// No xdg_output: the geometry's place and the mode over the scale.
			m.X, m.Y = int(o.gx), int(o.gy)
			m.Width, m.Height = int(o.modeW/o.scale), int(o.modeH/o.scale)
		default:
			continue
		}
		if m.Name == "" {
			continue // QEMU matches outputs by name
		}
		ms = append(ms, m)
	}
	sort.Slice(ms, func(i, j int) bool { return ms[i].Name < ms[j].Name })
	return ms
}
