/*
 * SDK-specific harness for generated excerpts of the actual patched hci.c.
 * Not a controller simulator: scheduling, allocation and event sinks are doubles.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTSTACK_FILE__ "hci_reconnect_test.c"
#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "hci.h"

static hci_stack_t stack;
static hci_stack_t *hci_stack = &stack;
static hci_connection_t pool[8];
static bool allocated[8];
static unsigned allocations, frees, events, connection_events, run_calls, diagnostics;
static uint8_t last_event[36];
static const bd_addr_t mouse = {0xf9, 0x37, 0xf0, 0x2a, 0x90, 0x26};
static const bd_addr_t pc = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const bd_addr_t zero_address = {0};

void btstack_assert_failed(const char *file, uint16_t line){
    fprintf(stderr, "BTstack assertion: %s:%u\n", file, line);
    abort();
}

void hci_dump_log(int level, const char *format, ...){
    (void)level;
    (void)format;
    ++diagnostics;
}

static hci_connection_t *create_connection_for_bd_addr_and_type(
        const bd_addr_t address, bd_addr_type_t type, hci_role_t role){
    unsigned i;
    for (i = 0; i < sizeof(pool) / sizeof(pool[0]); ++i){
        if (allocated[i]) continue;
        allocated[i] = true;
        memset(&pool[i], 0, sizeof(pool[i]));
        memcpy(pool[i].address, address, sizeof(bd_addr_t));
        pool[i].address_type = type;
        pool[i].role = role;
        pool[i].con_handle = HCI_CON_HANDLE_INVALID;
        btstack_linked_list_add(&hci_stack->connections, (btstack_linked_item_t *)&pool[i]);
        ++allocations;
        return &pool[i];
    }
    return NULL;
}

void btstack_memory_hci_connection_free(hci_connection_t *conn){
    unsigned i = (unsigned)(conn - pool);
    assert(i < sizeof(pool) / sizeof(pool[0]));
    assert(allocated[i]);
    allocated[i] = false;
    ++frees;
}

void hci_emit_btstack_event(uint8_t *event, uint16_t size, int dump){
    (void)dump;
    assert(size == sizeof(last_event));
    memcpy(last_event, event, size);
    ++events;
}

static void hci_emit_nr_connections_changed(void){ ++connection_events; }
static void hci_run(void){ ++run_calls; }
static void hci_update_advertisements_enabled_for_current_roles(void){}
static hci_connection_t *gap_get_outgoing_le_connection(void);

static void hci_emit_le_connection_complete(bd_addr_type_t type, const bd_addr_t address,
                                           hci_con_handle_t handle, uint8_t status){
    memset(last_event, 0, sizeof(last_event));
    last_event[3] = status;
    last_event[7] = type;
    little_endian_store_16(last_event, 4, handle);
    reverse_bd_addr(address, &last_event[8]);
    ++events;
}

#include "hci_reconnect_under_test.inc"

static void reset(void){
    memset(&stack, 0, sizeof(stack));
    memset(pool, 0, sizeof(pool));
    memset(allocated, 0, sizeof(allocated));
    memset(last_event, 0, sizeof(last_event));
    allocations = frees = events = connection_events = run_calls = diagnostics = 0;
}

static unsigned connection_count(void){
    unsigned count = 0;
    btstack_linked_item_t *it;
    for (it = stack.connections; it != NULL; it = it->next) ++count;
    return count;
}

static hci_connection_t *start_direct(bd_addr_type_t type){
    hci_connection_t *conn;
    assert(gap_connect(mouse, type) == ERROR_CODE_SUCCESS);
    conn = gap_get_outgoing_le_connection();
    assert(conn != NULL);
    assert(conn->state == SEND_CREATE_CONNECTION);
    assert(stack.le_connecting_request == LE_CONNECTING_DIRECT);
    // Model only the controller command having been sent, not HCI scheduling.
    conn->state = SENT_CREATE_CONNECTION;
    stack.le_connecting_state = LE_CONNECTING_DIRECT;
    return conn;
}

static hci_connection_t *add_open(const bd_addr_t address, bd_addr_type_t type, hci_role_t role){
    hci_connection_t *conn = create_connection_for_bd_addr_and_type(address, type, role);
    assert(conn != NULL);
    conn->state = OPEN;
    conn->con_handle = (hci_con_handle_t)(0x80 + allocations);
    return conn;
}

static void complete(uint8_t subevent, uint8_t status, hci_role_t role,
                     bd_addr_type_t type, const bd_addr_t address){
    uint8_t packet[36] = {0};
    packet[0] = HCI_EVENT_LE_META;
    packet[1] = subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE ? 19 :
        (subevent == HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1 ? 31 : 34);
    packet[2] = subevent;
    packet[3] = status;
    little_endian_store_16(packet, 4, 0x40);
    packet[6] = role;
    packet[7] = type;
    reverse_bd_addr(address, &packet[8]);
    little_endian_store_16(packet,
        subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE ? 14 : 26, 24);
    hci_handle_le_connection_complete_event(packet);
}

static void test_identity_success(uint8_t subevent, bd_addr_type_t identity_type){
    unsigned cycle;
    hci_connection_t *keyboard, *peripheral;
    reset();
    keyboard = add_open(pc, BD_ADDR_TYPE_ACL, HCI_ROLE_MASTER);
    peripheral = add_open(pc, BD_ADDR_TYPE_LE_PUBLIC, HCI_ROLE_SLAVE);
    for (cycle = 0; cycle < 16; ++cycle){
        hci_connection_t *pending = start_direct(identity_type);
        unsigned before = allocations;
        complete(subevent, 0, HCI_ROLE_MASTER, identity_type & 1, mouse);
        assert(pending->state == OPEN && pending->con_handle == 0x40);
        assert(pending->address_type == identity_type);
        assert(pending->le_connection_interval == 24);
        assert(allocations == before && frees == cycle);
        assert(connection_count() == 3);
        assert(stack.le_connecting_request == LE_CONNECTING_IDLE);
        assert(stack.le_connecting_state == LE_CONNECTING_IDLE);
        assert(keyboard->state == OPEN && keyboard->con_handle == 0x81);
        assert(peripheral->state == OPEN && peripheral->con_handle == 0x82);
        assert(last_event[7] == (identity_type & 1));
        assert(events == cycle + 1 && diagnostics == cycle + 1);
        // Simulate normal handle-based disconnection cleanup.
        btstack_linked_list_remove(&stack.connections, (btstack_linked_item_t *)pending);
        btstack_memory_hci_connection_free(pending);
    }
}

static void test_identity_match_boundaries(void){
    unsigned scenario;
    for (scenario = 0; scenario < 7; ++scenario){
        hci_connection_t *pending;
        hci_role_t event_role = HCI_ROLE_MASTER;
        bd_addr_type_t event_type = BD_ADDR_TYPE_LE_RANDOM;
        const uint8_t *event_address = mouse;
        CONNECTION_STATE original_state;
        reset();
        pending = start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
        switch (scenario){
            case 0: event_address = pc; break;
            case 1: event_role = HCI_ROLE_SLAVE; break;
            case 2: pending->role = HCI_ROLE_SLAVE; break;
            case 3: pending->state = OPEN; pending->con_handle = 0x81; break;
            case 4: pending->state = RECEIVED_DISCONNECTION_COMPLETE; break;
            case 5: pending->state = SEND_CREATE_CONNECTION; break;
            case 6: event_type = BD_ADDR_TYPE_LE_PUBLIC; break;
        }
        original_state = pending->state;
        complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0, event_role,
                 event_type, event_address);
        assert(pending->state == original_state);
        assert(pending->con_handle == (scenario == 3 ? 0x81 : HCI_CON_HANDLE_INVALID));
        assert(allocations == 2 && frees == 0 && connection_count() == 2);
        assert(diagnostics == 0);
        if (event_role == HCI_ROLE_SLAVE){
            assert(stack.le_connecting_state == LE_CONNECTING_DIRECT);
            assert(stack.le_connecting_request == LE_CONNECTING_DIRECT);
        }
    }
}

static void test_terminal_failures(void){
    const uint8_t statuses[] = {0x3e, 0x0c, 0x09, 0x0d};
    unsigned i, address_case;
    for (i = 0; i < sizeof(statuses); ++i){
        for (address_case = 0; address_case < 3; ++address_case){
            hci_connection_t *active, *pending;
            const uint8_t *error_address = address_case == 0 ? zero_address :
                (address_case == 1 ? pc : mouse);
            reset();
            active = add_open(pc, BD_ADDR_TYPE_LE_RANDOM, HCI_ROLE_SLAVE);
            pending = start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
            complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, statuses[i],
                     HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_RANDOM, error_address);
            assert(!allocated[pending - pool]);
            assert(frees == 1 && connection_count() == 1);
            assert(active->state == OPEN && active->con_handle == 0x81);
            assert(stack.le_connecting_state == LE_CONNECTING_IDLE);
            assert(stack.le_connecting_request == LE_CONNECTING_IDLE);
            assert(events == 1 && last_event[3] == statuses[i]);
            assert(diagnostics == 1);
            assert(gap_connect(mouse, BD_ADDR_TYPE_LE_RANDOM_IDENTITY) == 0);
        }
    }
}

static void test_cancellation(void){
    hci_connection_t *pending;
    reset();
    pending = start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    stack.le_connecting_state = LE_CONNECTING_CANCEL;
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0x02,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_PUBLIC, zero_address);
    assert(pending->state == SEND_CREATE_CONNECTION && frees == 0 && events == 0);
    assert(stack.le_connecting_request == LE_CONNECTING_DIRECT);
    assert(stack.le_connecting_state == LE_CONNECTING_IDLE);
    pending->state = SENT_CREATE_CONNECTION;
    stack.le_connecting_state = LE_CONNECTING_DIRECT;
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_RANDOM, mouse);
    assert(pending->state == OPEN && allocations == 1 && events == 1);

    reset();
    start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    assert(gap_connect_cancel() == 0 && stack.le_connecting_request == LE_CONNECTING_IDLE);
    stack.le_connecting_state = LE_CONNECTING_CANCEL;
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0x02,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_PUBLIC, zero_address);
    assert(frees == 1 && events == 1 && connection_count() == 0 && last_event[3] == 0x02);
    assert(stack.le_connecting_state == LE_CONNECTING_IDLE);
    assert(gap_connect(mouse, BD_ADDR_TYPE_LE_RANDOM_IDENTITY) == 0);

    reset();
    assert(gap_connect(mouse, BD_ADDR_TYPE_LE_RANDOM_IDENTITY) == 0);
    assert(gap_connect_cancel() == 0);
    assert(frees == 1 && events == 1 && connection_count() == 0);
    assert(stack.le_connecting_request == LE_CONNECTING_IDLE && last_event[3] == 0x02);

    reset();
    pending = start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    assert(gap_connect_cancel() == 0);
    stack.le_connecting_state = LE_CONNECTING_CANCEL;
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_RANDOM, mouse);
    assert(pending->state == OPEN && allocations == 1 && frees == 0 && events == 1);

    reset();
    start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    assert(gap_connect_cancel() == 0);
    stack.le_connecting_state = LE_CONNECTING_CANCEL;
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0x3e,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_PUBLIC, zero_address);
    assert(connection_count() == 0 && frees == 1 && events == 1 && last_event[3] == 0x3e);

    reset();
    stack.le_connecting_request = LE_CONNECTING_DIRECT;
    stack.le_connecting_state = LE_CONNECTING_CANCEL;
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0x02,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_PUBLIC, zero_address);
    assert(frees == 0 && events == 0 && stack.le_connecting_request == LE_CONNECTING_DIRECT);

    reset();
    stack.le_connecting_request = LE_CONNECTING_WHITELIST;
    stack.le_connecting_state = LE_CONNECTING_CANCEL;
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0x02,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_PUBLIC, zero_address);
    assert(events == 0 && stack.le_connecting_request == LE_CONNECTING_WHITELIST);
    assert(stack.le_connecting_state == LE_CONNECTING_IDLE);
    assert(gap_connect_cancel() == 0);
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0x02,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_PUBLIC, zero_address);
    assert(events == 1 && last_event[3] == 0x02);
}

static void test_failure_isolation(void){
    unsigned scenario;
    for (scenario = 0; scenario < 4; ++scenario){
        hci_connection_t *pending, *active;
        reset();
        active = add_open(pc, BD_ADDR_TYPE_LE_RANDOM, HCI_ROLE_SLAVE);
        pending = start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
        stack.le_advertisements_state = LE_ADVERTISEMENT_STATE_ACTIVE;
        complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1,
                 scenario & 1 ? ERROR_CODE_DIRECTED_ADVERTISING_TIMEOUT : 0x3e,
                 HCI_ROLE_SLAVE, scenario & 2 ? BD_ADDR_TYPE_LE_RANDOM_IDENTITY : BD_ADDR_TYPE_LE_RANDOM,
                 scenario & 2 ? mouse : pc);
        assert(pending->state == SENT_CREATE_CONNECTION && frees == 0);
        assert(active->state == OPEN && active->con_handle == 0x81);
        assert(stack.le_connecting_state == LE_CONNECTING_DIRECT);
        assert(stack.le_connecting_request == LE_CONNECTING_DIRECT);
        assert(events == 1 && connection_count() == 2);
        if (scenario & 1) assert(!(stack.le_advertisements_state & LE_ADVERTISEMENT_STATE_ACTIVE));
    }

    for (scenario = 0; scenario < 3; ++scenario){
        hci_connection_t *active;
        reset();
        active = add_open(mouse, BD_ADDR_TYPE_LE_RANDOM, HCI_ROLE_MASTER);
        stack.le_connecting_state = LE_CONNECTING_DIRECT;
        stack.le_connecting_request = scenario == 0 ? LE_CONNECTING_IDLE : LE_CONNECTING_DIRECT;
        complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1,
                 scenario == 2 ? 0x02 : 0x3e, HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_RANDOM, mouse);
        assert(frees == 0 && connection_count() == 1);
        assert(active->state == OPEN && active->con_handle == 0x81);
        if (scenario != 2) assert(stack.le_connecting_request == LE_CONNECTING_IDLE);
    }
}

static void test_existing_paths(void){
    hci_connection_t *pending;
    reset();
    pending = start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    complete(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, 0,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_RANDOM_IDENTITY, mouse);
    assert(pending->state == OPEN && allocations == 1 && diagnostics == 0);

    reset();
    pending = start_direct(BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    complete(HCI_SUBEVENT_LE_CONNECTION_COMPLETE, 0,
             HCI_ROLE_MASTER, BD_ADDR_TYPE_LE_RANDOM, pc);
    assert(pending->state == OPEN && allocations == 1 && diagnostics == 0);
    assert(last_event[7] == BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    {
        bd_addr_t address;
        gap_subevent_le_connection_complete_get_peer_address(last_event, address);
        assert(memcmp(address, mouse, sizeof(address)) == 0);
        gap_subevent_le_connection_complete_get_peer_resolvable_private_address(last_event, address);
        assert(memcmp(address, pc, sizeof(address)) == 0);
    }
}

int main(void){
    test_identity_success(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    test_identity_success(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1, BD_ADDR_TYPE_LE_PUBLIC_IDENTITY);
    test_identity_success(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V2, BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    test_identity_success(HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V2, BD_ADDR_TYPE_LE_PUBLIC_IDENTITY);
    test_identity_match_boundaries();
    test_terminal_failures();
    test_cancellation();
    test_failure_isolation();
    test_existing_paths();
    puts("Pinned BTstack reconnect handler/GAP API tests passed (64 reconnect cycles plus isolation/failure/cancel cases)");
    return 0;
}
