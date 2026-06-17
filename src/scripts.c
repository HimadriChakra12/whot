#define _POSIX_C_SOURCE 200809L

#include "scripts.h"
#include "../config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

// Expand a leading ~ in path into buf (size bufsz).  Returns buf.
static char *expand_home(const char *path, char *buf, size_t bufsz) {
    if (path[0] != '~') {
        strncpy(buf, path, bufsz - 1);
        buf[bufsz - 1] = '\0';
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home) {
        const struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) { buf[0] = '\0'; return buf; }
    snprintf(buf, bufsz, "%s%s", home, path + 1);
    return buf;
}

int scripts_load(Script scripts[MAX_SCRIPTS]) {
    char dir[512];
    expand_home(OPTSCRIPTDIR, dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d) {
        debug("Script dir not found: %s", dir);
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && count < MAX_SCRIPTS) {
        // Only *.sh files
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 3, ".sh") != 0)
            continue;

        snprintf(scripts[count].path, sizeof(scripts[count].path),
                 "%s/%s", dir, ent->d_name);

        // Name = basename without .sh
        strncpy(scripts[count].name, ent->d_name,
                sizeof(scripts[count].name) - 1);
        scripts[count].name[len - 3] = '\0'; // strip .sh

        debug("Loaded script [%d] %s -> %s",
              count, scripts[count].name, scripts[count].path);
        count++;
    }
    closedir(d);
    return count;
}

void scripts_run(const Script *s, const char *filepath, const Rect *r) {
    char x[16], y[16], w[16], h[16];
    snprintf(x, sizeof(x), "%d", r->x);
    snprintf(y, sizeof(y), "%d", r->y);
    snprintf(w, sizeof(w), "%d", r->w);
    snprintf(h, sizeof(h), "%d", r->h);

    debug("Running script %s  file=%s  rect=%s,%s %sx%s",
          s->path, filepath, x, y, w, h);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        // Child: set env, exec
        setenv("SCREENSHOT_FILE", filepath,  1);
        setenv("SCREENSHOT_X",    x,         1);
        setenv("SCREENSHOT_Y",    y,         1);
        setenv("SCREENSHOT_W",    w,         1);
        setenv("SCREENSHOT_H",    h,         1);

        char *args[] = { "/bin/sh", (char *)s->path, NULL };
        execvp(args[0], args);
        perror("execvp");
        _exit(1);
    }
    // Parent returns immediately; script runs in background.
}
