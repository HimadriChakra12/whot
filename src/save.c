#include "save.h"
#include "capture.h"
#include "../config.h"

#include <webp/encode.h>
#include <cairo/cairo.h>

#include <unistd.h>
#include <pwd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/limits.h>

static int build_path(char fn[PATH_MAX]) {
    fn[0] = '\0';
    if (OPTDIR[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) {
            const struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        if (!home) return 0;
        strncat(fn, home, PATH_MAX - strlen(fn) - 1);
        strncat(fn, &OPTDIR[1], PATH_MAX - strlen(fn) - 1);
    } else {
        strncat(fn, OPTDIR, PATH_MAX - strlen(fn) - 1);
    }
    mkdir(fn, 0755);
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    snprintf(fn + strlen(fn), PATH_MAX - strlen(fn), OPTFORMAT, OPTFORMATARGS);
    return 1;
}

static int encode_and_write(const char *fn) {
    FILE *fp = fopen(fn, "wb");
    if (!fp) {
        printf("\033[1;31mError:\033[0m Can't open %s\n", fn);
        return 1;
    }

    cairo_surface_flush(img);
    unsigned char *pixels = cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img);

    unsigned char *output = NULL;
    size_t output_size = WebPEncodeBGRA(pixels, img_w, img_h, stride,
                                        OPTQUALITY, &output);

    int ret = 1;
    if (output_size > 0 && output) {
        size_t written = fwrite(output, 1, output_size, fp);
        if (written == output_size) ret = 0;
        else printf("\033[1;31mError:\033[0m Wrote %zu/%zu bytes\n",
                    written, output_size);
    } else {
        printf("\033[1;31mError:\033[0m WebP encode failed\n");
    }

    WebPFree(output);
    fclose(fp);
    return ret;
}

int save_image_path(char *out_path, size_t out_size) {
    char fn[PATH_MAX];
    if (!build_path(fn)) {
        printf("\033[1;31mError:\033[0m Couldn't resolve output path\n");
        return 1;
    }
    if (encode_and_write(fn) != 0) return 1;

    strncpy(out_path, fn, out_size - 1);
    out_path[out_size - 1] = '\0';
    return 0;
}
