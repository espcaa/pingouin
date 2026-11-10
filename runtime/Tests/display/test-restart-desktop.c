/* Tests ui/pingouin-desktop-restart.h (pingouin-cocoa-restart-desktop.patch).
 * Usage: test-restart-desktop EMPTY_DIR */
#include "pingouin-desktop-restart.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void touch(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        close(fd);
    }
}

int main(int argc, char **argv)
{
    char lost[1024], target[1024];
    const char *name = "app.pingouin.Pingouin.desktop-restart.4242";

    if (argc != 2) {
        return 2;
    }
    snprintf(lost, sizeof(lost), "%s/desktop-lost", argv[1]);
    snprintf(target, sizeof(target), "%s/elsewhere", argv[1]);

    /* QEMU not started by Pingouin.app: no item at all. */
    unsetenv("PINGOUIN_DESKTOP_LOST");
    unsetenv("PINGOUIN_DESKTOP_RESTART_REQUEST");
    expect(!pingouin_desktop_restart_request(), "no variables: no item");
    expect(!pingouin_desktop_lost(), "no variables: not lost");
    setenv("PINGOUIN_DESKTOP_LOST", lost, 1);
    setenv("PINGOUIN_DESKTOP_RESTART_REQUEST", "", 1);
    touch(lost);
    expect(!pingouin_desktop_restart_request(), "empty request name: no item");
    expect(!pingouin_desktop_lost(), "empty request name: not lost even with the file");
    unlink(lost);
    setenv("PINGOUIN_DESKTOP_LOST", "", 1);
    setenv("PINGOUIN_DESKTOP_RESTART_REQUEST", name, 1);
    expect(!pingouin_desktop_restart_request(), "empty file name: no item");

    /* Started by the app. */
    setenv("PINGOUIN_DESKTOP_LOST", lost, 1);
    expect(pingouin_desktop_restart_request() &&
           !strcmp(pingouin_desktop_restart_request(), name), "both: the app's name");
    expect(!pingouin_desktop_lost(), "no desktop-lost file: item hidden");
    touch(lost);
    expect(pingouin_desktop_lost(), "desktop-lost there: item shown");
    unlink(lost);
    expect(!pingouin_desktop_lost(), "desktop-lost removed (restart, VM stop): hidden again");

    /* Not a plain file: hidden. */
    touch(target);
    expect(symlink(target, lost) == 0, "made a link for desktop-lost");
    expect(!pingouin_desktop_lost(), "desktop-lost as a link: hidden");
    unlink(lost);
    unlink(target);
    expect(mkdir(lost, 0700) == 0, "made a folder for desktop-lost");
    expect(!pingouin_desktop_lost(), "desktop-lost as a folder: hidden");
    rmdir(lost);

    /* The logs folder is gone (VM folder moved): hidden, no crash. */
    setenv("PINGOUIN_DESKTOP_LOST", "/nonexistent-pingouin/logs/desktop-lost", 1);
    expect(!pingouin_desktop_lost(), "missing folder: hidden");

    if (failures) {
        return 1;
    }
    printf("restart desktop menu: all checks passed\n");
    return 0;
}
