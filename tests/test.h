/* Tiny test harness: CHECK records a failure and keeps going; RUN runs a test. */
#ifndef MB_TEST_H
#define MB_TEST_H

#include <stdio.h>

extern int tests_run, tests_failed, checks_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            checks_failed++;                                                       \
        }                                                                          \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define RUN(fn)                                                                    \
    do {                                                                           \
        int before = checks_failed;                                                \
        tests_run++;                                                               \
        fn();                                                                      \
        if (checks_failed != before) {                                             \
            tests_failed++;                                                        \
            fprintf(stderr, "FAILED %s\n", #fn);                                   \
        }                                                                          \
    } while (0)

#endif
