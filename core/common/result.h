/**
 * Result/Error types for the C core. See docs/DESIGN.md section 3
 * ("Error handling"). No exceptions. Every fallible function returns
 * an Error by value; OK is the success sentinel.
 */
#ifndef SWITCH_COMMON_RESULT_H
#define SWITCH_COMMON_RESULT_H

#include <stdbool.h>

typedef enum Result
{
  RESULT_OK = 0,
  RESULT_OUT_OF_MEMORY = 1,
  RESULT_INVALID_ARGUMENT = 2,
  RESULT_NOT_FOUND = 3,
  RESULT_IO_ERROR = 4,
  RESULT_NOT_IMPLEMENTED = 5,
  RESULT_MEMORY_FAULT = 6,    /* guest VA unmapped / permission denied (vmm, §5) */
  RESULT_NOT_CONTIGUOUS = 7,  /* vmm_guest_to_host: range spans non-adjacent host pages */
} Result;

typedef struct Error
{
  Result code;
  const char *message;
} Error;

#define OK ((Error){.code = RESULT_OK, .message = (const char *)0})
#define ERR(c, m) ((Error){.code = (c), .message = (m)})

static inline bool error_is_ok(Error e) { return e.code == RESULT_OK; }

#endif /* SWITCH_COMMON_RESULT_H */
