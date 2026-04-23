package com.winlator.winhandler;

/**
 * Stub of Winlator's WinHandler. The real thing bridges wine/app side
 * for mouse-relative events, killing processes, window focus, clipboard,
 * gamepad feedback, etc. Our pipeline doesn't need any of that for the
 * rendering path — XServer and friends just hold a reference to it and
 * occasionally call no-critical methods. All no-ops here.
 */
public class WinHandler {
    public void sendMouseEvent(int flags, int dx, int dy, int wheel_delta, boolean cursor_locked) {}
    public void mouseEvent(int flags, int x, int y, int wheelDelta) {}
    public void sendGamepadInfo(String type, int id, boolean connected) {}
    public void bringToFront(String className, long hwnd) {}
    public void exec(String cmd) {}
    public void killProcess(String name) {}
    public void stop() {}
}
