#pragma once

#include <stdint.h>
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon.h>

// ── Wayland globals ───────────────────────────────────────────────────────────
extern struct wl_display    *wl_disp;
extern struct wl_registry   *wl_reg;
extern struct wl_compositor *wl_comp;
extern struct wl_shm        *wl_shm;
extern struct wl_seat        *wl_seat_obj;
extern struct wl_pointer    *wl_ptr;
extern struct wl_keyboard   *wl_kbd;

// zwlr protocols
extern struct zwlr_layer_shell_v1      *layer_shell;
extern struct zwlr_screencopy_manager_v1 *screencopy_mgr;

// Output (first monitor)
extern struct wl_output *wl_out;
extern int W, H;          // screen dimensions

// Overlay surface (layer-shell, OVERLAY layer)
extern struct wl_surface              *overlay_surface;
extern struct zwlr_layer_surface_v1   *layer_surface;

// Cairo drawing context for the overlay
extern cairo_surface_t *cr_surface;
extern cairo_t         *cr;

// xkb state for keyboard input
extern struct xkb_context *xkb_ctx;
extern struct xkb_keymap  *xkb_map;
extern struct xkb_state   *xkb_state;

// ── Keyboard focus tracking ──────────────────────────────────────────────────
// 1 once our overlay surface has received wl_keyboard.enter, 0 after leave.
int wutil_has_keyboard_focus(void);
// Block (dispatching events) until focus is confirmed or max_ms elapses.
// Returns 1 if focus was confirmed, 0 on timeout.
int wutil_wait_keyboard_focus(int max_ms);

// ── Screen capture ────────────────────────────────────────────────────────────
// Captures the first output; caller owns the returned surface.
cairo_surface_t *wutil_capture_screen(void);

// ── Lifecycle ─────────────────────────────────────────────────────────────────
int  wutil_init(void);                  // connect, discover globals, get W/H
int  wutil_create_overlay(void);        // map fullscreen overlay on layer-shell
void wutil_present(cairo_surface_t *s);          // submit back buffer; throttles to compositor rate
void wutil_present_if_pending(void);             // flush a pending present after frame/release events
void wutil_set_cursor_image(const char *name); // set named cursor on pointer
void wutil_cleanup(void);

// ── Event dispatch ────────────────────────────────────────────────────────────
// Process pending Wayland events (non-blocking).
void wutil_dispatch(void);
// Block until at least one event arrives.
void wutil_dispatch_blocking(void);
