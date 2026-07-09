#pragma once

#include "state.h"

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
