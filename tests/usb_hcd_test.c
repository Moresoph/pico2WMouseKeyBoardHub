#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "hub_usb_host.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "usb_registers.h"

#ifndef PICO_RP2350
#define PICO_RP2350 1
#endif

static struct {
    uint32_t sie_ctrl, sie_status, ints, inte, dev_addr_ctrl, buf_status;
    uint32_t main_ctrl, pwr;
#if PICO_RP2350
    uint32_t linestate_tuning;
#endif
} registers, clear_registers, set_registers, *usb_hw = &registers,
  *usb_hw_clear = &clear_registers, *usb_hw_set = &set_registers;
struct hw_endpoint {
    uint8_t dev_addr, ep_addr;
    bool active, configured;
    uint16_t wMaxPacketSize;
    uint32_t remaining_len, xferred_len;
    volatile uint32_t *buffer_control;
};
static struct hw_endpoint ep_pool[2];
#define epx (ep_pool[0])
static unsigned logs, locks;
static uint32_t irq_state;
static void hcd_log(const char *format, ...) { (void)format; ++logs; }
#define TU_LOG1 hcd_log
#include "../patches/tinyusb/host_diagnostics.inc"

typedef unsigned uint;
static unsigned root_speed = 2, attaches, removes, completions, interrupt_completions;
static unsigned transfer_completions;
static xfer_result_t last_result;
static uint8_t dev_speed(void) { return (uint8_t)root_speed; }
static void hcd_event_device_attach(uint8_t port, bool isr) {
    assert(port == 0 && isr); ++attaches;
}
static void hcd_event_device_remove(uint8_t port, bool isr) {
    assert(port == 0 && isr); ++removes;
}
static void hw_xfer_complete(struct hw_endpoint *ep, xfer_result_t result) {
    assert(ep == &epx && ep->active);
    ep->active = false;
    ++completions;
    last_result = result;
}
static void hw_handle_buff_status(void) {
    registers.buf_status &= ~clear_registers.buf_status;
    assert(!(registers.buf_status & 1u));
    if (registers.buf_status & 4u) ++interrupt_completions;
    registers.buf_status = 0;
}
static void hw_trans_complete(void) {
    ++transfer_completions;
    hw_xfer_complete(&epx, XFER_RESULT_SUCCESS);
}
#define __tusb_irq_path_func(name) name
#define RHPORT_NATIVE 0
#define pico_trace(...) hcd_log(__VA_ARGS__)
#define TU_LOG(level, ...) hcd_log(__VA_ARGS__)
#define tu_u32_low16(value) ((uint16_t)(value))
#define tu_u32_high16(value) ((uint16_t)((value) >> 16))
#define panic(...) assert(!"unexpected USB panic")
#define USBCTRL_IRQ 14
#define PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY 0xff
static unsigned irq_removals, irq_additions;
static void rp2040_usb_init(void) {
    memset(&registers, 0, sizeof(registers));
    memset(&set_registers, 0, sizeof(set_registers));
    memset(&clear_registers, 0, sizeof(clear_registers));
#if PICO_RP2350
    registers.linestate_tuning = USB_LINESTATE_TUNING_RESET;
#endif
}
static void irq_remove_handler(unsigned irq, void (*handler)(void)) {
    assert(irq == USBCTRL_IRQ && handler && !registers.main_ctrl);
#if PICO_RP2350
    /* Verify timing is set after reset, before the controller is enabled. */
    assert(set_registers.linestate_tuning == USB_LINESTATE_TUNING_MULTI_HUB_FIX_BITS);
    registers.linestate_tuning |= set_registers.linestate_tuning;
    assert(registers.linestate_tuning ==
           (USB_LINESTATE_TUNING_RESET | USB_LINESTATE_TUNING_MULTI_HUB_FIX_BITS));
#endif
    ++irq_removals;
}
static void irq_add_shared_handler(unsigned irq, void (*handler)(void), unsigned priority) {
    assert(irq == USBCTRL_IRQ && handler && priority == PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
    ++irq_additions;
}
static struct { uint8_t setup_packet[8]; } setup_ram, *usbh_dpram = &setup_ram;
static unsigned wire_calls, setup_delays;
static bool setup_pre;
#define TU_ASSERT(condition) do { if (!(condition)) return false; } while (0)
static struct hw_endpoint *_hw_endpoint_allocate(uint8_t address) {
    assert(!address);
    return &epx;
}
static void _hw_endpoint_init(struct hw_endpoint *ep, uint8_t address, uint8_t endpoint,
                              uint16_t size, unsigned type, unsigned interval) {
    assert(ep == &epx && !endpoint && size == 8 && !type && !interval);
    ep->dev_addr = address;
    ep->ep_addr = endpoint;
    ep->configured = true;
}
static bool need_pre(uint8_t address) { (void)address; return setup_pre; }
static void hub_usb_wire_setup(uint8_t address, const uint8_t request[8]) {
    assert(registers.dev_addr_ctrl == address && epx.active && epx.remaining_len == 8);
    assert(!memcmp(setup_ram.setup_packet, request, 8));
    assert(!(registers.sie_ctrl & USB_SIE_CTRL_START_TRANS_BITS));
    ++wire_calls;
}
static void busy_wait_at_least_cycles(unsigned cycles) {
    assert(cycles == 12 && wire_calls == setup_delays + 1);
    assert((registers.sie_ctrl & (USB_SIE_CTRL_START_TRANS_BITS | USB_SIE_CTRL_SEND_SETUP_BITS))
           == USB_SIE_CTRL_SEND_SETUP_BITS);
    ++setup_delays;
}
#include "usb_hcd_irq.inc"

uint32_t save_and_disable_interrupts(void) {
    uint32_t previous = irq_state;
    irq_state = 1;
    ++locks;
    return previous;
}
void restore_interrupts(uint32_t previous) { assert(irq_state); irq_state = previous; }

int main(void) {
    tusb_rhport_init_t config = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL};
    assert(hcd_init(0, &config));
    assert(irq_removals == 1 && irq_additions == 1);
    assert(registers.main_ctrl ==
           (USB_MAIN_CTRL_CONTROLLER_EN_BITS | USB_MAIN_CTRL_HOST_NDEVICE_BITS));
    logs = 0;
    hub_usb_hcd_state_t state;
    hub_usb_hcd_get_state(&state);
    assert(!state.active && !state.buffer_control && locks == 1 && !irq_state);
#if PICO_RP2350
    assert(state.hub_timing_supported && state.hub_timing_enabled);
    assert(state.linestate_tuning ==
           (USB_LINESTATE_TUNING_RESET | USB_LINESTATE_TUNING_MULTI_HUB_FIX_BITS));
    registers.linestate_tuning = USB_LINESTATE_TUNING_RESET;
    hub_usb_hcd_get_state(&state);
    assert(state.hub_timing_supported && !state.hub_timing_enabled);
    assert(state.linestate_tuning == USB_LINESTATE_TUNING_RESET);
    assert(registers.linestate_tuning == USB_LINESTATE_TUNING_RESET); /* Readback must not repair the bit. */
#else
    assert(!state.hub_timing_supported && !state.hub_timing_enabled && !state.linestate_tuning);
#endif
    volatile uint32_t buffer = 0x1234;
    epx.dev_addr = 5;
    epx.ep_addr = 0x80;
    epx.active = epx.configured = true;
    epx.wMaxPacketSize = 64;
    epx.remaining_len = 4;
    epx.xferred_len = 2;
    epx.buffer_control = &buffer;
    registers.sie_ctrl = 0x800a;
    registers.sie_status = 0x10000;
    registers.ints = 7;
    registers.inte = 9;
    registers.dev_addr_ctrl = 5;
    for (unsigned i = 0; i < 10; ++i) hub_usb_hcd_note_rx_timeout();
    assert(hub_rx_timeouts == 10 && logs == 4); /* Log 1, 2, 4, 8, not every IRQ. */
    hub_usb_hcd_get_state(&state);
    assert(state.active && state.address == 5 && state.endpoint == 0x80);
    assert(state.packet_size == 64 && state.remaining == 4 && state.transferred == 2);
    assert(state.rx_timeouts == 10 && state.buffer_control == buffer);
    assert(state.sie_ctrl == registers.sie_ctrl && state.sie_status == registers.sie_status);
    assert(state.ints == 7 && state.inte == 9 && state.address_control == 5);
    assert(epx.active && buffer == 0x1234 && registers.sie_ctrl == 0x800a && !irq_state);
    hub_rx_timeouts = UINT32_MAX;
    hub_usb_hcd_note_rx_timeout();
    assert(hub_rx_timeouts == UINT32_MAX && logs == 4);

    root_speed = 0;
    assert(hub_usb_hcd_bus_flags() == SIE_CTRL_BASE);
    registers.ints = USB_INTS_HOST_CONN_DIS_BITS;
    hcd_rp2040_irq();
    assert(removes == 1 && !attaches);
    root_speed = 1;
    hcd_rp2040_irq();
    assert(attaches == 1 && registers.sie_ctrl == (SIE_CTRL_BASE | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS));
    root_speed = 2;
    hcd_rp2040_irq();
    assert(attaches == 2 && registers.sie_ctrl == (SIE_CTRL_BASE | USB_SIE_CTRL_SOF_EN_BITS));
    assert(!(registers.sie_ctrl & USB_SIE_CTRL_KEEP_ALIVE_EN_BITS));

    epx.active = false;
    registers.inte = USB_INTE_HOST_CONN_DIS_BITS | USB_INTE_BUFF_STATUS_BITS;
    hcd_port_reset(0);
    assert(clear_registers.inte == USB_INTE_HOST_CONN_DIS_BITS && !irq_state);
    registers.inte &= ~clear_registers.inte; /* Apply the hardware CLR alias. */
    assert(registers.inte == USB_INTE_BUFF_STATUS_BITS);
    assert(registers.sie_ctrl == (SIE_CTRL_BASE | USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_RESET_BUS_BITS));
    hcd_port_reset(0); /* A duplicate request must not overwrite the saved IRQ mask. */
    root_speed = 0;
    registers.ints = USB_INTS_HOST_CONN_DIS_BITS & registers.inte;
    hcd_rp2040_irq();
    root_speed = 2;
    hcd_rp2040_irq();
    assert(attaches == 2 && removes == 1);
    registers.sie_ctrl &= ~USB_SIE_CTRL_RESET_BUS_BITS; /* Hardware self-clears the reset pulse. */
    hcd_port_reset_end(0);
    assert(clear_registers.sie_status == USB_SIE_STATUS_SPEED_BITS);
    assert(set_registers.inte == USB_INTE_HOST_CONN_DIS_BITS && !irq_state);
    registers.inte |= set_registers.inte;
    assert(registers.sie_ctrl == (SIE_CTRL_BASE | USB_SIE_CTRL_SOF_EN_BITS));
    root_speed = 0;
    registers.ints = USB_INTS_HOST_CONN_DIS_BITS & registers.inte;
    hcd_rp2040_irq();
    assert(removes == 2); /* A real unplug after reset must still be reported. */
    root_speed = 2;

    registers.inte = USB_INTE_BUFF_STATUS_BITS;
    hcd_port_reset(0);
    registers.sie_ctrl &= ~USB_SIE_CTRL_RESET_BUS_BITS;
    hcd_port_reset_end(0);
    assert(set_registers.inte == 0); /* Preserve a previously disabled connection IRQ. */
    memset(&clear_registers, 0, sizeof(clear_registers));
    epx.active = true;
    hcd_port_reset(0);
    hcd_port_reset_end(0);
    assert(!(registers.sie_ctrl & USB_SIE_CTRL_RESET_BUS_BITS));
    assert(!clear_registers.inte && !clear_registers.sie_status && !set_registers.inte && !irq_state);

    hub_rx_timeouts = 0;
    epx.active = true;
    registers.sie_ctrl |= USB_SIE_CTRL_SEND_SETUP_BITS | USB_SIE_CTRL_START_TRANS_BITS;
    registers.ints = USB_INTS_ERROR_RX_TIMEOUT_BITS | USB_INTS_TRANS_COMPLETE_BITS |
                     USB_INTS_BUFF_STATUS_BITS;
    registers.buf_status = 1u | 4u; /* Failed EP0 plus a valid Hub interrupt transfer. */
    memset(&clear_registers, 0, sizeof(clear_registers));
    hcd_rp2040_irq();
    assert(completions == 1 && last_result == XFER_RESULT_FAILED && !epx.active);
    assert(!transfer_completions && interrupt_completions == 1 && hub_rx_timeouts == 1);
    assert(registers.sie_ctrl == (SIE_CTRL_BASE | USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_STOP_TRANS_BITS));
    assert(clear_registers.buf_status == 1u);

    /* The hardware may raise a late timeout after STOP_TRANS; never complete twice. */
    registers.ints = USB_INTS_ERROR_RX_TIMEOUT_BITS;
    hcd_rp2040_irq();
    assert(completions == 1 && hub_rx_timeouts == 2);
    for (unsigned i = 0; i < 3; ++i) {
        epx.active = true; /* Upper-layer retries are allowed to finish with an error. */
        hcd_rp2040_irq();
    }
    assert(completions == 4 && !epx.active);
    epx.active = true;
    registers.ints = USB_INTS_STALL_BITS | USB_INTS_ERROR_RX_TIMEOUT_BITS |
                     USB_INTS_TRANS_COMPLETE_BITS;
    hcd_rp2040_irq();
    assert(completions == 5 && last_result == XFER_RESULT_STALLED && !transfer_completions);
    epx.active = true;
    registers.ints = USB_INTS_TRANS_COMPLETE_BITS;
    hcd_rp2040_irq();
    assert(completions == 6 && last_result == XFER_RESULT_SUCCESS && transfer_completions == 1);
    const uint8_t setup_request[] = {0x80, 6, 0, 1, 0, 0, 8, 0};
    for (unsigned pre = 0; pre < 2; ++pre) {
        setup_pre = pre != 0;
        epx.active = false;
        epx.wMaxPacketSize = 8;
        registers.sie_ctrl = hub_usb_hcd_bus_flags();
        assert(hcd_setup_send(0, (uint8_t)(pre * 5u), setup_request));
        assert(wire_calls == pre + 1 && setup_delays == wire_calls);
        assert(registers.sie_ctrl == (hub_usb_hcd_bus_flags() | USB_SIE_CTRL_SEND_SETUP_BITS |
               USB_SIE_CTRL_START_TRANS_BITS | (pre ? USB_SIE_CTRL_PREAMBLE_EN_BITS : 0)));
    }
    printf("USB HCD: production init/IRQ/reset/SETUP, capture-before-start, timing/readback and isolation passed (RP2350=%u)\n",
           (unsigned)PICO_RP2350);
    return 0;
}
