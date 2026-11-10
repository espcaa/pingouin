package main

import (
	"bufio"
	"errors"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func main() {
	log.SetFlags(0)
	log.SetPrefix("pingouind: ")
	if len(os.Args) > 1 && (os.Args[1] == "-v" || os.Args[1] == "--version") {
		fmt.Println("pingouind", version)
		return
	}
	// The display helper runs as the logged-in user (no uinput needed).
	if len(os.Args) > 1 && os.Args[1] == "displays" {
		runDisplays()
		return
	}

	padDev, err := newDevice(deviceSpec{
		// Apple vendor id + Magic Trackpad (USB-C) product id: libinput applies
		// its Apple touchpad handling. BUTTONPAD = clickpad.
		name: "Apple Inc. Magic Trackpad (Pingouin)",
		id:   inputID{Bustype: busUSB, Vendor: 0x05ac, Product: 0x0324, Version: 0x0111},
		keys: append([]uint16{btnLeft, btnTouch}, tools[:]...),
		abs: []absAxis{
			{absX, 0, padW, padRes}, {absY, 0, padH, padRes},
			{absMtSlot, 0, maxSlots - 1, 0},
			{absMtTrackingID, 0, 65535, 0},
			{absMtPositionX, 0, padW, padRes}, {absMtPositionY, 0, padH, padRes},
		},
		props: []uint16{inputPropPointer, inputPropButtonpad},
	})
	if err != nil {
		log.Fatal(err)
	}
	keyCodes := make([]uint16, 0, 248)
	for k := uint16(1); k < 249; k++ {
		keyCodes = append(keyCodes, k)
	}
	keyDev, err := newDevice(deviceSpec{name: "Pingouin keyboard (Mac shortcuts)", keys: keyCodes})
	if err != nil {
		padDev.Close()
		log.Fatal(err)
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		padDev.Close()
		keyDev.Close()
		os.Exit(0)
	}()

	pad, keys := newTouchpad(padDev), newKeys(keyDev)
	log.Printf("%s: virtual touchpad ready, waiting for Pingouin.app on %s", version, serialPort)
	serve(pad, keys)
}

const serialPort = "/dev/virtio-ports/org.pingouin.gestures"

// serialSession reads the app's virtio-serial channel. While the app's end
// isn't connected, reads return EOF at once, so an EOF means "wait and retry".
func serialSession(pad *Touchpad, keys *Keys, logf func(string, ...any)) error {
	f, err := os.OpenFile(serialPort, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	r := bufio.NewReaderSize(f, 64*1024)
	if _, err := r.Peek(1); err != nil {
		return errNotConnected
	}
	logf("connected to Pingouin.app over %s", serialPort)
	return pump(r, f, pad, keys, logf)
}

var errNotConnected = errors.New("Pingouin.app isn't connected yet")

// serve reconnects for good, with one log line per outage.
func serve(pad *Touchpad, keys *Keys) {
	quiet := false
	for {
		connected := false
		err := serialSession(pad, keys, func(f string, a ...any) {
			if strings.HasPrefix(f, "connected") {
				connected = true
			}
			log.Printf(f, a...)
		})
		pad.Release()
		keys.Release()
		if connected {
			quiet = false
		}
		if !quiet {
			log.Printf("%v; waiting", err)
			quiet = true
		}
		time.Sleep(time.Second)
	}
}
