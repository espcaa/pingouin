package main

// KDE Plasma: kscreen-doctor. `kscreen-doctor -j` says how the outputs are;
// `kscreen-doctor output.<name>.position.<x>,<y> output.<name>.scale.<s>
// output.<name>.priority.1` arranges them (Plasma 6; Plasma 5 has
// output.<name>.primary instead of priority, tried when that fails).
// Positions are logical on Wayland.

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

type kdeOutput struct {
	Name      string `json:"name"`
	Connected bool   `json:"connected"`
	Enabled   bool   `json:"enabled"`
	Pos       struct {
		X int `json:"x"`
		Y int `json:"y"`
	} `json:"pos"`
	Scale    float64 `json:"scale"`
	Priority int     `json:"priority"`
	Primary  bool    `json:"primary"`
}

func parseKscreen(out []byte) ([]kdeOutput, error) {
	var d struct {
		Outputs []kdeOutput `json:"outputs"`
	}
	if err := json.Unmarshal(out, &d); err != nil {
		return nil, err
	}
	return d.Outputs, nil
}

func fmtScale(s float64) string { return strconv.FormatFloat(s, 'f', -1, 64) }

// kscreenArgs: the arguments that arrange the connected outputs like the
// layout (outputs not in it stay as they are). plasma5: primary, not priority.
func kscreenArgs(outs []kdeOutput, l MacLayout, plasma5 bool) []string {
	connected := map[string]bool{}
	for _, o := range outs {
		connected[o.Name] = o.Connected
	}
	var args []string
	for _, p := range placements(l) {
		if !connected[p.Name] {
			continue
		}
		args = append(args,
			"output."+p.Name+".enable",
			fmt.Sprintf("output.%s.position.%d,%d", p.Name, p.X, p.Y),
			"output."+p.Name+".scale."+fmtScale(p.Scale))
		if p.Primary {
			if plasma5 {
				args = append(args, "output."+p.Name+".primary")
			} else {
				args = append(args, "output."+p.Name+".priority.1")
			}
		}
	}
	return args
}

// kdeDiffers: is a connected output of the layout off, elsewhere or at another scale?
func kdeDiffers(outs []kdeOutput, l MacLayout) bool {
	byName := map[string]kdeOutput{}
	for _, o := range outs {
		byName[o.Name] = o
	}
	for _, p := range placements(l) {
		o, ok := byName[p.Name]
		if !ok || !o.Connected {
			continue
		}
		if !o.Enabled || o.Pos.X != p.X || o.Pos.Y != p.Y || abs(o.Scale-p.Scale) > 0.01 {
			return true
		}
		if p.Primary && !(o.Priority == 1 || o.Primary) {
			return true
		}
	}
	return false
}

type kdeArranger struct{}

func newKdeArranger() arranger {
	if !strings.Contains(strings.ToUpper(os.Getenv("XDG_CURRENT_DESKTOP")), "KDE") {
		return nil
	}
	if _, err := exec.LookPath("kscreen-doctor"); err != nil {
		return nil
	}
	return kdeArranger{}
}

func (kdeArranger) name() string { return "KDE" }

func (kdeArranger) apply(l MacLayout) (bool, error) {
	out, err := exec.Command("kscreen-doctor", "-j").Output()
	if err != nil {
		return false, fmt.Errorf("kscreen-doctor -j: %w", err)
	}
	outs, err := parseKscreen(out)
	if err != nil {
		return false, fmt.Errorf("kscreen-doctor -j: %w", err)
	}
	if !kdeDiffers(outs, l) {
		return false, nil
	}
	args := kscreenArgs(outs, l, false)
	if len(args) == 0 {
		return false, nil
	}
	if b, err := exec.Command("kscreen-doctor", args...).CombinedOutput(); err != nil {
		// Plasma 5: no priority
		if b2, err2 := exec.Command("kscreen-doctor", kscreenArgs(outs, l, true)...).CombinedOutput(); err2 != nil {
			return true, fmt.Errorf("kscreen-doctor: %v: %s / %s", err2, strings.TrimSpace(string(b)), strings.TrimSpace(string(b2)))
		}
	}
	return true, nil
}

// watch: kscreen has no simple change signal here; look every 3 s.
func (kdeArranger) watch(f func()) {
	go func() {
		for {
			time.Sleep(3 * time.Second)
			f()
		}
	}()
}
