#ifndef HUB_SECURITY_POLICY_H
#define HUB_SECURITY_POLICY_H

#include <stdbool.h>

typedef enum {
    HUB_SECURITY_INPUTS,
    HUB_SECURITY_PC_CONSOLE,
    HUB_SECURITY_PC_KEYBOARD,
    HUB_SECURITY_PC_JUST_WORKS
} hub_security_profile_t;

bool hub_security_configure(hub_security_profile_t profile);

#endif
