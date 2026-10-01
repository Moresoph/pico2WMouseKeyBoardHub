// USB 2.0 9.1.2 / Figure 7-29: debounce precedes reset; recovery is a separate wait.
enum {
  HUB_ENUM_DEBOUNCE_MS = 150,
  HUB_ENUM_STATUS_INTERVAL_MS = 10,
  HUB_ENUM_RESET_WAIT_MS = 20,
  HUB_ENUM_RESET_TIMEOUT_MS = 200,
  HUB_ENUM_RECOVERY_MS = 10,
  HUB_ENUM_PREPARE_TIMEOUT_MS = 2000
};
enum { HUB_PREP_DEBOUNCE, HUB_PREP_RESET, HUB_PREP_RECOVERY };
enum {
  HUB_PREP_READ_DEBOUNCE, HUB_PREP_CLEAR_CONNECTION, HUB_PREP_CLEAR_OLD_RESET,
  HUB_PREP_SEND_RESET, HUB_PREP_READ_RESET, HUB_PREP_CLEAR_RESET, HUB_PREP_READ_READY
};
static struct {
  hub_port_status_response_t status;
  uint32_t generation, started_ms, stable_ms, reset_ms, wait_started_ms, wait_ms;
  uint32_t resets, ready, errors;
  uint8_t hub, port, phase, operation;
  bool active, pending;
  const char* error;
} hub_enum_prepare;

static void process_enumeration(tuh_xfer_t* xfer);
static void hub_enum_prepare_complete(tuh_xfer_t* xfer);

static uint32_t hub_enum_now(void) {
  return (uint32_t)(time_us_64() / 1000u);
}

static bool hub_enum_prepare_active(void) {
  return hub_enum_prepare.active;
}

static void hub_enum_prepare_removed(uint8_t address) {
  if (address != hub_enum_prepare.hub) return;
  if (hub_enum_prepare.active) _dev0.enumerating = 0;
  ++hub_enum_prepare.generation;
  hub_enum_prepare.active = hub_enum_prepare.pending = false;
}

static void hub_enum_prepare_fail(const char* error, bool outstanding) {
  hub_enum_prepare.active = false;
  hub_enum_prepare.error = error;
  ++hub_enum_prepare.errors;
  TU_LOG1("[usb-enum] prepare FAILED hub=%u port=%u: %s\n",
          hub_enum_prepare.hub, hub_enum_prepare.port, error);
  if (outstanding) {
    // A missing callback may still own the control buffer; only removal can release it.
    hub_usb_enum_block(hub_enum_prepare.hub, hub_enum_prepare.port, error);
  } else {
    hub_enum_prepare.pending = false;
    hub_enum_failed();
  }
}

static void hub_enum_prepare_wait(uint8_t phase, uint32_t delay) {
  hub_enum_prepare.phase = phase;
  hub_enum_prepare.wait_started_ms = hub_enum_now();
  hub_enum_prepare.wait_ms = delay;
}

static void hub_enum_prepare_send(uint8_t operation) {
  hub_enum_prepare.operation = operation;
  hub_enum_prepare.pending = true;
  uintptr_t const token = ++hub_enum_prepare.generation;
  bool accepted;
  if (operation == HUB_PREP_READ_DEBOUNCE || operation == HUB_PREP_READ_RESET ||
      operation == HUB_PREP_READ_READY) {
    accepted = hub_port_get_status(hub_enum_prepare.hub, hub_enum_prepare.port,
                                  &hub_enum_prepare.status, hub_enum_prepare_complete, token);
  } else if (operation == HUB_PREP_SEND_RESET) {
    accepted = hub_port_reset(hub_enum_prepare.hub, hub_enum_prepare.port,
                             hub_enum_prepare_complete, token);
  } else {
    uint8_t const feature = operation == HUB_PREP_CLEAR_CONNECTION ?
                           HUB_FEATURE_PORT_CONNECTION_CHANGE : HUB_FEATURE_PORT_RESET_CHANGE;
    accepted = hub_port_clear_feature(hub_enum_prepare.hub, hub_enum_prepare.port,
                                     feature, hub_enum_prepare_complete, token);
  }
  if (!accepted)
    hub_enum_prepare_fail("control submission failed", _ctrl_xfer.stage != CONTROL_STAGE_IDLE);
}

