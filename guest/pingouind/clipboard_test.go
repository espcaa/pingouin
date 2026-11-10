package main

import (
	"encoding/base64"
	"net"
	"os"
	"strings"
	"testing"

	"github.com/jezek/xgb/xproto"
	"golang.org/x/sys/unix"
)

func TestClipLineRoundTrip(t *testing.T) {
	for _, s := range []string{"", "hello", "naïve ⌘ 🐧\nline two", strings.Repeat("x", 1000)} {
		line := string(clipTextLine(s))
		if !strings.HasPrefix(line, "T ") || !strings.HasSuffix(line, "\n") {
			t.Fatalf("line %q", line)
		}
		got, ok, err := parseClipLine(line)
		if !ok || err != nil || got != s {
			t.Fatalf("round trip of %q: %q %v %v", s, got, ok, err)
		}
	}
}

func TestClipLineIgnoresOthers(t *testing.T) {
	for _, l := range []string{"H\n", "X something\n", "\n", "Tfoo\n"} {
		if _, ok, _ := parseClipLine(l); ok {
			t.Fatalf("%q taken as text", l)
		}
	}
	if _, ok, err := parseClipLine("T !!!not base64\n"); !ok || err == nil {
		t.Fatal("bad base64 not reported")
	}
}

func TestClipLineCap(t *testing.T) {
	ok1 := base64.StdEncoding.EncodeToString(make([]byte, clipMax))
	if _, _, err := parseClipLine("T " + ok1 + "\n"); err != nil {
		t.Fatalf("1 MiB refused: %v", err)
	}
	big := base64.StdEncoding.EncodeToString(make([]byte, clipMax+1))
	if _, ok, err := parseClipLine("T " + big + "\n"); !ok || err != errClipTooBig {
		t.Fatalf("over 1 MiB: ok=%v err=%v", ok, err)
	}
}

func TestClipNoEcho(t *testing.T) {
	var s clipState
	s.baseline("at start")
	if s.shouldSend("at start") {
		t.Fatal("the clipboard as it was at start was sent")
	}
	s.fromHost("from mac")
	if s.shouldSend("from mac") {
		t.Fatal("the Mac's text was sent back")
	}
	if !s.shouldSend("copied in linux") {
		t.Fatal("a Linux copy wasn't sent")
	}
	if s.shouldSend("copied in linux") {
		t.Fatal("the same text was sent twice in a row")
	}
	if !s.shouldSend("from mac") {
		t.Fatal("the Mac's old text, copied again in Linux, wasn't sent")
	}
}

func TestPickMime(t *testing.T) {
	if m := pickMime([]string{"image/png", "STRING", "text/plain;charset=utf-8"}); m != "text/plain;charset=utf-8" {
		t.Fatalf("picked %q", m)
	}
	if m := pickMime([]string{"TEXT", "UTF8_STRING"}); m != "UTF8_STRING" {
		t.Fatalf("picked %q", m)
	}
	if m := pickMime([]string{"image/png"}); m != "" {
		t.Fatalf("picked %q for an image", m)
	}
}

func TestX11Targets(t *testing.T) {
	x := &x11Clip{targets: 100, utf8: 101, plain: 102, text: 103}
	got := x.x11Targets()
	want := map[xproto.Atom]bool{100: true, 101: true, 102: true, 103: true, xproto.AtomString: true}
	if len(got) != len(want) {
		t.Fatalf("targets %v", got)
	}
	for _, a := range got {
		if !want[a] {
			t.Fatalf("unexpected target %d in %v", a, got)
		}
	}
}

func unixPair(t *testing.T) (*net.UnixConn, *net.UnixConn) {
	t.Helper()
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM, 0)
	if err != nil {
		t.Fatal(err)
	}
	conn := func(fd int) *net.UnixConn {
		f := os.NewFile(uintptr(fd), "pair")
		c, err := net.FileConn(f)
		f.Close()
		if err != nil {
			t.Fatal(err)
		}
		return c.(*net.UnixConn)
	}
	a, b := conn(fds[0]), conn(fds[1])
	t.Cleanup(func() { a.Close(); b.Close() })
	return a, b
}

// offer.receive(mime, fd): the request's bytes and the fd arrive together.
func TestWriteWithFD(t *testing.T) {
	a, b := unixPair(t)
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	msg := message(0xff000001, dcOfferReceive, putString(nil, "text/plain"))
	if err := writeWithFD(a, msg, int(w.Fd())); err != nil {
		t.Fatal(err)
	}
	w.Close()

	buf := make([]byte, 256)
	oob := make([]byte, unix.CmsgSpace(4))
	n, oobn, _, _, err := b.ReadMsgUnix(buf, oob)
	if err != nil {
		t.Fatal(err)
	}
	if string(buf[:n]) != string(msg) {
		t.Fatalf("bytes % x, want % x", buf[:n], msg)
	}
	cms, err := unix.ParseSocketControlMessage(oob[:oobn])
	if err != nil || len(cms) != 1 {
		t.Fatalf("control messages: %v %v", cms, err)
	}
	fds, err := unix.ParseUnixRights(&cms[0])
	if err != nil || len(fds) != 1 {
		t.Fatalf("rights: %v %v", fds, err)
	}
	// The passed fd is the pipe's write end: write through it, read from r.
	wf := os.NewFile(uintptr(fds[0]), "passed")
	if _, err := wf.Write([]byte("ok")); err != nil {
		t.Fatal(err)
	}
	wf.Close()
	got := make([]byte, 2)
	if _, err := r.Read(got); err != nil || string(got) != "ok" {
		t.Fatalf("through the passed fd: %q %v", got, err)
	}
}

// source.send(mime, fd) from the compositor: readEvents keeps the fd for the event.
func TestReadEventsTakesFDs(t *testing.T) {
	a, b := unixPair(t)
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	d := &dataControl{conn: b, offers: map[uint32][]string{}, sources: map[uint32]string{}}
	ev := message(7, dcSourceSend, putString(nil, "text/plain"))
	if err := writeWithFD(a, ev, int(w.Fd())); err != nil {
		t.Fatal(err)
	}
	w.Close()
	var gotFD = -1
	err = d.readEvents(func(obj uint32, op uint16, args *args) (bool, error) {
		if obj != 7 || op != dcSourceSend || args.string() != "text/plain" {
			t.Fatalf("event %d/%d", obj, op)
		}
		gotFD = d.popFD()
		return true, nil
	})
	if err != nil || gotFD < 0 {
		t.Fatalf("readEvents: %v, fd %d", err, gotFD)
	}
	unix.Close(gotFD)
}
