static uint32_t hub_root_connection_irq;
static bool hub_root_reset_active;

static uint32_t hub_usb_hcd_bus_flags(void) {
    uint8_t speed = dev_speed();
    return SIE_CTRL_BASE | (speed == 1 ? USB_SIE_CTRL_KEEP_ALIVE_EN_BITS :
                           speed == 2 ? USB_SIE_CTRL_SOF_EN_BITS : 0);
}

static void hub_usb_hcd_fail_epx(xfer_result_t result) {
    /* Stop the hardware retry loop before handing the failed transfer back. */
    usb_hw->sie_ctrl = hub_usb_hcd_bus_flags() | USB_SIE_CTRL_STOP_TRANS_BITS;
    usb_hw_clear->buf_status = 1u;
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
    if (epx.active) hw_xfer_complete(&epx, result);
}
