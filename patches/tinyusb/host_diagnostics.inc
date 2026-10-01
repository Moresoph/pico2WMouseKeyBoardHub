static uint32_t hub_rx_timeouts;

static void hub_usb_hcd_note_rx_timeout(void) {
    if (hub_rx_timeouts == UINT32_MAX) return;
    ++hub_rx_timeouts;
    if ((hub_rx_timeouts & (hub_rx_timeouts - 1u)) == 0) {
        TU_LOG1("[usb-hcd] RX-timeout count=%lu address=%u ep=%02x active=%u sie=%08lx\n",
                (unsigned long)hub_rx_timeouts, epx.dev_addr, epx.ep_addr, epx.active,
                (unsigned long)usb_hw->sie_ctrl);
    }
}

void hub_usb_hcd_get_state(hub_usb_hcd_state_t *state) {
    uint32_t irq = save_and_disable_interrupts();
    *state = (hub_usb_hcd_state_t) {
        .rx_timeouts = hub_rx_timeouts,
        .sie_ctrl = usb_hw->sie_ctrl,
        .sie_status = usb_hw->sie_status,
        .ints = usb_hw->ints,
        .inte = usb_hw->inte,
        .address_control = usb_hw->dev_addr_ctrl,
        .buffer_control = epx.configured ? *epx.buffer_control : 0,
        .remaining = epx.remaining_len,
        .transferred = epx.xferred_len,
        .packet_size = epx.wMaxPacketSize,
        .address = epx.dev_addr,
        .endpoint = epx.ep_addr,
        .active = epx.active
    };
#if defined(PICO_RP2350) && PICO_RP2350
    state->linestate_tuning = usb_hw->linestate_tuning;
    state->hub_timing_supported = true;
    state->hub_timing_enabled =
        (state->linestate_tuning & USB_LINESTATE_TUNING_MULTI_HUB_FIX_BITS) != 0;
#endif
    restore_interrupts(irq);
}
