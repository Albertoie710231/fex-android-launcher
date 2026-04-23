package com.winlator.winhandler;

/** Wine MOUSEEVENTF_* flag values + getFlagFor helper (called by InputDeviceManager). */
public final class MouseEventFlags {
    public static final int MOVE      = 0x0001;
    public static final int LEFTDOWN  = 0x0002;
    public static final int LEFTUP    = 0x0004;
    public static final int RIGHTDOWN = 0x0008;
    public static final int RIGHTUP   = 0x0010;
    public static final int MIDDLEDOWN= 0x0020;
    public static final int MIDDLEUP  = 0x0040;
    public static final int WHEEL     = 0x0800;
    public static final int XDOWN     = 0x0080;
    public static final int XUP       = 0x0100;
    public static final int ABSOLUTE  = 0x8000;
    private MouseEventFlags() {}
    /** Translate X11 pointer button (1=left,2=middle,3=right,4/5=wheel) + pressed→MOUSEEVENTF_ flags. */
    public static int getFlagFor(int button, boolean pressed) {
        switch (button) {
            case 1: return pressed ? LEFTDOWN   : LEFTUP;
            case 2: return pressed ? MIDDLEDOWN : MIDDLEUP;
            case 3: return pressed ? RIGHTDOWN  : RIGHTUP;
            case 4: case 5: return WHEEL;
            default: return 0;
        }
    }
    /** Overload for Pointer.Button enum used by InputDeviceManager. */
    public static int getFlagFor(com.winlator.xserver.Pointer.Button button, boolean pressed) {
        if (button == null) return 0;
        switch (button) {
            case BUTTON_LEFT:         return pressed ? LEFTDOWN   : LEFTUP;
            case BUTTON_MIDDLE:       return pressed ? MIDDLEDOWN : MIDDLEUP;
            case BUTTON_RIGHT:        return pressed ? RIGHTDOWN  : RIGHTUP;
            case BUTTON_SCROLL_UP:    return WHEEL;
            case BUTTON_SCROLL_DOWN:  return WHEEL;
            default: return 0;
        }
    }
}
