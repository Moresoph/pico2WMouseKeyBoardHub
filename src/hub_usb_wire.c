#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/usb.h"
#include "pico/time.h"
#include "tusb.h"
#include "host/hcd.h"
#include "hub_usb_wire.h"
#include "hub_usb_host.h"

#define WIRE_SAMPLES 2048u
#define WIRE_DIVIDER 4u
#define WIRE_CHUNK 64u
#define WIRE_TIMEOUT_US 1000000u

typedef enum { WIRE_WAITING, WIRE_ACTIVE, WIRE_READY, WIRE_FAILED } wire_state_t;
typedef struct {
    volatile uint32_t samples[WIRE_SAMPLES];
    wire_state_t state;
    uint32_t clock_hz, transferred, generation;
    uint64_t started_us;
    uint8_t hub, port;
    bool announced;
    const char *error;
} wire_capture_t;

static wire_capture_t captures[2];
static int active = -1, channel = -1, timer = -1, replay = -1;
static unsigned replay_offset;
static bool child_targeted;
static const char *const names[] = {"root", "child"};

static void release_dma(void) {
    if (timer >= 0) dma_timer_set_fraction((unsigned)timer, 0, 1);
    if (channel >= 0) {
        /* SDK cleanup disables chaining/EN before abort (including RP2350-E5). */
        dma_channel_cleanup((unsigned)channel);
        dma_channel_unclaim((unsigned)channel);
        channel = -1;
    }
    if (timer >= 0) {
        dma_timer_unclaim((unsigned)timer);
        timer = -1;
    }
}

static void finish_capture(void) {
    if (active < 0) return;
    wire_capture_t *capture = &captures[active];
    bool busy = dma_channel_is_busy((unsigned)channel);
    if (busy && time_us_64() - capture->started_us < WIRE_TIMEOUT_US) return;
    const dma_channel_hw_t *hw = dma_channel_hw_addr((unsigned)channel);
    uint32_t remaining = hw->transfer_count;
    if (busy) capture->error = "DMA timeout";
    else if (hw->ctrl_trig & (DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS))
        capture->error = "DMA bus error";
    else if (remaining) capture->error = "DMA incomplete";
    capture->transferred = remaining <= WIRE_SAMPLES ? WIRE_SAMPLES - remaining : 0;
    capture->state = capture->error ? WIRE_FAILED : WIRE_READY;
    release_dma();
    active = -1;
}

