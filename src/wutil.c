/*
 * wutil.c – Wayland display backend for shot-wayland.
 *
 * Replaces xutil.c.  All X11 types/calls are gone; we use:
 *   wl_display / wl_compositor / wl_shm  – core Wayland
 *   zwlr_layer_shell_v1                  – full-screen input-grabbing overlay
 *   zwlr_screencopy_manager_v1           – screen capture
 *   cairo (over wl_shm)                  – 2-D drawing
 *   xkbcommon                            – keyboard symbols
 *   wayland-cursor                       – cursor images
 */

/* Need _GNU_SOURCE for memfd_create */
#define _GNU_SOURCE

#include "wutil.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "../config.h"

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ── Public globals ────────────────────────────────────────────────────────────
struct wl_display    *wl_disp       = NULL;
struct wl_registry   *wl_reg        = NULL;
struct wl_compositor *wl_comp       = NULL;
struct wl_shm        *wl_shm        = NULL;
struct wl_seat       *wl_seat_obj   = NULL;
struct wl_pointer    *wl_ptr        = NULL;
struct wl_keyboard   *wl_kbd        = NULL;

struct zwlr_layer_shell_v1       *layer_shell   = NULL;
struct zwlr_screencopy_manager_v1 *screencopy_mgr = NULL;

struct wl_output *wl_out = NULL;
int W = 0, H = 0;

struct wl_surface            *overlay_surface = NULL;
struct zwlr_layer_surface_v1 *layer_surface   = NULL;

cairo_surface_t *cr_surface = NULL;
cairo_t         *cr         = NULL;

struct xkb_context *xkb_ctx   = NULL;
struct xkb_keymap  *xkb_map   = NULL;
struct xkb_state   *xkb_state = NULL;

// ── Internal: wl_shm anonymous buffer ────────────────────────────────────────
static int create_shm_fd(size_t size) {
    int fd = -1;
#ifdef __linux__
    fd = memfd_create("shot-wayland-shm", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) < 0) { close(fd); fd = -1; }
        return fd;
    }
#endif
    // Fallback: POSIX shm
    char name[64];
    snprintf(name, sizeof(name), "/shot-wayland-%d", getpid());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        shm_unlink(name);
        if (ftruncate(fd, (off_t)size) < 0) { close(fd); fd = -1; }
    }
    return fd;
}

typedef struct {
    struct wl_buffer *buf;
    void             *data;
    size_t            size;
    int               fd;
} ShmBuf;

static ShmBuf *shm_buf_create(int w, int h) {
    ShmBuf *sb = calloc(1, sizeof *sb);
    if (!sb) return NULL;
    sb->size = (size_t)w * h * 4; /* BGRA8888 = 4 bytes */
    sb->fd   = create_shm_fd(sb->size);
    if (sb->fd < 0) { free(sb); return NULL; }

    sb->data = mmap(NULL, sb->size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, sb->fd, 0);
    if (sb->data == MAP_FAILED) {
        close(sb->fd); free(sb); return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, sb->fd, (int32_t)sb->size);
    sb->buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
                                        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return sb;
}

static void shm_buf_destroy(ShmBuf *sb) {
    if (!sb) return;
    wl_buffer_destroy(sb->buf);
    munmap(sb->data, sb->size);
    close(sb->fd);
    free(sb);
}

// ── Output listener (get W, H) ────────────────────────────────────────────────
static void output_geometry(void *data, struct wl_output *out,
    int32_t x, int32_t y, int32_t pw, int32_t ph,
    int32_t sub, const char *make, const char *model, int32_t tfm)
{ (void)data;(void)out;(void)x;(void)y;(void)pw;(void)ph;
  (void)sub;(void)make;(void)model;(void)tfm; }

static void output_mode(void *data, struct wl_output *out,
    uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    (void)data; (void)out; (void)refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT) { W = w; H = h; }
}
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t f) { (void)d;(void)o;(void)f; }

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode     = output_mode,
    .done     = output_done,
    .scale    = output_scale,
};