static void hub_enum_prepare_begin(void) {
  hub_enum_prepare.hub = _dev0.hub_addr;
  hub_enum_prepare.port = _dev0.hub_port;
  hub_enum_prepare.active = true;
  hub_enum_prepare.pending = false;
  hub_enum_prepare.error = NULL;
  hub_enum_prepare.started_ms = hub_enum_prepare.stable_ms = hub_enum_now();
  ++hub_enum_prepare.generation;
  hub_enum_prepare_wait(HUB_PREP_DEBOUNCE, 0);
  TU_LOG1("[usb-enum] prepare hub=%u port=%u: debounce=%u ms BEFORE reset\n",
          hub_enum_prepare.hub, hub_enum_prepare.port, HUB_ENUM_DEBOUNCE_MS);
}

static void hub_enum_prepare_complete(tuh_xfer_t* xfer) {
  if (!hub_enum_prepare.active || !hub_enum_prepare.pending ||
      xfer->daddr != hub_enum_prepare.hub || xfer->user_data != hub_enum_prepare.generation) {
    TU_LOG1("[usb-enum] ignored late prepare completion\n");
    return;
  }
  hub_enum_prepare.pending = false;
  if (xfer->result != XFER_RESULT_SUCCESS) {
    hub_enum_prepare_fail("control transfer failed", false);
    return;
  }
  uint32_t const now = hub_enum_now();
  uint8_t const operation = hub_enum_prepare.operation;
  switch (operation) {
    case HUB_PREP_CLEAR_CONNECTION:
    case HUB_PREP_CLEAR_OLD_RESET:
      hub_enum_prepare.stable_ms = now;
      hub_enum_prepare_wait(HUB_PREP_DEBOUNCE, 0);
      return;
    case HUB_PREP_SEND_RESET:
      hub_enum_prepare.reset_ms = now;
      ++hub_enum_prepare.resets;
      hub_enum_prepare_wait(HUB_PREP_RESET, HUB_ENUM_RESET_WAIT_MS);
      return;
    case HUB_PREP_CLEAR_RESET:
      hub_enum_prepare_wait(HUB_PREP_RECOVERY, HUB_ENUM_RECOVERY_MS);
      return;
    default:
      break;
  }
  if (xfer->actual_len != sizeof(hub_enum_prepare.status)) {
    hub_enum_prepare_fail("port status read short", false);
    return;
  }
  uint16_t const status = tu_le16toh(hub_enum_prepare.status.status.value);
  uint16_t const change = tu_le16toh(hub_enum_prepare.status.change.value);
  if (!(status & (1u << HUB_FEATURE_PORT_CONNECTION))) {
    TU_LOG1("[usb-enum] prepare cancelled: hub=%u port=%u disconnected\n",
            hub_enum_prepare.hub, hub_enum_prepare.port);
    hub_enum_prepare.active = false;
    enum_full_complete();
    return;
  }
  if (!(status & (1u << HUB_FEATURE_PORT_POWER)) || (status & (1u << HUB_FEATURE_PORT_OVER_CURRENT))) {
    hub_enum_prepare_fail("port unpowered or over-current", false);
    return;
  }
  if (operation == HUB_PREP_READ_DEBOUNCE) {
    if (change & (1u << HUB_FEATURE_PORT_CONNECTION)) {
      hub_enum_prepare_send(HUB_PREP_CLEAR_CONNECTION);
    } else if (change & (1u << HUB_FEATURE_PORT_RESET)) {
      hub_enum_prepare_send(HUB_PREP_CLEAR_OLD_RESET);
    } else if (status & (1u << HUB_FEATURE_PORT_RESET)) {
      hub_enum_prepare.stable_ms = now;
      hub_enum_prepare_wait(HUB_PREP_DEBOUNCE, HUB_ENUM_STATUS_INTERVAL_MS);
    } else if ((uint32_t)(now - hub_enum_prepare.stable_ms) >= HUB_ENUM_DEBOUNCE_MS) {
      TU_LOG1("[usb-enum] reset hub=%u port=%u AFTER stable=%lu ms\n",
              hub_enum_prepare.hub, hub_enum_prepare.port,
              (unsigned long)(now - hub_enum_prepare.stable_ms));
      hub_enum_prepare_send(HUB_PREP_SEND_RESET);
    } else {
      hub_enum_prepare_wait(HUB_PREP_DEBOUNCE, HUB_ENUM_STATUS_INTERVAL_MS);
    }
    return;
  }
  if (change & (1u << HUB_FEATURE_PORT_CONNECTION)) {
    hub_enum_prepare_fail("connection changed during reset/recovery", false);
    return;
  }
  if (operation == HUB_PREP_READ_RESET) {
    if (!(status & (1u << HUB_FEATURE_PORT_RESET)) && (change & (1u << HUB_FEATURE_PORT_RESET))) {
      if (!(status & (1u << HUB_FEATURE_PORT_ENABLE))) {
        hub_enum_prepare_fail("reset completed without port enable", false);
      } else {
        hub_enum_prepare_send(HUB_PREP_CLEAR_RESET);
      }
    } else if ((uint32_t)(now - hub_enum_prepare.reset_ms) >= HUB_ENUM_RESET_TIMEOUT_MS) {
      hub_enum_prepare_fail("port reset completion timeout", false);
    } else {
      hub_enum_prepare_wait(HUB_PREP_RESET, HUB_ENUM_STATUS_INTERVAL_MS);
    }
    return;
  }
  if (!(status & (1u << HUB_FEATURE_PORT_ENABLE)) ||
      (status & (1u << HUB_FEATURE_PORT_RESET)) || (change & (1u << HUB_FEATURE_PORT_RESET))) {
    hub_enum_prepare_fail("port not ready after reset recovery", false);
    return;
  }
  _dev0.speed = (status & (1u << 10)) ? TUSB_SPEED_HIGH :
               (status & (1u << HUB_FEATURE_PORT_LOW_SPEED)) ? TUSB_SPEED_LOW : TUSB_SPEED_FULL;
  hub_enum_prepare.active = false;
  ++hub_enum_prepare.ready;
  TU_LOG1("[usb-enum] ready hub=%u port=%u status=%04x after reset recovery=%lu ms\n",
          hub_enum_prepare.hub, hub_enum_prepare.port, status,
          (unsigned long)(now - hub_enum_prepare.wait_started_ms));
  tuh_xfer_t ready = {.daddr = hub_enum_prepare.hub, .result = XFER_RESULT_SUCCESS,
                      .user_data = ENUM_ADDR0_DEVICE_DESC};
  process_enumeration(&ready);
}

