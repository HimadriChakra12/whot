#pragma once

#include "state.h"
#include "scripts.h"

typedef enum {
    SELECT_OK     =  0,
    SELECT_CANCEL =  1,
    SELECT_ERROR  = -1,
} SelectResult;

int     run_selection(void);

int     select_x(void);
int     select_y(void);
int     select_w(void);
int     select_h(void);
Action  select_action(void);
int     select_script_idx(void);
Script *select_scripts(void);
int     select_nscripts(void);
