#include "hub_button.h"

#include "hardware/regs/io_qspi.h"
#include "hardware/regs/sio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#if !PICO_RP2350
#error "BOOTSEL sampling in this firmware targets Pico 2 W / RP2350"
#endif

/*
 * BOOTSEL pulls flash CS low through a resistor. Sample it with CS output
 * briefly disabled, executing entirely from SRAM with interrupts masked.
 * Core 1 must remain unused; do not introduce concurrent XIP DMA readers.
 * Based on the sampling procedure documented by Raspberry Pi's button example.
 */
bool __no_inline_not_in_flash_func(hub_bootsel_pressed)(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    uint32_t original = ioqspi_hw->io[1].ctrl;
    hw_write_masked(&ioqspi_hw->io[1].ctrl,
                   IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_VALUE_DISABLE
                       << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                   IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile unsigned settle = 0; settle < 1000; ++settle) {
        __asm volatile ("nop");
    }
    bool pressed = (sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS) == 0;
    hw_write_masked(&ioqspi_hw->io[1].ctrl,
                   original & IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS,
                   IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(irq_state);
    return pressed;
}