static void hub_enum_prepare_poll(void) {
  if (!hub_enum_prepare.active) return;
  uint32_t const now = hub_enum_now();
  if ((uint32_t)(now - hub_enum_prepare.started_ms) >= HUB_ENUM_PREPARE_TIMEOUT_MS) {
    hub_enum_prepare_fail("prepare completion timeout",
                          hub_enum_prepare.pending || _ctrl_xfer.stage != CONTROL_STAGE_IDLE);
    return;
  }
  if (hub_enum_prepare.pending || _ctrl_xfer.stage != CONTROL_STAGE_IDLE ||
      (uint32_t)(now - hub_enum_prepare.wait_started_ms) < hub_enum_prepare.wait_ms) return;
  uint8_t const operation = hub_enum_prepare.phase == HUB_PREP_DEBOUNCE ? HUB_PREP_READ_DEBOUNCE :
                           hub_enum_prepare.phase == HUB_PREP_RESET ? HUB_PREP_READ_RESET : HUB_PREP_READ_READY;
  hub_enum_prepare_send(operation);
}

static void hub_enum_prepare_status(void) {
  printf("[usb-enum] prepare-active=%u pending=%u phase=%u hub=%u port=%u resets=%lu ready=%lu errors=%lu",
         hub_enum_prepare.active, hub_enum_prepare.pending, hub_enum_prepare.phase,
         hub_enum_prepare.hub, hub_enum_prepare.port, (unsigned long)hub_enum_prepare.resets,
         (unsigned long)hub_enum_prepare.ready, (unsigned long)hub_enum_prepare.errors);
  if (hub_enum_prepare.error) printf(" error=%s", hub_enum_prepare.error);
  printf("\n");
}
