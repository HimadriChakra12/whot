#pragma once

// ── Selection mode (pre-selection keypress) ───────────────────────────────────
typedef enum {
    MODE_REGION,
    MODE_FULLSCREEN,
} Mode;

// ── App states ────────────────────────────────────────────────────────────────
typedef enum {
    STATE_SELECTING,
    STATE_SELECTED,
    STATE_DONE,
} State;

// ── Post-selection action ─────────────────────────────────────────────────────
typedef enum {
    ACTION_NONE = 0,
    ACTION_SAVE,       // save to disk (w)
    ACTION_COPY,       // copy to clipboard (y)
    ACTION_ANNOTATE,   // open annotation tool (a)
    ACTION_SCRIPT,     // run a script from exec dir (1-9 or custom keybind)
} Action;

// ── Rect ──────────────────────────────────────────────────────────────────────
typedef struct {
    int x, y, w, h;
} Rect;
