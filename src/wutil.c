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

struct wl_display     *wl_disp       = NULL;
struct wl_registry     *wl_reg       = NULL;
struct wl_compositor   *wl_comp      = NULL;
struct wl_shm          *wl_shm       = NULL;
struct wl_seat         *wl_seat_obj  = NULL;
struct wl_pointer      *wl_ptr       = NULL;
struct wl_keyboard     *wl_kbd       = NULL;

struct zwlr_layer_shell_v1        *layer_shell    = NULL;
struct zwlr_screencopy_manager_v1 *screencopy_mgr = NULL;

struct wl_output *wl_out = NULL;
int W = 0, H = 0;

struct wl_surface            *overlay_surface = NULL;
struct zwlr_layer_surface_v1 *layer_surface    = NULL;

cairo_surface_t *cr_surface = NULL;
cairo_t         *cr         = NULL;

struct xkb_context *xkb_ctx   = NULL;
struct xkb_keymap  *xkb_map   = NULL;
struct xkb_state   *xkb_state = NULL;

typedef struct {
    struct wl_buffer *buf;
    void             *data;
    size_t            size;
    int               fd;
} ShmBuf;

static int create_shm_fd(size_t size) {
    int fd = -1;
#ifdef __linux__
    fd = memfd_create("whot-shm", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) < 0) { close(fd); fd = -1; }
        return fd;
    }
#endif
    char name[64];
    snprintf(name, sizeof(name), "/whot-%d", getpid());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        shm_unlink(name);
        if (ftruncate(fd, (off_t)size) < 0) { close(fd); fd = -1; }
    }
    return fd;
}

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

static ShmBuf *shm_buf_create(int w, int h) {
    return shm_buf_create_fmt(w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
}

static void shm_buf_destroy(ShmBuf *sb) {
    if (!sb) return;
    wl_buffer_destroy(sb->buf);
    munmap(sb->data, sb->size);
    close(sb->fd);
    free(sb);
}

static void output_geometry(void *d, struct wl_output *o,
    int32_t x, int32_t y, int32_t pw, int32_t ph,
    int32_t sub, const char *make, const char *model, int32_t tfm)
{ (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)model;(void)tfm; }

static void output_mode(void *d, struct wl_output *o,
    uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    (void)d; (void)o; (void)refresh;
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

static int kbd_has_focus = 0;
static int ptr_has_entered = 0;

static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t serial,
    struct wl_surface *surf, struct wl_array *keys)
{ (void)d;(void)k;(void)serial;(void)surf;(void)keys; kbd_has_focus = 1; }

static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surf)
{ (void)d;(void)k;(void)serial;(void)surf; kbd_has_focus = 0; }

int wutil_has_keyboard_focus(void) { return kbd_has_focus; }

int wutil_wait_keyboard_focus(int max_ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!kbd_has_focus) {
        wl_display_flush(wl_disp);
        if (wl_display_dispatch(wl_disp) < 0) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
                        + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= max_ms) break;
    }
    return kbd_has_focus;
}

int wutil_wait_pointer_focus(int max_ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!ptr_has_entered) {
        wl_display_flush(wl_disp);
        if (wl_display_dispatch(wl_disp) < 0) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
                        + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= max_ms) break;
    }
    return ptr_has_entered;
}

#define EVQUEUE_MAX 128

typedef struct { uint32_t sym; } KeyEvent;
static KeyEvent     key_queue[EVQUEUE_MAX];
static int          key_head = 0, key_tail = 0;
static PointerEvent ptr_queue[EVQUEUE_MAX];
static int          ptr_head = 0, ptr_tail = 0;
static int          current_ptr_x = 0, current_ptr_y = 0;

static void key_enqueue(uint32_t sym) {
    int next = (key_tail + 1) % EVQUEUE_MAX;
    if (next == key_head) return;
    key_queue[key_tail].sym = sym;
    key_tail = next;
}

int wutil_key_dequeue(uint32_t *sym_out) {
    if (key_head == key_tail) return 0;
    *sym_out = key_queue[key_head].sym;
    key_head = (key_head + 1) % EVQUEUE_MAX;
    return 1;
}

static void ptr_enqueue(PointerEvent ev) {
    int next = (ptr_tail + 1) % EVQUEUE_MAX;
    if (next == ptr_head) return;
    ptr_queue[ptr_tail] = ev;
    ptr_tail = next;
}

int wutil_ptr_dequeue(PointerEvent *ev_out) {
    if (ptr_head == ptr_tail) return 0;
    *ev_out = ptr_queue[ptr_head];
    ptr_head = (ptr_head + 1) % EVQUEUE_MAX;
    return 1;
}

