#include "ps2_log.h"

static int s_verbose = 0;

void ps2_log_set_verbose(int on)
{
    s_verbose = on ? 1 : 0;
}

int ps2_log_verbose(void)
{
    return s_verbose;
}
