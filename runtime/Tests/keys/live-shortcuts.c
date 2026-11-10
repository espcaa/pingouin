/*
 * This Mac's shortcut list as macOS's window server has it (its defaults and
 * the user's changes in com.apple.symbolichotkeys): id, enabled, Mac key code,
 * modifiers, name. Read only; prints nothing where there is no window server.
 */
#include <stdio.h>
#include <CoreGraphics/CoreGraphics.h>

extern CGError CGSGetSymbolicHotKeyValue(int hotkey, unsigned short *ascii, unsigned short *keycode,
                                         unsigned int *modifiers);
extern bool CGSIsSymbolicHotKeyEnabled(int hotkey);

int main(void)
{
    for (int i = 0; i < 512; i++) {
        unsigned short ascii = 0, keycode = 0;
        unsigned int modifiers = 0;
        if (CGSGetSymbolicHotKeyValue(i, &ascii, &keycode, &modifiers) != kCGErrorSuccess) continue;
        printf("%d\t%d\t%u\t0x%x\tmacOS shortcut %d\n", i, CGSIsSymbolicHotKeyEnabled(i), keycode, modifiers, i);
    }
    return 0;
}
