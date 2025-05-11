#pragma once

#define ENABLE_DEBUG_LOG 0

#if ENABLE_DEBUG_LOG
#define LOG_DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void) 0)
#endif