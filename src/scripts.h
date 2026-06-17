#pragma once

#include "state.h"

// Maximum number of scripts loaded from OPTSCRIPTDIR
#define MAX_SCRIPTS 32

typedef struct {
    char name[64];   // basename without .sh, used as display label
    char path[512];  // full path to the script
} Script;

// Scan OPTSCRIPTDIR and populate scripts[].  Returns count found (0..MAX_SCRIPTS).
int  scripts_load(Script scripts[MAX_SCRIPTS]);

// Run scripts[idx] with the selection rect passed as env vars:
//   SCREENSHOT_FILE=<path>  SCREENSHOT_X SCREENSHOT_Y SCREENSHOT_W SCREENSHOT_H
// Forks; does not wait (let the script background itself if needed).
void scripts_run(const Script *s, const char *filepath, const Rect *r);
