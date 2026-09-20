#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef __TINYC__
#define OUTPUT_TEST_JOIN_(a, b) a##b
#define OUTPUT_TEST_JOIN(a, b) OUTPUT_TEST_JOIN_(a, b)
#define _Static_assert(condition, message) \
    typedef char OUTPUT_TEST_JOIN(output_static_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

/* Exercise production persistence, logical mapping, and all queued callbacks.
   Only controller/ATT/TLV/router boundaries are mocked. */
#include "..\src\hub_output_store.c"
#include "..\src\hub_output.c"

#define TEST_HANDLES 32
static uint8_t flash_record[HUB_OUTPUT_STORE_BYTES + 1];
static int flash_length;
static unsigned flash_writes, flash_attempts;
static bool fail_write, fail_readback, no_tlv, fail_read;
static bool synchronous_send;
static uint8_t notify_error, disconnect_error, registration_error;
static uint32_t clock_ms, clock_us;
static unsigned notify_attempts, registration_attempts, callback_depth, max_callback_depth;
static btstack_context_callback_registration_t *main_callbacks[16];
static unsigned main_callback_count;
static bool yield_timer_pending;
static unsigned yield_timer_delay;
static unsigned disconnects, router_releases, pairing_requests, db_reads;
static struct {
    int type;
    bd_addr_t address;
    int key_size;
} bonds[8];
static struct {
    bool connected;
    int index;
    uint8_t key_size;
    btstack_context_callback_registration_t *pending;
} links[TEST_HANDLES];
static struct {
    hci_con_handle_t handle;
    uint16_t attribute;
    uint8_t data[8];
    uint16_t length;
} notifications[512];
static unsigned notification_count;
static const uint8_t key_down[8] = {0, 0, 4};
static const bd_addr_t private_address = {0x4a, 1, 2, 3, 4, 5};

static int flash_get(void *context, uint32_t tag, uint8_t *data, uint32_t length) {
    (void)context;
    assert(tag == HUB_OUTPUT_STORE_TAG);
    if (fail_read) return -1;
    unsigned copy = (unsigned)flash_length < length ? (unsigned)flash_length : length;
    if (data && copy) {
        memcpy(data, flash_record, copy);
        if (fail_readback) data[0] ^= 1;
    }
    return flash_length;
}

static int flash_store(void *context, uint32_t tag, const uint8_t *data, uint32_t length) {
    (void)context;
    assert(tag == HUB_OUTPUT_STORE_TAG && length == HUB_OUTPUT_STORE_BYTES);
    ++flash_attempts;
    if (fail_write) return -1;
    ++flash_writes;
    memcpy(flash_record, data, length);
    flash_length = (int)length;
    return 0;
}

static void flash_delete(void *context, uint32_t tag) {
    (void)context;
    (void)tag;
    assert(!"Output persistence must never delete records or bonds");
}
static const btstack_tlv_t mock_tlv = {flash_get, flash_store, flash_delete};
void btstack_tlv_get_instance(const btstack_tlv_t **impl, void **context) {
    *impl = no_tlv ? NULL : &mock_tlv;
    *context = NULL;
}
uint32_t btstack_run_loop_get_time_ms(void) { return clock_ms; }
uint32_t hub_latency_now_us(void) { return clock_us; }
void hub_perf_output_wait(uint16_t handle, unsigned slot, bool mouse, uint32_t wait_us) {
    (void)mouse; (void)wait_us;
    assert(computer_for_handle(handle) != NULL);
    assert(slot < HUB_COMPUTER_COUNT && computer_for_handle(handle)->slot == slot);
}
void btstack_run_loop_add_timer(btstack_timer_source_t *timer) {
    assert(timer == &service_timer || timer == &pump_timer);
    if (timer == &pump_timer) {
        assert(!yield_timer_pending);
        yield_timer_pending = true;
    }
}
void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t ms) {
    if (timer == &pump_timer) yield_timer_delay = ms;
}
void btstack_run_loop_execute_on_main_thread(btstack_context_callback_registration_t *registration) {
    assert(main_callback_count < 16);
    for (unsigned i = 0; i < main_callback_count; ++i) assert(main_callbacks[i] != registration);
    main_callbacks[main_callback_count++] = registration;
}
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer,
                                      void (*handler)(btstack_timer_source_t *)) {
    timer->process = handler;
}
void hub_router_release_all(void) { ++router_releases; hub_output_release_all(); }
bool hub_security_pairing_open(void) { return false; }
bool hub_security_allow_pairing(hci_con_handle_t handle) { (void)handle; return false; }
void hci_add_event_handler(btstack_packet_callback_registration_t *registration) {
    (void)registration;
}
void sm_add_event_handler(btstack_packet_callback_registration_t *registration) {
    (void)registration;
}
void sm_request_pairing(hci_con_handle_t handle) { (void)handle; ++pairing_requests; }
int sm_le_device_index(hci_con_handle_t handle) {
    assert(handle < TEST_HANDLES);
    return links[handle].index;
}
int le_device_db_max_count(void) { return 8; }
void le_device_db_info(int i, int *type, bd_addr_t address, sm_key_t irk) {
    assert(i >= 0 && i < 8 && irk == NULL);
    ++db_reads;
    *type = bonds[i].type;
    memcpy(address, bonds[i].address, 6);
}
void le_device_db_encryption_get(int i, uint16_t *ediv, uint8_t rand[8], sm_key_t key,
                                 int *size, int *authenticated, int *authorized, int *sc) {
    assert(i >= 0 && i < 8);
    assert(ediv == NULL && rand == NULL && key == NULL);
    assert(authenticated == NULL && authorized == NULL && sc == NULL);
    *size = bonds[i].key_size;
}
uint8_t gap_encryption_key_size(hci_con_handle_t handle) {
    assert(handle < TEST_HANDLES);
    return links[handle].key_size;
}
hci_connection_t *hci_connection_for_handle(hci_con_handle_t handle) {
    (void)handle;
    return NULL;
}
uint8_t gap_disconnect(hci_con_handle_t handle) {
    assert(handle < TEST_HANDLES);
    ++disconnects;
    return disconnect_error;
}
void gap_set_max_number_peripheral_connections(int count) { assert(count == 4); }
void gap_advertisements_enable(int enabled) { (void)enabled; }
void gap_advertisements_set_params(uint16_t min, uint16_t max, uint8_t type,
                                   uint8_t direct_type, bd_addr_t address,
                                   uint8_t channel_map, uint8_t policy) {
    (void)min; (void)max; (void)type; (void)direct_type;
    (void)address; (void)channel_map; (void)policy;
}
void gap_advertisements_set_data(uint8_t length, uint8_t *data) { (void)length; (void)data; }
void gap_scan_response_set_data(uint8_t length, uint8_t *data) { (void)length; (void)data; }
void att_server_init(const uint8_t *db, att_read_callback_t read_cb, att_write_callback_t write_cb) {
    assert(db == profile_data && read_cb == read_attribute && write_cb == write_attribute);
}
static void call_send(btstack_context_callback_registration_t *registration) {
    ++callback_depth;
    if (callback_depth > max_callback_depth) max_callback_depth = callback_depth;
    assert(callback_depth <= 2); /* Fail a recursive pump before overflowing the stack. */
    registration->callback(registration->context);
    --callback_depth;
}
uint8_t att_server_request_to_send_notification(btstack_context_callback_registration_t *registration,
                                               hci_con_handle_t handle) {
    assert(handle < TEST_HANDLES && links[handle].connected);
    assert(links[handle].pending == NULL);
    assert(((computer_t *)registration->context)->handle == handle);
    ++registration_attempts;
    if (registration_error) return registration_error;
    if (synchronous_send) call_send(registration);
    else links[handle].pending = registration;
    return ERROR_CODE_SUCCESS;
}
uint8_t att_server_notify(hci_con_handle_t handle, uint16_t attribute,
                         const uint8_t *data, uint16_t length) {
    assert(handle < TEST_HANDLES && links[handle].connected);
    assert(links[handle].key_size >= 16);
    computer_t *computer = computer_for_handle(handle);
    assert(computer != NULL && computer->identity_authenticated && !output_store.fault);
    assert(attribute == KEYBOARD_HANDLE || attribute == MOUSE_HANDLE);
    assert(length == (attribute == KEYBOARD_HANDLE ? 8 : 6));
    ++notify_attempts;
    if (notify_error) return notify_error;
    assert(notification_count < 512);
    notifications[notification_count].handle = handle;
    notifications[notification_count].attribute = attribute;
    notifications[notification_count].length = length;
    memcpy(notifications[notification_count].data, data, length);
    ++notification_count;
    return ERROR_CODE_SUCCESS;
}
uint16_t att_read_callback_handle_blob(const uint8_t *data, uint16_t length,
                                      uint16_t offset, uint8_t *buffer, uint16_t size) {
    if (buffer == NULL) return length;
    if (offset > length) return 0;
    uint16_t copy = length - offset < size ? length - offset : size;
    memcpy(buffer, data + offset, copy);
    return copy;
}
uint16_t att_read_callback_handle_little_endian_16(uint16_t value, uint16_t offset,
                                                  uint8_t *buffer, uint16_t size) {
    uint8_t data[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return att_read_callback_handle_blob(data, 2, offset, buffer, size);
}
uint16_t att_read_callback_handle_byte(uint8_t value, uint16_t offset,
                                      uint8_t *buffer, uint16_t size) {
    return att_read_callback_handle_blob(&value, 1, offset, buffer, size);
}
void hci_dump_log(int level, const char *format, ...) { (void)level; (void)format; }
void btstack_assert_failed(const char *file, const uint16_t line) {
    fprintf(stderr, "BTstack assertion: %s:%u\n", file, line);
    abort();
}

static void boot(void) {
    memset(computers, 0, sizeof(computers));
    memset(slot_connections, 0, sizeof(slot_connections));
    memset(&output_store, 0, sizeof(output_store));
    memset(links, 0, sizeof(links));
    for (unsigned i = 0; i < TEST_HANDLES; ++i) links[i].index = -1;
    started = switch_pending = advertising_requested = false;
    active_slot = switch_old_slot = switch_target_slot = 0;
    switch_old_handle = HCI_CON_HANDLE_INVALID;
    notification_count = disconnects = router_releases = pairing_requests = db_reads = 0;
    clock_ms = clock_us = 0;
    notify_attempts = registration_attempts = callback_depth = max_callback_depth = 0;
    main_callback_count = 0;
    yield_timer_pending = false;
    yield_timer_delay = 0;
    hub_output_init();
    hub_output_start();
}

static void fresh(void) {
    memset(flash_record, 0, sizeof(flash_record));
    flash_length = 0;
    flash_writes = flash_attempts = 0;
    fail_write = fail_readback = no_tlv = fail_read = synchronous_send = false;
    notify_error = disconnect_error = registration_error = 0;
    memset(bonds, 0, sizeof(bonds));
    for (unsigned i = 0; i < 8; ++i) {
        bonds[i].type = i == 1 ? 1 : 0;
        bonds[i].address[0] = i == 1 ? 0xc1 : 0x12;
        bonds[i].address[5] = (uint8_t)(i + 1);
        bonds[i].key_size = 16;
    }
    boot();
}

static void tick(void) {
    clock_ms += OUTPUT_SERVICE_INTERVAL_MS;
    clock_us += OUTPUT_SERVICE_INTERVAL_MS * 1000;
    service(&service_timer);
}

static void deliver(unsigned handle) {
    btstack_context_callback_registration_t *registration = links[handle].pending;
    if (!registration) return;
    links[handle].pending = NULL;
    assert(((computer_t *)registration->context)->handle == handle);
    call_send(registration);
}

static unsigned run_main_callbacks(void) {
    unsigned iterations = 0;
    /* Match base_execute_callbacks: newly appended work runs in this SAME pass. */
    while (main_callback_count) {
        assert(++iterations <= 16);
        btstack_context_callback_registration_t *registration = main_callbacks[0];
        --main_callback_count;
        memmove(main_callbacks, main_callbacks + 1,
                main_callback_count * sizeof(main_callbacks[0]));
        registration->callback(registration->context);
    }
    return iterations;
}

static void yield_tick(void) {
    assert(yield_timer_pending && yield_timer_delay == 1);
    yield_timer_pending = false;
    ++clock_ms;
    clock_us += 1000;
    pump_timer.process(&pump_timer);
}

static void pump(void) {
    for (unsigned iteration = 0; iteration < 5; ++iteration) {
        tick();
        for (unsigned handle = 0; handle < TEST_HANDLES; ++handle) deliver(handle);
    }
}

static void connect_peer(unsigned handle, unsigned role, int index, bool private_addr) {
    uint8_t packet[36] = {HCI_EVENT_META_GAP, 34, GAP_SUBEVENT_LE_CONNECTION_COMPLETE, 0};
    little_endian_store_16(packet, 4, (uint16_t)handle);
    packet[6] = (uint8_t)role;
    packet[7] = private_addr ? 1 : (uint8_t)bonds[index].type;
    reverse_bytes(private_addr ? private_address : bonds[index].address, &packet[8], 6);
    links[handle].connected = true;
    links[handle].index = index;
    hci_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void security_complete(unsigned handle) {
    uint8_t packet[12] = {SM_EVENT_REENCRYPTION_COMPLETE, 10};
    little_endian_store_16(packet, 2, (uint16_t)handle);
    security_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void subscribe(unsigned handle) {
    uint8_t cccd[2] = {1, 0};
    assert(write_attribute((hci_con_handle_t)handle, KEYBOARD_CCCD, 0, 0, cccd, 2) == 0);
    assert(write_attribute((hci_con_handle_t)handle, MOUSE_CCCD, 0, 0, cccd, 2) == 0);
}

static void ready_peer(unsigned handle, int index, bool private_addr) {
    connect_peer(handle, HCI_ROLE_SLAVE, index, private_addr);
    links[handle].key_size = 16;
    subscribe(handle); /* ATT may restore both CCCDs before SM identity is ready. */
    security_complete(handle);
    pump();
    computer_t *computer = computer_for_handle((hci_con_handle_t)handle);
    assert(computer && computer->slot < 4 && hub_output_ready(computer->slot));
}

static void disconnect_peer(unsigned handle) {
    uint8_t packet[6] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0};
    little_endian_store_16(packet, 3, (uint16_t)handle);
    packet[5] = 0x08;
    links[handle].connected = false;
    hci_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    /* SDK's ATT disconnect handler purges its queue; app contexts stay retired
       until the subsequent timer, even if its HCI handler ran first. */
    links[handle].pending = NULL;
    links[handle].key_size = 0;
}

static unsigned nonneutral_notifications(void) {
    unsigned count = 0;
    for (unsigned n = 0; n < notification_count; ++n) {
        for (unsigned b = 0; b < notifications[n].length; ++b) {
            if (notifications[n].data[b]) { ++count; break; }
        }
    }
    return count;
}

static void migration_and_restore(void) {
    fresh();
    assert(active_slot == 0 && flash_writes == 0 && !hub_output_ready(0));
    connect_peer(7, HCI_ROLE_MASTER, 2, false); /* Bonded mouse never claims a slot. */
    links[7].key_size = 16;
    security_complete(7);
    pump();
    assert(computer_for_handle(7) == NULL && flash_writes == 0 && db_reads == 0);
    ready_peer(1, 0, false);
    assert(output_store.identities[0].reserved && flash_writes == 1);
    assert(!memcmp(output_store.identities[0].address, bonds[0].address, 6));
    assert(notification_count == 2 && nonneutral_notifications() == 0);
    assert(hub_output_keyboard(key_down));
    pump();
    assert(nonneutral_notifications() == 1);
    ready_peer(2, 1, false);
    assert(flash_writes == 2 && computer_for_handle(2)->slot == 1);
    boot(); /* Flash and the existing bond DB survive. */
    assert(active_slot == 0 && flash_writes == 2);
    ready_peer(4, 1, false); /* PC2 returns first; selected PC1 remains offline. */
    assert(computer_for_handle(4)->slot == 1 && !hub_output_ready(0));
    assert(!hub_output_keyboard(key_down) && !hub_output_mouse(1, 1, 0, 0));
    pump();
    assert(nonneutral_notifications() == 0 && flash_writes == 2);
    ready_peer(5, 0, false);
    assert(computer_for_handle(5)->slot == 0 && flash_writes == 2);
    assert(hub_output_keyboard(key_down));
    pump();
    assert(notifications[notification_count - 1].handle == 5);
    assert(hub_output_select(1) && flash_writes == 3 && output_store.selected == 1);
    assert(hub_output_switching() && !hub_output_keyboard(key_down));
    pump();
    assert(active_slot == 1 && !hub_output_switching());
    assert(hub_output_select(1) && flash_writes == 3); /* No repeated writes. */
    boot();
    assert(active_slot == 1 && output_store.selected == 1);
    ready_peer(6, 0, false);
    assert(!hub_output_keyboard(key_down) && active_slot == 1);
    ready_peer(8, 1, true); /* Rotated RPA resolves to PC2's static identity. */
    assert(computer_for_handle(8)->slot == 1 && flash_writes == 3);
    assert(hub_output_mouse(1, 3, 4, 0));
    pump();
    assert(notifications[notification_count - 1].handle == 8);
    disconnect_peer(8);
    assert(output_store.identities[1].reserved && active_slot == 1);
    assert(!hub_output_keyboard(key_down) && !hub_output_ready(1));
    pump();
    assert(flash_writes == 3 && output_store.selected == 1);
    hub_output_status();
}

static void provisional_and_duplicates(void) {
    fresh();
    connect_peer(1, HCI_ROLE_SLAVE, 1, true);
    computer_t *physical = computer_for_handle(1);
    links[1].index = -1;
    links[1].key_size = 16;
    subscribe(1);
    security_complete(1);
    pump();
    assert(flash_writes == 0 && physical->slot == 4 && notification_count == 0);
    assert(!hub_output_keyboard(key_down));
    links[1].index = 1;
    tick();
    assert(physical == computer_for_slot(0)); /* No memcpy of queued registrations. */
    assert(physical->keyboard_cccd == 1 && physical->mouse_cccd == 1);
    assert(links[1].pending == &physical->send_registration && !hub_output_ready(0));
    pump();
    assert(hub_output_ready(0) && flash_writes == 1);
    connect_peer(2, HCI_ROLE_SLAVE, 1, true);
    links[2].key_size = 16;
    subscribe(2);
    security_complete(2);
    assert(disconnects == 1 && computer_for_handle(2)->rejected);
    assert(computer_for_slot(0) == physical && hub_output_ready(0));
    assert(flash_writes == 1 && links[2].pending == NULL);
    assert(hub_output_keyboard(key_down));
    pump();
    assert(notifications[notification_count - 1].handle == 1);
    /* Re-encryption preserves per-link CCCDs, but must send new neutral reports. */
    links[1].key_size = 0;
    tick();
    assert(!hub_output_ready(0) && !physical->identity_authenticated);
    links[1].key_size = 16;
    tick();
    assert(!hub_output_ready(0)); /* Encryption alone isn't SM completion. */
    security_complete(1);
    assert(!hub_output_ready(0));
    pump();
    assert(hub_output_ready(0) && flash_writes == 1);
    unsigned ordinary = nonneutral_notifications();
    assert(hub_output_keyboard(key_down));
    uint8_t restart[11] = {SM_EVENT_REENCRYPTION_STARTED, 9};
    little_endian_store_16(restart, 2, 1);
    links[1].key_size = 0;
    security_event(HCI_EVENT_PACKET, 0, restart, sizeof(restart));
    pump();
    assert(!hub_output_ready(0) && nonneutral_notifications() == ordinary);
    links[1].key_size = 16;
    security_complete(1);
    pump();
    assert(hub_output_ready(0) && flash_writes == 1);
    links[1].index = 0; /* A link must not migrate to another identity mid-session. */
    security_event(HCI_EVENT_PACKET, 0, restart, sizeof(restart));
    security_complete(1);
    assert(physical->rejected && physical->slot == 0 && !hub_output_ready(0));
    assert(output_store.identities[0].type == 1 && flash_writes == 1);
}

static void redundant_reencryption_does_not_revoke_verified_link(void) {
    fresh();
    ready_peer(1, 0, false);
    computer_t *computer = computer_for_handle(1);
    unsigned writes = flash_writes;
    uint8_t start[11] = {SM_EVENT_REENCRYPTION_STARTED, 9};
    little_endian_store_16(start, 2, 1);
    for (unsigned i = 0; i < 3; ++i) {
        security_event(HCI_EVENT_PACKET, 0, start, sizeof(start));
        pump();
        assert(hub_output_ready(0) && computer->authentication_complete);
        assert(computer->identity_authenticated && flash_writes == writes);
    }
    assert(hub_output_keyboard(key_down));
    pump();
    assert(nonneutral_notifications() == 1);
    /* Fresh pairing is not a redundant re-encryption notification. */
    start[0] = SM_EVENT_PAIRING_STARTED;
    security_event(HCI_EVENT_PACKET, 0, start, sizeof(start));
    pump();
    assert(!hub_output_ready(0) && !computer->authentication_complete);
    assert(!hub_output_keyboard(key_down));
    security_complete(1);
    pump();
    assert(hub_output_ready(0));
    /* A later authentication failure must still revoke an encrypted link. */
    uint8_t failed[12] = {SM_EVENT_REENCRYPTION_COMPLETE, 10};
    little_endian_store_16(failed, 2, 1);
    failed[11] = ERROR_CODE_AUTHENTICATION_FAILURE;
    security_event(HCI_EVENT_PACKET, 0, failed, sizeof(failed));
    assert(!hub_output_ready(0) && computer->rejected && disconnects == 1);
}

static void authentication_guards(void) {
    fresh();
    connect_peer(1, HCI_ROLE_SLAVE, 0, false);
    links[1].key_size = 16;
    subscribe(1);
    pump();
    assert(flash_writes == 0 && notification_count == 0); /* No SM success yet. */
    bonds[0].key_size = 0;
    security_complete(1);
    pump();
    assert(flash_writes == 0); /* Encrypted but no persistent bond. */
    bonds[0].key_size = 16;
    links[1].key_size = 15;
    pump();
    assert(flash_writes == 0);
    links[1].key_size = 16;
    bonds[0].type = 1;
    memcpy(bonds[0].address, private_address, 6);
    security_complete(1);
    pump();
    assert(flash_writes == 0); /* Unresolved RPA in DB is never a stable identity. */
    clock_ms += OUTPUT_AUTH_TIMEOUT_MS;
    tick();
    assert(disconnects == 1);
    assert(!hub_output_ready(0) && !hub_output_keyboard(key_down));
}

static void selection_target_disappears(void) {
    fresh();
    ready_peer(1, 0, false);
    ready_peer(2, 1, false);
    ready_peer(3, 3, false);
    assert(!hub_output_select(4) && flash_writes == 3);
    assert(hub_output_keyboard(key_down)); /* Old notification still in SDK queue. */
    btstack_context_callback_registration_t *old_callback = links[1].pending;
    assert(hub_output_select(1) && flash_writes == 4);
    assert(links[1].pending == old_callback);
    assert(!hub_output_select(2) && flash_writes == 4);
    disconnect_peer(2);
    assert(active_slot == 0 && switch_pending);
    pump();
    assert(active_slot == 1 && output_store.selected == 1 && !switch_pending);
    assert(!hub_output_keyboard(key_down) && nonneutral_notifications() == 0);
    ready_peer(4, 4, false);
    assert(computer_for_handle(4)->slot == 3); /* New PC cannot seize offline PC2. */
    assert(active_slot == 1 && !hub_output_keyboard(key_down));
    assert(flash_writes == 5);
    boot();
    assert(active_slot == 1 && output_store.identities[1].reserved);
}

static void context_disconnect_lifetime(void) {
    fresh();
    ready_peer(1, 0, false);
    assert(hub_output_keyboard(key_down));
    computer_t *old = computer_for_handle(1);
    btstack_context_callback_registration_t *callback = links[1].pending;
    disconnect_peer(1);
    assert(old->retiring && !old->occupied);
    unsigned count = notification_count;
    callback->callback(callback->context); /* A handler still in the same HCI dispatch. */
    assert(notification_count == count);
    connect_peer(2, HCI_ROLE_SLAVE, 1, false);
    assert(computer_for_handle(2) != old);
    pump(); /* SDK has purged, then timer ends retirement. */
    connect_peer(1, HCI_ROLE_SLAVE, 0, true); /* HCI handle and physical context reused. */
    assert(computer_for_handle(1) == old && old->slot == 4 && !old->send_registered);
    links[1].key_size = 16;
    subscribe(1);
    security_complete(1);
    pump();
    assert(hub_output_ready(0) && nonneutral_notifications() == 0 && flash_writes == 1);
}

static void reservation_capacity(void) {
    fresh();
    for (unsigned i = 0; i < 4; ++i) ready_peer(i + 1, (int)i, false);
    assert(!advertising_requested && flash_writes == 4);
    connect_peer(5, HCI_ROLE_SLAVE, 4, false);
    assert(disconnects == 1 && computer_for_handle(5) == NULL);
    for (unsigned i = 1; i <= 4; ++i) disconnect_peer(i);
    pump();
    connect_peer(6, HCI_ROLE_SLAVE, 4, false);
    links[6].key_size = 16;
    security_complete(6);
    assert(computer_for_handle(6)->rejected && disconnects == 2);
    assert(flash_writes == 4 && active_slot == 0);
    ready_peer(7, 3, true);
    assert(computer_for_handle(7)->slot == 3 && flash_writes == 4);
}

static void assert_storage_locked(void) {
    boot();
    assert(output_store.fault);
    connect_peer(1, HCI_ROLE_SLAVE, 0, false);
    links[1].key_size = 16;
    subscribe(1);
    security_complete(1);
    unsigned attempts = flash_attempts;
    pump();
    assert(flash_attempts == attempts && computer_for_handle(1)->slot == 4);
    assert(!hub_output_ready(0) && !hub_output_select(0) && !hub_output_keyboard(key_down));
    assert(notification_count == 0);
}

static void reseal(void) {
    uint32_t crc = record_crc(flash_record, HUB_OUTPUT_STORE_BYTES - 4);
    little_endian_store_32(flash_record, HUB_OUTPUT_STORE_BYTES - 4, crc);
}

static void malformed_records(void) {
    fresh();
    ready_peer(1, 0, false);
    uint8_t saved[HUB_OUTPUT_STORE_BYTES];
    memcpy(saved, flash_record, sizeof(saved));
    flash_record[12] ^= 1; /* CRC mismatch. */
    assert_storage_locked();
    memcpy(flash_record, saved, sizeof(saved));
    flash_record[4] = 2; reseal(); /* Future version. */
    assert_storage_locked();
    memcpy(flash_record, saved, sizeof(saved));
    flash_record[5] = 4; reseal();
    assert_storage_locked();
    memcpy(flash_record, saved, sizeof(saved));
    flash_record[5] = 1; reseal(); /* Selected unreserved slot. */
    assert_storage_locked();
    memcpy(flash_record, saved, sizeof(saved));
    flash_record[8] = 2; reseal(); /* Invalid reserved flag. */
    assert_storage_locked();
    memcpy(flash_record, saved, sizeof(saved));
    flash_record[9] = 2; reseal(); /* Resolved address type must be 0 or 1. */
    assert_storage_locked();
    memcpy(flash_record, saved, sizeof(saved));
    memcpy(flash_record + 16, flash_record + 8, 8); reseal(); /* Duplicate identities. */
    assert_storage_locked();
    memcpy(flash_record, saved, sizeof(saved));
    --flash_length;
    assert_storage_locked();
    flash_length += 2; /* Oversized records aren't accepted by truncation. */
    assert_storage_locked();
    flash_length = HUB_OUTPUT_STORE_BYTES;
    fail_read = true;
    assert_storage_locked();
    fail_read = false;
    no_tlv = true;
    assert_storage_locked();
}

static void persistence_failures(void) {
    for (unsigned readback = 0; readback < 2; ++readback) {
        fresh();
        fail_write = readback == 0;
        fail_readback = readback != 0;
        connect_peer(1, HCI_ROLE_SLAVE, 0, false);
        links[1].key_size = 16;
        subscribe(1);
        security_complete(1);
        assert(output_store.fault && !output_store.identities[0].reserved);
        assert(computer_for_handle(1)->rejected && flash_attempts == 1);
        pump();
        assert(flash_attempts == 1 && notification_count == 0);
        assert(!hub_output_keyboard(key_down));
        fresh();
        ready_peer(1, 0, false);
        ready_peer(2, 1, false);
        assert(hub_output_keyboard(key_down)); /* Pending input must be discarded. */
        fail_write = readback == 0;
        fail_readback = readback != 0;
        assert(!hub_output_select(1) && !switch_pending && active_slot == 0);
        assert(output_store.fault && output_store.selected == 0 && flash_attempts == 3);
        pump();
        assert(flash_attempts == 3 && nonneutral_notifications() == 0);
        assert(!hub_output_ready(0) && !hub_output_ready(1));
    }
}

static void readiness_and_release_errors(void) {
    fresh();
    synchronous_send = true;
    ready_peer(1, 0, false);
    ready_peer(2, 1, false);
    uint8_t suspend = 0;
    assert(write_attribute(1, ATT_CHARACTERISTIC_2A4C_01_VALUE_HANDLE, 0, 0, &suspend, 1) == 0);
    pump();
    assert(!hub_output_ready(0) && hub_output_ready(1));
    assert(!hub_output_keyboard(key_down));
    suspend = 1;
    assert(write_attribute(1, ATT_CHARACTERISTIC_2A4C_01_VALUE_HANDLE, 0, 0, &suspend, 1) == 0);
    pump();
    assert(hub_output_ready(0));
    synchronous_send = false;
    notify_error = 0x0c;
    assert(hub_output_select(1));
    pump();
    assert(switch_pending && active_slot == 0 && output_store.selected == 1);
    assert(!hub_output_keyboard(key_down));
    unsigned attempts = flash_attempts;
    pump();
    assert(flash_attempts == attempts); /* Switch progress never writes flash. */
    notify_error = 0;
    pump();
    assert(active_slot == 1 && !switch_pending);
    assert(hub_output_keyboard(key_down));
    pump();
    assert(notifications[notification_count - 1].handle == 2);
}

static void fast_async_fifo(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_set_low_latency(true);
    const uint8_t key_up[8] = {0};
    unsigned first = notification_count;
    uint32_t before = clock_ms;
    for (unsigned i = 0; i < 6; ++i)
        assert(hub_output_keyboard(i & 1 ? key_up : key_down));
    for (unsigned i = 0; i < 6; ++i) {
        assert(links[1].pending);
        deliver(1);
        run_main_callbacks();
        assert(notifications[first + i].attribute == KEYBOARD_HANDLE);
        assert(notifications[first + i].data[2] == (i & 1 ? 0 : 4));
    }
    assert(clock_ms == before && notification_count == first + 6);
    assert(!links[1].pending && !computer_for_slot(0)->count);
    assert(max_callback_depth == 1 && !yield_timer_pending);
}

static void fast_synchronous_budget(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_set_low_latency(true);
    unsigned first = notification_count;
    for (unsigned i = 0; i < OUTPUT_QUEUE_LENGTH; ++i) {
        uint8_t key[8] = {0, 0, (uint8_t)(i + 1)};
        assert(hub_output_keyboard(key));
    }
    deliver(1); /* Controller frees a slot, then permits synchronous sends. */
    assert(main_callback_count == 1);
    synchronous_send = true;
    assert(run_main_callbacks() == 1);
    assert(notification_count == first + 1 + OUTPUT_PUMP_BUDGET);
    assert(yield_timer_pending && computer_for_slot(0)->yield_pending);
    assert(output_metrics[0].reports[0].queue_highwater == OUTPUT_QUEUE_LENGTH);
    unsigned attempts = notify_attempts;
    for (unsigned i = 0; i < 30; ++i) request_send(computer_for_slot(0));
    assert(notify_attempts == attempts); /* No bypassing the cooperative yield. */
    unsigned yields = 0;
    while (computer_for_slot(0)->count) {
        unsigned before = notify_attempts;
        assert(++yields <= OUTPUT_QUEUE_LENGTH / OUTPUT_PUMP_BUDGET);
        yield_tick();
        assert(notify_attempts - before <= OUTPUT_PUMP_BUDGET);
        assert(run_main_callbacks() == 0);
    }
    assert(max_callback_depth == 1 && !yield_timer_pending);
    assert(notification_count == first + OUTPUT_QUEUE_LENGTH);
    for (unsigned i = 0; i < OUTPUT_QUEUE_LENGTH; ++i)
        assert(notifications[first + i].data[2] == i + 1);
    assert(output_metrics[0].budget_yields == yields);
}

static void fast_mouse_coalescing(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_set_low_latency(true);
    unsigned first = notification_count;
    assert(hub_output_mouse(1, 30000, -30000, 120));
    assert(hub_output_mouse(1, 10000, -10000, 80));
    assert(hub_output_mouse(0, 5, 6, 0));
    assert(hub_output_mouse(1, -7, 8, 0));
    assert(computer_for_slot(0)->count == 3);
    deliver(1);
    synchronous_send = true;
    run_main_callbacks();
    assert(notification_count == first + 4 && !computer_for_slot(0)->count);
    assert(notifications[first].data[0] == 1 && notifications[first + 1].data[0] == 1);
    assert(notifications[first + 2].data[0] == 0 && notifications[first + 3].data[0] == 1);
    int32_t x = (int16_t)little_endian_read_16(notifications[first].data, 1)
        + (int16_t)little_endian_read_16(notifications[first + 1].data, 1);
    int32_t y = (int16_t)little_endian_read_16(notifications[first].data, 3)
        + (int16_t)little_endian_read_16(notifications[first + 1].data, 3);
    int32_t wheel = (int8_t)notifications[first].data[5]
        + (int8_t)notifications[first + 1].data[5];
    assert(x == 40000 && y == -40000 && wheel == 200);
    assert(output_metrics[0].mouse_merges == 1);
    assert(output_metrics[0].reports[1].enqueued == 4);
    assert(output_metrics[0].reports[1].notified == 4);
    assert(output_metrics[0].reports[1].wait_count == 4);
    assert(output_metrics[0].reports[1].queue_highwater == 3);
    assert(max_callback_depth == 1);
}

static void fast_independent_release_callbacks(void) {
    fresh();
    for (unsigned i = 0; i < 4; ++i) ready_peer(i + 1, (int)i, false);
    hub_output_set_low_latency(true);
    hub_output_latency_reset();
    hub_output_release_all();
    unsigned first = notification_count;
    for (unsigned handle = 1; handle <= 4; ++handle) {
        assert(links[handle].pending == &computer_for_handle((hci_con_handle_t)handle)->send_registration);
        deliver(handle);
    }
    assert(main_callback_count == 1);
    run_main_callbacks();
    for (unsigned handle = 1; handle <= 4; ++handle) {
        assert(links[handle].pending == &computer_for_handle((hci_con_handle_t)handle)->send_registration);
        deliver(handle);
        assert(hub_output_ready(handle - 1));
        assert(output_metrics[handle - 1].releases == 2);
        assert(output_metrics[handle - 1].reports[0].wait_count == 0);
        assert(output_metrics[handle - 1].reports[1].wait_count == 0);
    }
    assert(notification_count == first + 8 && !run_main_callbacks());
    assert(!yield_timer_pending && !disconnects && max_callback_depth == 1);
    assert(active_slot == 0 && !nonneutral_notifications());
}

static void fast_errors_wait_for_service(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_latency_reset();
    hub_output_set_low_latency(true);
    synchronous_send = true;
    unsigned attempts = notify_attempts;
    notify_error = 0x0c;
    assert(hub_output_keyboard(key_down));
    assert(hub_output_keyboard(key_down));
    assert(notify_attempts == attempts + 1 && !yield_timer_pending);
    assert(!run_main_callbacks());
    assert(output_metrics[0].reports[0].notified == 0);
    tick();
    assert(notify_attempts == attempts + 2);
    assert(!run_main_callbacks() && !yield_timer_pending);
    notify_error = 0;
    tick();
    assert(notify_attempts == attempts + 4 && !computer_for_slot(0)->count);
    assert(output_metrics[0].reports[0].notify_errors == 2);
    assert(output_metrics[0].reports[0].wait_count == 2);
    registration_error = 0x0c;
    attempts = registration_attempts;
    assert(hub_output_keyboard(key_down));
    assert(hub_output_keyboard(key_down));
    assert(registration_attempts == attempts + 1 && !run_main_callbacks());
    registration_error = 0;
    tick();
    assert(registration_attempts == attempts + 3 && !computer_for_slot(0)->count);
    assert(output_metrics[0].registration_errors == 1 && max_callback_depth == 1);
}

static void pump_mode_toggle(void) {
    fresh();
    assert(!low_latency && hub_output_active_handle() == HCI_CON_HANDLE_INVALID);
    ready_peer(1, 0, false);
    assert(hub_output_active_handle() == 1);
    assert(hub_output_keyboard(key_down));
    assert(hub_output_keyboard(key_down));
    deliver(1);
    assert(!links[1].pending && !main_callback_count && computer_for_slot(0)->count == 1);
    hub_output_set_low_latency(true);
    assert(main_callback_count == 1);
    hub_output_set_low_latency(false);
    unsigned attempts = registration_attempts;
    run_main_callbacks();
    assert(registration_attempts == attempts && computer_for_slot(0)->count == 1);
    tick();
    deliver(1);
    assert(!computer_for_slot(0)->count);
    assert(hub_output_mouse(0, 0, 0, 127 * 20));
    hub_output_set_low_latency(true);
    deliver(1);
    synchronous_send = true;
    run_main_callbacks();
    assert(yield_timer_pending && computer_for_slot(0)->count == 1);
    hub_output_set_low_latency(false);
    attempts = notify_attempts;
    yield_tick();
    assert(notify_attempts == attempts && computer_for_slot(0)->count == 1);
    tick();
    assert(notify_attempts == attempts + 1); /* Back to one report per 5 ms tick. */
    hub_output_set_low_latency(true);
    run_main_callbacks();
    while (computer_for_slot(0)->count) yield_tick();
    assert(!computer_for_slot(0)->count && !computer_for_slot(0)->last_mouse[0]);
    assert(output_metrics[0].reports[1].notified == 20);
    uint8_t suspended = 0;
    assert(write_attribute(1, ATT_CHARACTERISTIC_2A4C_01_VALUE_HANDLE, 0, 0, &suspended, 1) == 0);
    assert(hub_output_active_handle() == HCI_CON_HANDLE_INVALID);
}

static void fast_switch_and_overflow_safety(void) {
    fresh();
    ready_peer(1, 0, false);
    ready_peer(2, 1, false);
    hub_output_set_low_latency(true);
    unsigned first = notification_count;
    assert(hub_output_keyboard(key_down));
    assert(hub_output_select(1));
    assert(hub_output_active_handle() == HCI_CON_HANDLE_INVALID);
    deliver(1);
    assert(switch_pending && active_slot == 0 && !hub_output_keyboard(key_down));
    run_main_callbacks();
    assert(links[1].pending);
    deliver(1);
    run_main_callbacks();
    assert(!switch_pending && active_slot == 1 && hub_output_active_handle() == 2);
    assert(hub_output_keyboard(key_down));
    deliver(2);
    assert(notifications[first].handle == 1 && notifications[first].attribute == KEYBOARD_HANDLE);
    assert(notifications[first + 1].handle == 1 && notifications[first + 1].attribute == MOUSE_HANDLE);
    for (unsigned i = first; i < first + 2; ++i)
        for (unsigned b = 0; b < notifications[i].length; ++b) assert(!notifications[i].data[b]);
    assert(notifications[first + 2].handle == 2 && notifications[first + 2].data[2] == 4);
    assert(!disconnects && hub_output_ready(0) && hub_output_ready(1));
    for (unsigned i = 0; i < OUTPUT_QUEUE_LENGTH; ++i) assert(hub_output_keyboard(key_down));
    unsigned ordinary = nonneutral_notifications();
    assert(!hub_output_keyboard(key_down));
    assert(!computer_for_slot(1)->count && !hub_output_ready(1));
    synchronous_send = true;
    for (unsigned h = 1; h <= 2; ++h) deliver(h);
    run_main_callbacks();
    assert(hub_output_ready(0) && hub_output_ready(1));
    assert(nonneutral_notifications() == ordinary);
    assert(computer_for_slot(1)->queued[0] == 0 && computer_for_slot(1)->queued[1] == 0);
}

static void deferred_disconnect_reuse(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_set_low_latency(true);
    assert(hub_output_keyboard(key_down));
    assert(hub_output_keyboard(key_down));
    deliver(1);
    computer_t *old = computer_for_handle(1);
    assert(old->continuation_pending && main_callback_count == 1);
    disconnect_peer(1);
    assert(!old->continuation_pending);
    connect_peer(2, HCI_ROLE_SLAVE, 1, false);
    assert(computer_for_handle(2) != old);
    tick();
    synchronous_send = true;
    ready_peer(1, 0, true);
    assert(computer_for_handle(1) == old && hub_output_ready(0));
    unsigned ordinary = nonneutral_notifications();
    unsigned before = notification_count;
    run_main_callbacks(); /* Old callback scans flags, never owns a recycled context. */
    assert(notification_count == before && nonneutral_notifications() == ordinary);
    assert(output_metrics[0].reports[0].enqueued == 2);
    assert(output_metrics[0].reports[0].notified == 1); /* Survives reconnect by logical PC. */
    assert(hub_output_mouse(1, 0, 0, 127 * 20));
    assert(old->yield_pending && yield_timer_pending);
    disconnect_peer(1);
    assert(!old->yield_pending);
    tick();
    ready_peer(1, 0, false);
    before = notification_count;
    yield_tick(); /* Old cooldown timer cannot replay old input into the new link. */
    assert(notification_count == before && !old->count && !old->last_mouse[0]);
}

static void precise_queue_metrics(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_latency_reset();
    hub_output_set_low_latency(true);
    const uint8_t key_up[8] = {0};
    clock_us = 100;
    assert(hub_output_keyboard(key_down));
    clock_us = 400;
    assert(hub_output_mouse(1, 1, 0, 0));
    clock_us = 700;
    assert(hub_output_mouse(1, 2, 0, 0));
    clock_us = 1000;
    assert(hub_output_keyboard(key_up));
    clock_us = 1200;
    assert(hub_output_mouse(0, -2, 0, 0));
    assert(output_metrics[0].queue_highwater == 4);
    assert(output_metrics[0].reports[0].queue_highwater == 2);
    assert(output_metrics[0].reports[1].queue_highwater == 2);
    const uint32_t send_times[] = {2000, 6500, 12000, 25000};
    for (unsigned i = 0; i < 4; ++i) {
        clock_us = send_times[i];
        deliver(1);
        run_main_callbacks();
    }
    output_report_metrics_t *keyboard = &output_metrics[0].reports[0];
    output_report_metrics_t *mouse = &output_metrics[0].reports[1];
    assert(keyboard->enqueued == 2 && keyboard->notified == 2 && keyboard->wait_count == 2);
    assert(keyboard->wait_sum_us == 12900 && keyboard->wait_max_us == 11000);
    assert(keyboard->wait_bins[1] == 1 && keyboard->wait_bins[3] == 1);
    assert(mouse->enqueued == 3 && mouse->notified == 2 && mouse->wait_count == 2);
    assert(mouse->wait_sum_us == 29900 && mouse->wait_max_us == 23800);
    assert(mouse->wait_bins[2] == 1 && mouse->wait_bins[4] == 1);
    assert(output_metrics[0].mouse_merges == 1 && output_metrics[0].releases == 0);
    assert(!computer_for_slot(0)->count && !computer_for_slot(0)->queued[0]
        && !computer_for_slot(0)->queued[1]);
    hub_output_latency_status();
}

static void timing_rollover_and_bins(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_latency_reset();
    clock_us = UINT32_MAX - 10;
    assert(hub_output_keyboard(key_down));
    clock_us = 9;
    deliver(1);
    output_report_metrics_t *metrics = &output_metrics[0].reports[0];
    assert(metrics->wait_sum_us == 20 && metrics->wait_max_us == 20);
    const uint32_t waits[] = {999, 1000, 4999, 5000, 9999, 10000, 19999, 20000};
    for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
        clock_us = 100;
        assert(hub_output_keyboard(key_down));
        clock_us += waits[i];
        deliver(1);
    }
    assert(metrics->wait_bins[0] == 2 && metrics->wait_bins[1] == 2);
    assert(metrics->wait_bins[2] == 2 && metrics->wait_bins[3] == 2 && metrics->wait_bins[4] == 1);
    hub_output_latency_reset();
    for (unsigned i = 0; i < 2; ++i) {
        clock_us = 0;
        assert(hub_output_keyboard(key_down));
        clock_us = UINT32_MAX - 1;
        deliver(1);
    }
    assert(metrics->wait_sum_us == UINT64_C(8589934588));
    assert(metrics->wait_count == 2 && metrics->wait_max_us == UINT32_MAX - 1);
    hub_output_latency_status();
}

static void split_mouse_oldest_timestamp(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_latency_reset();
    hub_output_set_low_latency(true);
    clock_us = 100;
    assert(hub_output_mouse(1, 0, 0, 300));
    clock_us = 600;
    deliver(1);
    run_main_callbacks();
    clock_us = 1100;
    assert(hub_output_mouse(1, 0, 0, 20)); /* After a partial send, still oldest=100. */
    clock_us = 1600;
    deliver(1);
    run_main_callbacks();
    clock_us = 5600;
    deliver(1);
    run_main_callbacks();
    const output_report_metrics_t *metrics = &output_metrics[0].reports[1];
    assert(metrics->enqueued == 2 && metrics->notified == 3 && metrics->wait_count == 3);
    assert(metrics->wait_sum_us == 7500 && metrics->wait_max_us == 5500);
    assert(metrics->wait_bins[0] == 1 && metrics->wait_bins[1] == 1 && metrics->wait_bins[2] == 1);
    assert(output_metrics[0].mouse_merges == 1 && !computer_for_slot(0)->count);
}

static void reset_preserves_pending_state(void) {
    fresh();
    ready_peer(1, 0, false);
    hub_output_set_low_latency(true);
    assert(hub_output_mouse(1, 0, 0, 300));
    clock_us += 100;
    deliver(1); /* A partially drained mouse event keeps its original timestamp. */
    assert(output_metrics[0].reports[1].wait_count == 1);
    computer_t *computer = computer_for_slot(0);
    uint32_t sent = computer->sent;
    unsigned attempts = notify_attempts;
    unsigned writes = flash_writes;
    unsigned callbacks = main_callback_count;
    hub_output_latency_reset();
    assert(computer->count == 1 && computer->last_mouse[0] == 1 && computer->sent == sent);
    assert(computer->identity_authenticated && hub_output_ready(0));
    assert(hub_output_active_handle() == 1 && flash_writes == writes);
    assert(main_callback_count == callbacks && notify_attempts == attempts);
    assert(output_metrics[0].queue_highwater == 1 && output_metrics[0].reports[1].queue_highwater == 1);
    assert(hub_output_mouse(1, 0, 0, 100)); /* Merged into pre-reset event: no timing. */
    assert(hub_output_keyboard(key_down));
    synchronous_send = true;
    deliver(1);
    run_main_callbacks();
    assert(!computer->count && computer->last_keyboard[2] == 4);
    output_report_metrics_t *mouse = &output_metrics[0].reports[1];
    assert(mouse->enqueued == 1 && mouse->notified == 3 && mouse->wait_count == 0);
    assert(!mouse->wait_sum_us && !mouse->wait_max_us && output_metrics[0].mouse_merges == 1);
    assert(output_metrics[0].reports[0].wait_count == 1);
    hub_output_release_all();
    assert(hub_output_ready(0) && !computer->last_mouse[0] && !computer->last_keyboard[2]);
    assert(output_metrics[0].releases == 2 && mouse->wait_count == 0);
    assert(output_metrics[0].reports[0].wait_count == 1);
}

int main(void) {
    migration_and_restore();
    provisional_and_duplicates();
    authentication_guards();
    redundant_reencryption_does_not_revoke_verified_link();
    selection_target_disappears();
    context_disconnect_lifetime();
    reservation_capacity();
    malformed_records();
    persistence_failures();
    readiness_and_release_errors();
    fast_async_fifo();
    fast_synchronous_budget();
    fast_mouse_coalescing();
    fast_independent_release_callbacks();
    fast_errors_wait_for_service();
    pump_mode_toggle();
    fast_switch_and_overflow_safety();
    deferred_disconnect_reuse();
    precise_queue_metrics();
    timing_rollover_and_bins();
    split_mouse_oldest_timestamp();
    reset_preserves_pending_state();
    puts("Output persistence/state-machine/latency tests passed (22 scenario groups)");
    return 0;
}