// ── Seat / keyboard / pointer listeners ───────────────────────────────────────

/* forward-declared; filled in in wutil_init() after xkb is set up */
static int kbd_has_focus = 0; /* 1 once our surface receives wl_keyboard.enter */

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t fmt,
                        int32_t fd, uint32_t size);
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                       struct wl_surface *surf, struct wl_array *keys) {
    (void)d;(void)k;(void)serial;(void)surf;(void)keys;
    kbd_has_focus = 1;
}
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                       struct wl_surface *surf) {
    (void)d;(void)k;(void)serial;(void)surf;
    kbd_has_focus = 0;
}

int wutil_has_keyboard_focus(void) { return kbd_has_focus; }

/* Block (dispatching events) until our surface has keyboard focus, or
 * until max_ms has elapsed — whichever comes first. Returns 1 if focus
 * was confirmed, 0 if we timed out (caller may proceed anyway; some
 * compositors don't send enter promptly even though input still works). */
int wutil_wait_keyboard_focus(int max_ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!kbd_has_focus) {
        wl_display_flush(wl_disp);
        /* Use a short blocking dispatch so we don't spin a busy loop */
        if (wl_display_dispatch(wl_disp) < 0) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
                        + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= max_ms) break;
    }
    return kbd_has_focus;
}

/* Public event queues — select.c reads these */
typedef struct { uint32_t key; uint32_t sym; int pressed; } KeyEvent;
typedef struct { int x; int y; int button; int pressed; int is_motion; } PointerEvent;

#define EVQUEUE_MAX 128
static KeyEvent     key_queue[EVQUEUE_MAX];
static int          key_queue_head = 0, key_queue_tail = 0;
static PointerEvent ptr_queue[EVQUEUE_MAX];
static int          ptr_queue_head = 0, ptr_queue_tail = 0;

static int current_ptr_x = 0, current_ptr_y = 0;

void wutil_key_enqueue(uint32_t sym, int pressed) {
    int next = (key_queue_tail + 1) % EVQUEUE_MAX;
    if (next == key_queue_head) return; /* drop on overflow */
    key_queue[key_queue_tail].sym = sym;
    key_queue[key_queue_tail].pressed = pressed;
    key_queue_tail = next;
}

int wutil_key_dequeue(uint32_t *sym_out) {
    if (key_queue_head == key_queue_tail) return 0;
    *sym_out = key_queue[key_queue_head].sym;
    key_queue_head = (key_queue_head + 1) % EVQUEUE_MAX;
    return 1;
}

void wutil_ptr_enqueue(PointerEvent ev) {
    int next = (ptr_queue_tail + 1) % EVQUEUE_MAX;
    if (next == ptr_queue_head) return;
    ptr_queue[ptr_queue_tail] = ev;
    ptr_queue_tail = next;
}

int wutil_ptr_dequeue(PointerEvent *ev_out) {
    if (ptr_queue_head == ptr_queue_tail) return 0;
    *ev_out = ptr_queue[ptr_queue_head];
    ptr_queue_head = (ptr_queue_head + 1) % EVQUEUE_MAX;
    return 1;
}

int wutil_ptr_x(void) { return current_ptr_x; }
int wutil_ptr_y(void) { return current_ptr_y; }

static void kbd_key(void *d, struct wl_keyboard *k, uint32_t serial,
                     uint32_t time, uint32_t key, uint32_t state)
{
    (void)d;(void)k;(void)serial;(void)time;
    if (!xkb_state) return;
    /* Only queue presses. select.c's dispatch_key()/phase loops only ever
     * act on key-down; queuing releases too meant every keypress left a
     * stale release event sitting in the ring buffer, which would later
     * get dequeued and (harmlessly, but wastefully) checked against every
     * keybind in whatever phase happened to be running by then. */
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    uint32_t sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
    wutil_key_enqueue(sym, 1);
}

static void kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
    uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    (void)d;(void)k;(void)serial;
    if (xkb_state) xkb_state_update_mask(xkb_state, dep, lat, lock, 0, 0, grp);
}
static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)d;(void)k;(void)rate;(void)delay; }

