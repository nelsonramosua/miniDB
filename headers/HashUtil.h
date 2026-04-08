#ifndef HASHUTIL_H
#define HASHUTIL_H

#include <stdint.h>

static inline uint32_t hashFnv1a32(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

#endif /* HASHUTIL_H */