void hub_usb_wire_setup(uint8_t address, const uint8_t setup[8]) {
    static const uint8_t first_descriptor[] = {0x80, 6, 0, 1, 0, 0, 8, 0};
    if (address || memcmp(setup, first_descriptor, sizeof(first_descriptor))) return;
    hcd_devtree_info_t info;
    hcd_devtree_get_info(address, &info);
    unsigned index = info.hub_addr ? 1u : 0u;
    wire_capture_t *capture = &captures[index];
    if (capture->state != WIRE_WAITING) return;
    if (index && child_targeted && (capture->hub != info.hub_addr || capture->port != info.hub_port)) return;
    capture->hub = info.hub_addr;
    capture->port = info.hub_port;
    capture->clock_hz = clock_get_hz(clk_sys);
    finish_capture();
    capture->state = WIRE_FAILED;
    if (info.speed != TUSB_SPEED_FULL) {
        capture->error = "only full-speed digital sampling is supported";
        return;
    }
    if (capture->clock_hz < 96000000u) {
        capture->error = "system clock too slow for this sampling divider";
        return;
    }
    if (active >= 0) {
        capture->error = "previous capture still active";
        return;
    }
    channel = dma_claim_unused_channel(false);
    if (channel < 0) {
        capture->error = "no free DMA channel";
        return;
    }
    timer = dma_claim_unused_timer(false);
    if (timer < 0) {
        capture->error = "no free DMA timer";
        release_dma();
        return;
    }
    dma_channel_cleanup((unsigned)channel);
    dma_channel_hw_addr((unsigned)channel)->ctrl_trig =
        DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS;
    dma_channel_config config = dma_channel_get_default_config((unsigned)channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_irq_quiet(&config, true);
    channel_config_set_high_priority(&config, true);
    channel_config_set_dreq(&config, dma_get_timer_dreq((unsigned)timer));
    dma_timer_set_fraction((unsigned)timer, 1, WIRE_DIVIDER);
    capture->started_us = time_us_64();
    capture->state = WIRE_ACTIVE;
    active = (int)index;
    /* Read pin states only: never enable PHY overrides or drive USB/GPIO pins. */
    dma_channel_configure((unsigned)channel, &config, capture->samples,
                          &usb_hw->phy_direct, WIRE_SAMPLES, true);
}

void hub_usb_wire_status(void) {
    finish_capture();
    static const char *const states[] = {"waiting", "capturing", "ready", "failed"};
    printf("[usb-wire] digital upstream samples; nominal rate only, NOT an analog measurement\n");
    for (unsigned i = 0; i < 2; ++i) {
        const wire_capture_t *capture = &captures[i];
        printf("[usb-wire] %s state=%s hub=%u port=%u samples=%" PRIu32 " generation=%" PRIu32,
               names[i], states[capture->state], capture->hub, capture->port,
               capture->transferred, capture->generation);
        if (capture->error) printf(" error=%s", capture->error);
        printf("\n");
    }
}

void hub_usb_wire_arm(void) {
    finish_capture();
    wire_capture_t *capture = &captures[1];
    if (active >= 0 || replay >= 0 || !hub_usb_enum_can_rearm()) {
        printf("[usb-wire] rearm rejected: capture/replay/enumeration busy or cleanup blocked\n");
        return;
    }
    if (capture->state == WIRE_WAITING || !capture->hub || !capture->port) {
        printf("[usb-wire] rearm rejected: waiting for the first downstream capture\n");
        return;
    }
    if (capture->generation == UINT32_MAX) {
        printf("[usb-wire] rearm rejected: generation exhausted\n");
        return;
    }
    ++capture->generation;
    capture->state = WIRE_WAITING;
    capture->clock_hz = capture->transferred = 0;
    capture->started_us = 0;
    capture->announced = false;
    capture->error = NULL;
    child_targeted = true;
    printf("[usb-wire] armed generation=%" PRIu32 " hub=%u port=%u; root reference retained\n",
           capture->generation, capture->hub, capture->port);
    printf("[usb-wire] unplug/replug the receiver at that same port; keep Pico/Hub powered\n");
}

void hub_usb_wire_dump(void) {
    if (replay >= 0) {
        printf("[usb-wire] replay already in progress\n");
        return;
    }
    hub_usb_wire_status();
    for (unsigned i = 0; i < 2; ++i) {
        if (captures[i].state == WIRE_READY) {
            replay = (int)i;
            replay_offset = 0;
            return;
        }
    }
    printf("[usb-wire] no complete capture to replay\n");
}

static bool write_line(const char *line, int length, size_t capacity) {
    if (length < 0 || (size_t)length >= capacity ||
        fwrite(line, 1, (size_t)length, stdout) != (size_t)length) {
        replay = -1;
        printf("[usb-wire] UART replay FAILED (format/short write)\n");
        return false;
    }
    return true;
}

void hub_usb_wire_poll(void) {
    finish_capture();
    for (unsigned i = 0; i < 2; ++i) {
        wire_capture_t *capture = &captures[i];
        if (capture->announced || capture->state < WIRE_READY) continue;
        capture->announced = true;
        if (capture->error) printf("[usb-wire] %s capture FAILED: %s\n", names[i], capture->error);
        else printf("[usb-wire] %s capture ready; use usb wire (nominal samples, decoding unverified)\n", names[i]);
    }
    if (replay < 0) return;
    const wire_capture_t *capture = &captures[replay];
    char line[160];
    if (!replay_offset) {
        int length = snprintf(line, sizeof(line),
            "[usb-wire] BEGIN %s samples=%u sys_hz=%" PRIu32 " divider=%u hub=%u port=%u"
            " generation=%" PRIu32 " started_us=%" PRIu64 "\n",
            names[replay], WIRE_SAMPLES, capture->clock_hz, WIRE_DIVIDER,
            capture->hub, capture->port, capture->generation, capture->started_us);
        if (!write_line(line, length, sizeof(line))) return;
        replay_offset = 1;
        return;
    }
    unsigned offset = replay_offset - 1u;
    if (offset < WIRE_SAMPLES) {
        static const char hex[] = "0123456789abcdef";
        int length = snprintf(line, sizeof(line), "[usb-wire] DATA %s offset=%u hex=", names[replay], offset / 4u);
        if (length < 0 || (size_t)length + WIRE_CHUNK / 2u + 1u >= sizeof(line)) {
            write_line(line, -1, sizeof(line));
            return;
        }
        for (unsigned b = 0; b < WIRE_CHUNK; b += 4) {
            unsigned packed = 0;
            for (unsigned s = 0; s < 4; ++s) {
                uint32_t sample = capture->samples[offset + b + s];
                unsigned state = (sample & USB_USBPHY_DIRECT_RX_DP_BITS ? 1u : 0u) |
                                 (sample & USB_USBPHY_DIRECT_RX_DM_BITS ? 2u : 0u);
                packed |= state << (2u * s);
            }
            line[length++] = hex[packed >> 4];
            line[length++] = hex[packed & 15u];
        }
        line[length++] = '\n';
        if (write_line(line, length, sizeof(line))) replay_offset += WIRE_CHUNK;
        return;
    }
    int length = snprintf(line, sizeof(line), "[usb-wire] END %s\n", names[replay]);
    if (!write_line(line, length, sizeof(line))) return;
    ++replay;
    if (replay >= 2 || captures[replay].state != WIRE_READY) replay = -1;
    replay_offset = 0;
}