static void kbd_keymap(void *d, struct wl_keyboard *k,
                        uint32_t fmt, int32_t fd, uint32_t size)
{
    (void)d;(void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (str == MAP_FAILED) { close(fd); return; }

    if (xkb_map)   { xkb_keymap_unref(xkb_map);  xkb_map  = NULL; }
    if (xkb_state) { xkb_state_unref(xkb_state); xkb_state = NULL; }

    xkb_map = xkb_keymap_new_from_string(xkb_ctx, str,
                  XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(str, size);
    close(fd);
    if (xkb_map)
        xkb_state = xkb_state_new(xkb_map);
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap     = kbd_keymap,
    .enter      = kbd_enter,
    .leave      = kbd_leave,
    .key        = kbd_key,
    .modifiers  = kbd_modifiers,
    .repeat_info = kbd_repeat,
};

/* Pointer */
static uint32_t last_ptr_serial = 0; /* tracked here; used by wutil_set_cursor_image */

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{ (void)d;(void)p;(void)surf;
  last_ptr_serial = serial;
  current_ptr_x = wl_fixed_to_int(sx);
  current_ptr_y = wl_fixed_to_int(sy); }

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *surf)
{ (void)d;(void)p;(void)serial;(void)surf; }

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy)
{
    (void)d;(void)p;(void)time;
    current_ptr_x = wl_fixed_to_int(sx);
    current_ptr_y = wl_fixed_to_int(sy);
    PointerEvent ev = { current_ptr_x, current_ptr_y, 0, 0, 1 };
    wutil_ptr_enqueue(ev);
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
    uint32_t time, uint32_t btn, uint32_t state)
{
    (void)d;(void)p;(void)serial;(void)time;
    /* Linux BTN_LEFT=0x110 BTN_RIGHT=0x111 → map to 1/3 like X11 */
    int b = (btn == 0x110) ? 1 : (btn == 0x111) ? 3 : (int)(btn - 0x10f);
    PointerEvent ev = { current_ptr_x, current_ptr_y, b,
                        state == WL_POINTER_BUTTON_STATE_PRESSED, 0 };
    wutil_ptr_enqueue(ev);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
    uint32_t axis, wl_fixed_t val)
{ (void)d;(void)p;(void)time;(void)axis;(void)val; }

/* wl_pointer v5 extras — we don't use these but must provide handlers */
static void ptr_frame(void *d, struct wl_pointer *p)
{ (void)d;(void)p; }

static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t src)
{ (void)d;(void)p;(void)src; }

static void ptr_axis_stop(void *d, struct wl_pointer *p,
    uint32_t time, uint32_t axis)
{ (void)d;(void)p;(void)time;(void)axis; }

static void ptr_axis_discrete(void *d, struct wl_pointer *p,
    uint32_t axis, int32_t discrete)
{ (void)d;(void)p;(void)axis;(void)discrete; }

static const struct wl_pointer_listener ptr_listener = {
    .enter         = ptr_enter,
    .leave         = ptr_leave,
    .motion        = ptr_motion,
    .button        = ptr_button,
    .axis          = ptr_axis,
    .frame         = ptr_frame,
    .axis_source   = ptr_axis_source,
    .axis_stop     = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

/* Seat */
static void seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl_ptr) {
        wl_ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl_ptr, &ptr_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl_kbd) {
        wl_kbd = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl_kbd, &kbd_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *seat, const char *name)
{ (void)d;(void)seat;(void)name; }
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

// ── Registry listener ─────────────────────────────────────────────────────────
static void registry_global(void *d, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name))
        wl_comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        wl_seat_obj = wl_registry_bind(reg, name, &wl_seat_interface, 5);
        wl_seat_add_listener(wl_seat_obj, &seat_listener, NULL);
    }
    else if (!strcmp(iface, wl_output_interface.name) && !wl_out) {
        wl_out = wl_registry_bind(reg, name, &wl_output_interface, 2);
        wl_output_add_listener(wl_out, &output_listener, NULL);
    }
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        layer_shell = wl_registry_bind(reg, name,
                          &zwlr_layer_shell_v1_interface,
                          ver < 4 ? ver : 4);
    else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
        screencopy_mgr = wl_registry_bind(reg, name,
                             &zwlr_screencopy_manager_v1_interface, 3);
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d;(void)r;(void)name; }

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

