package main

// Virtual input devices through /dev/uinput, with plain ioctls (no cgo, no
// libevdev). Uses the modern setup API (UI_DEV_SETUP / UI_ABS_SETUP, Linux 4.5+).

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"unsafe"

	"golang.org/x/sys/unix"
)

// linux/uinput.h, _IO/_IOW('U', nr, size)
const (
	uiDevCreate  = 0x5501
	uiDevDestroy = 0x5502
	uiDevSetup   = 0x405c5503 // struct uinput_setup, 92 bytes
	uiAbsSetup   = 0x401c5504 // struct uinput_abs_setup, 28 bytes
	uiSetEvbit   = 0x40045564
	uiSetKeybit  = 0x40045565
	uiSetAbsbit  = 0x40045567
	uiSetPropbit = 0x4004556e
)

type inputID struct{ Bustype, Vendor, Product, Version uint16 }

type uinputSetup struct {
	ID           inputID
	Name         [80]byte
	FFEffectsMax uint32
}

type absInfo struct{ Value, Minimum, Maximum, Fuzz, Flat, Resolution int32 }

type uinputAbsSetup struct {
	Code uint16
	_    uint16
	Info absInfo
}

type absAxis struct {
	code     uint16
	min, max int32
	res      int32
}

type deviceSpec struct {
	name  string
	id    inputID
	keys  []uint16
	abs   []absAxis
	props []uint16
}

// Device is one uinput device. Writes are buffered until Sync.
type Device struct {
	f   *os.File
	buf bytes.Buffer
}

func ioctl(fd uintptr, req uintptr, arg uintptr) error {
	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, fd, req, arg); errno != 0 {
		return errno
	}
	return nil
}

func newDevice(s deviceSpec) (*Device, error) {
	f, err := os.OpenFile("/dev/uinput", os.O_WRONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("open /dev/uinput (needs root): %w", err)
	}
	fd := f.Fd()
	fail := func(what string, err error) (*Device, error) {
		f.Close()
		return nil, fmt.Errorf("uinput %s: %w", what, err)
	}
	if len(s.keys) > 0 {
		if err := ioctl(fd, uiSetEvbit, evKey); err != nil {
			return fail("EV_KEY", err)
		}
		for _, k := range s.keys {
			if err := ioctl(fd, uiSetKeybit, uintptr(k)); err != nil {
				return fail(fmt.Sprintf("key %#x", k), err)
			}
		}
	}
	if len(s.abs) > 0 {
		if err := ioctl(fd, uiSetEvbit, evAbs); err != nil {
			return fail("EV_ABS", err)
		}
		for _, a := range s.abs {
			if err := ioctl(fd, uiSetAbsbit, uintptr(a.code)); err != nil {
				return fail(fmt.Sprintf("abs %#x", a.code), err)
			}
			as := uinputAbsSetup{Code: a.code, Info: absInfo{Minimum: a.min, Maximum: a.max, Resolution: a.res}}
			if err := ioctl(fd, uiAbsSetup, uintptr(unsafe.Pointer(&as))); err != nil {
				return fail(fmt.Sprintf("abs setup %#x", a.code), err)
			}
		}
	}
	for _, p := range s.props {
		if err := ioctl(fd, uiSetPropbit, uintptr(p)); err != nil {
			return fail(fmt.Sprintf("prop %#x", p), err)
		}
	}
	setup := uinputSetup{ID: s.id}
	copy(setup.Name[:len(setup.Name)-1], s.name)
	if err := ioctl(fd, uiDevSetup, uintptr(unsafe.Pointer(&setup))); err != nil {
		return fail("setup", err)
	}
	if err := ioctl(fd, uiDevCreate, 0); err != nil {
		return fail("create", err)
	}
	return &Device{f: f}, nil
}

// Emit queues one event; the kernel stamps the time.
func (d *Device) Emit(typ, code uint16, value int32) {
	var ev [24]byte // struct input_event on 64-bit: timeval(16) type code value
	binary.LittleEndian.PutUint16(ev[16:], typ)
	binary.LittleEndian.PutUint16(ev[18:], code)
	binary.LittleEndian.PutUint32(ev[20:], uint32(value))
	d.buf.Write(ev[:])
}

// Sync ends the frame and writes everything queued.
func (d *Device) Sync() error {
	d.Emit(evSyn, synReport, 0)
	_, err := d.f.Write(d.buf.Bytes())
	d.buf.Reset()
	return err
}

func (d *Device) Close() error {
	ioctl(d.f.Fd(), uiDevDestroy, 0)
	return d.f.Close()
}

// Compile-time checks that the structs match the kernel's sizes.
var (
	_ = [1]struct{}{}[unsafe.Sizeof(uinputSetup{})-92]
	_ = [1]struct{}{}[unsafe.Sizeof(uinputAbsSetup{})-28]
)