int wutil_ptr_x(void) { return current_ptr_x; }
int wutil_ptr_y(void) { return current_ptr_y; }

static void kbd_key(void *d, struct wl_keyboard *k, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state)
{
    (void)d;(void)k;(void)serial;(void)time;
    if (!xkb_state || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    key_enqueue(xkb_state_key_get_one_sym(xkb_state, key + 8));
}

static void kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
    uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    (void)d;(void)k;(void)serial;
    if (xkb_state) xkb_state_update_mask(xkb_state, dep, lat, lock, 0, 0, grp);
}

static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)d;(void)k;(void)rate;(void)delay; }

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)d;(void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (str == MAP_FAILED) { close(fd); return; }

    if (xkb_map)   { xkb_keymap_unref(xkb_map);  xkb_map   = NULL; }
    if (xkb_state) { xkb_state_unref(xkb_state); xkb_state = NULL; }

    xkb_map = xkb_keymap_new_from_string(xkb_ctx, str,
                  XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(str, size);
    close(fd);
    if (xkb_map) xkb_state = xkb_state_new(xkb_map);
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap      = kbd_keymap,
    .enter       = kbd_enter,
    .leave       = kbd_leave,
    .key         = kbd_key,
    .modifiers   = kbd_modifiers,
    .repeat_info = kbd_repeat,
};

static uint32_t last_ptr_serial = 0;
static char     pending_cursor_name[32] = "";

static void apply_cursor_now(const char *name);

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)d;(void)p;(void)surf;
    last_ptr_serial = serial;
    ptr_has_entered = 1;
    current_ptr_x = wl_fixed_to_int(sx);
    current_ptr_y = wl_fixed_to_int(sy);
    if (pending_cursor_name[0])
        apply_cursor_now(pending_cursor_name);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf)
{ (void)d;(void)p;(void)serial;(void)surf; ptr_has_entered = 0; }

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d;(void)p;(void)time;
    current_ptr_x = wl_fixed_to_int(sx);
    current_ptr_y = wl_fixed_to_int(sy);
    ptr_enqueue((PointerEvent){ current_ptr_x, current_ptr_y, 0, 0, 1 });
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
    uint32_t time, uint32_t btn, uint32_t state)
{
    (void)d;(void)p;(void)serial;(void)time;
    int b = (btn == 0x110) ? 1 : (btn == 0x111) ? 3 : (int)(btn - 0x10f);
    ptr_enqueue((PointerEvent){
        current_ptr_x, current_ptr_y, b,
        state == WL_POINTER_BUTTON_STATE_PRESSED, 0
    });
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t val)
{ (void)d;(void)p;(void)time;(void)axis;(void)val; }
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d;(void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t src) { (void)d;(void)p;(void)src; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis)
{ (void)d;(void)p;(void)time;(void)axis; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t disc)
{ (void)d;(void)p;(void)axis;(void)disc; }

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
static void seat_name(void *d, struct wl_seat *seat, const char *name) { (void)d;(void)seat;(void)name; }
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

static void registry_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
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
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
        screencopy_mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 3);
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d;(void)r;(void)name; }

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static int overlay_configured = 0;

static void layer_surface_configure(void *d, struct zwlr_layer_surface_v1 *ls,
    uint32_t serial, uint32_t w, uint32_t h)
{
    (void)d;
    if (w) W = (int)w;
    if (h) H = (int)h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    overlay_configured = 1;
}
static void layer_surface_closed(void *d, struct zwlr_layer_surface_v1 *ls) { (void)d;(void)ls; }
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

typedef struct {
    ShmBuf  *sb;
    int      done, failed;
    int      width, height, stride;
    uint32_t fmt;
    int      pending_copy;
} CopyState;

static void copy_buffer(void *d, struct zwlr_screencopy_frame_v1 *f,
    uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
    (void)f;
    CopyState *cs = d;
    cs->fmt = fmt;
    cs->width = (int)w;
    cs->height = (int)h;
    cs->stride = (int)stride;
    cs->pending_copy = 1;
}

static void copy_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fl) { (void)d;(void)f;(void)fl; }
static void copy_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)d;(void)f;(void)x;(void)y;(void)w;(void)h; }
static void copy_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
    uint32_t fmt, uint32_t w, uint32_t h) { (void)d;(void)f;(void)fmt;(void)w;(void)h; }

static void copy_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *frame) {
    CopyState *cs = d;
    if (!cs->pending_copy) { cs->failed = 1; return; }
    cs->pending_copy = 0;

    shm_buf_destroy(cs->sb);
    cs->sb = shm_buf_create_fmt(cs->width, cs->height, cs->stride, cs->fmt);
    if (!cs->sb) { cs->failed = 1; zwlr_screencopy_frame_v1_destroy(frame); return; }

    zwlr_screencopy_frame_v1_copy(frame, cs->sb->buf);
}

