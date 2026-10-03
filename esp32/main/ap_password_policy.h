#ifndef HUB_AP_PASSWORD_POLICY_H
#define HUB_AP_PASSWORD_POLICY_H

#include <stdbool.h>
#include <stddef.h>

static inline bool hub_ap_password_valid(const char *password) {
    if (!password) return false;
    size_t length = 0;
    for (; password[length]; ++length) {
        unsigned char value = (unsigned char)password[length];
        if (length >= 63 || value < 32 || value > 126) return false;
    }
    return length >= 8;
}

#endif
