package main

// The Mac side's line protocol, read from the app's virtio-serial channel
// (/dev/virtio-ports/org.pingouin.gestures). Only the app holds the other end.
//   F <n> [<id> <x> <y> <size>]...  finger frame, x/y 0..1, y down
//   O <natural> <w> <h>             trackpad size in 1/100 mm
//   K <code> <value>                a key on the virtual keyboard (Cmd as Super)
//   S <on|off|esc>                  capture state: anything but "on" lets go
//   Q                               which display is focused? -> "D -" (one display)

import (
	"bufio"
	"fmt"
	"io"
	"strconv"
	"strings"
	"time"
)

// pump reads lines until the stream ends, flushing coalesced frames on time.
func pump(r *bufio.Reader, w io.Writer, pad *Touchpad, keys *Keys, logf func(string, ...any)) error {
	lines := make(chan string, 256)
	readErr := make(chan error, 1)
	go func() {
		for {
			line, err := r.ReadString('\n')
			if err != nil {
				readErr <- err
				close(lines)
				return
			}
			lines <- line
		}
	}()

	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	for {
		if pad.Pending() {
			timer.Reset(minGap)
		}
		select {
		case line, ok := <-lines:
			if !ok {
				return fmt.Errorf("the Mac side went away: %w", <-readErr)
			}
			if err := handle(strings.Fields(line), w, pad, keys, logf); err != nil {
				return err
			}
		case <-timer.C:
		}
		if err := pad.Flush(); err != nil {
			return err
		}
	}
}

// handle acts on one line. Malformed lines are logged and skipped; only
// device write errors are returned.
func handle(p []string, w io.Writer, pad *Touchpad, keys *Keys, logf func(string, ...any)) error {
	if len(p) == 0 {
		return nil
	}
	bad := func() error { logf("skipped a malformed line: %.80q", strings.Join(p, " ")); return nil }
	switch p[0] {
	case "F":
		n, err := strconv.Atoi(get(p, 1))
		if err != nil || n < 0 || len(p) < 2+4*n {
			return bad()
		}
		cs := make([]contact, 0, n)
		for i := 0; i < n; i++ {
			id, e1 := strconv.Atoi(p[2+4*i])
			x, e2 := strconv.ParseFloat(p[3+4*i], 64)
			y, e3 := strconv.ParseFloat(p[4+4*i], 64)
			if e1 != nil || e2 != nil || e3 != nil {
				return bad()
			}
			px, py := pad.Point(x, y)
			cs = append(cs, contact{id, px, py})
		}
		return pad.Frame(cs)
	case "O":
		w, e1 := strconv.Atoi(get(p, 2))
		h, e2 := strconv.Atoi(get(p, 3))
		if e1 != nil || e2 != nil {
			return bad()
		}
		if pad.SetSize(w, h) {
			logf("Mac trackpad %d x %d mm", w/padRes, h/padRes)
		}
	case "K":
		code, e1 := strconv.Atoi(get(p, 1))
		val, e2 := strconv.Atoi(get(p, 2))
		if e1 != nil || e2 != nil {
			return bad()
		}
		return keys.Key(code, val)
	case "S":
		state := get(p, 1)
		logf("capture %s", state)
		if state != "on" {
			if err := pad.Release(); err != nil {
				return err
			}
			return keys.Release()
		}
	case "Q":
		_, err := w.Write([]byte("D -\n"))
		return err
	}
	return nil
}

func get(p []string, i int) string {
	if i < len(p) {
		return p[i]
	}
	return ""
}
