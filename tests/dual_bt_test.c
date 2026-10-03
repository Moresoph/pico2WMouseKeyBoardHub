#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_external_input.h"

static unsigned connected[2], disconnected[2], keys[2], mice[2];
static int32_t motion;
void hub_router_input_connected(unsigned i) { ++connected[i]; }
void hub_router_input_disconnected(unsigned i) { ++disconnected[i]; }
void hub_router_keyboard(unsigned i, const uint8_t report[8]) { (void)report; ++keys[i]; }
void hub_router_mouse(unsigned i, uint8_t buttons, int32_t x, int32_t y, int32_t wheel) {
    (void)buttons; (void)y; (void)wheel; ++mice[i]; motion += x;
}
static bool parse(const uint8_t *wire, size_t n, hub_link_frame_t *frame) {
    hub_link_parser_t parser = {0};
    unsigned frames = 0;
    for (size_t i = 0; i < n; ++i) if (hub_link_feed(&parser, wire[i], frame)) ++frames;
    return frames == 1;
}
int main(void) {
    hub_link_input_t inputs[2] = {
        {.epoch = 1, .roles = HUB_LINK_KEYBOARD, .keyboard = {0x7d,0,0x7e}},
        {.epoch = 4, .roles = HUB_LINK_MOUSE, .buttons = 3, .dx = -32768, .dy = 32767, .wheel = -127}
    };
    hub_link_frame_t f = {.nonce = 0x7e7d0102, .session = 123, .sequence = 65535}, decoded;
    hub_link_pack(&f, inputs);
    uint8_t wire[HUB_LINK_WIRE_MAX], bad[HUB_LINK_WIRE_MAX];
    size_t n = hub_link_encode(&f, wire);
    assert(n <= sizeof(wire) && parse(wire, n, &decoded));
    assert(decoded.nonce == f.nonce && decoded.session == 123 && decoded.sequence == 65535);
    hub_link_input_t got[2];
    assert(hub_link_unpack(&decoded, got));
    assert(got[0].keyboard[0] == 0x7d && got[0].keyboard[2] == 0x7e);
    assert(got[1].dx == -32768 && got[1].dy == 32767 && got[1].wheel == -127);
    for (size_t i = 1; i + 1 < n; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            memcpy(bad, wire, n); bad[i] ^= (uint8_t)(1u << bit);
            assert(!parse(bad, n, &decoded));
        }
    }
    hub_link_parser_t parser = {0};
    hub_link_feed(&parser, 0x7e, &decoded);
    for (unsigned i = 0; i < 300; ++i) hub_link_feed(&parser, 1, &decoded);
    hub_link_feed(&parser, 0x7e, &decoded);
    assert(parser.errors == 1);
    bool ready = false;
    for (size_t i = 0; i < n; ++i) ready |= hub_link_feed(&parser, wire[i], &decoded);
    assert(ready && decoded.session == f.session);
    decoded.payload[1] = 4;
    assert(!hub_link_unpack(&decoded, got));
    decoded = f; decoded.payload[3] = 1;
    assert(!hub_link_unpack(&decoded, got));
    decoded = f; decoded.payload[33] = 32;
    assert(!hub_link_unpack(&decoded, got));
    decoded = f; decoded.payload[24] = 0;
    assert(!hub_link_unpack(&decoded, got));
    hub_link_frame_t request = {.type = HUB_LINK_REQUEST, .nonce = 42};
    n = hub_link_encode(&request, wire);
    assert(parse(wire, n, &decoded) && decoded.type == HUB_LINK_REQUEST);
    request.nonce = 0;
    assert(!hub_link_encode(&request, wire));

    hub_external_input_t state = {0};
    hub_external_reset(&state, f.nonce);
    assert(disconnected[0] == 1 && disconnected[1] == 1);
    assert(hub_external_receive(&state, &f, 0xfffffff0));
    assert(state.online && connected[0] == 1 && connected[1] == 1 && motion == 0);
    hub_link_output_t output = {.selected = 1, .version = {0,5,2}};
    hub_link_frame_t telemetry = {.nonce = f.nonce, .session = f.session};
    hub_link_pack_output(&telemetry, &output);
    assert(hub_external_receive(&state, &telemetry, 100));
    assert(state.last_rx == 0xfffffff0 && state.accepted == 1 && motion == 0);
    hub_link_frame_t control={.type=HUB_LINK_UI,.length=1,.nonce=f.nonce,.session=f.session};
    assert(hub_external_receive(&state,&control,100));
    control.type=HUB_LINK_MANAGE;control.length=12;
    assert(hub_external_receive(&state,&control,101));
    assert(state.last_rx==0xfffffff0 && state.accepted==1);
    assert(!hub_external_expired(&state, 0x10));
    assert(hub_external_expired(&state, 0x1e4));
    assert(hub_external_receive(&state, &f, 10)); /* Duplicate doesn't refresh the watchdog. */
    assert(state.last_rx == 0xfffffff0 && motion == 0 && state.accepted == 1);
    inputs[1].dx = 10;
    f.sequence = 0;
    hub_link_pack(&f, inputs);
    assert(hub_external_receive(&state, &f, 20));
    assert(motion == 10 && state.sequence == 0);
    assert(hub_external_receive(&state, &f, 30) && motion == 10);
    f.sequence = 2;
    assert(!hub_external_receive(&state, &f, 40)); /* No silently lost key transitions. */
    assert(motion == 10);
    f.sequence = 1; f.session = 999;
    assert(!hub_external_receive(&state, &f, 40));
    f.session = 123; f.nonce++;
    assert(hub_external_receive(&state, &f, 40)); /* Stale challenge is ignored. */
    assert(state.last_rx == 20);
    f.nonce--;
    assert(!hub_external_receive(&state, &f, 520)); /* Exactly 500 ms. */
    hub_external_reset(&state, 99);
    assert(!state.online && !state.inputs[0].roles);
    f.nonce = 99; f.sequence = 40;
    inputs[0].roles = 0; memset(inputs[0].keyboard, 0, 8);
    hub_link_pack(&f, inputs);
    assert(hub_external_receive(&state, &f, 600) && motion == 10);
    inputs[1].epoch++; f.sequence++;
    hub_link_pack(&f, inputs);
    assert(hub_external_receive(&state, &f, 601) && motion == 10);
    puts("dual Bluetooth framing, CRC, freshness, watchdog, sequence and release tests passed");
    return 0;
}
