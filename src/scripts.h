#pragma once

#include "state.h"

#define MAX_SCRIPTS 32

typedef struct {
    char name[64];
    char path[512];
} Script;

int  scripts_load(Script scripts[MAX_SCRIPTS]);
void scripts_run(const Script *s, const char *filepath, const Rect *r);
