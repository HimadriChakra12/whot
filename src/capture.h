#pragma once

#include <cairo/cairo.h>

extern cairo_surface_t *img;
extern int img_w, img_h;

int screenshot(void);
int img_crop(int x, int y, int w, int h);