// ── Layer-surface listener ────────────────────────────────────────────────────
static int overlay_configured = 0;

static void layer_surface_configure(void *d,
    struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
    (void)d;
    if (w) W = (int)w;
    if (h) H = (int)h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    overlay_configured = 1;
}
static void layer_surface_closed(void *d, struct zwlr_layer_surface_v1 *ls)
{ (void)d; (void)ls; }
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

// ── Screencopy ────────────────────────────────────────────────────────────────
typedef struct {
    ShmBuf  *sb;
    int      done;
    int      failed;
    int      width;
    int      height;
    int      stride;
    uint32_t fmt;        /* WL_SHM_FORMAT_* as advertised by compositor */
    int      pending_copy;
} CopyState;

/*
 * wl_shm buffer with explicit format + stride (compositor-supplied values).
 * shm_buf_create() hardcodes ARGB8888/w*4 — this variant doesn't.
 */
static ShmBuf *shm_buf_create_fmt(int w, int h, int stride, uint32_t fmt) {
    ShmBuf *sb = calloc(1, sizeof *sb);
    if (!sb) return NULL;
    sb->size = (size_t)stride * h;
    sb->fd   = create_shm_fd(sb->size);
    if (sb->fd < 0) { free(sb); return NULL; }
    sb->data = mmap(NULL, sb->size, PROT_READ | PROT_WRITE, MAP_SHARED, sb->fd, 0);
    if (sb->data == MAP_FAILED) { close(sb->fd); free(sb); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, sb->fd, (int32_t)sb->size);
    sb->buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, fmt);
    wl_shm_pool_destroy(pool);
    return sb;
}

static void copy_buffer(void *d, struct zwlr_screencopy_frame_v1 *frame,
    uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
    CopyState *cs = d;
    (void)frame;
    /* Record what the compositor wants; allocate happens in buffer_done. */
    cs->fmt    = fmt;
    cs->width  = (int)w;
    cs->height = (int)h;
    cs->stride = (int)stride;
    cs->pending_copy = 1;
}

static void copy_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fl)
{ (void)d;(void)f;(void)fl; }

static void copy_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)d;(void)f;(void)x;(void)y;(void)w;(void)h; }

static void copy_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
    uint32_t fmt, uint32_t w, uint32_t h)
{ (void)d;(void)f;(void)fmt;(void)w;(void)h; }

static void copy_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *frame)
{
    CopyState *cs = d;
    /* v3: compositor finished advertising buffer types. Allocate shm buffer
     * using the exact format+stride it told us, then trigger the copy. */
    if (!cs->pending_copy) { cs->failed = 1; return; }
    cs->pending_copy = 0;

    shm_buf_destroy(cs->sb);
    cs->sb = shm_buf_create_fmt(cs->width, cs->height, cs->stride, cs->fmt);
    if (!cs->sb) { cs->failed = 1; zwlr_screencopy_frame_v1_destroy(frame); return; }

    zwlr_screencopy_frame_v1_copy(frame, cs->sb->buf);
}

static void copy_ready(void *d, struct zwlr_screencopy_frame_v1 *frame,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)tv_sec_hi;(void)tv_sec_lo;(void)tv_nsec;
    CopyState *cs = d;
    cs->done = 1;
    zwlr_screencopy_frame_v1_destroy(frame);
}

static void copy_failed(void *d, struct zwlr_screencopy_frame_v1 *frame)
{
    CopyState *cs = d;
    cs->failed = 1;
    zwlr_screencopy_frame_v1_destroy(frame);
}

