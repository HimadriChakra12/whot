/*
 * main.c – shot-wayland entry point.
 *
 * All X11 includes gone; xutil → wutil.
 * xclip → wl-copy  (same exec pattern, drop-in replacement).
 * XSubImage → img_crop() (cairo crop in capture.c).
 * Everything else is structurally identical to the original.
 */

#include "config.h"
#include "src/wutil.h"
#include "src/capture.h"
#include "src/select.h"
#include "src/save.h"
#include "src/scripts.h"
#include "src/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

// ── Built-in actions ──────────────────────────────────────────────────────────

static void action_save(const char *path) {
    printf("%s\n", path);
}

/*
 * wl-copy replaces xclip, but the two have different lifecycles:
 *
 *   xclip:    exec it, it daemonizes itself, parent (shot) can exit.
 *   wl-copy:  ALSO daemonizes itself (fork, parent exits, child holds the
 *             clipboard selection alive) — but it expects to be launched
 *             as a normal child process with its own fork, not to *become*
 *             the calling process via execvp(). Replacing `shot` itself
 *             with `sh -c "wl-copy < file"` via execvp() means wl-copy's
 *             internal daemonizing fork happens with `shot` already gone,
 *             which on some compositors (process reaped as soon as the
 *             keybind handler returns) kills the backgrounded child before
 *             it finishes registering the clipboard offer — copy silently
 *             does nothing.
 *
 * Fix: actually fork() here ourselves, let the CHILD exec wl-copy (it can
 * daemonize fully on its own from there), and let the PARENT (shot) return
 * normally so main() proceeds to a clean exit instead of being replaced.
 */
static void action_copy(const char *path) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        /* Child: detach from shot's session so we survive shot exiting. */
        setsid();
        char cmd[4200];
        snprintf(cmd, sizeof(cmd),
                 "wl-copy --type image/png < '%s'", path);
        char *args[] = { "sh", "-c", cmd, NULL };
        execvp(args[0], args);
        perror("wl-copy");
        _exit(127);
    }
    /* Parent: don't wait — wl-copy (via sh) backgrounds itself and stays
     * alive holding the clipboard selection after this child's immediate
     * `sh` ancestor exits. We deliberately don't waitpid() here so shot
     * can return immediately; the detached child is reparented to init. */
}

static void action_annotate(const char *path) {
#ifdef OPTANNOTATE
    #ifdef OPTANNOTATE_FLAGS
        char *args[] = { OPTANNOTATE, OPTANNOTATE_FLAGS, (char *)path, NULL };
    #else
        char *args[] = { OPTANNOTATE, (char *)path, NULL };
    #endif
    execvp(args[0], args);
    perror(OPTANNOTATE);
#else
    (void)path;
    fprintf(stderr, "Annotation disabled (OPTANNOTATE not set)\n");
#endif
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    (void)argv;

    if (!wutil_init())
        die("Failed to connect to Wayland display / discover protocols");

    // ── Headless full-screen mode (any CLI argument) ──────────────────────────
    if (argc > 1) {
        if (!screenshot()) die("Failed to capture screen");

        char path[4096];
        if (save_image_path(path, sizeof(path)) != 0)
            die("Failed to save screenshot");

        action_copy(path);
        goto end;
    }

    // ── Interactive mode ──────────────────────────────────────────────────────
    if (!wutil_create_overlay())
        die("Failed to create fullscreen overlay (layer-shell)");

    {
        int result = run_selection();
        if (result == SELECT_ERROR)  die("Failed to capture screen");
        if (result == SELECT_CANCEL) goto end;
    }

    /* Crop the captured image to the selection */
    {
        int x = select_x(), y = select_y();
        int w = select_w(), h = select_h();
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > W) w = W - x;
        if (y + h > H) h = H - y;
        if (w < 1 || h < 1) goto end;
        if (!img_crop(x, y, w, h)) die("img_crop failed");
    }

    /* Unmap overlay before any external tool runs */
    wutil_cleanup();

    /* Save to disk — all actions receive the file path */
    {
        char path[4096];
        if (save_image_path(path, sizeof(path)) != 0)
            die("Failed to save screenshot");

        Action act = select_action();
        Rect   r   = { select_x(), select_y(), select_w(), select_h() };

        switch (act) {
        case ACTION_SAVE:
            action_save(path);
            break;

        case ACTION_COPY:
            action_copy(path);   /* forks a detached child; always returns */
            break;

        case ACTION_ANNOTATE:
            action_annotate(path); /* execs — doesn't return on success */
            break;

        case ACTION_SCRIPT: {
            int idx = select_script_idx();
            if (idx >= 0 && idx < select_nscripts())
                scripts_run(&select_scripts()[idx], path, &r);
            break;
        }

        case ACTION_NONE:
        default:
            action_save(path);
            break;
        }
    }

    if (img) { cairo_surface_destroy(img); img = NULL; }
    return 0;

end:
    wutil_cleanup();
    if (img) { cairo_surface_destroy(img); img = NULL; }
    return 1;
}
