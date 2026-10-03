#ifndef HUB_CONFIG_H
#define HUB_CONFIG_H

#define HUB_INPUT_COUNT 2
#ifndef HUB_DUAL_BLUETOOTH
#define HUB_DUAL_BLUETOOTH 0
#endif
#if HUB_DUAL_BLUETOOTH
#define HUB_COMPUTER_COUNT 7
#else
#define HUB_COMPUTER_COUNT 4
#endif
#define HUB_PC_CONNECTION_LIMIT 4
#define HUB_PAIRING_WINDOW_MS 120000u
#define HUB_INPUT_ENROLLMENT_MS 60000u
/* Existing SDK initiating-scan defaults, also advertised through Scan Parameters. */
#define HUB_INPUT_SCAN_INTERVAL 0x0060u
#define HUB_INPUT_SCAN_WINDOW 0x0030u
#ifndef HUB_CLASSIC_OPEN_TEST
#define HUB_CLASSIC_OPEN_TEST 0
#endif
#ifndef HUB_CLASSIC_OUTPUT_TEST
#define HUB_CLASSIC_OUTPUT_TEST 0
#endif
#ifndef HUB_USB_HOST_PROBE
#define HUB_USB_HOST_PROBE 0
#endif
#ifndef HUB_CLASSIC_INPUT_CENTRAL
#define HUB_CLASSIC_INPUT_CENTRAL 1
#endif
#ifndef HUB_CLASSIC_MOUSE_7P5_TEST
#define HUB_CLASSIC_MOUSE_7P5_TEST 0
#endif
#ifndef HUB_CLASSIC_OUTPUT_7P5_TEST
#define HUB_CLASSIC_OUTPUT_7P5_TEST 0
#endif
#if HUB_CLASSIC_MOUSE_7P5_TEST && !HUB_CLASSIC_OUTPUT_TEST
#error "The isolated mouse interval trial requires Classic output"
#endif
#if HUB_CLASSIC_OUTPUT_7P5_TEST && !HUB_CLASSIC_OUTPUT_TEST
#error "The output pacing trial requires Classic output"
#endif
#if HUB_CLASSIC_OUTPUT_TEST
#define HUB_DEVICE_NAME "Pico2W Classic HID Test"
#else
#define HUB_DEVICE_NAME "Pico2W Keyboard Mouse Hub"
#endif

#endif