static void copy_ready(void *d, struct zwlr_screencopy_frame_v1 *frame,
    uint32_t hi, uint32_t lo, uint32_t ns)
{
    (void)hi;(void)lo;(void)ns;
    CopyState *cs = d;
    cs->done = 1;
    zwlr_screencopy_frame_v1_destroy(frame);
}

static void copy_failed(void *d, struct zwlr_screencopy_frame_v1 *frame) {
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

static void force_opaque(uint32_t *px, int w, int h, int stride, int swap_rb) {
    for (int i = 0; i < h; i++) {
        uint32_t *row = (uint32_t *)((uint8_t *)px + i * stride);
        for (int j = 0; j < w; j++) {
            uint32_t p = row[j] | 0xFF000000u;
            if (swap_rb)
                p = (p & 0xFF000000u)
                  | ((p & 0x000000FFu) << 16)
                  | (p & 0x0000FF00u)
                  | ((p & 0x00FF0000u) >> 16);
            row[j] = p;
        }
    }
}

cairo_surface_t *wutil_capture_screen(void) {
    if (!screencopy_mgr) {
        fprintf(stderr, "whot: zwlr_screencopy_manager_v1 not available\n");
        return NULL;
    }
    if (!wl_out) {
        fprintf(stderr, "whot: no wl_output found\n");
        return NULL;
    }

    CopyState cs = {0};
    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(screencopy_mgr, 0, wl_out);
    zwlr_screencopy_frame_v1_add_listener(frame, &copy_listener, &cs);

    while (!cs.done && !cs.failed) {
        if (wl_display_dispatch(wl_disp) < 0) {
            fprintf(stderr, "whot: lost connection to Wayland display during capture\n");
            cs.failed = 1;
            break;
        }
    }

    if (cs.failed) {
        fprintf(stderr, "whot: compositor reported screencopy failure\n");
        shm_buf_destroy(cs.sb);
        return NULL;
    }
    if (!cs.sb) {
        fprintf(stderr, "whot: screencopy completed but no buffer was allocated\n");
        return NULL;
    }

    if (cs.fmt == WL_SHM_FORMAT_XRGB8888 || cs.fmt == WL_SHM_FORMAT_XBGR8888)
        force_opaque(cs.sb->data, cs.width, cs.height, cs.stride,
                     cs.fmt == WL_SHM_FORMAT_XBGR8888);

    cairo_surface_t *s = cairo_image_surface_create_for_data(
        cs.sb->data, CAIRO_FORMAT_ARGB32, cs.width, cs.height, cs.stride);
    cairo_surface_set_user_data(s, (const cairo_user_data_key_t *)&cs.sb,
                                cs.sb, (cairo_destroy_func_t)shm_buf_destroy);
    return s;
}

static struct wl_cursor_theme *cursor_theme   = NULL;
static struct wl_surface      *cursor_surf    = NULL;
static ShmBuf                 *fallback_sb    = NULL;
static int                      fallback_size = 24;

static const char *cursor_aliases(const char *name, int idx) {
    static const char *crosshair[] = { "crosshair", "cross", "tcross", NULL };
    static const char *fleur[]     = { "fleur", "move", "grab", "grabbing", "all-scroll", NULL };
    static const char *nwse[]      = { "nwse-resize", "size_fdiag", "bd_double_arrow", "fd_double_arrow", NULL };
    static const char *nesw[]      = { "nesw-resize", "size_bdiag", "fd_double_arrow", "bd_double_arrow", NULL };
    static const char *ns[]        = { "ns-resize", "size_ver", "sb_v_double_arrow", "v_double_arrow", "row-resize", NULL };
    static const char *ew[]        = { "ew-resize", "size_hor", "sb_h_double_arrow", "h_double_arrow", "col-resize", NULL };
    static const char *def[]       = { "default", "left_ptr", "arrow", NULL };

    const char **list = def;
    if (!strcmp(name, "crosshair"))   list = crosshair;
    else if (!strcmp(name, "fleur"))  list = fleur;
    else if (!strcmp(name, "nwse-resize")) list = nwse;
    else if (!strcmp(name, "nesw-resize")) list = nesw;
    else if (!strcmp(name, "ns-resize"))   list = ns;
    else if (!strcmp(name, "ew-resize"))   list = ew;

    return list[idx];
}

static struct wl_cursor *find_cursor(const char *name) {
    if (!cursor_theme) return NULL;
    for (int i = 0; ; i++) {
        const char *alias = cursor_aliases(name, i);
        if (!alias) return NULL;
        struct wl_cursor *cur = wl_cursor_theme_get_cursor(cursor_theme, alias);
        if (cur && cur->image_count) return cur;
    }
}

static ShmBuf *build_fallback_cursor(void) {
    int n = fallback_size;
    ShmBuf *sb = shm_buf_create(n, n);
    if (!sb) return NULL;

    cairo_surface_t *s = cairo_image_surface_create_for_data(
        sb->data, CAIRO_FORMAT_ARGB32, n, n, n * 4);
    cairo_t *c = cairo_create(s);
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(c, 0, 0, 0, 0);
    cairo_paint(c);
    cairo_set_operator(c, CAIRO_OPERATOR_OVER);

    double mid = n / 2.0;
    cairo_set_line_width(c, 3.0);
    cairo_set_source_rgb(c, 0, 0, 0);
    cairo_move_to(c, mid, 2); cairo_line_to(c, mid, n - 2);
    cairo_move_to(c, 2, mid); cairo_line_to(c, n - 2, mid);
    cairo_stroke(c);

    cairo_set_line_width(c, 1.2);
    cairo_set_source_rgb(c, 1, 1, 1);
    cairo_move_to(c, mid, 2); cairo_line_to(c, mid, n - 2);
    cairo_move_to(c, 2, mid); cairo_line_to(c, n - 2, mid);
    cairo_stroke(c);

    cairo_destroy(c);
    cairo_surface_destroy(s);
    return sb;
}

static void apply_cursor_now(const char *name) {
    if (!wl_ptr || !ptr_has_entered) return;

    struct wl_cursor *cur = find_cursor(name);
    if (cur) {
        struct wl_cursor_image *img = cur->images[0];
        struct wl_buffer       *buf = wl_cursor_image_get_buffer(img);
        if (!cursor_surf) cursor_surf = wl_compositor_create_surface(wl_comp);
        wl_surface_attach(cursor_surf, buf, 0, 0);
        wl_surface_damage(cursor_surf, 0, 0, (int32_t)img->width, (int32_t)img->height);
        wl_surface_commit(cursor_surf);
        wl_pointer_set_cursor(wl_ptr, last_ptr_serial, cursor_surf,
                              (int32_t)img->hotspot_x, (int32_t)img->hotspot_y);
        return;
    }

    if (!fallback_sb) fallback_sb = build_fallback_cursor();
    if (!fallback_sb) { wl_pointer_set_cursor(wl_ptr, last_ptr_serial, NULL, 0, 0); return; }

    if (!cursor_surf) cursor_surf = wl_compositor_create_surface(wl_comp);
    wl_surface_attach(cursor_surf, fallback_sb->buf, 0, 0);
    wl_surface_damage(cursor_surf, 0, 0, fallback_size, fallback_size);
    wl_surface_commit(cursor_surf);
    wl_pointer_set_cursor(wl_ptr, last_ptr_serial, cursor_surf,
                          fallback_size / 2, fallback_size / 2);
}

void wutil_set_cursor_image(const char *name) {
    strncpy(pending_cursor_name, name, sizeof(pending_cursor_name) - 1);
    pending_cursor_name[sizeof(pending_cursor_name) - 1] = '\0';
    apply_cursor_now(name);
}

int wutil_init(void) {
    wl_disp = wl_display_connect(NULL);
    if (!wl_disp) { fprintf(stderr, "Cannot connect to Wayland display\n"); return 0; }

    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_ctx) return 0;

    wl_reg = wl_display_get_registry(wl_disp);
    wl_registry_add_listener(wl_reg, &registry_listener, NULL);
    wl_display_roundtrip(wl_disp);
    wl_display_roundtrip(wl_disp);

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

static ShmBuf *overlay_sb[2]  = { NULL, NULL };
static int     front_idx      = 0;
static int     back_idx       = 1;
static int     buf_released[2] = { 1, 1 };
static int     frame_done      = 1;
static int     pending_redraw  = 0;

static void buffer_release(void *data, struct wl_buffer *buf) {
    (void)buf;
    buf_released[(int)(intptr_t)data] = 1;
}
static const struct wl_buffer_listener buf_listener = { .release = buffer_release };

static void frame_callback(void *data, struct wl_callback *cb, uint32_t t) {
    (void)data; (void)t;
    wl_callback_destroy(cb);
    frame_done = 1;
}
static const struct wl_callback_listener frame_listener = { .done = frame_callback };

static void rebind_cairo_to_back_buffer(void) {
    cairo_destroy(cr);
    cairo_surface_destroy(cr_surface);
    cr_surface = cairo_image_surface_create_for_data(
        overlay_sb[back_idx]->data, CAIRO_FORMAT_ARGB32, W, H, W * 4);
    cr = cairo_create(cr_surface);
}

static void do_commit(void) {
    int tmp   = front_idx;
    front_idx = back_idx;
    back_idx  = tmp;
    buf_released[front_idx] = 0;

    struct wl_callback *cb = wl_surface_frame(overlay_surface);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    frame_done = 0;

    wl_surface_attach(overlay_surface, overlay_sb[front_idx]->buf, 0, 0);
    wl_surface_damage(overlay_surface, 0, 0, W, H);
    wl_surface_commit(overlay_surface);
    wl_display_flush(wl_disp);

    rebind_cairo_to_back_buffer();
}

int wutil_create_overlay(void) {
    overlay_surface = wl_compositor_create_surface(wl_comp);
    if (!overlay_surface) return 0;

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, overlay_surface, wl_out,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "whot");
    if (!layer_surface) return 0;

    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(overlay_surface);
    wl_display_roundtrip(wl_disp);

    if (!overlay_configured) return 0;

    for (int i = 0; i < 2; i++) {
        overlay_sb[i] = shm_buf_create(W, H);
        if (!overlay_sb[i]) return 0;
        wl_buffer_add_listener(overlay_sb[i]->buf, &buf_listener, (void *)(intptr_t)i);
    }

    struct wl_region *opaque = wl_compositor_create_region(wl_comp);
    wl_region_add(opaque, 0, 0, W, H);
    wl_surface_set_opaque_region(overlay_surface, opaque);
    wl_region_destroy(opaque);

    cr_surface = cairo_image_surface_create_for_data(
        overlay_sb[back_idx]->data, CAIRO_FORMAT_ARGB32, W, H, W * 4);
    cr = cairo_create(cr_surface);
    return 1;
}

