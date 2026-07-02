#include "save.h"
#include "capture.h"
#include "../config.h"

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

#if OPTFORMAT_TYPE == FORMAT_WEBP
#include <webp/encode.h>
#endif

#if OPTFORMAT_TYPE == FORMAT_JPEG
#include <jpeglib.h>
#include <setjmp.h>
#endif

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

#if OPTFORMAT_TYPE == FORMAT_PNG

static int encode_and_write(const char *fn) {
    cairo_surface_flush(img);
    cairo_status_t st = cairo_surface_write_to_png(img, fn);
    if (st != CAIRO_STATUS_SUCCESS) {
        printf("\033[1;31mError:\033[0m PNG write failed: %s\n",
               cairo_status_to_string(st));
        return 1;
    }
    return 0;
}

#elif OPTFORMAT_TYPE == FORMAT_JPEG

struct jpeg_err_mgr_s {
    struct jpeg_error_mgr pub;
    jmp_buf               jb;
};

static void jpeg_err_exit(j_common_ptr cinfo) {
    struct jpeg_err_mgr_s *e = (struct jpeg_err_mgr_s *)cinfo->err;
    longjmp(e->jb, 1);
}

static int encode_and_write(const char *fn) {
    FILE *fp = fopen(fn, "wb");
    if (!fp) {
        printf("\033[1;31mError:\033[0m Can't open %s\n", fn);
        return 1;
    }

    cairo_surface_flush(img);
    unsigned char *src    = cairo_image_surface_get_data(img);
    int            stride = cairo_image_surface_get_stride(img);

    unsigned char *rgb = malloc((size_t)img_w * img_h * 3);
    if (!rgb) {
        fclose(fp);
        printf("\033[1;31mError:\033[0m Out of memory\n");
        return 1;
    }

    for (int y = 0; y < img_h; y++) {
        unsigned char *row = src + y * stride;
        unsigned char *dst = rgb + y * img_w * 3;
        for (int x = 0; x < img_w; x++) {
            dst[x*3 + 0] = row[x*4 + 2];
            dst[x*3 + 1] = row[x*4 + 1];
            dst[x*3 + 2] = row[x*4 + 0];
        }
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_err_mgr_s       jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;

    int ret = 1;
    if (setjmp(jerr.jb)) {
        char buf[256];
        jerr.pub.format_message((j_common_ptr)&cinfo, buf);
        printf("\033[1;31mError:\033[0m JPEG encode failed: %s\n", buf);
        jpeg_destroy_compress(&cinfo);
        free(rgb);
        fclose(fp);
        return 1;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width      = (JDIMENSION)img_w;
    cinfo.image_height     = (JDIMENSION)img_h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, OPTQUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while ((int)cinfo.next_scanline < img_h) {
        JSAMPROW row = rgb + cinfo.next_scanline * img_w * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    ret = 0;

    free(rgb);
    fclose(fp);
    return ret;
}

#elif OPTFORMAT_TYPE == FORMAT_WEBP

static int encode_and_write(const char *fn) {
    FILE *fp = fopen(fn, "wb");
    if (!fp) {
        printf("\033[1;31mError:\033[0m Can't open %s\n", fn);
        return 1;
    }

    cairo_surface_flush(img);
    unsigned char *pixels = cairo_image_surface_get_data(img);
    int            stride = cairo_image_surface_get_stride(img);

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

#else
#error "OPTFORMAT_TYPE must be FORMAT_PNG, FORMAT_JPEG, or FORMAT_WEBP"
#endif

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
