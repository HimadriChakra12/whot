#pragma once

typedef enum {
    MODE_REGION,
    MODE_FULLSCREEN,
} Mode;

typedef enum {
    ACTION_NONE = 0,
    ACTION_SAVE,
    ACTION_COPY,
    ACTION_ANNOTATE,
    ACTION_SCRIPT,
} Action;

typedef struct {
    int x, y, w, h;
} Rect;
