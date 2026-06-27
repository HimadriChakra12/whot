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
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && count < MAX_SCRIPTS) {
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcmp(ent->d_name + len - 3, ".sh") != 0)
            continue;

        Script *s = &scripts[count];
        snprintf(s->path, sizeof(s->path), "%s/%s", dir, ent->d_name);

        size_t namelen = len - 3;
        if (namelen >= sizeof(s->name)) namelen = sizeof(s->name) - 1;
        memcpy(s->name, ent->d_name, namelen);
        s->name[namelen] = '\0';

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

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        setenv("SCREENSHOT_FILE", filepath, 1);
        setenv("SCREENSHOT_X", x, 1);
        setenv("SCREENSHOT_Y", y, 1);
        setenv("SCREENSHOT_W", w, 1);
        setenv("SCREENSHOT_H", h, 1);

        char *args[] = { "/bin/sh", (char *)s->path, NULL };
        execvp(args[0], args);
        perror("execvp");
        _exit(1);
    }
}
