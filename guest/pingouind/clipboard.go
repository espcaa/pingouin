package main

// pingouind clipboard: the Mac's clipboard and the desktop's, as text, over
// the virtio port app.pingouin.clipboard. Runs as the logged-in user.
//
// Lines (newline-terminated):
//   to the Mac:   H            on every (re)connect
//                 T <base64>   the Linux clipboard changed to this text
//   from the Mac: T <base64>   set the Linux clipboard to this text
// Text is UTF-8, base64 std encoding, at most clipMax bytes once decoded.
// What one side got from the other is never sent back (clipState), and the
// clipboard as it is at connect time is not sent: only changes after that.
//
// Backends (pickClipBackend): Wayland data-control (ext-data-control-v1, else
// wlr-data-control-unstable-v1: KDE, Hyprland, Sway, niri...), else X11
// (GNOME: XWayland mirrors the Wayland clipboard; any X11 session), else none.

import (
	"bufio"
	"encoding/base64"
	"errors"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/sys/unix"
)

const (
	clipPort = "/dev/virtio-ports/app.pingouin.clipboard"
	clipMax  = 1 << 20 // bytes of text, decoded
)

// ---- protocol ----

func clipTextLine(text string) []byte {
	return []byte("T " + base64.StdEncoding.EncodeToString([]byte(text)) + "\n")
}

var errClipTooBig = errors.New("clipboard text over 1 MiB")

// parseClipLine reads a line from the Mac. ok false: not a text line (ignored).
func parseClipLine(line string) (text string, ok bool, err error) {
	line = strings.TrimRight(line, "\r\n")
	if !strings.HasPrefix(line, "T ") {
		return "", false, nil
	}
	enc := line[2:]
	if base64.StdEncoding.DecodedLen(len(enc)) > clipMax+3 {
		return "", true, errClipTooBig
	}
	b, err := base64.StdEncoding.DecodeString(enc)
	if err != nil {
		return "", true, err
	}
	if len(b) > clipMax {
		return "", true, errClipTooBig
	}
	return string(b), true, nil
}

// ---- no echo ----

// clipState is the text both sides are known to share. The Mac's text, once
// set here, comes back as a Linux clipboard change: it isn't sent back; nor
// is the same text twice in a row.
type clipState struct {
	mu     sync.Mutex
	shared string
	known  bool
}

// fromHost: the Mac sent this (and it goes on the Linux clipboard).
func (s *clipState) fromHost(t string) {
	s.mu.Lock()
	s.shared, s.known = t, true
	s.mu.Unlock()
}

// baseline: the Linux clipboard as it was at start (not sent).
func (s *clipState) baseline(t string) { s.fromHost(t) }

// shouldSend: the Linux clipboard changed to t; send it to the Mac?
func (s *clipState) shouldSend(t string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.known && t == s.shared {
		return false
	}
	s.shared, s.known = t, true
	return true
}

// ---- backends ----

// clipBackend is the desktop's clipboard. run reports every change of the
// CLIPBOARD selection (initial: the selection as it was when run started)
// and returns when the connection to the desktop ends.
type clipBackend interface {
	name() string
	run(changed func(text string, initial bool)) error
	set(text string) error
}

type noClipBackend struct{}

func (noClipBackend) name() string { return "none" }
func (noClipBackend) run(func(string, bool)) error {
	select {} // keep the channel open: the Mac still sees H
}
func (noClipBackend) set(string) error { return nil }

func pickClipBackend() clipBackend {
	if path, ok := waylandSocket(); ok {
		if b, err := dialDataControl(path); err == nil {
			return b
		} else {
			log.Printf("clipboard: Wayland: %v", err)
		}
	}
	if os.Getenv("DISPLAY") != "" {
		if b, err := dialX11Clip(); err == nil {
			return b
		} else {
			log.Printf("clipboard: X11: %v", err)
		}
	}
	log.Printf("clipboard: no clipboard access here (no Wayland data-control, no X11)")
	return noClipBackend{}
}

// ---- the helper ----

