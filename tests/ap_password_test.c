#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ap_password_policy.h"

int main(void) {
    assert(!hub_ap_password_valid(NULL));
    assert(!hub_ap_password_valid(""));
    assert(!hub_ap_password_valid("abcdefg"));
    assert(hub_ap_password_valid("abcdefgh"));
    char value[65];
    memset(value, 'A', sizeof(value));
    value[63] = 0;
    assert(hub_ap_password_valid(value));
    value[63] = 'A'; value[64] = 0;
    assert(!hub_ap_password_valid(value));
    assert(!hub_ap_password_valid("abcd\nefgh"));
    assert(!hub_ap_password_valid("abcd\x7f""efgh"));
    assert(!hub_ap_password_valid("abcd\x80""efgh"));
    assert(hub_ap_password_valid("abcd efgh"));
    puts("AP password printable-ASCII and 8/63-character boundary tests passed");
    return 0;
}
