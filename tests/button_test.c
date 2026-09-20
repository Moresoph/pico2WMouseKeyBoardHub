#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "hub_button.h"

static hub_button_t button;

static void released_start(uint32_t start) {
    hub_button_init(&button);
    assert(hub_button_tick(&button, false, start) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, false, start + 50u) == HUB_BUTTON_NONE);
    assert(button.armed);
}

static void test_short_press_and_bounce(void) {
    released_start(0);
    assert(hub_button_tick(&button, true, 100) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, false, 110) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, true, 120) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, true, 169) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, true, 170) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, false, 220) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, true, 230) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, false, 240) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, false, 290) == HUB_BUTTON_ENROLL);
    assert(hub_button_tick(&button, false, 500) == HUB_BUTTON_NONE);
}

static void test_reset_once_and_no_short_press_afterwards(void) {
    released_start(0);
    hub_button_tick(&button, true, 100);
    hub_button_tick(&button, true, 150);
    assert(hub_button_tick(&button, true, 8149) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, true, 8150) == HUB_BUTTON_RESET_INPUTS);
    assert(hub_button_tick(&button, true, 9000) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, true, 20000) == HUB_BUTTON_NONE);
    hub_button_tick(&button, false, 21000);
    assert(hub_button_tick(&button, false, 21050) == HUB_BUTTON_NONE);
    hub_button_tick(&button, true, 22000);
    hub_button_tick(&button, true, 22050);
    hub_button_tick(&button, false, 22100);
    assert(hub_button_tick(&button, false, 22150) == HUB_BUTTON_ENROLL);
}

static void test_held_at_boot_does_not_reset(void) {
    hub_button_init(&button);
    hub_button_tick(&button, true, 0);
    assert(hub_button_tick(&button, true, 20000) == HUB_BUTTON_NONE);
    assert(!button.armed);
    hub_button_tick(&button, false, 20100);
    assert(hub_button_tick(&button, false, 20150) == HUB_BUTTON_NONE);
    assert(button.armed);
}

static void test_release_before_deadline_is_not_reset(void) {
    released_start(0);
    hub_button_tick(&button, true, 100);
    hub_button_tick(&button, true, 150);
    hub_button_tick(&button, false, 8140);
    assert(hub_button_tick(&button, false, 8160) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, false, 8190) == HUB_BUTTON_ENROLL);
}

static void test_clock_wrap(void) {
    uint32_t base = UINT32_MAX - 1000u;
    released_start(base);
    hub_button_tick(&button, true, base + 100u);
    hub_button_tick(&button, true, base + 150u);
    assert(hub_button_tick(&button, true, base + 8149u) == HUB_BUTTON_NONE);
    assert(hub_button_tick(&button, true, base + 8150u) == HUB_BUTTON_RESET_INPUTS);
}

int main(void) {
    test_short_press_and_bounce();
    test_reset_once_and_no_short_press_afterwards();
    test_held_at_boot_does_not_reset();
    test_release_before_deadline_is_not_reset();
    test_clock_wrap();
    puts("All button tests passed.");
    return 0;
}
