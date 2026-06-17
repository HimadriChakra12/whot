/*
 * select.c – interactive region selection for shot-wayland.
 *
 * Replaces the X11 select.c.  All XEvent / Xlib calls are gone.
 * Input is fed through wutil's event queues (keyboard + pointer).
 * Drawing uses cairo directly on the overlay cairo_surface_t.
 *
 * Three-phase FSM is identical to the original:
 *   Phase 1 – mode key (f = fullscreen, anything else = region drag)
 *   Phase 2 – drag to select rectangle
 *   Phase 3 – adjust / pick action key
 */

#define _POSIX_C_SOURCE 200809L

#include "select.h"
#include "wutil.h"
#include "capture.h"
#include "scripts.h"
#include "../config.h"

#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

// ── Results ───────────────────────────────────────────────────────────────────
static Rect   rect;
static Action chosen_action     = ACTION_NONE;
static int    chosen_script_idx = -1;
static Script loaded_scripts[MAX_SCRIPTS];
static int    nscripts          = 0;

int     select_x(void)          { return rect.x; }
int     select_y(void)          { return rect.y; }
int     select_w(void)          { return rect.w; }
int     select_h(void)          { return rect.h; }
Action  select_action(void)     { return chosen_action; }
int     select_script_idx(void) { return chosen_script_idx; }
Script *select_scripts(void)    { return loaded_scripts; }
int     select_nscripts(void)   { return nscripts; }

// ── Zone hit-testing (identical logic to original) ─────────────────────────────
typedef enum {
    ZONE_NONE = 0, ZONE_BODY,
    ZONE_TL, ZONE_T, ZONE_TR,
    ZONE_L,          ZONE_R,
    ZONE_BL, ZONE_B, ZONE_BR,
} Zone;

static Zone hit_zone(int mx, int my) {
    int x2 = rect.x + rect.w, y2 = rect.y + rect.h, hs = OPTHANDLESIZE;
    if (mx < rect.x-hs || mx > x2+hs || my < rect.y-hs || my > y2+hs)
        return ZONE_NONE;
    int L = mx <= rect.x+hs, R = mx >= x2-hs;
    int T = my <= rect.y+hs, B = my >= y2-hs;
    if (T && L) return ZONE_TL;
    if (T && R) return ZONE_TR;
    if (B && L) return ZONE_BL;
    if (B && R) return ZONE_BR;
    if (T) return ZONE_T;
    if (B) return ZONE_B;
    if (L) return ZONE_L;
    if (R) return ZONE_R;
    return ZONE_BODY;
}

/* Map zone → named Wayland cursor */
static const char *zone_cursor_name(Zone z) {
    switch (z) {
        case ZONE_TL: case ZONE_BR: return "nwse-resize";
        case ZONE_TR: case ZONE_BL: return "nesw-resize";
        case ZONE_T:  case ZONE_B:  return "ns-resize";
        case ZONE_L:  case ZONE_R:  return "ew-resize";
        case ZONE_BODY:             return "fleur";
        default:                    return "crosshair";
    }
}

// ── Rect helpers ──────────────────────────────────────────────────────────────
static void rect_clamp(void) {
    if (rect.w < 2) rect.w = 2;
    if (rect.h < 2) rect.h = 2;
    if (rect.x < 0)           { rect.w += rect.x; rect.x = 0; }
    if (rect.y < 0)           { rect.h += rect.y; rect.y = 0; }
    if (rect.x + rect.w > W)    rect.w = W - rect.x;
    if (rect.y + rect.h > H)    rect.h = H - rect.y;
    if (rect.w < 2) rect.w = 2;
    if (rect.h < 2) rect.h = 2;
}

