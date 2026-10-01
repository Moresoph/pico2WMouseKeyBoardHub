#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hardware/structs/usb.h"

static char output[16384];
static size_t output_used;
static bool short_write;
static bool enumeration_idle = true;
bool hub_usb_enum_can_rearm(void) { return enumeration_idle; }
static size_t wire_write(const void *data, size_t size, size_t count, FILE *stream) {
    assert(size == 1 && stream == stdout && output_used + count < sizeof(output));
    if (short_write) return count - 1;
    memcpy(output + output_used, data, count);
    output_used += count;
    output[output_used] = 0;
    return count;
}
#define fwrite wire_write
#include "../src/hub_usb_wire.c"
#undef fwrite

static usb_hw_t usb_registers;
usb_hw_t *usb_hw = &usb_registers;
static dma_channel_hw_t dma_registers;
static hcd_devtree_info_t topology;
static uint64_t now;
static uint32_t clock_hz = 150000000;
static bool busy, no_channel, no_timer, claimed_channel, claimed_timer;
static unsigned starts, cleanups;
static volatile uint32_t *destination;

uint32_t clock_get_hz(unsigned clock) { assert(clock == clk_sys); return clock_hz; }
uint64_t time_us_64(void) { return now; }
void hcd_devtree_get_info(uint8_t address, hcd_devtree_info_t *info) {
    assert(!address); *info = topology;
}
int dma_claim_unused_channel(bool required) {
    assert(!required && !claimed_channel);
    if (no_channel) return -1;
    claimed_channel = true;
    return 3;
}
int dma_claim_unused_timer(bool required) {
    assert(!required && !claimed_timer);
    if (no_timer) return -1;
    claimed_timer = true;
    return 2;
}
void dma_channel_cleanup(unsigned c) { assert(c == 3 && claimed_channel); busy = false; ++cleanups; }
void dma_channel_unclaim(unsigned c) { assert(c == 3 && claimed_channel && !busy); claimed_channel = false; }
void dma_timer_unclaim(unsigned t) { assert(t == 2 && claimed_timer); claimed_timer = false; }
bool dma_channel_is_busy(unsigned c) { assert(c == 3 && claimed_channel); return busy; }
dma_channel_hw_t *dma_channel_hw_addr(unsigned c) { assert(c == 3 && claimed_channel); return &dma_registers; }
dma_channel_config dma_channel_get_default_config(unsigned c) {
    assert(c == 3);
    dma_channel_config config = {0};
    return config;
}
void channel_config_set_transfer_data_size(dma_channel_config *c, unsigned value) { c->size = value; }
void channel_config_set_read_increment(dma_channel_config *c, bool value) { c->read_increment = value; }
void channel_config_set_write_increment(dma_channel_config *c, bool value) { c->write_increment = value; }
void channel_config_set_irq_quiet(dma_channel_config *c, bool value) { c->quiet = value; }
void channel_config_set_high_priority(dma_channel_config *c, bool value) { c->priority = value; }
void channel_config_set_dreq(dma_channel_config *c, unsigned value) { c->dreq = value; }
unsigned dma_get_timer_dreq(unsigned t) { assert(t == 2 && claimed_timer); return 61; }
void dma_timer_set_fraction(unsigned t, uint16_t numerator, uint16_t denominator) {
    assert(t == 2 && claimed_timer);
    assert((numerator == 1 && denominator == 4) || (!numerator && denominator == 1));
}
void dma_channel_configure(unsigned c, const dma_channel_config *config,
                           volatile void *write, const volatile void *read,
                           uint32_t count, bool start) {
    assert(c == 3 && claimed_channel && claimed_timer && start && count == WIRE_SAMPLES);
    assert(read == &usb_hw->phy_direct);
    assert(config->size == DMA_SIZE_32 && !config->read_increment && config->write_increment);
    assert(config->quiet && config->priority && config->dreq == 61);
    destination = write;
    dma_registers.transfer_count = count;
    dma_registers.ctrl_trig = 0;
    busy = true;
    ++starts;
}
static void complete_dma(void) {
    for (unsigned i = 0; i < WIRE_SAMPLES; ++i)
        destination[i] = ((i & 1u) ? USB_USBPHY_DIRECT_RX_DP_BITS : 0) |
                         ((i & 2u) ? USB_USBPHY_DIRECT_RX_DM_BITS : 0);
    dma_registers.transfer_count = 0;
    busy = false;
}
static void reset_test(void) {
    assert(!claimed_channel && !claimed_timer);
    memset(captures, 0, sizeof(captures));
    topology = (hcd_devtree_info_t){.speed = TUSB_SPEED_FULL};
    no_channel = no_timer = short_write = false;
    replay = active = -1;
    child_targeted = false;
    enumeration_idle = true;
    replay_offset = 0;
    output_used = 0;
    clock_hz = 150000000;
}

