#include <assert.h>
#include <stdio.h>
#include "hub_security_policy.h"
#include "ble/sm.h"

static bool sc_only;
static io_capability_t io;
static uint8_t auth, minimum, maximum;
static unsigned calls;

void sm_set_secure_connections_only_mode(bool enabled) { sc_only = enabled; ++calls; }
void sm_set_io_capabilities(io_capability_t value) { io = value; ++calls; }
void sm_set_authentication_requirements(uint8_t value) { auth = value; ++calls; }
void sm_set_encryption_key_size_range(uint8_t low, uint8_t high) {
    minimum = low; maximum = high; ++calls;
}

int main(void) {
    assert(hub_security_configure(HUB_SECURITY_INPUTS));
    assert(!sc_only && io == IO_CAPABILITY_DISPLAY_YES_NO);
    assert(auth == (SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION));
    assert(minimum == 16 && maximum == 16);
    assert(hub_security_configure(HUB_SECURITY_PC_KEYBOARD));
    assert(sc_only && io == IO_CAPABILITY_KEYBOARD_ONLY);
    assert(auth & SM_AUTHREQ_MITM_PROTECTION);
    assert(hub_security_configure(HUB_SECURITY_INPUTS));
    assert(!sc_only && !(auth & SM_AUTHREQ_MITM_PROTECTION));
    assert(hub_security_configure(HUB_SECURITY_PC_CONSOLE));
    assert(sc_only && io == IO_CAPABILITY_DISPLAY_YES_NO);
    assert(!(auth & SM_AUTHREQ_MITM_PROTECTION));
    assert(hub_security_configure(HUB_SECURITY_PC_JUST_WORKS));
    assert(sc_only && io == IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    assert(auth == (SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION));
    assert(minimum == 16 && maximum == 16);
    unsigned before = calls;
    assert(!hub_security_configure((hub_security_profile_t)99));
    assert(calls == before && sc_only);
    assert(hub_security_configure(HUB_SECURITY_INPUTS) && !sc_only);
    assert(minimum == 16 && maximum == 16);
    puts("All security profile compatibility tests passed.");
    return 0;
}