static void apply_drag(Zone z, int dx, int dy) {
    if (z == ZONE_BODY) {
        rect.x += dx; rect.y += dy;
        if (rect.x < 0)           rect.x = 0;
        if (rect.y < 0)           rect.y = 0;
        if (rect.x + rect.w > W)  rect.x = W - rect.w;
        if (rect.y + rect.h > H)  rect.y = H - rect.h;
        return;
    }
    switch (z) {
        case ZONE_TL: rect.x+=dx; rect.w-=dx; rect.y+=dy; rect.h-=dy; break;
        case ZONE_TR: rect.w+=dx;              rect.y+=dy; rect.h-=dy; break;
        case ZONE_BL: rect.x+=dx; rect.w-=dx; rect.h+=dy;             break;
        case ZONE_BR: rect.w+=dx;              rect.h+=dy;             break;
        case ZONE_T:                           rect.y+=dy; rect.h-=dy; break;
        case ZONE_B:                           rect.h+=dy;             break;
        case ZONE_L:  rect.x+=dx; rect.w-=dx;                         break;
        case ZONE_R:  rect.w+=dx;                                      break;
        default: break;
    }
    rect_clamp();
}

// ── Drawing (cairo, equivalent to original XPutImage + XDrawRectangle) ────────

/*
 * We keep a pristine copy of the captured screenshot as a cairo surface so
 * we can redraw the dim overlay from scratch every frame — exactly like the
 * original code kept img_orig[] for dim_strip().
 */
static cairo_surface_t *img_clean = NULL; /* copy of captured screen, never modified */

static void drawing_init(void) {
    if (img_clean) { cairo_surface_destroy(img_clean); img_clean = NULL; }
    /* Clone the captured screen surface into img_clean.
     * Use SOURCE so XRGB pixels (alpha=0xFF) are copied verbatim without
     * premultiplication — avoids the greenish tint from OVER compositing. */
    img_clean = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *c = cairo_create(img_clean);
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(c, img, 0, 0);
    cairo_paint(c);
    cairo_destroy(c);
}

static void draw_handle(cairo_t *c, int cx, int cy) {
    int hs = OPTHANDLESIZE;
    cairo_rectangle(c, cx - hs/2, cy - hs/2, hs, hs);
    cairo_fill(c);
}

static void redraw(void) {
    if (!cr || !img_clean) return;

    /* ── 1. Base: clean screenshot — SOURCE replaces dst pixels exactly,
     *    no premultiplication against stale alpha in the overlay buffer. ── */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, img_clean, 0, 0);
    cairo_paint(cr);

    /* ── 2. Dim the four strips outside the selection ── */
    double alpha = OPTDIMALPHA / 255.0;
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* top */
    if (rect.y > 0)
        cairo_rectangle(cr, 0, 0, W, rect.y);
    /* bottom */
    if (rect.y + rect.h < H)
        cairo_rectangle(cr, 0, rect.y + rect.h, W, H - rect.y - rect.h);
    /* left */
    if (rect.x > 0)
        cairo_rectangle(cr, 0, rect.y, rect.x, rect.h);
    /* right */
    if (rect.x + rect.w < W)
        cairo_rectangle(cr, rect.x + rect.w, rect.y, W - rect.x - rect.w, rect.h);
    cairo_fill(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

    /* ── 3. Selection border ── */
    cairo_set_source_rgb(cr, OPTR/255.0, OPTG/255.0, OPTB/255.0);
    cairo_set_line_width(cr, OPTWIDTH);
    cairo_rectangle(cr, rect.x + 0.5, rect.y + 0.5, rect.w, rect.h);
    cairo_stroke(cr);

    /* ── 4. Resize handles ── */
    int mx = rect.x + rect.w / 2, my = rect.y + rect.h / 2;
    draw_handle(cr, rect.x,         rect.y);
    draw_handle(cr, mx,             rect.y);
    draw_handle(cr, rect.x+rect.w, rect.y);
    draw_handle(cr, rect.x,         my);
    draw_handle(cr, rect.x+rect.w, my);
    draw_handle(cr, rect.x,         rect.y+rect.h);
    draw_handle(cr, mx,             rect.y+rect.h);
    draw_handle(cr, rect.x+rect.w, rect.y+rect.h);

    /* ── 5. Dimension label ── */
    char label[32];
    snprintf(label, sizeof(label), "%d x %d", rect.w, rect.h);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    int ly = rect.y > 18 ? rect.y - 6 : rect.y + rect.h + 14;
    cairo_move_to(cr, rect.x, ly);
    cairo_show_text(cr, label);

    /* ── 6. Upload to compositor ── */
    wutil_present(cr_surface);
    wutil_dispatch();
}

