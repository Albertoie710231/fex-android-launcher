// Minimal X11 type stub so that Vulkan's VK_USE_PLATFORM_XLIB_KHR surface
// entrypoints compile on Android NDK without libX11 headers installed. The
// Vortek client's Xlib functions are no-op stubs — they construct surface
// handles from an opaque window id — so we only need Display, Window, and
// VisualID to exist as type names.
#ifndef _VORTEK_X11_XLIB_STUB_H
#define _VORTEK_X11_XLIB_STUB_H

typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;

#endif
