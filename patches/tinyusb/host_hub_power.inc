bool hub_usb_hub_power_pending(void) {
  for (unsigned i = 0; i < CFG_TUH_HUB; ++i) {
    if (hub_data[i].power_waiting) return true;
  }
  return false;
}

void hub_usb_hub_poll(void) {
  uint32_t const now = (uint32_t)(time_us_64() / 1000u);
  for (unsigned i = 0; i < CFG_TUH_HUB; ++i) {
    hub_interface_t* hub = &hub_data[i];
    if (!hub->power_waiting || (uint32_t)(now - hub->power_started_ms) < hub->power_good_ms) continue;
    uint8_t const address = (uint8_t)(CFG_TUH_DEVICE_MAX + 1u + i);
    hub->power_waiting = false;
    TU_LOG1("[usb-hub] hub=%u power-good elapsed=%lu ms (required=%u)\n", address,
            (unsigned long)(now - hub->power_started_ms), hub->power_good_ms);
    if (!hub_edpt_status_xfer(address)) {
      hub_usb_enum_block(address, 0, "Hub status polling submission failed");
      return;
    }
    usbh_driver_set_config_complete(address, hub->itf_num);
  }
}
