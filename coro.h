#pragma once
#include <setjmp.h>


typedef struct coro {
    jmp_buf env;
    int i;  // iterations
    int alive;
    char name[12];
} coro_t;
