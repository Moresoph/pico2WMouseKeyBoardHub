#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_external_input.h"
#include "hub_router.h"
#include "hub_output.h"
#include "hub_security.h"
#include "hub_latency.h"

static uint8_t last_key[8], last_buttons;
static int32_t dx;
static unsigned selected, releases;
static unsigned escape_packets;
static uint32_t clock_us;
uint32_t hub_latency_now_us(void) { return clock_us; }
bool hub_output_ready(unsigned i) { return i < 4; }
unsigned hub_output_active_slot(void) { return selected; }
bool hub_output_switching(void) { return false; }
bool hub_output_select(unsigned i) {
    if (i >= 4) return false;
    hub_output_release_all(); selected = i; return true;
}
bool hub_output_keyboard(const uint8_t report[8]) {
    for (unsigned i = 2; i < 8; ++i) escape_packets += report[i] == 0x29;
    memcpy(last_key, report, 8);
    return true;
}
bool hub_output_mouse(uint8_t buttons, int32_t x, int32_t y, int32_t wheel) {
    (void)y; (void)wheel; last_buttons = buttons; dx += x; return true;
}
void hub_output_release_all(void) { memset(last_key, 0, 8); last_buttons = 0; ++releases; }
bool hub_security_capture_active(void) { return false; }
bool hub_security_keyboard(unsigned i, const uint8_t report[8]) { (void)i; (void)report; return false; }
void hub_security_input_disconnected(unsigned i) { (void)i; }
int main(void) {
    hub_router_init();
    hub_external_input_t state = {0};
    hub_external_reset(&state, 7);
    hub_link_frame_t f = {.nonce = 7, .session = 8};
    hub_link_input_t inputs[2] = {
        {.epoch = 1, .roles = HUB_LINK_KEYBOARD, .keyboard = {0,0,4}},
        {.epoch = 1, .roles = HUB_LINK_MOUSE, .buttons = 1, .dx = 10}
    };
    hub_link_pack(&f, inputs);
    assert(hub_external_receive(&state, &f, 1));
    assert(!last_key[2] && !last_buttons && dx == 0);
    ++f.sequence;
    assert(hub_external_receive(&state, &f, 2));
    assert(!last_key[2] && !last_buttons && dx == 0); /* Held at connection: do not replay. */
    memset(inputs[0].keyboard, 0, 8); inputs[1].buttons = 0; inputs[1].dx = 0;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 3));
    inputs[0].keyboard[2] = 4; inputs[1].buttons = 1; inputs[1].dx = 7;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 4));
    assert(last_key[2] == 4 && last_buttons == 1 && dx == 7);
    assert(hub_external_expired(&state, 504));
    hub_external_reset(&state, 9);
    assert(!last_key[2] && !last_buttons);
    f.nonce = 9; ++f.sequence;
    assert(hub_external_receive(&state, &f, 505));
    assert(!last_key[2] && !last_buttons && dx == 7);
    memset(inputs[0].keyboard, 0, 8); inputs[1].buttons = 0; inputs[1].dx = 0;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 506));
    inputs[0].keyboard[0] = 5; inputs[0].keyboard[2] = 0x4f;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 507));
    assert(selected == 1 && !last_key[2] && releases);
    ++f.sequence;
    assert(hub_external_receive(&state, &f, 508) && selected == 1);
    memset(inputs[0].keyboard, 0, 8);
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 509));
    inputs[0].keyboard[2] = 5;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 510));
    assert(selected == 1 && last_key[2] == 5);
    memset(inputs[0].keyboard, 0, 8);
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 511));
    inputs[0].keyboard[2] = 0x29; inputs[0].keyboard[3] = 0x21;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 512));
    assert(selected == 3 && !last_key[2] && !last_buttons);
    memset(inputs[0].keyboard, 0, 8);
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 513));
    inputs[1].buttons = 3;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 514));
    clock_us = 800000;
    hub_router_poll();
    assert(selected == 3);
    ++clock_us;
    hub_router_poll();
    assert(selected == 0 && !last_buttons);
    ++f.sequence;
    assert(hub_external_receive(&state, &f, 515));
    clock_us += 3000000;
    hub_router_poll();
    assert(selected == 0 && !last_buttons && !escape_packets);
    inputs[1].buttons = 0;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 516));
    inputs[0].keyboard[2] = 0x29;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 517));
    assert(!escape_packets && !last_key[2]);
    ++f.sequence;
    assert(hub_external_receive(&state, &f, 518));
    assert(!escape_packets);
    memset(inputs[0].keyboard, 0, 8);
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 519));
    assert(escape_packets == 1 && !last_key[2]);
    inputs[0].keyboard[2] = 0x29;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 520));
    inputs[0].keyboard[3] = 0x21;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 521));
    assert(selected == 3 && escape_packets == 1);
    memset(inputs[0].keyboard, 0, 8);
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 522));
    assert(escape_packets == 1 && !last_key[2]);
    inputs[0].keyboard[2] = 0x29;
    hub_link_pack(&f, inputs); ++f.sequence;
    assert(hub_external_receive(&state, &f, 523));
    hub_external_reset(&state, 10);
    assert(escape_packets == 1 && !last_key[2]);
    puts("real router: UART release/rearm, deferred Esc tap, consumed Esc+PC digit and timed mouse switching passed");
    return 0;
}
