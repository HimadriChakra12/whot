#pragma once

#include <cairo/cairo.h>
#include <stdint.h>

/* Captured screen image.  Pixel format: CAIRO_FORMAT_ARGB32 (BGRA in memory). */
extern cairo_surface_t *img;          /* full screen or cropped region        */
extern int              img_w, img_h; /* dimensions of img                    */

/* pixel channel masks – kept for save.c compatibility */
extern unsigned long int img_r, img_g, img_b;

/*
 * Capture the entire screen into img.
 * Returns 1 on success, 0 on failure.
 */
int screenshot(void);

/*
 * Crop img to the rectangle (x,y,w,h).
 * Destroys the old img; replaces it with a new one.
 * Returns 1 on success, 0 on failure.
 */
int img_crop(int x, int y, int w, int h);
