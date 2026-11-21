#pragma once

#include <cstdio>
#include <cstdlib>

// Always-on check enforced even in Release (NDEBUG) builds.
#define OS_REQUIRE(cond)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                  \
            std::fflush(stderr);                                                  \
            std::exit(1);                                                         \
        }                                                                         \
    } while (0)
