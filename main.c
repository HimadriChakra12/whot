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

static void action_save(const char *path) {
    printf("%s\n", path);
}

static void action_copy(const char *path) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        setsid();
        char cmd[4200];
        snprintf(cmd, sizeof(cmd), "wl-copy --type image/png < '%s'", path);
        char *args[] = { "sh", "-c", cmd, NULL };
        execvp(args[0], args);
        perror("wl-copy");
        _exit(127);
    }
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

int main(int argc, char *argv[]) {
    (void)argv;

    if (!wutil_init())
        die("Failed to connect to Wayland display / discover protocols");

    if (argc > 1) {
        if (!screenshot())
            die("Failed to capture screen (screencopy request failed or timed out)");

        char path[4096];
        if (save_image_path(path, sizeof(path)) != 0)
            die("Failed to save screenshot");

        printf("%s\n", path);
        action_copy(path);
        goto end;
    }

    if (!wutil_create_overlay())
        die("Failed to create fullscreen overlay (layer-shell)");

    {
        int result = run_selection();
        if (result == SELECT_ERROR)  die("Failed to capture screen");
        if (result == SELECT_CANCEL) goto end;
    }

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

    wutil_cleanup();

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
            action_copy(path);
            break;
        case ACTION_ANNOTATE:
            action_annotate(path);
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
