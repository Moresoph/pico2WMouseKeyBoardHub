#include "hub_input.h"
#include "hub_external_input.h"
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
static uint32_t last_request, framing_errors;
static bool started;

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
    if ((uint32_t)(now - last_request) >= HUB_LINK_HEARTBEAT_MS) {
        hub_link_frame_t request = {.type = HUB_LINK_REQUEST, .nonce = state.nonce};
        uint8_t wire[HUB_LINK_WIRE_MAX];
        size_t length = hub_link_encode(&request, wire);
        uart_write_blocking(LINK_UART, wire, length);
        last_request = now;
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
bool hub_input_enrolling(void) { return false; }
bool hub_input_forget(unsigned mask) { (void)mask; return use_esp_console(); }
bool hub_input_open_test_connect(unsigned slot, const char *address) {
    (void)slot; (void)address; return use_esp_console();
}
bool hub_input_open_test_peer(const uint8_t address[6]) { (void)address; return false; }
bool hub_input_open_bridge_enabled(void) { return false; }
bool hub_input_set_open_bridge(bool enabled) { (void)enabled; return use_esp_console(); }