static const struct zwlr_screencopy_frame_v1_listener copy_listener = {
    .buffer       = copy_buffer,
    .flags        = copy_flags,
    .ready        = copy_ready,
    .failed       = copy_failed,
    .damage       = copy_damage,
    .linux_dmabuf = copy_linux_dmabuf,
    .buffer_done  = copy_buffer_done,
};

// Capture the entire first output into a new cairo surface.
// Returns NULL on failure.  Caller owns the surface.
cairo_surface_t *wutil_capture_screen(void) {
    if (!screencopy_mgr || !wl_out) return NULL;

    CopyState cs = {0};
    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(screencopy_mgr, 0, wl_out);
    zwlr_screencopy_frame_v1_add_listener(frame, &copy_listener, &cs);

    while (!cs.done && !cs.failed)
        wl_display_dispatch(wl_disp);

    if (cs.failed || !cs.sb) {
        shm_buf_destroy(cs.sb);
        return NULL;
    }

    int w = cs.width, h = cs.height, stride = cs.stride;

    /*
     * Most compositors give us WL_SHM_FORMAT_XRGB8888 (alpha channel = 0x00).
     * Cairo treats alpha=0 as fully transparent, so the screen would render
     * as invisible.  Fix: set every alpha byte to 0xFF in-place.
     *
     * WL_SHM_FORMAT_XRGB8888 = 0x34325258 (fourcc "XR24")
     * WL_SHM_FORMAT_ARGB8888 = 0          (wayland canonical)
     * Byte layout (little-endian): B G R X/A
     */
    if (cs.fmt == WL_SHM_FORMAT_XRGB8888) {
        uint8_t *px = cs.sb->data;
        for (int i = 0; i < h; i++) {
            uint32_t *row = (uint32_t *)(px + i * stride);
            for (int j = 0; j < w; j++)
                row[j] |= 0xFF000000u;
        }
    }

    /* Wrap in cairo. cairo ARGB32 == BGRA in memory == what we have. */
    cairo_surface_t *s = cairo_image_surface_create_for_data(
        cs.sb->data,
        CAIRO_FORMAT_ARGB32,
        w, h,
        stride
    );
    /* Attach the ShmBuf as user-data so it's freed when the surface dies. */
    cairo_surface_set_user_data(s, (const cairo_user_data_key_t *)&cs.sb,
                                cs.sb, (cairo_destroy_func_t)shm_buf_destroy);
    return s;
}

// ── Cursor ────────────────────────────────────────────────────────────────────
static struct wl_cursor_theme *cursor_theme = NULL;
static struct wl_surface      *cursor_surf  = NULL;

void wutil_set_cursor_image(const char *name) {
    if (!cursor_theme || !wl_ptr) return;
    struct wl_cursor *cur = wl_cursor_theme_get_cursor(cursor_theme, name);
    if (!cur || !cur->image_count) return;
    struct wl_cursor_image  *img = cur->images[0];
    struct wl_buffer        *buf = wl_cursor_image_get_buffer(img);
    if (!cursor_surf) {
        cursor_surf = wl_compositor_create_surface(wl_comp);
    }
    wl_surface_attach(cursor_surf, buf, 0, 0);
    wl_surface_damage(cursor_surf, 0, 0, (int32_t)img->width, (int32_t)img->height);
    wl_surface_commit(cursor_surf);
    wl_pointer_set_cursor(wl_ptr, last_ptr_serial, cursor_surf,
                          (int32_t)img->hotspot_x, (int32_t)img->hotspot_y);
}

// ── Public API ────────────────────────────────────────────────────────────────
int wutil_init(void) {
    wl_disp = wl_display_connect(NULL);
    if (!wl_disp) { fprintf(stderr, "Cannot connect to Wayland display\n"); return 0; }

    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_ctx) return 0;

    wl_reg = wl_display_get_registry(wl_disp);
    wl_registry_add_listener(wl_reg, &registry_listener, NULL);
    wl_display_roundtrip(wl_disp); /* bind globals */
    wl_display_roundtrip(wl_disp); /* get output geometry */

    if (!wl_comp || !wl_shm || !layer_shell || !screencopy_mgr) {
        fprintf(stderr, "Required Wayland protocols unavailable\n"
                "Need: wl_compositor, wl_shm, zwlr_layer_shell_v1, "
                "zwlr_screencopy_manager_v1\n");
        return 0;
    }
    if (!W || !H) {
        fprintf(stderr, "Could not determine output dimensions\n");
        return 0;
    }

    cursor_theme = wl_cursor_theme_load(NULL, 24, wl_shm);
    return 1;
}

