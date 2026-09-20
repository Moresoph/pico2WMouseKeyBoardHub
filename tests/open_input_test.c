#define main normal_input_tests_main
#include "input_test.c"
#undef main

int main(void) {
    assert(HUB_CLASSIC_OPEN_TEST);
    reset_fixture();
    seed_target(0, false, 1, 3, 1);
    hub_input_start();
    inputs[0].paused = true;
    inputs[0].state = INPUT_FAILED;
    assert(!hub_input_open_bridge_enabled());
    assert(!hub_input_set_open_bridge(true));
    assert(!hub_input_enroll_start());
    assert(!hub_input_connect(1, true, 0, "00:18:00:01:02:03"));
    assert(hub_input_open_test_connect(1, "00:18:00:01:02:03"));
    input_slot_t *slot = &inputs[1];
    assert(slot->open_test && !slot->pairing_authorized && !slot->stored);
    slot->handle = HCI_CON_HANDLE_INVALID; /* Mock reports zero encryption for this handle. */
    slot->state = INPUT_DISCOVERING;
    slot->service_count = 1;
    assert(save_descriptor(slot, 0, keyboard_map, sizeof(keyboard_map)));
    input_ready(slot);
    assert(slot->state == INPUT_READY && !slot->security_complete && !slot->stored);
    assert(flash_lengths[1] == 0 && flash_lengths[0] == HUB_INPUT_TARGET_BYTES);
    unsigned keys_before = routed_keyboard, mouse_before = routed_mouse;
    const uint8_t report[] = {1};
    handle_report(slot, 0, report, sizeof(report));
    assert(slot->test_reports == 1);
    assert(routed_keyboard == keys_before && routed_mouse == mouse_before);
    assert(!hub_input_keyboard_ready());
    assert(hub_input_set_open_bridge(true));
    assert(hub_input_open_bridge_enabled() && hub_input_keyboard_ready());
    handle_report(slot, 0, report, sizeof(report));
    assert(routed_keyboard == keys_before + 1);
    assert(hub_input_set_open_bridge(false));
    handle_report(slot, 0, report, sizeof(report));
    assert(routed_keyboard == keys_before + 1 && !hub_input_keyboard_ready());
    assert(!hub_input_is_expected_classic(slot->address));
    close_input(slot, 0x13, true);
    finish_disconnect(slot);
    assert(slot->state == INPUT_FAILED && !slot->retry_allowed);
    assert(inputs[0].stored && le_db[0].key);
    uint8_t incoming[14] = {HCI_EVENT_HID_META, 12, HID_SUBEVENT_INCOMING_CONNECTION};
    little_endian_store_16(incoming, 3, 7);
    reverse_bytes(slot->address, incoming + 5, 6);
    little_endian_store_16(incoming, 11, 11);
    incoming[5] ^= 1;
    input_hid_event(HCI_EVENT_PACKET, 0, incoming, sizeof(incoming));
    assert(incoming_declines == 1 && incoming_accepts == 0);
    incoming[5] ^= 1;
    slot->paused = true;
    input_hid_event(HCI_EVENT_PACKET, 0, incoming, sizeof(incoming));
    assert(incoming_declines == 2 && incoming_accepts == 0);
    slot->paused = false;
    slot->open_test = false;
    input_hid_event(HCI_EVENT_PACKET, 0, incoming, sizeof(incoming));
    assert(incoming_declines == 3 && incoming_accepts == 0);
    slot->open_test = true;
    input_hid_event(HCI_EVENT_PACKET, 0, incoming, sizeof(incoming));
    assert(incoming_accepts == 1 && slot->state == INPUT_CONNECTING);
    assert(slot->cid == 7 && slot->handle == 11 && establishing == 1);
    assert(!slot->stored && flash_lengths[1] == 0);
    puts("All isolated Level 0 input tests passed.");
    return 0;
}
