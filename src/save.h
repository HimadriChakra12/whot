#pragma once
#include <stddef.h>

// Save img to disk, write the path into out_path.  Returns 0 on success.
int save_image_path(char *out_path, size_t out_size);

// Legacy full-screen helper: save + exec xclip.
int save_image(void);