func runClipboard() {
	if _, err := os.Stat(clipPort); err != nil {
		log.Printf("clipboard: no %s (the VM has no clipboard channel)", clipPort)
		os.Exit(0)
	}
	waitForLock("pingouin-clipboard.lock", "clipboard")

	state := &clipState{}
	out := make(chan string, 1) // the latest Linux change to send
	var backendMu sync.Mutex
	var backend clipBackend = noClipBackend{}

	tooBig := false
	changed := func(text string, initial bool) {
		if len(text) > clipMax {
			if !tooBig {
				log.Printf("clipboard: Linux text over 1 MiB: not sent")
				tooBig = true
			}
			return
		}
		if initial {
			state.baseline(text)
			return
		}
		if !state.shouldSend(text) {
			return
		}
		select {
		case <-out: // an unsent older one goes
		default:
		}
		out <- text
	}

	go func() {
		for {
			b := pickClipBackend()
			backendMu.Lock()
			backend = b
			backendMu.Unlock()
			log.Printf("clipboard: using %s", b.name())
			if err := b.run(changed); err != nil {
				log.Printf("clipboard: %s: %v", b.name(), err)
			}
			time.Sleep(2 * time.Second)
		}
	}()

	setLinux := func(text string) {
		state.fromHost(text)
		backendMu.Lock()
		b := backend
		backendMu.Unlock()
		if err := b.set(text); err != nil {
			log.Printf("clipboard: setting the Linux clipboard: %v", err)
		}
	}

	quiet := false
	for {
		err := clipSession(out, setLinux, func(f string, a ...any) { quiet = false; log.Printf(f, a...) })
		if !quiet {
			log.Printf("clipboard: %v; waiting", err)
			quiet = true
		}
		time.Sleep(time.Second)
	}
}

// clipSession runs one connection to the Mac: H, then text both ways.
func clipSession(out chan string, setLinux func(string), logf func(string, ...any)) error {
	f, err := os.OpenFile(clipPort, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := f.Write([]byte("H\n")); err != nil {
		return errHostGone
	}
	// Not the clipboard as it is now: only changes from here on.
	select {
	case <-out:
	default:
	}

	readDone := make(chan error, 1)
	var got atomic.Bool
	go func() {
		r := bufio.NewReaderSize(f, 64*1024)
		tooBig := false
		for {
			line, err := r.ReadString('\n')
			if len(line) > 0 {
				got.Store(true)
				text, ok, perr := parseClipLine(line)
				switch {
				case !ok:
				case errors.Is(perr, errClipTooBig):
					if !tooBig {
						logf("clipboard: Mac text over 1 MiB: dropped")
						tooBig = true
					}
				case perr != nil:
					logf("clipboard: a bad line from the Mac: %v", perr)
				default:
					setLinux(text)
				}
			}
			if err != nil {
				readDone <- err
				return
			}
		}
	}()

	logf("clipboard: connected to Pingouin")
	for {
		select {
		case text := <-out:
			if _, err := f.Write(clipTextLine(text)); err != nil {
				return errHostGone
			}
		case err := <-readDone:
			if !got.Load() {
				return errHostGone // EOF at once: nobody on the host side
			}
			return err
		}
	}
}

// waitForLock: one helper of a kind per session (the systemd unit and the
// XDG autostart may both start it); the second stands by until the first ends.
func waitForLock(file, what string) {
	dir := os.Getenv("XDG_RUNTIME_DIR")
	if dir == "" {
		dir = os.TempDir()
	}
	f, err := os.OpenFile(filepath.Join(dir, file), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		log.Printf("%s: no lock file (%v): running anyway", what, err)
		return
	}
	if unix.Flock(int(f.Fd()), unix.LOCK_EX|unix.LOCK_NB) != nil {
		log.Printf("%s: another helper runs in this session: standing by", what)
		if err := unix.Flock(int(f.Fd()), unix.LOCK_EX); err != nil {
			log.Printf("%s: lock: %v: running anyway", what, err)
		}
	}
	// f stays open (and locked) for the life of the process.
}
