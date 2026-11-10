package main

// The clipboard over Wayland data-control: ext-data-control-v1, else
// wlr-data-control-unstable-v1 (same requests and events, same opcodes).
// Its own connection to the compositor (wayland.go's wire helpers). File
// descriptors travel beside the bytes (SCM_RIGHTS): ours with offer.receive,
// the compositor's with source.send.

import (
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"sync"
	"time"

	"golang.org/x/sys/unix"
)

const (
	// *_data_control_manager_v1 requests
	dcManagerCreateSource = 0
	dcManagerGetDevice    = 1
	// *_data_control_device_v1 requests and events
	dcDeviceSetSelection    = 0
	dcDeviceDataOffer       = 0
	dcDeviceSelection       = 1
	dcDeviceFinished        = 2
	dcDevicePrimarySelected = 3
	// *_data_control_source_v1 requests and events
	dcSourceOffer     = 0
	dcSourceDestroy   = 1
	dcSourceSend      = 0
	dcSourceCancelled = 1
	// *_data_control_offer_v1 requests and events
	dcOfferReceive = 0
	dcOfferDestroy = 1
	dcOfferOffer   = 0
)

// Text types, best first: what we ask an offer for, and what we offer.
var clipMimes = []string{"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"}

type dataControl struct {
	conn  *net.UnixConn
	wmu   sync.Mutex // writes (requests from several goroutines)
	mu    sync.Mutex // the state below
	next  uint32
	iface string // "ext_data_control_manager_v1" or "zwlr_data_control_manager_v1"

	registry, manager, seat, device uint32
	offers                          map[uint32][]string // offer id -> mimes
	sources                         map[uint32]string   // our source id -> its text
	fds                             []int               // received, not yet taken by an event
	selections                      int                 // selection events seen
	changed                         func(string, bool)
}

// dialDataControl connects and binds data-control, or says the compositor has none.
func dialDataControl(path string) (*dataControl, error) {
	c, err := net.Dial("unix", path)
	if err != nil {
		return nil, err
	}
	d := &dataControl{conn: c.(*net.UnixConn), next: 2, offers: map[uint32][]string{}, sources: map[uint32]string{}}
	d.registry = d.newID()
	cb := d.newID()
	msgs := message(wlDisplayID, wlDisplayGetRegistry, putUint(nil, d.registry))
	msgs = append(msgs, message(wlDisplayID, wlDisplaySync, putUint(nil, cb))...)
	if err := d.write(msgs, -1); err != nil {
		c.Close()
		return nil, err
	}
	// Globals until the sync's callback: is there a data-control manager and a seat?
	var ext, wlr, seat uint32
	var wlrVersion uint32
	_ = d.conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	err = d.readEvents(func(obj uint32, op uint16, a *args) (bool, error) {
		switch {
		case obj == d.registry && op == wlRegistryGlobal:
			name, iface, version := a.uint(), a.string(), a.uint()
			switch iface {
			case "ext_data_control_manager_v1":
				ext = name
			case "zwlr_data_control_manager_v1":
				wlr, wlrVersion = name, version
			case "wl_seat":
				if seat == 0 {
					seat = name
				}
			}
		case obj == cb && op == wlCallbackDone:
			return true, nil
		case obj == wlDisplayID && op == wlDisplayError:
			a.uint()
			code := a.uint()
			return true, fmt.Errorf("protocol error %d: %s", code, a.string())
		}
		return false, a.err
	})
	_ = d.conn.SetReadDeadline(time.Time{})
	if err != nil {
		c.Close()
		return nil, err
	}
	if seat == 0 || (ext == 0 && wlr == 0) {
		c.Close()
		return nil, errors.New("the compositor has no data-control (GNOME doesn't)")
	}
	var mgr uint32
	var ver uint32 = 1
	if ext != 0 {
		d.iface, mgr = "ext_data_control_manager_v1", ext
	} else {
		d.iface, mgr, ver = "zwlr_data_control_manager_v1", wlr, min(wlrVersion, 2)
	}
	d.manager, d.seat, d.device = d.newID(), d.newID(), d.newID()
	msgs = message(d.registry, wlRegistryBind, bindArgs(mgr, d.iface, ver, d.manager))
	msgs = append(msgs, message(d.registry, wlRegistryBind, bindArgs(seat, "wl_seat", 1, d.seat))...)
	a := putUint(nil, d.device)
	a = putUint(a, d.seat)
	msgs = append(msgs, message(d.manager, dcManagerGetDevice, a)...)
	if err := d.write(msgs, -1); err != nil {
		c.Close()
		return nil, err
	}
	return d, nil
}

func bindArgs(name uint32, iface string, version, id uint32) []byte {
	a := putUint(nil, name)
	a = putString(a, iface)
	a = putUint(a, version)
	return putUint(a, id)
}

func (d *dataControl) name() string { return "Wayland " + d.iface }

func (d *dataControl) newID() uint32 { id := d.next; d.next++; return id }

// write sends requests, with one file descriptor beside them (fd >= 0).
func (d *dataControl) write(b []byte, fd int) error {
	d.wmu.Lock()
	defer d.wmu.Unlock()
	return writeWithFD(d.conn, b, fd)
}

func writeWithFD(c *net.UnixConn, b []byte, fd int) error {
	var oob []byte
	if fd >= 0 {
		oob = unix.UnixRights(fd)
	}
	n, oobn, err := c.WriteMsgUnix(b, oob, nil)
	if err != nil {
		return err
	}
	if n != len(b) || oobn != len(oob) {
		return io.ErrShortWrite
	}
	return nil
}

