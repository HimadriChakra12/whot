#include "capture.h"
#include "wutil.h"

cairo_surface_t *img   = NULL;
int              img_w = 0;
int              img_h = 0;

int screenshot(void) {
    if (img) { cairo_surface_destroy(img); img = NULL; }

    img = wutil_capture_screen();
    if (!img) return 0;

    img_w = cairo_image_surface_get_width(img);
    img_h = cairo_image_surface_get_height(img);
    return 1;
}

int img_crop(int x, int y, int w, int h) {
    if (!img || w < 1 || h < 1) return 0;

    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (!out || cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) return 0;

    cairo_t *c = cairo_create(out);
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(c, img, -x, -y);
    cairo_paint(c);
    cairo_destroy(c);

    cairo_surface_destroy(img);
    img   = out;
    img_w = w;
    img_h = h;
    return 1;
}