// ── Post-selection key dispatch ───────────────────────────────────────────────
// Returns: 1 = action chosen, 0 = ignore, -1 = cancel
static int dispatch_key(uint32_t sym) {
    if (sym == XKB_KEY_Escape)       return -1;
    if (sym == OPTKEY_SAVE)     { chosen_action = ACTION_SAVE;     debug("ACTION_SAVE");     return 1; }
    if (sym == OPTKEY_COPY)     { chosen_action = ACTION_COPY;     debug("ACTION_COPY");     return 1; }
    if (sym == OPTKEY_ANNOTATE) { chosen_action = ACTION_ANNOTATE; debug("ACTION_ANNOTATE"); return 1; }

    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
        int idx = (int)(sym - XKB_KEY_1);
        if (idx < nscripts) {
            chosen_action     = ACTION_SCRIPT;
            chosen_script_idx = idx;
            debug("ACTION_SCRIPT %d (%s)", idx, loaded_scripts[idx].name);
            return 1;
        }
        return 0;
    }

#ifdef OPTSCRIPTBINDS
    {
        static const struct { uint32_t sym; const char *name; } kb[] = OPTSCRIPTBINDS;
        for (size_t i = 0; i < sizeof(kb)/sizeof(kb[0]); i++) {
            if (sym != kb[i].sym) continue;
            for (int j = 0; j < nscripts; j++) {
                if (strcmp(loaded_scripts[j].name, kb[i].name) != 0) continue;
                chosen_action     = ACTION_SCRIPT;
                chosen_script_idx = j;
                debug("ACTION_SCRIPT name=%s idx=%d", kb[i].name, j);
                return 1;
            }
        }
    }
#endif
    return 0;
}

// ── Drag loop ─────────────────────────────────────────────────────────────────
// Returns 0 on cancel, 1 on valid selection.
typedef struct { int x; int y; int button; int pressed; int is_motion; } PointerEvent;
extern int wutil_key_dequeue(uint32_t *sym_out);
extern int wutil_ptr_dequeue(PointerEvent *ev_out);
extern int wutil_ptr_x(void);
extern int wutil_ptr_y(void);

static int do_drag(void) {
    int anchor_x = 0, anchor_y = 0;
    int pressing = 0;

    while (1) {
        wutil_dispatch_blocking();
        wutil_dispatch();              /* drain any remaining pending events */

        uint32_t sym;
        while (wutil_key_dequeue(&sym)) {
            if (sym == XKB_KEY_Escape) return 0;
        }

        /*
         * Coalesce motion: only the LAST motion event in the queue matters
         * for drawing. Track it separately and redraw once after the queue
         * is drained, instead of once per queued event.
         */
        int have_motion = 0, mx = 0, my = 0;

        PointerEvent pe;
        while (wutil_ptr_dequeue(&pe)) {
            if (!pressing) {
                /* Wait for left click to start drag */
                if (!pe.is_motion && pe.pressed && pe.button == 1) {
                    pressing = 1;
                    anchor_x = rect.x = pe.x;
                    anchor_y = rect.y = pe.y;
                    rect.w = rect.h = 0;
                }
                if (!pe.is_motion && pe.pressed && pe.button == 3) return 0;
            } else {
                if (pe.is_motion) {
                    have_motion = 1;
                    mx = pe.x; my = pe.y;
                } else if (!pe.pressed && pe.button == 1) {
                    if (rect.w < 2 || rect.h < 2) return 0;
                    return 1;
                } else if (pe.pressed && pe.button == 3) {
                    return 0;
                }
            }
        }

        if (have_motion) {
            rect.x = anchor_x < mx ? anchor_x : mx;
            rect.y = anchor_y < my ? anchor_y : my;
            rect.w = abs(mx - anchor_x);
            rect.h = abs(my - anchor_y);
            redraw();
        }

        /* Flush a redraw that was throttled because the compositor was
         * still displaying the previous frame. */
        wutil_present_if_pending();
    }
}

