#ifndef HUB_USB_TEST_DMA_H
#define HUB_USB_TEST_DMA_H
#include <stdbool.h>
#include <stdint.h>
enum { DMA_SIZE_32 = 2 };
#define DMA_CH0_CTRL_TRIG_READ_ERROR_BITS (1u << 30)
#define DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS (1u << 29)
typedef struct { uint32_t ctrl_trig, transfer_count; } dma_channel_hw_t;
typedef struct {
    unsigned size, dreq;
    bool read_increment, write_increment, quiet, priority;
} dma_channel_config;
int dma_claim_unused_channel(bool required);
int dma_claim_unused_timer(bool required);
void dma_channel_cleanup(unsigned channel);
void dma_channel_unclaim(unsigned channel);
void dma_timer_unclaim(unsigned timer);
bool dma_channel_is_busy(unsigned channel);
dma_channel_hw_t *dma_channel_hw_addr(unsigned channel);
dma_channel_config dma_channel_get_default_config(unsigned channel);
void channel_config_set_transfer_data_size(dma_channel_config *config, unsigned size);
void channel_config_set_read_increment(dma_channel_config *config, bool value);
void channel_config_set_write_increment(dma_channel_config *config, bool value);
void channel_config_set_irq_quiet(dma_channel_config *config, bool value);
void channel_config_set_high_priority(dma_channel_config *config, bool value);
void channel_config_set_dreq(dma_channel_config *config, unsigned dreq);
unsigned dma_get_timer_dreq(unsigned timer);
void dma_timer_set_fraction(unsigned timer, uint16_t numerator, uint16_t denominator);
void dma_channel_configure(unsigned channel, const dma_channel_config *config,
                           volatile void *write, const volatile void *read,
                           uint32_t count, bool start);
#endif