/*
 * Double-buffered overlay:
 *   sb[0] and sb[1] alternate.  front_idx is the buffer currently owned
 *   by the compositor (submitted but not yet released).  We always draw
 *   into the *other* one.  The wl_buffer.release callback marks a buffer
 *   free so we never write into memory the compositor is still scanning.
 *
 *   frame_done tracks whether we got a wl_surface.frame callback since the
 *   last commit.  We skip commits until the compositor signals it's ready
 *   for the next frame, eliminating the input-lag / flicker from sending
 *   frames faster than the compositor can display them.
 */
static ShmBuf *overlay_sb[2]  = { NULL, NULL };
static int     front_idx       = 0;   /* currently submitted buffer index  */
static int     back_idx        = 1;   /* buffer we are free to draw into   */
static int     buf_released[2] = { 1, 1 }; /* 1 = compositor released / free */
static int     frame_done      = 1;   /* 1 = compositor ready for next frame */
static int     pending_redraw  = 0;   /* redraw requested while throttled   */

static void buffer_release(void *data, struct wl_buffer *buf) {
    (void)buf;
    int idx = (int)(intptr_t)data;
    buf_released[idx] = 1;
}
static const struct wl_buffer_listener buf_listener = { .release = buffer_release };

static void frame_callback(void *data, struct wl_callback *cb, uint32_t t) {
    (void)data; (void)t;
    wl_callback_destroy(cb);
    frame_done = 1;
}
static const struct wl_callback_listener frame_listener = { .done = frame_callback };

static void do_commit(void) {
    /* Swap back→front */
    int tmp   = front_idx;
    front_idx = back_idx;
    back_idx  = tmp;
    buf_released[front_idx] = 0;

    /* Request a frame callback so we know when the compositor wants the next */
    struct wl_callback *cb = wl_surface_frame(overlay_surface);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    frame_done = 0;

    wl_surface_attach(overlay_surface, overlay_sb[front_idx]->buf, 0, 0);
    wl_surface_damage(overlay_surface, 0, 0, W, H);
    wl_surface_commit(overlay_surface);
    wl_display_flush(wl_disp);
}

int wutil_create_overlay(void) {
    overlay_surface = wl_compositor_create_surface(wl_comp);
    if (!overlay_surface) return 0;

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell,
        overlay_surface,
        wl_out,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "shot"
    );
    if (!layer_surface) return 0;

    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP  |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT  |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(overlay_surface);
    wl_display_roundtrip(wl_disp); /* get configure → W, H */

    if (!overlay_configured) return 0;

    /* Allocate both back-buffers */
    for (int i = 0; i < 2; i++) {
        overlay_sb[i] = shm_buf_create(W, H);
        if (!overlay_sb[i]) return 0;
        wl_buffer_add_listener(overlay_sb[i]->buf, &buf_listener,
                               (void *)(intptr_t)i);
    }

    /* Cairo draws into the back buffer (back_idx = 1 initially) */
    cr_surface = cairo_image_surface_create_for_data(
        overlay_sb[back_idx]->data,
        CAIRO_FORMAT_ARGB32,
        W, H, W * 4
    );
    cr = cairo_create(cr_surface);
    return 1;
}

/*
 * wutil_present – submit the current back buffer to the compositor.
 *
 * If the compositor hasn't sent a frame callback yet (still displaying the
 * previous frame) or the back buffer isn't released, we set pending_redraw
 * and return immediately — the caller must call wutil_dispatch() in its loop
 * so that release/frame events arrive and trigger the deferred commit.
 *
 * Once committed, we point cairo at the new back buffer so the next draw
 * goes into free memory.
 */
