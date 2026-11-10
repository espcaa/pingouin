/* Test for qemu-darwin-disk-flush-barrier.patch: pingouin_darwin_flush(), taken
 * from the patched block/file-posix.c by test-flush.sh, with a stand-in
 * fcntl() and fdatasync() that record each call.
 *
 *   test-flush CASE   (PINGOUIN_DISK_FLUSH set by test-flush.sh)
 *
 * Cases: barrier, full, fsync (the mode's own call, nothing else), refused
 * (F_BARRIERFSYNC refused: fdatasync, one message for two flushes),
 * full-refused (F_FULLFSYNC refused: F_BARRIERFSYNC), both-refused (full
 * mode, both refused: fdatasync), eopnotsupp (a refusal as EOPNOTSUPP, a
 * different number from ENOTSUP on macOS), eio and full-eio (an I/O error is
 * returned as is, no weaker call after it).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRY_ON_EINTR(expr) (expr)
#define qatomic_xchg(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)

static char calls[256];
static int refuse_full, refuse_barrier, fail_eio, refuse_errno = ENOTSUP;

static void note(const char *c) { strlcat(calls, c, sizeof calls); strlcat(calls, " ", sizeof calls); }
static int stand_in_fcntl(int fd, int cmd)
{
    (void)fd;
    if (cmd == F_FULLFSYNC) {
        note("full");
        if (fail_eio) { errno = EIO; return -1; }
        if (refuse_full) { errno = refuse_errno; return -1; }
        return 0;
    }
    if (cmd == F_BARRIERFSYNC) {
        note("barrier");
        if (fail_eio) { errno = EIO; return -1; }
        if (refuse_barrier) { errno = refuse_errno; return -1; }
        return 0;
    }
    note("other");
    return 0;
}
static int qemu_fdatasync(int fd) { (void)fd; note("fdatasync"); return 0; }
#define fcntl(fd, cmd) stand_in_fcntl(fd, cmd)

#include "flush.inc"

static int fails;
static void expect(const char *what, const char *want, int ret, int want_ret)
{
    if (strcmp(calls, want) != 0 || ret != want_ret) {
        printf("FAIL %s: calls '%s' ret %d, want '%s' ret %d\n", what, calls, ret, want, want_ret);
        fails++;
    } else {
        printf("ok   %s: %s\n", what, calls);
    }
    calls[0] = 0;
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    int r;

    if (strcmp(c, "barrier") == 0) {
        r = pingouin_darwin_flush(3); expect("default", "barrier ", r, 0);
    } else if (strcmp(c, "full") == 0) {
        r = pingouin_darwin_flush(3); expect("full", "full ", r, 0);
    } else if (strcmp(c, "fsync") == 0) {
        r = pingouin_darwin_flush(3); expect("fsync", "fdatasync ", r, 0);
    } else if (strcmp(c, "refused") == 0) {
        refuse_barrier = 1;
        r = pingouin_darwin_flush(3); expect("barrier refused", "barrier fdatasync ", r, 0);
        r = pingouin_darwin_flush(3); expect("barrier refused again", "barrier fdatasync ", r, 0);
    } else if (strcmp(c, "full-refused") == 0) {
        refuse_full = 1;
        r = pingouin_darwin_flush(3); expect("full refused", "full barrier ", r, 0);
    } else if (strcmp(c, "both-refused") == 0) {
        refuse_full = refuse_barrier = 1;
        r = pingouin_darwin_flush(3); expect("both refused", "full barrier fdatasync ", r, 0);
    } else if (strcmp(c, "eopnotsupp") == 0) {
        refuse_barrier = 1; refuse_errno = EOPNOTSUPP;
        r = pingouin_darwin_flush(3); expect("barrier refused (EOPNOTSUPP)", "barrier fdatasync ", r, 0);
    } else if (strcmp(c, "full-eio") == 0) {
        fail_eio = 1;
        r = pingouin_darwin_flush(3);
        int e = errno;
        expect("I/O error, full", "full ", r, -1);
        if (e != EIO) { printf("FAIL I/O error, full: errno %d, want EIO\n", e); fails++; }
    } else if (strcmp(c, "eio") == 0) {
        fail_eio = 1;
        r = pingouin_darwin_flush(3);
        int e = errno;
        expect("I/O error", "barrier ", r, -1);
        if (e != EIO) { printf("FAIL I/O error: errno %d, want EIO\n", e); fails++; }
    } else {
        fprintf(stderr, "usage: test-flush barrier|full|fsync|refused|full-refused|both-refused|eopnotsupp|eio|full-eio\n");
        return 2;
    }
    return fails != 0;
}
