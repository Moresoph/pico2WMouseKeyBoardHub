#include "hub_input.h"
#include "hub_config.h"
#include "hub_external_input.h"
#include "hub_output.h"
#include "btstack_run_loop.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define LINK_UART uart1
#define RX_SIZE 2048u
static uint8_t rx[RX_SIZE];
static volatile unsigned rx_write, rx_read;
static volatile bool rx_fault;
static hub_link_parser_t parser;
static hub_external_input_t state;
static btstack_timer_source_t timer;
static uint32_t last_request, last_output, framing_errors, output_packets;
static uint16_t output_sequence;
static uint8_t tx[HUB_LINK_WIRE_MAX], tx_type;
static size_t tx_length, tx_offset;
static bool started;
static uint32_t ui_received, ui_session;
static uint16_t ui_sequence;
static bool ui_valid, ui_enrollment;
static uint32_t manage_id;
static bool manage_result, reply_pending;
static uint16_t manage_sequence;

static void uart_irq(void) {
    while (uart_is_readable(LINK_UART)) {
        uint32_t data = uart_get_hw(LINK_UART)->dr;
        unsigned next = (rx_write + 1u) % RX_SIZE;
        if ((data & 0xf00u) || next == rx_read) rx_fault = true;
        else {
            rx[rx_write] = (uint8_t)data;
            rx_write = next;
        }
    }
}

static void reset_link(const char *reason) {
    uint32_t nonce;
    do { nonce = get_rand_32(); } while (!nonce || nonce == state.nonce);
    hub_external_reset(&state, nonce);
    uint32_t saved = save_and_disable_interrupts();
    rx_read = rx_write;
    rx_fault = false;
    uart_get_hw(LINK_UART)->rsr = 0;
    restore_interrupts(saved);
    memset(&parser, 0, sizeof(parser));
    last_request = btstack_run_loop_get_time_ms() - HUB_LINK_HEARTBEAT_MS;
    last_output = btstack_run_loop_get_time_ms() - HUB_LINK_OUTPUT_PERIOD_MS;
    output_sequence = 0;
    tx_length = tx_offset = 0;
    ui_valid = reply_pending = false;
    manage_id = 0;
    printf("[dual-bt] link reset: %s; release keys/buttons to resume\n", reason);
}

static void poll_link(btstack_timer_source_t *source) {
    uint32_t now = btstack_run_loop_get_time_ms();
    if (rx_fault) reset_link("UART overrun/framing error");
    if (hub_external_expired(&state, now)) reset_link("ESP32 heartbeat timeout");
    for (unsigned budget = 0; budget < 512 && rx_read != rx_write; ++budget) {
        uint8_t byte = rx[rx_read];
        rx_read = (rx_read + 1u) % RX_SIZE;
        hub_link_frame_t frame;
        uint32_t errors = parser.errors;
        if (hub_link_feed(&parser, byte, &frame)) {
            if (state.online && frame.nonce == state.nonce && frame.session == state.session) {
                if (frame.type == HUB_LINK_UI) {
                    uint16_t distance = (uint16_t)(frame.sequence - ui_sequence);
                    if (!ui_valid || ui_session != frame.session || (distance && distance < 0x8000u)) {
                        ui_sequence = frame.sequence; ui_session = frame.session;
                        ui_received = now; ui_valid = true; ui_enrollment = frame.payload[0] != 0;
                    }
                } else if (frame.type == HUB_LINK_MANAGE) {
                    uint32_t id = (uint32_t)frame.payload[0] | (uint32_t)frame.payload[1] << 8 |
                        (uint32_t)frame.payload[2] << 16 | (uint32_t)frame.payload[3] << 24;
                    uint16_t distance=(uint16_t)(frame.sequence-manage_sequence);
                    if (id && (!manage_id || (distance && distance<0x8000u))) {
                        manage_result = frame.payload[4] >= 1 && frame.payload[4] <= HUB_COMPUTER_COUNT &&
                            hub_output_forget(frame.payload[4] - 1, frame.payload[5], frame.payload + 6);
                        manage_id = id;
                        manage_sequence=frame.sequence;
                    }
                    reply_pending = id != 0 && id==manage_id;
                }
            }
            bool was_online = state.online;
            if (!hub_external_receive(&state, &frame, now)) {
                reset_link("invalid state / sequence gap / ESP32 restart");
                break;
            }
            if (!was_online && state.online) printf("[dual-bt] ESP32 link online\n");
        } else if (parser.errors != errors) {
            ++framing_errors;
            reset_link("UART packet CRC/format error");
            break;
        }
    }
    if (!tx_length && (uint32_t)(now - last_request) >= HUB_LINK_HEARTBEAT_MS) {
        hub_link_frame_t request = {.type = HUB_LINK_REQUEST, .nonce = state.nonce};
        tx_length = hub_link_encode(&request, tx);
        tx_type = HUB_LINK_REQUEST;
        last_request = now;
    } else if (!tx_length && state.online && reply_pending) {
        hub_link_frame_t reply = {.type = HUB_LINK_MANAGE_REPLY, .length = 5,
            .nonce = state.nonce, .session = state.session};
        for (unsigned i = 0; i < 4; ++i) reply.payload[i] = (uint8_t)(manage_id >> (8u * i));
        reply.payload[4] = manage_result;
        tx_length = hub_link_encode(&reply, tx); tx_type = reply.type; reply_pending = false;
    } else if (!tx_length && state.online &&
               (uint32_t)(now - last_output) >= HUB_LINK_OUTPUT_PERIOD_MS) {
        hub_link_output_t output;
        hub_output_snapshot(&output);
        output.flags |= HUB_LINK_OUTPUT_INPUT_ONLINE;
        hub_link_frame_t frame = {.nonce = state.nonce, .session = state.session,
                                  .sequence = output_sequence++};
        hub_link_pack_output(&frame, &output);
        tx_length = hub_link_encode(&frame, tx);
        tx_type = HUB_LINK_OUTPUT;
        last_output = now;
    }
    for (unsigned budget = 0; tx_offset < tx_length && budget < 32 && uart_is_writable(LINK_UART); ++budget)
        uart_get_hw(LINK_UART)->dr = tx[tx_offset++];
    if (tx_length && tx_offset == tx_length) {
        if (tx_type == HUB_LINK_OUTPUT) ++output_packets;
        tx_length = tx_offset = 0;
    }
    btstack_run_loop_set_timer(source, 1);
    btstack_run_loop_add_timer(source);
}

