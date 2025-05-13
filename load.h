#pragma once

#include <linux/types.h>

#define FSHIFT 11
#define FIXED_1 (1 << FSHIFT)

#define EXP_5S 1676


struct kxo_loadavg {
    unsigned long avg_5s;
    s64 active_nsec;
};