void wutil_present(cairo_surface_t *s) {
    if (!overlay_surface) return;
    cairo_surface_flush(s);

    if (!frame_done || !buf_released[back_idx]) {
        pending_redraw = 1;
        return;
    }
    pending_redraw = 0;
    do_commit();
}

void wutil_present_if_pending(void) {
    if (!pending_redraw || !frame_done || !buf_released[back_idx]) return;
    pending_redraw = 0;
    do_commit();
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
    if (cr)              { cairo_destroy(cr);            cr = NULL; }
    if (cr_surface)      { cairo_surface_destroy(cr_surface); cr_surface = NULL; }
    for (int i = 0; i < 2; i++)
        if (overlay_sb[i]) { shm_buf_destroy(overlay_sb[i]); overlay_sb[i] = NULL; }
    if (cursor_surf)     { wl_surface_destroy(cursor_surf); cursor_surf = NULL; }
    if (cursor_theme)    { wl_cursor_theme_destroy(cursor_theme); cursor_theme = NULL; }
    if (fallback_sb)     { shm_buf_destroy(fallback_sb); fallback_sb = NULL; }
    if (layer_surface)   { zwlr_layer_surface_v1_destroy(layer_surface); layer_surface = NULL; }
    if (overlay_surface) { wl_surface_destroy(overlay_surface); overlay_surface = NULL; }
    if (wl_ptr)          { wl_pointer_destroy(wl_ptr);    wl_ptr = NULL; }
    if (wl_kbd)          { wl_keyboard_destroy(wl_kbd);   wl_kbd = NULL; }
    if (wl_seat_obj)     { wl_seat_destroy(wl_seat_obj);  wl_seat_obj = NULL; }
    if (layer_shell)     { zwlr_layer_shell_v1_destroy(layer_shell); layer_shell = NULL; }
    if (screencopy_mgr)  { zwlr_screencopy_manager_v1_destroy(screencopy_mgr); screencopy_mgr = NULL; }
    if (wl_shm)          { wl_shm_destroy(wl_shm);        wl_shm = NULL; }
    if (wl_comp)         { wl_compositor_destroy(wl_comp); wl_comp = NULL; }
    if (xkb_state)       { xkb_state_unref(xkb_state);   xkb_state = NULL; }
    if (xkb_map)         { xkb_keymap_unref(xkb_map);    xkb_map = NULL; }
    if (xkb_ctx)         { xkb_context_unref(xkb_ctx);   xkb_ctx = NULL; }
    if (wl_reg)          { wl_registry_destroy(wl_reg);   wl_reg = NULL; }
    if (wl_disp)         { wl_display_disconnect(wl_disp); wl_disp = NULL; }
}
