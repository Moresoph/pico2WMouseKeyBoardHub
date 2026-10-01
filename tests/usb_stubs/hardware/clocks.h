#ifndef HUB_USB_TEST_CLOCKS_H
#define HUB_USB_TEST_CLOCKS_H
#include <stdint.h>
enum { clk_sys };
uint32_t clock_get_hz(unsigned clock);
#endif
