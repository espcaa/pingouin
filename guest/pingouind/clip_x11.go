package main

// The clipboard over X11 (pure Go, github.com/jezek/xgb): for GNOME, whose
// Wayland has no data-control but whose XWayland mirrors the clipboard both
// ways, and for any X11 session. An unmapped window watches CLIPBOARD's owner
// (XFixes), asks each new owner for UTF8_STRING, and owns CLIPBOARD to set it.

import (
	"errors"
	"fmt"
	"log"
	"os"
	"sync"
	"time"

	"github.com/jezek/xgb"
	"github.com/jezek/xgb/xfixes"
	"github.com/jezek/xgb/xproto"
)

// X11's limit for one request without BIG-REQUESTS is 256 KiB: text past
// this (less the request's own bytes) isn't set or read (no INCR).
const x11ClipMax = 256*1024 - 64

type x11Clip struct {
	c   *xgb.Conn
	win xproto.Window
	// atoms
	clipboard, utf8, targets, text, plain, incr, prop xproto.Atom

	mu    sync.Mutex
	owned bool
	text_ string
	big   bool // the "too big" log, once
}

func dialX11Clip() (*x11Clip, error) {
	c, err := xgb.NewConn()
	if err != nil {
		return nil, err
	}
	if err := xfixes.Init(c); err != nil {
		c.Close()
		return nil, fmt.Errorf("no XFixes: %w", err)
	}
	if _, err := xfixes.QueryVersion(c, 5, 0).Reply(); err != nil {
		c.Close()
		return nil, err
	}
	x := &x11Clip{c: c}
	for _, a := range []struct {
		name string
		dst  *xproto.Atom
	}{
		{"CLIPBOARD", &x.clipboard}, {"UTF8_STRING", &x.utf8}, {"TARGETS", &x.targets},
		{"TEXT", &x.text}, {"text/plain;charset=utf-8", &x.plain}, {"INCR", &x.incr},
		{"PINGOUIN_CLIPBOARD", &x.prop},
	} {
		r, err := xproto.InternAtom(c, false, uint16(len(a.name)), a.name).Reply()
		if err != nil {
			c.Close()
			return nil, err
		}
		*a.dst = r.Atom
	}
	screen := xproto.Setup(c).DefaultScreen(c)
	win, err := xproto.NewWindowId(c)
	if err != nil {
		c.Close()
		return nil, err
	}
	x.win = win
	if err := xproto.CreateWindowChecked(c, 0, win, screen.Root, 0, 0, 1, 1, 0,
		xproto.WindowClassInputOnly, 0, xproto.CwEventMask,
		[]uint32{xproto.EventMaskPropertyChange}).Check(); err != nil {
		c.Close()
		return nil, err
	}
	if err := xfixes.SelectSelectionInputChecked(c, win, x.clipboard,
		xfixes.SelectionEventMaskSetSelectionOwner).Check(); err != nil {
		c.Close()
		return nil, err
	}
	return x, nil
}

func (x *x11Clip) name() string { return "X11" }

func (x *x11Clip) run(changed func(string, bool)) error {
	defer x.c.Close()
	// Under XWayland (GNOME), a Wayland app's copy doesn't reliably show up as
	// an owner change here, but the X server answers with the current text
	// when asked: ask every 1.5 s too (repeats are dropped by clipState).
	// The first answer is the clipboard as it was: a baseline, not sent.
	initial := true
	sawEvent := false
	if os.Getenv("WAYLAND_DISPLAY") != "" {
		go func() {
			for range time.Tick(1500 * time.Millisecond) {
				x.mu.Lock()
				owned := x.owned
				x.mu.Unlock()
				if !owned {
					xproto.ConvertSelection(x.c, x.win, x.clipboard, x.utf8, x.prop, xproto.TimeCurrentTime)
				}
			}
		}()
	} else {
		initial = false // plain X11: every owner change is a real copy
	}
	for {
		ev, xerr := x.c.WaitForEvent()
		if ev == nil && xerr == nil {
			return errors.New("the X server closed the connection")
		}
		if xerr != nil {
			continue // a request's error (a requestor went away...): not fatal
		}
		switch e := ev.(type) {
		case xfixes.SelectionNotifyEvent:
			if e.Owner == x.win || e.Owner == 0 {
				continue // ours, or nobody's
			}
			if !sawEvent {
				log.Printf("clipboard: X11: the clipboard's owner changes are seen")
				sawEvent = true
			}
			x.mu.Lock()
			x.owned = false
			x.mu.Unlock()
			xproto.ConvertSelection(x.c, x.win, x.clipboard, x.utf8, x.prop, e.SelectionTimestamp)
		case xproto.SelectionNotifyEvent:
			if e.Requestor != x.win || e.Property == 0 {
				continue // the owner has no UTF8_STRING
			}
			if text, ok := x.readProperty(); ok {
				changed(text, initial)
				initial = false
			}
		case xproto.SelectionRequestEvent:
			x.answer(e)
		case xproto.SelectionClearEvent:
			x.mu.Lock()
			x.owned = false
			x.mu.Unlock()
		}
	}
}

