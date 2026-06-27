#pragma once

#include <stdint.h>
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon.h>

extern struct wl_display     *wl_disp;
extern struct wl_registry    *wl_reg;
extern struct wl_compositor  *wl_comp;
extern struct wl_shm         *wl_shm;
extern struct wl_seat        *wl_seat_obj;
extern struct wl_pointer     *wl_ptr;
extern struct wl_keyboard    *wl_kbd;

extern struct zwlr_layer_shell_v1        *layer_shell;
extern struct zwlr_screencopy_manager_v1 *screencopy_mgr;

extern struct wl_output *wl_out;
extern int W, H;

extern struct wl_surface            *overlay_surface;
extern struct zwlr_layer_surface_v1 *layer_surface;

extern cairo_surface_t *cr_surface;
extern cairo_t         *cr;

extern struct xkb_context *xkb_ctx;
extern struct xkb_keymap  *xkb_map;
extern struct xkb_state   *xkb_state;

typedef struct {
    int x, y, button, pressed, is_motion;
} PointerEvent;

int wutil_key_dequeue(uint32_t *sym_out);
int wutil_ptr_dequeue(PointerEvent *ev_out);
int wutil_ptr_x(void);
int wutil_ptr_y(void);

int wutil_has_keyboard_focus(void);
int wutil_wait_keyboard_focus(int max_ms);
int wutil_wait_pointer_focus(int max_ms);

cairo_surface_t *wutil_capture_screen(void);

int  wutil_init(void);
int  wutil_create_overlay(void);
void wutil_present(cairo_surface_t *s);
void wutil_present_if_pending(void);
void wutil_set_cursor_image(const char *name);
void wutil_cleanup(void);

void wutil_dispatch(void);
void wutil_dispatch_blocking(void);
