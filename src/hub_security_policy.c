#include "hub_security_policy.h"

#include <stdio.h>
#include "ble/sm.h"

bool hub_security_configure(hub_security_profile_t profile) {
    if (profile < HUB_SECURITY_INPUTS || profile > HUB_SECURITY_PC_JUST_WORKS) {
        printf("[security] invalid security profile; policy unchanged\n");
        return false;
    }
    bool computer = profile != HUB_SECURITY_INPUTS;
    bool keyboard = profile == HUB_SECURITY_PC_KEYBOARD;
    bool just_works = profile == HUB_SECURITY_PC_JUST_WORKS;
    /* SDK 2.3.1 defaults to SC-only, which rejects legitimate legacy BLE mice. */
    sm_set_secure_connections_only_mode(computer);
    sm_set_io_capabilities(just_works ? IO_CAPABILITY_NO_INPUT_NO_OUTPUT
                                    : keyboard ? IO_CAPABILITY_KEYBOARD_ONLY
                                               : IO_CAPABILITY_DISPLAY_YES_NO);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION
                                      | (keyboard ? SM_AUTHREQ_MITM_PROTECTION : 0));
    sm_set_encryption_key_size_range(16, 16);
    printf("[security] profile=%s SC-only=%u encryption-key=16\n",
           just_works ? "PC-JustWorks" : computer ? keyboard ? "PC-keyboard" : "PC-console" : "inputs",
           computer);
    return true;
}