int main(void) {
    const uint8_t request[] = {0x80,6,0,1,0,0,8,0};
    const uint8_t different[] = {0x80,6,0,2,0,0,9,0};
    reset_test();
    usb_registers.phy_direct = 0xdeadbeef;
    usb_registers.phy_direct_override = 0x1234;
    usb_registers.sie_ctrl = 0x20008200;
    hub_usb_wire_setup(5, request);
    hub_usb_wire_setup(0, different);
    assert(!starts && captures[0].state == WIRE_WAITING);
    hub_usb_wire_setup(0, request);
    assert(starts == 1 && captures[0].state == WIRE_ACTIVE);
    hub_usb_wire_setup(0, request);
    hub_usb_wire_poll();
    assert(starts == 1 && captures[0].state == WIRE_ACTIVE && !output_used);
    complete_dma();
    topology.hub_addr = 5;
    topology.hub_port = 1;
    hub_usb_wire_setup(0, request);
    assert(captures[0].state == WIRE_READY && captures[1].state == WIRE_ACTIVE && starts == 2);
    complete_dma();
    hub_usb_wire_poll();
    assert(captures[1].state == WIRE_READY && !claimed_channel && !claimed_timer);
    assert(captures[1].hub == 5 && captures[1].port == 1);
    assert(usb_registers.phy_direct == 0xdeadbeef && usb_registers.phy_direct_override == 0x1234);
    assert(usb_registers.sie_ctrl == 0x20008200);
    hub_usb_wire_dump();
    hub_usb_wire_poll();
    assert(strstr(output, "BEGIN root") && !strstr(output, "DATA"));
    size_t before = output_used;
    hub_usb_wire_poll();
    assert(output_used - before < 100 && strstr(output, "hex=e4e4e4e4"));
    for (unsigned i = 0; i < 100; ++i) hub_usb_wire_poll();
    assert(replay == -1 && strstr(output, "END root") && strstr(output, "END child"));
    hub_usb_wire_dump();
    short_write = true;
    hub_usb_wire_poll();
    assert(replay == -1);

    reset_test();
    no_channel = true;
    hub_usb_wire_setup(0, request);
    assert(captures[0].state == WIRE_FAILED && captures[0].error && !claimed_timer);
    reset_test();
    no_timer = true;
    hub_usb_wire_setup(0, request);
    assert(captures[0].state == WIRE_FAILED && !claimed_channel && !claimed_timer);
    reset_test();
    topology.speed = TUSB_SPEED_LOW;
    hub_usb_wire_setup(0, request);
    assert(captures[0].state == WIRE_FAILED && !claimed_channel);
    reset_test();
    clock_hz = 48000000;
    hub_usb_wire_setup(0, request);
    assert(captures[0].state == WIRE_FAILED && !claimed_channel);
    reset_test();
    hub_usb_wire_setup(0, request);
    now += WIRE_TIMEOUT_US;
    hub_usb_wire_poll();
    assert(captures[0].state == WIRE_FAILED && !claimed_channel && !claimed_timer && !busy);
    reset_test();
    hub_usb_wire_setup(0, request);
    complete_dma();
    dma_registers.ctrl_trig = DMA_CH0_CTRL_TRIG_READ_ERROR_BITS;
    hub_usb_wire_poll();
    assert(captures[0].state == WIRE_FAILED && !claimed_channel);
    reset_test();
    hub_usb_wire_setup(0, request);
    complete_dma();
    dma_registers.transfer_count = 4;
    hub_usb_wire_poll();
    assert(captures[0].state == WIRE_FAILED && captures[0].transferred == WIRE_SAMPLES - 4);
    assert(!claimed_channel && !claimed_timer && cleanups);
    reset_test();
    hub_usb_wire_setup(0, request);
    topology.hub_addr = 5;
    topology.hub_port = 1;
    hub_usb_wire_setup(0, request);
    assert(captures[0].state == WIRE_ACTIVE && captures[1].state == WIRE_FAILED);
    assert(active == 0 && claimed_channel && claimed_timer);
    complete_dma();
    hub_usb_wire_poll();
    assert(captures[0].state == WIRE_READY && !claimed_channel && !claimed_timer);
    reset_test();
    hub_usb_wire_arm();
    assert(!captures[1].generation && !child_targeted);
    hub_usb_wire_setup(0, request);
    complete_dma();
    hub_usb_wire_poll();
    uint32_t root_first = captures[0].samples[0];
    topology.hub_addr = 5;
    topology.hub_port = 1;
    hub_usb_wire_setup(0, request);
    complete_dma();
    hub_usb_wire_poll();
    enumeration_idle = false;
    hub_usb_wire_arm();
    assert(captures[1].state == WIRE_READY && !captures[1].generation);
    enumeration_idle = true;
    hub_usb_wire_dump();
    hub_usb_wire_arm();
    assert(captures[1].state == WIRE_READY && !captures[1].generation);
    for (unsigned i = 0; i < 100; ++i) hub_usb_wire_poll();
    hub_usb_wire_arm();
    assert(captures[1].state == WIRE_WAITING && captures[1].generation == 1 && child_targeted);
    assert(captures[0].state == WIRE_READY && captures[0].samples[0] == root_first);
    hub_usb_wire_arm();
    assert(captures[1].generation == 1);
    topology.hub_port = 4;
    hub_usb_wire_setup(0, request);
    assert(captures[1].state == WIRE_WAITING && !claimed_channel);
    topology.hub_port = 1;
    now += 123456;
    hub_usb_wire_setup(0, request);
    hub_usb_wire_arm();
    assert(captures[1].state == WIRE_ACTIVE && captures[1].generation == 1);
    complete_dma();
    hub_usb_wire_poll();
    assert(captures[1].started_us == now && captures[1].state == WIRE_READY);
    output_used = 0;
    hub_usb_wire_dump();
    for (unsigned i = 0; i < 100; ++i) hub_usb_wire_poll();
    assert(strstr(output, "generation=1 started_us="));
    puts("USB wire: bounded read-only DMA, root/child isolation, packing, replay and failure cleanup passed");
    return 0;
}
