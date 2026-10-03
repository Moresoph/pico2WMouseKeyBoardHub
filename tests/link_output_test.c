#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_link.h"

static void roundtrip(hub_link_frame_t *frame) {
    uint8_t wire[HUB_LINK_WIRE_MAX];
    size_t length = hub_link_encode(frame, wire);
    assert(length && length <= sizeof(wire));
    hub_link_parser_t parser = {0};
    hub_link_frame_t got = {0};
    unsigned count = 0;
    for (size_t i = 0; i < length; ++i) count += hub_link_feed(&parser, wire[i], &got);
    assert(count == 1 && !parser.errors && got.type == HUB_LINK_OUTPUT);
    assert(got.nonce == frame->nonce && got.session == frame->session && got.sequence == frame->sequence);
    assert(!memcmp(got.payload, frame->payload, HUB_LINK_OUTPUT_PAYLOAD));
}

int main(void) {
    hub_link_output_t output = {.selected = 1, .flags = HUB_LINK_OUTPUT_INPUT_ONLINE,
        .version = {0, 5, 2}, .computers = {
            {.flags = 15, .address_type = 1, .address = {0x7e,0x7d,1,2,3,4}, .interval = 6},
            {.flags = HUB_LINK_PC_RESERVED, .address = {1,2,3,4,5,6}},
            {0}, {0}
        }};
    hub_link_frame_t frame = {.nonce = 123, .session = 456, .sequence = 65535};
    hub_link_pack_output(&frame, &output);
    assert(frame.length == 76 && HUB_LINK_RAW_MAX == 91 && HUB_LINK_PC_SLOTS == 7);
    roundtrip(&frame);
    hub_link_output_t decoded = {0};
    assert(hub_link_unpack_output(&frame, &decoded));
    assert(decoded.selected == 1 && decoded.computers[0].flags == 15 && decoded.computers[0].interval == 6);
    assert(decoded.computers[1].flags == HUB_LINK_PC_RESERVED && !decoded.computers[2].flags);

    hub_link_output_cache_t cache = {0};
    assert(hub_link_receive_output(&cache, &frame, 123, 456, 0xfffffff0u));
    assert(hub_link_output_fresh(&cache, 123, 456, 0x10u));
    assert(hub_link_output_fresh(&cache, 123, 456, 0xba7u));
    assert(!hub_link_output_fresh(&cache, 123, 456, 0xba8u));
    assert(!hub_link_output_fresh(&cache, 124, 456, 0x10u));
    assert(!hub_link_output_fresh(&cache, 123, 457, 0x10u));
    assert(!hub_link_output_fresh(&cache, 0, 456, 0x10u));
    assert(!hub_link_receive_output(&cache, &frame, 123, 456, 10));
    assert(cache.received_at == 0xfffffff0u);
    frame.sequence = 0;
    assert(hub_link_receive_output(&cache, &frame, 123, 456, 20));
    frame.sequence = 65535;
    assert(!hub_link_receive_output(&cache, &frame, 123, 456, 30));
    assert(cache.received_at == 20 && cache.sequence == 0);
    frame.sequence = 2;
    assert(hub_link_receive_output(&cache, &frame, 123, 456, 40));
    assert(!hub_link_receive_output(&cache, &frame, 124, 456, 50));
    assert(!hub_link_receive_output(&cache, &frame, 123, 457, 50));
    frame.nonce = 124;
    assert(hub_link_receive_output(&cache, &frame, 124, 456, 60));
    frame.sequence++;
    frame.payload[0] = 2;
    assert(!hub_link_receive_output(&cache, &frame, 124, 456, 70) && cache.received_at == 60);

    const unsigned offsets[] = {0, 1, 1, 2, 6, 7, 6, 14, 6};
    const uint8_t values[] = {2, 0, 8, 32, 32, 2, HUB_LINK_PC_READY, 0, 31};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        hub_link_pack_output(&frame, &output);
        frame.payload[offsets[i]] = values[i];
        decoded.selected = 4;
        assert(!hub_link_unpack_output(&frame, &decoded));
        assert(decoded.selected == 4);
    }
    hub_link_pack_output(&frame, &output);
    frame.payload[6] = HUB_LINK_PC_RESERVED;
    assert(!hub_link_unpack_output(&frame, &decoded)); /* Offline must not report an interval. */
    frame.payload[14] = 0;
    assert(hub_link_unpack_output(&frame, &decoded));
    frame.type = HUB_LINK_STATE;
    assert(!hub_link_unpack_output(&frame, &decoded));
    hub_link_frame_t ui={.type=HUB_LINK_UI,.length=1,.nonce=123,.session=456,.sequence=7};
    uint8_t management_wire[HUB_LINK_WIRE_MAX];
    ui.payload[0]=1;
    assert(hub_link_encode(&ui,management_wire));
    ui.payload[0]=2;assert(!hub_link_encode(&ui,management_wire));
    hub_link_frame_t control={.type=HUB_LINK_MANAGE,.length=12,.nonce=123,.session=456};
    control.payload[0]=0x7e;control.payload[4]=7;
    size_t count=hub_link_encode(&control,management_wire);assert(count);
    hub_link_parser_t management_parser={0};hub_link_frame_t management_decoded={0};
    for(size_t i=0;i<count;++i)hub_link_feed(&management_parser,management_wire[i],&management_decoded);
    assert(management_decoded.type==HUB_LINK_MANAGE && management_decoded.payload[4]==7);
    control.type=HUB_LINK_MANAGE_REPLY;control.length=5;control.payload[4]=1;
    assert(hub_link_encode(&control,management_wire));
    control.payload[4]=2;assert(!hub_link_encode(&control,management_wire));
    puts("Pico computer telemetry schema, CRC/escaping, replay/session guards, stale TTL and rollover passed");
    return 0;
}
