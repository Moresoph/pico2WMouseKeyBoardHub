#ifndef HUB_INPUT_RECORDS_H
#define HUB_INPUT_RECORDS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define HUB_SAVED_INPUTS 5u
#define HUB_INPUT_RECORD_V1_BYTES 68u
#define HUB_INPUT_NAME_BYTES 64u
#define HUB_INPUT_RECORD_STRIDE (12u + HUB_INPUT_NAME_BYTES)
#define HUB_INPUT_RECORD_BYTES (8u + HUB_SAVED_INPUTS * HUB_INPUT_RECORD_STRIDE)
#define HUB_RECORD_NONE 255u
#define HUB_RECORD_CLASSIC 0u
#define HUB_RECORD_BLE 1u
typedef struct {
    bool used, allow_open;
    uint8_t role, transport, type, address[6], order;
    char name[HUB_INPUT_NAME_BYTES];
} hub_saved_input_t;
typedef struct {
    uint8_t active[2];
    hub_saved_input_t slots[HUB_SAVED_INPUTS];
} hub_input_records_t;
static inline void hub_records_init(hub_input_records_t *r) {
    memset(r, 0, sizeof(*r)); r->active[0] = r->active[1] = HUB_RECORD_NONE;
}
static inline void hub_input_name_copy(char out[HUB_INPUT_NAME_BYTES], const char *name) {
    size_t length = 0;
    while (length < HUB_INPUT_NAME_BYTES && name[length]) ++length;
    if (length >= HUB_INPUT_NAME_BYTES) {
        length = HUB_INPUT_NAME_BYTES - 1;
        while (length && ((unsigned char)name[length] & 0xc0u) == 0x80u) --length;
    }
    memset(out, 0, HUB_INPUT_NAME_BYTES);
    memcpy(out, name, length);
}
static inline uint32_t hub_records_crc(const uint8_t *data, unsigned length) {
    uint32_t crc = 0xffffffffu;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return ~crc;
}
static inline void hub_records_encode(const hub_input_records_t *r, uint8_t data[HUB_INPUT_RECORD_BYTES]) {
    memset(data, 0, HUB_INPUT_RECORD_BYTES); data[0] = 2; data[1] = r->active[0]; data[2] = r->active[1];
    for (unsigned i = 0; i < HUB_SAVED_INPUTS; ++i) {
        const hub_saved_input_t *s = &r->slots[i]; uint8_t *p = data + 4 + i * HUB_INPUT_RECORD_STRIDE;
        if (!s->used) continue;
        p[0] = 1; p[1] = s->role; p[2] = s->transport; p[3] = s->type; p[4] = s->allow_open;
        memcpy(p + 5, s->address, 6); p[11] = s->order;
        memcpy(p + 12, s->name, HUB_INPUT_NAME_BYTES);
    }
    uint32_t crc = hub_records_crc(data, HUB_INPUT_RECORD_BYTES - 4);
    for (unsigned i = 0; i < 4; ++i) data[HUB_INPUT_RECORD_BYTES - 4 + i] = (uint8_t)(crc >> (8 * i));
}
static inline bool hub_records_decode_sized(hub_input_records_t *r, const uint8_t *data, size_t size) {
    bool legacy = size == HUB_INPUT_RECORD_V1_BYTES && data[0] == 1;
    if (!legacy && (size != HUB_INPUT_RECORD_BYTES || data[0] != 2)) return false;
    uint32_t crc = 0; for (unsigned i = 0; i < 4; ++i) crc |= (uint32_t)data[size-4+i] << (8*i);
    if (data[3] || crc != hub_records_crc(data, (unsigned)size - 4)) return false;
    hub_input_records_t next; hub_records_init(&next); unsigned orders = 0;
    unsigned stride = legacy ? 12u : HUB_INPUT_RECORD_STRIDE;
    for (unsigned i = 0; i < HUB_SAVED_INPUTS; ++i) {
        const uint8_t *p = data + 4 + i * stride;
        if (!p[0]) { for (unsigned j=1;j<stride;++j) if(p[j]) return false; continue; }
        if (p[0]!=1 || (p[1]!=1 && p[1]!=2) || p[2]>HUB_RECORD_BLE || p[3]>1 ||
            p[4]>1 || (p[4] && p[2]!=HUB_RECORD_CLASSIC) || !p[11] || p[11]>5 || (orders & (1u<<p[11]))) return false;
        orders |= 1u << p[11]; hub_saved_input_t *s=&next.slots[i];
        s->used=true; s->role=p[1]; s->transport=p[2]; s->type=p[3]; s->allow_open=p[4]; s->order=p[11];
        memcpy(s->address,p+5,6);
        if (!legacy) {
            if (!memchr(p + 12, 0, HUB_INPUT_NAME_BYTES)) return false;
            memcpy(s->name, p + 12, HUB_INPUT_NAME_BYTES);
        }
        for(unsigned j=0;j<i;++j) if(next.slots[j].used && !memcmp(next.slots[j].address,s->address,6))return false;
    }
    for(unsigned i=0;i<2;++i) {
        uint8_t a=data[1+i]; if(a!=255 && (a>=5 || !next.slots[a].used || next.slots[a].role!=i+1))return false;
        next.active[i]=a;
    }
    *r=next;return true;
}
static inline bool hub_records_decode(hub_input_records_t *r, const uint8_t data[HUB_INPUT_RECORD_BYTES]) {
    return hub_records_decode_sized(r, data, HUB_INPUT_RECORD_BYTES);
}
static inline int hub_records_oldest(const hub_input_records_t *r) {
    int target=-1;
    for(unsigned i=0;i<5;++i) if(r->slots[i].used && (target<0 || r->slots[i].order<r->slots[target].order))target=(int)i;
    return target;
}
static inline bool hub_records_forget(hub_input_records_t *r,unsigned slot) {
    if(slot>=5 || !r->slots[slot].used)return false;
    unsigned order=r->slots[slot].order;
    memset(&r->slots[slot],0,sizeof(r->slots[slot]));
    for(unsigned i=0;i<5;++i)if(r->slots[i].order>order)--r->slots[i].order;
    for(unsigned i=0;i<2;++i)if(r->active[i]==slot)r->active[i]=255;
    return true;
}
static inline int hub_records_insert(hub_input_records_t *r,const hub_saved_input_t *input) {
    if(!input || (input->role!=1 && input->role!=2) || input->transport>HUB_RECORD_BLE ||
       input->type>1 || (input->allow_open && input->transport!=HUB_RECORD_CLASSIC))return -1;
    int target=-1;
    for(unsigned i=0;i<5;++i)if(r->slots[i].used && !memcmp(r->slots[i].address,input->address,6)) {
        if(r->slots[i].role!=input->role || r->slots[i].transport!=input->transport || r->slots[i].type!=input->type)return -1;
        uint8_t order=r->slots[i].order;
        char previous_name[HUB_INPUT_NAME_BYTES];memcpy(previous_name,r->slots[i].name,sizeof(previous_name));
        r->slots[i]=*input;r->slots[i].used=true;r->slots[i].order=order;
        if (!input->name[0]) memcpy(r->slots[i].name,previous_name,sizeof(previous_name));
        r->active[input->role-1]=(uint8_t)i;return (int)i;
    }
    for(unsigned i=0;i<5;++i)if(!r->slots[i].used){target=(int)i;break;}
    if(target<0){target=hub_records_oldest(r);hub_records_forget(r,(unsigned)target);}
    unsigned newest=0;for(unsigned i=0;i<5;++i)if(r->slots[i].order>newest)newest=r->slots[i].order;
    r->slots[target]=*input;r->slots[target].used=true;r->slots[target].order=(uint8_t)(newest+1);
    r->active[input->role-1]=(uint8_t)target;return target;
}
#endif