void wutil_present(cairo_surface_t *s) {
    if (!overlay_surface) return;
    cairo_surface_flush(s);

    if (!frame_done || !buf_released[back_idx]) {
        /* Compositor not ready yet — mark dirty and return; the event loop
         * will call wutil_present_if_pending() when the callbacks arrive. */
        pending_redraw = 1;
        return;
    }
    pending_redraw = 0;
    do_commit();

    /* Re-point cairo at the new back buffer */
    cairo_destroy(cr);
    cairo_surface_destroy(cr_surface);
    cr_surface = cairo_image_surface_create_for_data(
        overlay_sb[back_idx]->data,
        CAIRO_FORMAT_ARGB32,
        W, H, W * 4
    );
    cr = cairo_create(cr_surface);
}

/* Called by the event loop after dispatching — flushes a pending redraw if
 * both the frame callback and buffer release have now arrived. */
void wutil_present_if_pending(void) {
    if (!pending_redraw) return;
    if (!frame_done || !buf_released[back_idx]) return;
    pending_redraw = 0;
    do_commit();

    cairo_destroy(cr);
    cairo_surface_destroy(cr_surface);
    cr_surface = cairo_image_surface_create_for_data(
        overlay_sb[back_idx]->data,
        CAIRO_FORMAT_ARGB32,
        W, H, W * 4
    );
    cr = cairo_create(cr_surface);
}

void wutil_dispatch(void) {
    wl_display_dispatch_pending(wl_disp);
    wl_display_flush(wl_disp);
    wutil_present_if_pending();
}

void wutil_dispatch_blocking(void) {
    wl_display_dispatch(wl_disp);
    wutil_present_if_pending();
}

void wutil_cleanup(void) {
    if (cr)             { cairo_destroy(cr);           cr = NULL; }
    if (cr_surface)     { cairo_surface_destroy(cr_surface); cr_surface = NULL; }
    for (int i = 0; i < 2; i++) {
        if (overlay_sb[i]) { shm_buf_destroy(overlay_sb[i]); overlay_sb[i] = NULL; }
    }
    if (cursor_surf)    { wl_surface_destroy(cursor_surf); cursor_surf = NULL; }
    if (cursor_theme)   { wl_cursor_theme_destroy(cursor_theme); cursor_theme = NULL; }
    if (layer_surface)  { zwlr_layer_surface_v1_destroy(layer_surface); layer_surface = NULL; }
    if (overlay_surface){ wl_surface_destroy(overlay_surface); overlay_surface = NULL; }
    if (wl_ptr)         { wl_pointer_destroy(wl_ptr);   wl_ptr = NULL; }
    if (wl_kbd)         { wl_keyboard_destroy(wl_kbd);  wl_kbd = NULL; }
    if (wl_seat_obj)    { wl_seat_destroy(wl_seat_obj); wl_seat_obj = NULL; }
    if (layer_shell)    { zwlr_layer_shell_v1_destroy(layer_shell); layer_shell = NULL; }
    if (screencopy_mgr) { zwlr_screencopy_manager_v1_destroy(screencopy_mgr); screencopy_mgr = NULL; }
    if (wl_shm)         { wl_shm_destroy(wl_shm);       wl_shm = NULL; }
    if (wl_comp)        { wl_compositor_destroy(wl_comp); wl_comp = NULL; }
    if (xkb_state)      { xkb_state_unref(xkb_state);  xkb_state = NULL; }
    if (xkb_map)        { xkb_keymap_unref(xkb_map);   xkb_map  = NULL; }
    if (xkb_ctx)        { xkb_context_unref(xkb_ctx);  xkb_ctx  = NULL; }
    if (wl_reg)         { wl_registry_destroy(wl_reg);  wl_reg  = NULL; }
    if (wl_disp)        { wl_display_disconnect(wl_disp); wl_disp = NULL; }
}
