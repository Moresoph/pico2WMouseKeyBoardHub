#ifndef HUB_DESCRIPTOR_RETRY_H
#define HUB_DESCRIPTOR_RETRY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_DESCRIPTOR_FAILURE_LIMIT = 3 };
typedef struct {
    uint8_t *bytes;
    size_t length;
    unsigned failures;
} input_descriptor_retry_t;

static inline void input_descriptor_reset(input_descriptor_retry_t *retry) {
    free(retry->bytes);
    memset(retry, 0, sizeof(*retry));
}

/* Call once per failed connection. Compare actual bytes, not a hash used as proof. */
static inline bool input_descriptor_repeated(input_descriptor_retry_t *retry,
                                             const uint8_t *bytes, size_t length) {
    if (retry->bytes && retry->length == length && !memcmp(retry->bytes, bytes, length)) {
        if (retry->failures < INPUT_DESCRIPTOR_FAILURE_LIMIT) ++retry->failures;
    } else {
        input_descriptor_reset(retry);
        retry->bytes = malloc(length);
        if (!retry->bytes) return false;
        memcpy(retry->bytes, bytes, length);
        retry->length = length;
        retry->failures = 1;
    }
    return retry->failures >= INPUT_DESCRIPTOR_FAILURE_LIMIT;
}

#endif
