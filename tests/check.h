/**
 * Minimal assertion macros for the C test binaries. Each test defines
 * CHECK_NAME before including this so failures name their suite.
 */
#ifndef VOLAND_TESTS_CHECK_H
#define VOLAND_TESTS_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#ifndef CHECK_NAME
#define CHECK_NAME "test"
#endif

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "[" CHECK_NAME "] FAIL %s:%d: %s\n", __FILE__, __LINE__, \
              #cond);                                                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define CHECK_OK(expr) CHECK((expr).code == RESULT_OK)
#define CHECK_CODE(expr, expected) CHECK((expr).code == (expected))

#endif /* VOLAND_TESTS_CHECK_H */
