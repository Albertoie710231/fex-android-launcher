/*
 * Fake libxcb.so.1 - Stub XCB library for headless Vulkan rendering
 *
 * This library provides stub implementations of XCB functions so that
 * Vulkan applications like vkcube can run without a real X11 server.
 *
 * Usage: Put in LD_LIBRARY_PATH before the real libxcb.so.1
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// XCB types
typedef struct xcb_connection_t {
    int fd;
    int has_error;
    char padding[1024];
} xcb_connection_t;

typedef struct {
    uint32_t root;
    uint32_t default_colormap;
    uint32_t white_pixel;
    uint32_t black_pixel;
    uint16_t width_in_pixels;
    uint16_t height_in_pixels;
    uint16_t width_in_millimeters;
    uint16_t height_in_millimeters;
    uint8_t root_depth;
    uint8_t allowed_depths_len;
} xcb_screen_t;

typedef struct {
    xcb_screen_t *data;
    int rem;
    int index;
} xcb_screen_iterator_t;

typedef struct {
    uint8_t status;
    uint8_t pad0;
    uint16_t protocol_major_version;
    uint16_t protocol_minor_version;
    uint16_t length;
    uint32_t release_number;
    uint32_t resource_id_base;
    uint32_t resource_id_mask;
    uint32_t motion_buffer_size;
    uint16_t vendor_len;
    uint16_t maximum_request_length;
    uint8_t roots_len;
    uint8_t pixmap_formats_len;
} xcb_setup_t;

typedef struct { unsigned int sequence; } xcb_void_cookie_t;
typedef struct { unsigned int sequence; } xcb_intern_atom_cookie_t;
typedef struct { uint8_t response_type; uint32_t atom; } xcb_intern_atom_reply_t;
typedef struct { uint8_t response_type; uint8_t depth; uint16_t sequence; uint32_t length; uint32_t root; int16_t x; int16_t y; uint16_t width; uint16_t height; } xcb_get_geometry_reply_t;
typedef struct { unsigned int sequence; } xcb_get_geometry_cookie_t;

// Static fake data
static xcb_connection_t fake_conn = { .fd = 3, .has_error = 0 };
static xcb_screen_t fake_screen = {
    .root = 0x123,
    .default_colormap = 0x456,
    .white_pixel = 0xFFFFFF,
    .black_pixel = 0x000000,
    .width_in_pixels = 1920,
    .height_in_pixels = 1080,
    .width_in_millimeters = 508,
    .height_in_millimeters = 286,
    .root_depth = 24,
    .allowed_depths_len = 1
};
static xcb_setup_t fake_setup = {
    .status = 1,
    .protocol_major_version = 11,
    .protocol_minor_version = 0,
    .length = 0,
    .roots_len = 1
};

// Main connection function
xcb_connection_t* xcb_connect(const char *displayname, int *screenp) {
    fprintf(stderr, "[FakeXCB] xcb_connect('%s') -> fake connection\n",
            displayname ? displayname : ":0");
    if (screenp) *screenp = 0;
    return &fake_conn;
}

void xcb_disconnect(xcb_connection_t *c) {
    (void)c;
    fprintf(stderr, "[FakeXCB] xcb_disconnect()\n");
}

int xcb_connection_has_error(xcb_connection_t *c) {
    (void)c;
    return 0;
}

const xcb_setup_t* xcb_get_setup(xcb_connection_t *c) {
    (void)c;
    return &fake_setup;
}

xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t *R) {
    (void)R;
    xcb_screen_iterator_t iter = { .data = &fake_screen, .rem = 1, .index = 0 };
    return iter;
}

void xcb_screen_next(xcb_screen_iterator_t *i) {
    if (i && i->rem > 0) { i->rem--; i->index++; }
}

uint32_t xcb_generate_id(xcb_connection_t *c) {
    static uint32_t next_id = 0x1000;
    (void)c;
    return next_id++;
}

xcb_void_cookie_t xcb_create_window(xcb_connection_t *c, uint8_t depth,
    uint32_t wid, uint32_t parent, int16_t x, int16_t y,
    uint16_t width, uint16_t height, uint16_t border_width,
    uint16_t _class, uint32_t visual, uint32_t value_mask,
    const void *value_list) {
    (void)c; (void)depth; (void)wid; (void)parent; (void)x; (void)y;
    (void)width; (void)height; (void)border_width; (void)_class;
    (void)visual; (void)value_mask; (void)value_list;
    fprintf(stderr, "[FakeXCB] xcb_create_window(%ux%u)\n", width, height);
    xcb_void_cookie_t cookie = { .sequence = 1 };
    return cookie;
}

xcb_void_cookie_t xcb_create_window_checked(xcb_connection_t *c, uint8_t depth,
    uint32_t wid, uint32_t parent, int16_t x, int16_t y,
    uint16_t width, uint16_t height, uint16_t border_width,
    uint16_t _class, uint32_t visual, uint32_t value_mask,
    const void *value_list) {
    return xcb_create_window(c, depth, wid, parent, x, y, width, height,
        border_width, _class, visual, value_mask, value_list);
}

xcb_void_cookie_t xcb_map_window(xcb_connection_t *c, uint32_t window) {
    (void)c; (void)window;
    fprintf(stderr, "[FakeXCB] xcb_map_window()\n");
    xcb_void_cookie_t cookie = { .sequence = 2 };
    return cookie;
}

xcb_void_cookie_t xcb_map_window_checked(xcb_connection_t *c, uint32_t window) {
    return xcb_map_window(c, window);
}

xcb_void_cookie_t xcb_destroy_window(xcb_connection_t *c, uint32_t window) {
    (void)c; (void)window;
    xcb_void_cookie_t cookie = { .sequence = 3 };
    return cookie;
}

int xcb_flush(xcb_connection_t *c) {
    (void)c;
    return 1;
}

void* xcb_poll_for_event(xcb_connection_t *c) {
    (void)c;
    static int poll_count = 0;
    if (poll_count < 20) {
        fprintf(stderr, "[FakeXCB] xcb_poll_for_event (call #%d)\n", poll_count);
    }
    poll_count++;
    return NULL;
}

void* xcb_wait_for_event(xcb_connection_t *c) {
    (void)c;
    static int wait_count = 0;
    if (wait_count < 20 || (wait_count % 60 == 0)) {
        fprintf(stderr, "[FakeXCB] xcb_wait_for_event (call #%d) - THIS IS THE BLOCKING VERSION!\n", wait_count);
        fflush(stderr);
    }
    wait_count++;
    // Block briefly to simulate waiting
    usleep(16000); // ~60fps
    return NULL;
}

int xcb_get_file_descriptor(xcb_connection_t *c) {
    (void)c;
    return 3;
}

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t *c, uint8_t only_if_exists,
    uint16_t name_len, const char *name) {
    (void)c; (void)only_if_exists; (void)name_len; (void)name;
    xcb_intern_atom_cookie_t cookie = { .sequence = 10 };
    return cookie;
}

xcb_intern_atom_cookie_t xcb_intern_atom_unchecked(xcb_connection_t *c, uint8_t only_if_exists,
    uint16_t name_len, const char *name) {
    return xcb_intern_atom(c, only_if_exists, name_len, name);
}

xcb_intern_atom_reply_t* xcb_intern_atom_reply(xcb_connection_t *c,
    xcb_intern_atom_cookie_t cookie, void **e) {
    (void)c; (void)cookie;
    if (e) *e = NULL;
    // IMPORTANT: Return malloc'd memory - caller will free() this!
    xcb_intern_atom_reply_t* reply = malloc(sizeof(xcb_intern_atom_reply_t));
    if (reply) {
        reply->response_type = 1;
        reply->atom = 1;
    }
    return reply;
}

xcb_void_cookie_t xcb_change_property(xcb_connection_t *c, uint8_t mode,
    uint32_t window, uint32_t property, uint32_t type, uint8_t format,
    uint32_t data_len, const void *data) {
    (void)c; (void)mode; (void)window; (void)property;
    (void)type; (void)format; (void)data_len; (void)data;
    xcb_void_cookie_t cookie = { .sequence = 20 };
    return cookie;
}

xcb_void_cookie_t xcb_change_property_checked(xcb_connection_t *c, uint8_t mode,
    uint32_t window, uint32_t property, uint32_t type, uint8_t format,
    uint32_t data_len, const void *data) {
    return xcb_change_property(c, mode, window, property, type, format, data_len, data);
}

void* xcb_request_check(xcb_connection_t *c, xcb_void_cookie_t cookie) {
    (void)c; (void)cookie;
    return NULL; // No error
}

xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t *c, uint32_t drawable) {
    (void)c; (void)drawable;
    xcb_get_geometry_cookie_t cookie = { .sequence = 30 };
    return cookie;
}

xcb_get_geometry_cookie_t xcb_get_geometry_unchecked(xcb_connection_t *c, uint32_t drawable) {
    return xcb_get_geometry(c, drawable);
}

xcb_get_geometry_reply_t* xcb_get_geometry_reply(xcb_connection_t *c,
    xcb_get_geometry_cookie_t cookie, void **e) {
    (void)c; (void)cookie;
    if (e) *e = NULL;
    // IMPORTANT: Return malloc'd memory - caller will free() this!
    xcb_get_geometry_reply_t* reply = malloc(sizeof(xcb_get_geometry_reply_t));
    if (reply) {
        reply->response_type = 1;
        reply->depth = 24;
        reply->sequence = 30;
        reply->length = 0;
        reply->root = 0x123;
        reply->x = 0;
        reply->y = 0;
        reply->width = 1920;
        reply->height = 1080;
    }
    return reply;
}

void xcb_discard_reply(xcb_connection_t *c, unsigned int sequence) {
    (void)c; (void)sequence;
}

// xcb_configure_window - needed by vkcube for window resizing
xcb_void_cookie_t xcb_configure_window(xcb_connection_t *c, uint32_t window,
    uint16_t value_mask, const void *value_list) {
    (void)c; (void)window; (void)value_mask; (void)value_list;
    xcb_void_cookie_t cookie = { .sequence = 40 };
    return cookie;
}


// Constructor to log when library is loaded
__attribute__((constructor))
static void init(void) {
    fprintf(stderr, "[FakeXCB] Fake libxcb.so.1 loaded - X11 calls will be stubbed\n");
}
