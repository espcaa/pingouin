package main

// `pingouind displays`: the Mac's QEMU window talks to this over the virtio
// port app.pingouin.display, one JSON object per line.
//   to QEMU:   {"hello":1}             the helper runs (the guest draws its pointer)
//              {"external":true}       extra screens are welcome
//              {"monitors":[{"name":"Virtual-1","x":0,"y":0,"width":1512,"height":982},...]}
//                                      where the desktop put its outputs (logical pixels),
//                                      so the pointer lands on the right screen
//   from QEMU: {"layout":[...],"builtin":"Virtual-1",...}   the Mac's displays:
//                                      the guest arranges its screens the same
//                                      way, at the same scales (arrange.go)
// Runs as the logged-in user (a systemd user service): the desktop's layout
// is read from the compositor (Wayland: xdg-output) or xrandr (X11).

import (
	"bufio"
	"encoding/json"
	"errors"
	"log"
	"os"
	"os/exec"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

const displayPort = "/dev/virtio-ports/app.pingouin.display"

type Monitor struct {
	Name   string `json:"name"`
	X      int    `json:"x"`
	Y      int    `json:"y"`
	Width  int    `json:"width"`
	Height int    `json:"height"`
}

func monitorsLine(ms []Monitor) []byte {
	if ms == nil {
		ms = []Monitor{}
	}
	b, _ := json.Marshal(struct {
		Monitors []Monitor `json:"monitors"`
	}{ms})
	return append(b, '\n')
}

// layoutSource keeps the latest layout and says when it changes.
type layoutSource struct {
	mu      sync.Mutex
	current []Monitor
	known   bool
	updates chan struct{} // buffered 1: "look again"
}

func newLayoutSource() *layoutSource { return &layoutSource{updates: make(chan struct{}, 1)} }

func (s *layoutSource) set(ms []Monitor) {
	s.mu.Lock()
	changed := !s.known || !reflect.DeepEqual(ms, s.current)
	s.current, s.known = ms, true
	s.mu.Unlock()
	if changed {
		select {
		case s.updates <- struct{}{}:
		default:
		}
	}
}

func (s *layoutSource) get() ([]Monitor, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.current, s.known
}

// watchWayland follows the compositor for good (reconnecting if it restarts).
func watchWayland(path string, src *layoutSource) {
	quiet := false
	for {
		var c *wlClient
		var timer *time.Timer
		var tmu sync.Mutex
		changed := func() {
			// debounce: one update ~200 ms after the last change
			tmu.Lock()
			defer tmu.Unlock()
			if timer != nil {
				timer.Stop()
			}
			timer = time.AfterFunc(200*time.Millisecond, func() { src.set(c.Monitors()) })
		}
		var err error
		c, err = dialWayland(path, changed)
		if err == nil {
			log.Printf("displays: reading the desktop's layout from Wayland (%s)", path)
			quiet = false
			err = c.Run()
			c.Close()
		}
		if !quiet {
			log.Printf("displays: %v; retrying", err)
			quiet = true
		}
		time.Sleep(2 * time.Second)
	}
}

// ---- X11 ----

var xrandrConnected = regexp.MustCompile(`^(\S+) connected (?:primary )?(\d+)x(\d+)\+(-?\d+)\+(-?\d+)`)

// parseXrandr reads `xrandr --query`: the connected outputs that are on.
func parseXrandr(out string) []Monitor {
	var ms []Monitor
	for _, line := range strings.Split(out, "\n") {
		m := xrandrConnected.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		w, _ := strconv.Atoi(m[2])
		h, _ := strconv.Atoi(m[3])
		x, _ := strconv.Atoi(m[4])
		y, _ := strconv.Atoi(m[5])
		ms = append(ms, Monitor{Name: m[1], X: x, Y: y, Width: w, Height: h})
	}
	return ms
}

func watchXrandr(src *layoutSource) {
	if _, err := exec.LookPath("xrandr"); err != nil {
		log.Printf("displays: X11 session without xrandr: the layout isn't reported")
		return
	}
	log.Printf("displays: reading the desktop's layout with xrandr (X11)")
	for {
		if out, err := exec.Command("xrandr", "--query").Output(); err == nil {
			src.set(parseXrandr(string(out)))
		}
		time.Sleep(2 * time.Second)
	}
}

// ---- the session with QEMU ----

func runDisplays() {
	if _, err := os.Stat(displayPort); err != nil {
		log.Printf("displays: no %s (the VM has no display channel)", displayPort)
		os.Exit(0)
	}
	src := newLayoutSource()
	arr := newArrangeLoop(pickArranger())
	if path, ok := waylandSocket(); ok {
		go watchWayland(path, src)
	} else if os.Getenv("DISPLAY") != "" {
		go watchXrandr(src)
	} else {
		log.Printf("displays: neither Wayland nor X11 here: the layout isn't reported")
	}

	quiet := false
	for {
		err := displaySession(src, arr.set, func(f string, a ...any) { quiet = false; log.Printf(f, a...) })
		if !quiet {
			log.Printf("displays: %v; waiting", err)
			quiet = true
		}
		time.Sleep(time.Second)
	}
}

var errHostGone = errors.New("Pingouin's window isn't connected")

// displaySession runs one connection: hello, then the layout on every change.
// onLayout gets each layout QEMU sends.
func displaySession(src *layoutSource, onLayout func(MacLayout), logf func(string, ...any)) error {
	f, err := os.OpenFile(displayPort, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	// While the host's end isn't connected, writes fail or reads end at once.
	if _, err := f.Write([]byte("{\"hello\":1}\n{\"external\":true}\n")); err != nil {
		return errHostGone
	}
	readDone := make(chan error, 1)
	go func() {
		r := bufio.NewReader(f)
		logged := false
		for {
			line, err := r.ReadBytes('\n')
			if len(line) > 0 {
				if l, ok := parseLayout(line); ok {
					if !logged {
						logf("displays: QEMU sent the Mac's layout")
						logged = true
					}
					if onLayout != nil {
						onLayout(l)
					}
				}
			}
			if err != nil {
				readDone <- err
				return
			}
		}
	}()

	sent := false
	send := func() error {
		ms, ok := src.get()
		if !ok {
			return nil
		}
		_, err := f.Write(monitorsLine(ms))
		if err == nil && !sent {
			logf("displays: connected; %d screen(s) reported", len(ms))
			sent = true
		}
		return err
	}
	if err := send(); err != nil {
		return errHostGone
	}
	for {
		select {
		case <-src.updates:
			if err := send(); err != nil {
				return errHostGone
			}
		case err := <-readDone:
			if !sent {
				return errHostGone // EOF at once: nobody on the host side
			}
			return err
		}
	}
}