void hub_input_init(void) {
    memset(&state, 0, sizeof(state));
    uart_init(LINK_UART, 460800);
    gpio_set_function(4, GPIO_FUNC_UART);
    gpio_set_function(5, GPIO_FUNC_UART);
    gpio_pull_up(5);
    uart_set_hw_flow(LINK_UART, false, false);
    uart_set_format(LINK_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(LINK_UART, true);
    irq_set_exclusive_handler(UART1_IRQ, uart_irq);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(LINK_UART, true, false);
    reset_link("startup");
}

void hub_input_start(void) {
    if (started) return;
    started = true;
    btstack_run_loop_set_timer_handler(&timer, poll_link);
    btstack_run_loop_set_timer(&timer, 1);
    btstack_run_loop_add_timer(&timer);
    printf("[dual-bt] INPUT=ESP32 UART1 TX=GP4 RX=GP5 460800 8N1; onboard radio OUTPUT only\n");
}

void hub_input_status(void) {
    printf("[dual-bt] online=%u packets=%" PRIu32 " resets=%" PRIu32 " framing-errors=%" PRIu32
           " last-rx-age-ms=%" PRIu32 "\n", state.online, state.accepted, state.resets, framing_errors,
           state.online ? btstack_run_loop_get_time_ms() - state.last_rx : 0);
    printf("[dual-bt] computer telemetry packets=%" PRIu32 " period=1000ms\n", output_packets);
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i)
        printf("[dual-bt] input %u keyboard=%u mouse=%u epoch=%u\n", i + 1,
               !!(state.inputs[i].roles & HUB_LINK_KEYBOARD),
               !!(state.inputs[i].roles & HUB_LINK_MOUSE), state.inputs[i].epoch);
}

static bool use_esp_console(void) {
    printf("[dual-bt] manage input scan/connect/pairing/forget on the ESP32 USB console; enter help there\n");
    return false;
}
void hub_input_scan(bool classic) { (void)classic; use_esp_console(); }
void hub_input_scan_stop(void) { use_esp_console(); }
bool hub_input_connect(unsigned slot, bool classic, uint8_t type, const char *address) {
    (void)slot; (void)classic; (void)type; (void)address;
    return use_esp_console();
}
void hub_input_disconnect(unsigned slot) { (void)slot; use_esp_console(); }
bool hub_input_is_connection(uint16_t handle) { (void)handle; return false; }
bool hub_input_is_ble_mouse_ready(uint16_t handle) { (void)handle; return false; }
bool hub_input_is_expected_classic(const uint8_t address[6]) { (void)address; return false; }
bool hub_input_is_classic_target(const uint8_t address[6]) { (void)address; return false; }
bool hub_input_allow_pairing(uint16_t handle) { (void)handle; return false; }
bool hub_input_keyboard_ready(void) {
    if (!state.online || hub_external_expired(&state, btstack_run_loop_get_time_ms())) return false;
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i)
        if (state.inputs[i].roles & HUB_LINK_KEYBOARD) return true;
    return false;
}
bool hub_input_pairing_busy(void) { return false; }
bool hub_input_radio_busy(void) { return false; }
bool hub_input_enroll_start(void) { return use_esp_console(); }
void hub_input_enroll_stop(void) { use_esp_console(); }
bool hub_input_link_online(void) {
    return state.online && !hub_external_expired(&state, btstack_run_loop_get_time_ms());
}
bool hub_input_enrolling(void) {
    return hub_input_link_online() && ui_valid && ui_session == state.session &&
        btstack_run_loop_get_time_ms() - ui_received < HUB_LINK_OUTPUT_TIMEOUT_MS && ui_enrollment;
}
bool hub_input_forget(unsigned mask) { (void)mask; return use_esp_console(); }
bool hub_input_open_test_connect(unsigned slot, const char *address) {
    (void)slot; (void)address; return use_esp_console();
}
bool hub_input_open_test_peer(const uint8_t address[6]) { (void)address; return false; }
bool hub_input_open_bridge_enabled(void) { return false; }
bool hub_input_set_open_bridge(bool enabled) { (void)enabled; return use_esp_console(); }
