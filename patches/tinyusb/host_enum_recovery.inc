enum { HUB_ENUM_CLEANUP_TIMEOUT_MS = 2000 };
static bool hub_enum_prepare_active(void);
static void hub_enum_prepare_poll(void);
static void hub_enum_prepare_status(void);
static void hub_enum_prepare_removed(uint8_t address);
static struct {
  hub_port_status_response_t status;
  uint32_t generation, started_ms, disabled_count, failure_count;
  uint8_t rhport, hub, port;
  bool active, blocked, status_valid;
  const char* error;
} hub_enum_cleanup;

static bool hub_enum_cleanup_pending(void) {
  return hub_enum_cleanup.active || hub_enum_prepare_active() || hub_usb_hub_power_pending();
}

static bool hub_enum_cleanup_blocked(void) {
  return hub_enum_cleanup.blocked;
}

bool hub_usb_enum_can_rearm(void) {
  return !_dev0.enumerating && !hub_enum_cleanup_pending() && !hub_enum_cleanup.blocked;
}

static void hub_enum_cleanup_failed(const char* error) {
  hub_enum_cleanup.active = false;
  hub_enum_cleanup.blocked = true;
  hub_enum_cleanup.error = error;
  ++hub_enum_cleanup.failure_count;
  _dev0.enumerating = 0;
  TU_LOG1("[usb-enum] cleanup FAILED hub=%u port=%u: %s; reconnect Hub to resume\n",
          hub_enum_cleanup.hub, hub_enum_cleanup.port, error);
}

void hub_usb_enum_poll(void) {
  hub_enum_prepare_poll();
  if (hub_enum_cleanup.active &&
      (uint32_t)((uint32_t)(time_us_64() / 1000u) - hub_enum_cleanup.started_ms) >= HUB_ENUM_CLEANUP_TIMEOUT_MS) {
    // Do not reuse an outstanding control buffer or start another enumeration.
    hub_enum_cleanup_failed("control completion timeout");
  }
}

void hub_usb_enum_status(void) {
  hub_enum_prepare_status();
  printf("[usb-enum] cleanup-active=%u blocked=%u hub=%u port=%u disabled=%lu errors=%lu",
         hub_enum_cleanup.active, hub_enum_cleanup.blocked,
         hub_enum_cleanup.hub, hub_enum_cleanup.port,
         (unsigned long)hub_enum_cleanup.disabled_count, (unsigned long)hub_enum_cleanup.failure_count);
  if (hub_enum_cleanup.status_valid)
    printf(" port-status=%04x", tu_le16toh(hub_enum_cleanup.status.status.value));
  if (hub_enum_cleanup.error) printf(" error=%s; reconnect Hub to resume", hub_enum_cleanup.error);
  printf("\n");
}

static void hub_enum_parent_removed(uint8_t address) {
  hub_enum_prepare_removed(address);
  if (address != hub_enum_cleanup.hub) return;
  if (hub_enum_cleanup.active || hub_enum_cleanup.blocked) _dev0.enumerating = 0;
  ++hub_enum_cleanup.generation;
  hub_enum_cleanup.active = hub_enum_cleanup.blocked = false;
  hub_enum_cleanup.status_valid = false;
  hub_enum_cleanup.error = NULL;
  hub_enum_cleanup.hub = hub_enum_cleanup.port = 0;
}

void hub_usb_enum_block(uint8_t hub, uint8_t port, const char* error) {
  hub_enum_cleanup.hub = hub;
  hub_enum_cleanup.port = port;
  hub_enum_cleanup.status_valid = false;
  hub_enum_cleanup_failed(error);
}

void hub_usb_enum_cancel_power_wait(void) {
  _dev0.enumerating = 0;
}

static bool hub_enum_current_callback(tuh_xfer_t const* xfer) {
  if (hub_enum_cleanup.active && xfer->user_data == hub_enum_cleanup.generation &&
      xfer->daddr == hub_enum_cleanup.hub) return true;
  TU_LOG1("[usb-enum] ignored late cleanup completion\n");
  return false;
}

static void hub_enum_disabled_status(tuh_xfer_t* xfer) {
  if (!hub_enum_current_callback(xfer)) return;
  if (xfer->result != XFER_RESULT_SUCCESS || xfer->actual_len != sizeof(hub_enum_cleanup.status)) {
    hub_enum_cleanup_failed("port status read failed/short");
    return;
  }
  hub_enum_cleanup.status_valid = true;
  uint16_t const status = tu_le16toh(hub_enum_cleanup.status.status.value);
  if (status & (1u << HUB_FEATURE_PORT_ENABLE)) {
    hub_enum_cleanup_failed("port still enabled after disable");
    return;
  }
  TU_LOG1("[usb-enum] disabled hub=%u port=%u verified status=%04x (no power-switch request)\n",
          hub_enum_cleanup.hub, hub_enum_cleanup.port, status);
  // Release any address reserved before a later enumeration failure.
  process_removing_device(hub_enum_cleanup.rhport, hub_enum_cleanup.hub, hub_enum_cleanup.port);
  hcd_device_close(hub_enum_cleanup.rhport, 0);
  ++hub_enum_cleanup.disabled_count;
  hub_enum_cleanup.active = false;
  enum_full_complete();
}

static void hub_enum_disable_complete(tuh_xfer_t* xfer) {
  if (!hub_enum_current_callback(xfer)) return;
  if (xfer->result != XFER_RESULT_SUCCESS) {
    hub_enum_cleanup_failed("port disable transfer failed");
    return;
  }
  if (!hub_port_get_status(hub_enum_cleanup.hub, hub_enum_cleanup.port,
                          &hub_enum_cleanup.status, hub_enum_disabled_status,
                          hub_enum_cleanup.generation)) {
    hub_enum_cleanup_failed("port status submission failed");
  }
}

static void hub_enum_failed(void) {
  if (!_dev0.enumerating || !_dev0.hub_addr) {
    enum_full_complete();
    return;
  }
  hub_enum_cleanup.rhport = _dev0.rhport;
  hub_enum_cleanup.hub = _dev0.hub_addr;
  hub_enum_cleanup.port = _dev0.hub_port;
  hub_enum_cleanup.active = true;
  hub_enum_cleanup.blocked = hub_enum_cleanup.status_valid = false;
  hub_enum_cleanup.error = NULL;
  ++hub_enum_cleanup.generation;
  hub_enum_cleanup.started_ms = (uint32_t)(time_us_64() / 1000u);
  TU_LOG1("[usb-enum] disabling failed hub=%u port=%u before next enumeration\n",
          hub_enum_cleanup.hub, hub_enum_cleanup.port);
  if (!hub_port_clear_feature(hub_enum_cleanup.hub, hub_enum_cleanup.port,
                             HUB_FEATURE_PORT_ENABLE, hub_enum_disable_complete,
                             hub_enum_cleanup.generation)) {
    hub_enum_cleanup_failed("port disable submission failed");
  }
}