// ── Main entry point ──────────────────────────────────────────────────────────
int run_selection(void) {
    nscripts          = scripts_load(loaded_scripts);
    chosen_action     = ACTION_NONE;
    chosen_script_idx = -1;

    /* Capture screen → show it on the overlay */
    if (!screenshot()) return SELECT_ERROR;
    drawing_init();

    /* Initial full-screen blit so the overlay is visible before phase 1.
     * SOURCE operator: copy pixels as-is, no premult against cleared alpha. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER); /* restore default */
    wutil_present(cr_surface);
    wutil_dispatch();

    /*
     * Wait for our overlay surface to actually receive keyboard focus
     * before reading phase-1 input. Without this, a keypress (e.g. 'f'
     * for fullscreen) sent immediately after launch can race the
     * compositor's focus handoff and get delivered to whatever surface
     * had focus before shot started — so it never reaches kbd_key() at
     * all and silently does nothing.
     */
    if (!wutil_wait_keyboard_focus(OPTGRABDELAY)) {
        /* Compositor didn't confirm focus in time — give it one more
         * short grace period rather than failing outright; some
         * compositors still deliver input correctly without ever
         * sending an explicit enter event. */
        struct timespec ts = { 0, OPTGRABDELAY * 1000000L };
        nanosleep(&ts, NULL);
    }

    /* Cursor: crosshair for phase 1 */
    wutil_set_cursor_image("crosshair");

    // ── Phase 1: mode ─────────────────────────────────────────────────────────
    Mode mode = MODE_REGION;
    {
        int phase1_done = 0;
        while (!phase1_done) {
            wutil_dispatch_blocking();

            uint32_t sym;
            while (wutil_key_dequeue(&sym) && !phase1_done) {
                debug("phase1 key: %u", sym);
                if (sym == XKB_KEY_Escape) return SELECT_CANCEL;
                if (sym == OPTKEY_FULLSCREEN) { mode = MODE_FULLSCREEN; phase1_done = 1; break; }
                phase1_done = 1; /* any other key → region */
            }

            PointerEvent pe;
            while (wutil_ptr_dequeue(&pe) && !phase1_done) {
                if (!pe.is_motion) {
                    if (pe.pressed && pe.button == 3) return SELECT_CANCEL;
                    if (pe.pressed && pe.button == 1) {
                        /* Put the click back so do_drag() sees it.
                         * We can't literally "put back" in Wayland, but
                         * do_drag() starts from the next click, so we seed
                         * the anchor from the current pointer position. */
                        rect.x = pe.x; rect.y = pe.y;
                        rect.w = rect.h = 0;
                        int anchor_x = pe.x, anchor_y = pe.y;
                        int pressing = 1;

                        /* Mini inline drag to handle this initial click */
                        int dragging = 1;
                        while (dragging) {
                            wutil_dispatch_blocking();
                            wutil_dispatch();

                            int have_motion = 0, mx2 = 0, my2 = 0;
                            PointerEvent pe2;
                            while (wutil_ptr_dequeue(&pe2)) {
                                if (pe2.is_motion) {
                                    have_motion = 1;
                                    mx2 = pe2.x; my2 = pe2.y;
                                } else if (!pe2.pressed && pe2.button == 1) {
                                    dragging = 0;
                                } else if (pe2.pressed && pe2.button == 3) {
                                    return SELECT_CANCEL;
                                }
                            }
                            if (have_motion) {
                                rect.x = anchor_x < mx2 ? anchor_x : mx2;
                                rect.y = anchor_y < my2 ? anchor_y : my2;
                                rect.w = abs(mx2 - anchor_x);
                                rect.h = abs(my2 - anchor_y);
                                redraw();
                            }
                            wutil_present_if_pending();

                            uint32_t ks2;
                            while (wutil_key_dequeue(&ks2))
                                if (ks2 == XKB_KEY_Escape) return SELECT_CANCEL;
                        }
                        (void)pressing;
                        if (rect.w < 2 || rect.h < 2) return SELECT_CANCEL;
                        phase1_done = 1;
                        goto phase2_done; /* skip do_drag() */
                    }
                }
            }
        }
    }

    // ── Phase 2: capture rect ─────────────────────────────────────────────────
    if (mode == MODE_FULLSCREEN) {
        rect.x = 0; rect.y = 0; rect.w = W; rect.h = H;
    } else {
        if (!do_drag()) return SELECT_CANCEL;
    }

phase2_done:
    redraw();
    debug("rect: %d,%d %dx%d", rect.x, rect.y, rect.w, rect.h);

    // ── Phase 3: post-selection ───────────────────────────────────────────────
    wutil_set_cursor_image("fleur");

    Zone dragging = ZONE_NONE;
    int px = 0, py = 0;

    while (1) {
        wutil_dispatch_blocking();
        wutil_dispatch();

        uint32_t sym;
        while (wutil_key_dequeue(&sym)) {
            debug("phase3 key: %u", sym);
            int r = dispatch_key(sym);
            if (r == -1) return SELECT_CANCEL;
            if (r ==  1) goto phase3_done;
        }

        /*
         * Coalesce: accumulate the total motion delta across every queued
         * motion event, then apply it once and redraw once. Without this,
         * a burst of 10 queued motion events triggers 10 full-screen
         * redraws + commits before the compositor can even display the
         * first one — that backlog is what causes the flicker.
         */
        int   moved = 0;
        int   last_x = px, last_y = py;
        int   total_dx = 0, total_dy = 0;
        int   click_outside = 0, click_x = 0, click_y = 0;
        int   release_left = 0, press_right = 0;
        Zone  clicked_zone = ZONE_NONE;

        PointerEvent pe;
        while (wutil_ptr_dequeue(&pe)) {
            if (pe.is_motion) {
                moved = 1;
                total_dx += pe.x - last_x;
                total_dy += pe.y - last_y;
                last_x = pe.x; last_y = pe.y;
            } else if (pe.pressed && pe.button == 1) {
                Zone z = hit_zone(pe.x, pe.y);
                if (z == ZONE_NONE) { click_outside = 1; click_x = pe.x; click_y = pe.y; }
                else                { clicked_zone  = z; }
                last_x = pe.x; last_y = pe.y;
            } else if (!pe.pressed && pe.button == 1) {
                release_left = 1;
            } else if (pe.pressed && pe.button == 3) {
                press_right = 1;
            }
        }

        if (press_right) return SELECT_CANCEL;

        if (click_outside) {
            /* Clicked outside the selection → re-select via drag */
            (void)click_x; (void)click_y;
            if (!do_drag()) return SELECT_CANCEL;
            redraw();
            dragging = ZONE_NONE;
            px = last_x; py = last_y;
            continue;
        }

        if (clicked_zone != ZONE_NONE) {
            dragging = clicked_zone;
            px = last_x; py = last_y;
        }

        if (release_left) {
            dragging = ZONE_NONE;
        }

        if (moved) {
            if (dragging != ZONE_NONE) {
                apply_drag(dragging, total_dx, total_dy);
                redraw();
            } else {
                wutil_set_cursor_image(zone_cursor_name(hit_zone(last_x, last_y)));
            }
            px = last_x; py = last_y;
        }

        wutil_present_if_pending();
    }

phase3_done:
    if (img_clean) { cairo_surface_destroy(img_clean); img_clean = NULL; }
    return SELECT_OK;
}