// readEvents reads messages (taking any file descriptors that come with
// them) and calls ev for each until it says stop or the connection ends.
func (d *dataControl) readEvents(ev func(obj uint32, op uint16, a *args) (bool, error)) error {
	buf := make([]byte, 0, 64*1024)
	chunk := make([]byte, 16*1024)
	oob := make([]byte, unix.CmsgSpace(4*28))
	for {
		n, oobn, _, _, err := d.conn.ReadMsgUnix(chunk, oob)
		if oobn > 0 {
			d.takeFDs(oob[:oobn])
		}
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
				stop, err := ev(obj, op, &args{b: buf[8:size]})
				if err != nil {
					return err
				}
				buf = buf[size:]
				if stop {
					return nil
				}
			}
			buf = append(buf[:0:0], buf...)
		}
		if err != nil {
			if errors.Is(err, io.EOF) {
				return errors.New("the compositor closed the connection")
			}
			return err
		}
	}
}

func (d *dataControl) takeFDs(oob []byte) {
	msgs, err := unix.ParseSocketControlMessage(oob)
	if err != nil {
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	for _, m := range msgs {
		if fds, err := unix.ParseUnixRights(&m); err == nil {
			d.fds = append(d.fds, fds...)
		}
	}
}

func (d *dataControl) popFD() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	if len(d.fds) == 0 {
		return -1
	}
	fd := d.fds[0]
	d.fds = d.fds[1:]
	return fd
}

func (d *dataControl) run(changed func(string, bool)) error {
	d.changed = changed
	defer d.conn.Close()
	return d.readEvents(d.event)
}

func (d *dataControl) event(obj uint32, op uint16, a *args) (bool, error) {
	switch {
	case obj == wlDisplayID && op == wlDisplayError:
		a.uint()
		code := a.uint()
		return true, fmt.Errorf("protocol error %d: %s", code, a.string())
	case obj == d.device:
		switch op {
		case dcDeviceDataOffer:
			id := a.uint()
			d.mu.Lock()
			d.offers[id] = nil
			d.mu.Unlock()
		case dcDeviceSelection:
			id := a.uint()
			d.mu.Lock()
			initial := d.selections == 0
			d.selections++
			mimes := d.offers[id]
			d.mu.Unlock()
			if id == 0 {
				return false, a.err // the clipboard is empty
			}
			d.receive(id, pickMime(mimes), initial)
		case dcDevicePrimarySelected:
			if id := a.uint(); id != 0 {
				d.destroyOffer(id) // not the CLIPBOARD
			}
		case dcDeviceFinished:
			return true, errors.New("the data-control device ended")
		}
	case obj >= 0xff000000: // the compositor's objects: offers
		if op == dcOfferOffer {
			mime := a.string()
			d.mu.Lock()
			if _, ok := d.offers[obj]; ok {
				d.offers[obj] = append(d.offers[obj], mime)
			}
			d.mu.Unlock()
		}
	default:
		d.mu.Lock()
		text, ours := d.sources[obj]
		d.mu.Unlock()
		if !ours {
			break
		}
		switch op {
		case dcSourceSend:
			a.string() // the mime: all of ours are the same text
			fd := d.popFD()
			if fd >= 0 {
				go func() {
					f := os.NewFile(uintptr(fd), "clipboard-send")
					_, _ = io.WriteString(f, text)
					f.Close()
				}()
			}
		case dcSourceCancelled:
			d.mu.Lock()
			delete(d.sources, obj)
			d.mu.Unlock()
			_ = d.write(message(obj, dcSourceDestroy, nil), -1)
		}
	}
	return false, a.err
}

// pickMime: the best text type an offer has ("" if none).
func pickMime(offered []string) string {
	for _, want := range clipMimes {
		for _, m := range offered {
			if m == want {
				return m
			}
		}
	}
	return ""
}

func (d *dataControl) destroyOffer(id uint32) {
	d.mu.Lock()
	delete(d.offers, id)
	d.mu.Unlock()
	_ = d.write(message(id, dcOfferDestroy, nil), -1)
}

// receive asks the offer for its text through a pipe, reads it (at most 2 s)
// on a goroutine, then lets the offer go.
func (d *dataControl) receive(offer uint32, mime string, initial bool) {
	if mime == "" {
		d.destroyOffer(offer) // not text
		return
	}
	r, w, err := os.Pipe() // close-on-exec; r is pollable (the deadline)
	if err != nil {
		log.Printf("clipboard: pipe: %v", err)
		d.destroyOffer(offer)
		return
	}
	err = d.write(message(offer, dcOfferReceive, putString(nil, mime)), int(w.Fd()))
	w.Close() // the compositor has its copy
	if err != nil {
		r.Close()
		return
	}
	go func() {
		defer d.destroyOffer(offer)
		defer r.Close()
		_ = r.SetReadDeadline(time.Now().Add(2 * time.Second))
		b, err := io.ReadAll(io.LimitReader(r, clipMax+1))
		if err != nil && len(b) == 0 {
			return
		}
		d.changed(string(b), initial)
	}()
}

// set offers text as the new CLIPBOARD selection.
func (d *dataControl) set(text string) error {
	d.mu.Lock()
	src := d.newID()
	d.sources[src] = text
	d.mu.Unlock()
	msgs := message(d.manager, dcManagerCreateSource, putUint(nil, src))
	for _, m := range clipMimes {
		msgs = append(msgs, message(src, dcSourceOffer, putString(nil, m))...)
	}
	msgs = append(msgs, message(d.device, dcDeviceSetSelection, putUint(nil, src))...)
	return d.write(msgs, -1)
}
