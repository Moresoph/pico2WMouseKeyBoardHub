#include <assert.h>
#include <stdio.h>
#include "../src/hub_usb_host.c"

static bool init_result, configured[USB_ADDRESSES];
static unsigned timer_adds, transport_polls, initializations, root_reads, trace_polls, wire_polls, hub_polls;
static uint16_t vid = 0x0a12, pid = 1;
static bool ids_valid = true;
bool tusb_rhport_init(uint8_t port, const tusb_rhport_init_t *config) {
    assert(port == 0 && config->role == TUSB_ROLE_HOST && config->speed == TUSB_SPEED_FULL);
    ++initializations;
    if (init_result) observer.init();
    return init_result;
}
bool tuh_vid_pid_get(uint8_t address, uint16_t *vendor, uint16_t *product) {
    assert(address && address < USB_ADDRESSES);
    *vendor = vid; *product = pid;
    return ids_valid;
}
bool tuh_mounted(uint8_t address) {
    assert(address && address < USB_ADDRESSES);
    return configured[address];
}
tusb_speed_t tuh_speed_get(uint8_t address) { (void)address; return TUSB_SPEED_FULL; }
void tuh_task_ext(uint32_t timeout, bool in_isr) {
    assert(!timeout && !in_isr); ++transport_polls;
}
bool tuh_task_event_ready(void) { return false; }
bool hcd_port_connect_status(uint8_t port) { assert(port == 0); ++root_reads; return true; }
uint32_t hcd_frame_number(uint8_t port) { assert(port == 0); return 1234; }
void hub_usb_hcd_get_state(hub_usb_hcd_state_t *state) { memset(state, 0, sizeof(*state)); }
void hub_usb_trace_poll(void) { ++trace_polls; }
void hub_usb_trace_status(void) {}
void hub_usb_wire_poll(void) { ++wire_polls; }
void hub_usb_wire_status(void) {}
void hub_usb_enum_poll(void) {}
void hub_usb_hub_poll(void) { ++hub_polls; }
void hub_usb_enum_status(void) {}
void hub_usb_mouse_init(void) {}
void hub_usb_mouse_poll(void) {}
void hub_usb_mouse_status(void) {}
void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t delay) {
    assert(timer == &usb_timer && delay == 1);
}
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer,
        void (*handler)(btstack_timer_source_t *)) { timer->process = handler; }
void btstack_run_loop_add_timer(btstack_timer_source_t *timer) {
    assert(timer == &usb_timer); ++timer_adds;
}
int main(void) {
    assert(!hub_usb_host_init() && !started && !timer_adds);
    init_result = true;
    assert(hub_usb_host_init() && started && timer_adds == 1);
    assert(hub_usb_host_init() && initializations == 2 && timer_adds == 1);
    uint8_t count;
    const usbh_class_driver_t *driver = usbh_app_driver_get_cb(&count);
    assert(count == 1 && driver == &observer);
    uint8_t hci[] = {9,4,0,0,3,0xe0,1,1,0, 7,5,0x81,3,16,0,1,
                     7,5,0x82,2,64,0,0, 7,5,2,2,64,0,0};
    assert(!driver->open(0, 1, (const tusb_desc_interface_t *)hci, sizeof(hci)));
    assert(peers[1].seen && peers[1].bluetooth_hci && !peers[1].announced);
    assert(peers[1].vid == vid && peers[1].pid == pid);
    usb_timer.process(&usb_timer);
    assert(transport_polls == 1 && polls == 1 && !mounts);
    configured[1] = true;
    usb_timer.process(&usb_timer);
    usb_timer.process(&usb_timer);
    assert(mounts == 1 && peers[1].announced && timer_adds == 4);
    const uint8_t hub[] = {9,4,0,0,1,9,0,0,0, 7,5,0x81,3,1,0,12};
    uint8_t hub_address = CFG_TUH_DEVICE_MAX + 1;
    assert(!driver->open(0, hub_address, (const tusb_desc_interface_t *)hub, sizeof(hub)));
    configured[hub_address] = true;
    usb_timer.process(&usb_timer);
    assert(peers[hub_address].hub && mounts == 2);
    hub_usb_host_status();
    assert(root_reads == 1 && polls == transport_polls && trace_polls == polls &&
           wire_polls == polls && hub_polls == polls);
    driver->close(1);
    assert(!peers[1].seen && removals == 1 && peers[hub_address].hub);
    driver->close(1);
    assert(removals == 1);
    hci[5] = 0xff;
    assert(!driver->open(0, 1, (const tusb_desc_interface_t *)hci, sizeof(hci)));
    assert(peers[1].seen && !peers[1].bluetooth_hci); /* Address reuse clears classification. */
    driver->close(1);
    ids_valid = false;
    assert(!driver->open(0, 1, (const tusb_desc_interface_t *)hci, sizeof(hci)) && !peers[1].seen);
    ids_valid = true;
    hci[9] = 0;
    assert(!driver->open(0, 1, (const tusb_desc_interface_t *)hci, sizeof(hci)));
    assert(malformed == 1 && !peers[1].seen);
    assert(!driver->open(0, USB_ADDRESSES, (const tusb_desc_interface_t *)hci, sizeof(hci)));
    driver->close(hub_address);
    hub_usb_host_status();
    assert(observer.deinit());
    puts("USB Host probe: initialization, nonblocking poll, unclaimed interfaces, hub and unplug/reuse passed");
    return 0;
}