func (x *x11Clip) readProperty() (string, bool) {
	r, err := xproto.GetProperty(x.c, true, x.win, x.prop, xproto.AtomAny, 0, (clipMax+4)/4).Reply()
	if err != nil {
		return "", false
	}
	if r.Type == x.incr || r.BytesAfter > 0 {
		x.tooBig("Linux text too big for X11 (INCR): not sent")
		return "", false
	}
	return string(r.Value), true
}

func (x *x11Clip) tooBig(what string) {
	x.mu.Lock()
	defer x.mu.Unlock()
	if !x.big {
		log.Printf("clipboard: %s", what)
		x.big = true
	}
}

// x11Targets: what we answer TARGETS with (the list of types we can give).
func (x *x11Clip) x11Targets() []xproto.Atom {
	return []xproto.Atom{x.targets, x.utf8, x.plain, x.text, xproto.AtomString}
}

// answer gives a requestor our text (we own CLIPBOARD).
func (x *x11Clip) answer(e xproto.SelectionRequestEvent) {
	x.mu.Lock()
	text, owned := x.text_, x.owned
	x.mu.Unlock()
	prop := e.Property
	if prop == 0 {
		prop = e.Target // obsolete requestors
	}
	ok := owned && e.Selection == x.clipboard
	if ok {
		switch e.Target {
		case x.targets:
			ts := x.x11Targets()
			b := make([]byte, 4*len(ts))
			for i, a := range ts {
				xgb.Put32(b[4*i:], uint32(a))
			}
			xproto.ChangeProperty(x.c, xproto.PropModeReplace, e.Requestor, prop, xproto.AtomAtom, 32,
				uint32(len(ts)), b)
		case x.utf8, x.plain, x.text, xproto.AtomString:
			typ := e.Target
			if typ == x.text {
				typ = x.utf8
			}
			xproto.ChangeProperty(x.c, xproto.PropModeReplace, e.Requestor, prop, typ, 8,
				uint32(len(text)), []byte(text))
		default:
			ok = false
		}
	}
	if !ok {
		prop = 0 // refused
	}
	n := xproto.SelectionNotifyEvent{Time: e.Time, Requestor: e.Requestor, Selection: e.Selection,
		Target: e.Target, Property: prop}
	xproto.SendEvent(x.c, false, e.Requestor, 0, string(n.Bytes()))
}

// set owns CLIPBOARD with text.
func (x *x11Clip) set(text string) error {
	if len(text) > x11ClipMax {
		x.tooBig("Mac text too big for X11 (over 256 KiB): not set")
		return nil
	}
	x.mu.Lock()
	x.text_, x.owned = text, true
	x.mu.Unlock()
	if err := xproto.SetSelectionOwnerChecked(x.c, x.win, x.clipboard, xproto.TimeCurrentTime).Check(); err != nil {
		return err
	}
	r, err := xproto.GetSelectionOwner(x.c, x.clipboard).Reply()
	if err != nil {
		return err
	}
	if r.Owner != x.win {
		x.mu.Lock()
		x.owned = false
		x.mu.Unlock()
		return errors.New("the X server didn't give us CLIPBOARD")
	}
	return nil
}
