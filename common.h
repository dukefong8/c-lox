#ifndef clox_common_h
#define clox_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UINT8_COUNT (UINT8_MAX + 1)

#define DEBUG_PRINT_CODE
#define DEBUG_TRACE_EXECUTION

// In the book these read as defined, but the real build leaves them off so
// test output stays clean. Mirror the oracle's common.h `omit` section.
#undef DEBUG_PRINT_CODE
#undef DEBUG_TRACE_EXECUTION

#endif
