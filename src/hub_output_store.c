#include <stdio.h>
#include <string.h>

#include "btstack_tlv.h"
#include "hub_output_store.h"

static uint32_t record_crc(const uint8_t *data, unsigned length) {
    uint32_t crc = 0xffffffffu;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return ~crc;
}

bool hub_output_identity_valid(uint8_t type, const uint8_t address[6]) {
    if (type > 1 || address == NULL) return false;
    unsigned nonzero = 0, nonff = 0;
    for (unsigned i = 0; i < 6; ++i) {
        nonzero |= address[i];
        nonff |= address[i] ^ 0xffu;
    }
    /* BD_ADDR is stored most-significant octet first. Never save an RPA/NRPA. */
    return nonzero && nonff && (type == 0 || (address[0] & 0xc0u) == 0xc0u);
}

static bool fail(hub_output_store_t *store, const char *reason) {
    store->fault = true;
    printf("[output-store] %s; OUTPUT LOCKED, no slot reassignment/fallback. "
           "Inspect/repair " HUB_OUTPUT_STORE_NAME " storage then restart; bond DB left untouched\n", reason);
    return false;
}

static void encode(const hub_output_store_t *store, uint8_t record[HUB_OUTPUT_STORE_BYTES]) {
    memset(record, 0, HUB_OUTPUT_STORE_BYTES);
    memcpy(record, "PHO1", 4);
    record[4] = 1;
    record[5] = (uint8_t)store->selected;
    record[6] = HUB_OUTPUT_STORE_SLOTS;
    for (unsigned i = 0; i < HUB_OUTPUT_STORE_SLOTS; ++i) {
        const hub_output_identity_t *identity = &store->identities[i];
        if (!identity->reserved) continue;
        uint8_t *entry = &record[8 + 8 * i];
        entry[0] = 1;
        entry[1] = identity->type;
        memcpy(&entry[2], identity->address, 6);
    }
    uint32_t crc = record_crc(record, HUB_OUTPUT_STORE_BYTES - 4);
    for (unsigned i = 0; i < 4; ++i)
        record[HUB_OUTPUT_STORE_BYTES - 4 + i] = (uint8_t)(crc >> (8 * i));
}

int hub_output_store_find(const hub_output_store_t *store, uint8_t type,
                          const uint8_t address[6]) {
    for (unsigned i = 0; i < HUB_OUTPUT_STORE_SLOTS; ++i) {
        const hub_output_identity_t *identity = &store->identities[i];
        if (identity->reserved && identity->type == type
            && memcmp(identity->address, address, 6) == 0) return (int)i;
    }
    return -1;
}

static bool decode(hub_output_store_t *store, const uint8_t record[HUB_OUTPUT_STORE_BYTES]) {
    if (memcmp(record, "PHO1", 4) || record[4] != 1
        || record[5] >= HUB_OUTPUT_STORE_SLOTS
        || record[6] != HUB_OUTPUT_STORE_SLOTS || record[7]) return false;
    uint32_t crc = 0;
    for (unsigned i = 0; i < 4; ++i)
        crc |= (uint32_t)record[HUB_OUTPUT_STORE_BYTES - 4 + i] << (8 * i);
    if (crc != record_crc(record, HUB_OUTPUT_STORE_BYTES - 4)) return false;
    hub_output_store_t decoded = {0};
    decoded.selected = record[5];
    bool any = false;
    for (unsigned i = 0; i < HUB_OUTPUT_STORE_SLOTS; ++i) {
        const uint8_t *entry = &record[8 + 8 * i];
        if (entry[0] == 0) {
            for (unsigned j = 1; j < 8; ++j)
                if (entry[j]) return false;
            continue;
        }
        if (entry[0] != 1 || !hub_output_identity_valid(entry[1], &entry[2])
            || hub_output_store_find(&decoded, entry[1], &entry[2]) >= 0) return false;
        decoded.identities[i].reserved = true;
        decoded.identities[i].type = entry[1];
        memcpy(decoded.identities[i].address, &entry[2], 6);
        any = true;
    }
    if ((any && !decoded.identities[decoded.selected].reserved)
        || (!any && decoded.selected != 0)) return false;
    decoded.loaded = true;
    *store = decoded;
    return true;
}

bool hub_output_store_load(hub_output_store_t *store) {
    memset(store, 0, sizeof(*store));
    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    btstack_tlv_get_instance(&tlv, &context);
    if (!tlv || !tlv->get_tag || !tlv->store_tag)
        return fail(store, "TLV unavailable");
    uint8_t record[HUB_OUTPUT_STORE_BYTES];
    int length = tlv->get_tag(context, HUB_OUTPUT_STORE_TAG, NULL, 0);
    if (length == 0) {
        store->loaded = true;
        printf("[output-store] " HUB_OUTPUT_STORE_NAME " absent: selected PC1; slots may be claimed only "
               "by bonded, encrypted PC connections\n");
        return true;
    }
    if (length != HUB_OUTPUT_STORE_BYTES
        || tlv->get_tag(context, HUB_OUTPUT_STORE_TAG, record, sizeof(record)) != length
        || !decode(store, record)) return fail(store, "Malformed/unsupported " HUB_OUTPUT_STORE_NAME " record");
    printf("[output-store] restored identities and selected PC%u\n", store->selected + 1);
    return true;
}

static bool commit(hub_output_store_t *store, const hub_output_store_t *next) {
    const btstack_tlv_t *tlv = NULL;
    void *context = NULL;
    btstack_tlv_get_instance(&tlv, &context);
    if (!tlv || !tlv->get_tag || !tlv->store_tag) return fail(store, "TLV unavailable on write");
    uint8_t record[HUB_OUTPUT_STORE_BYTES], readback[HUB_OUTPUT_STORE_BYTES];
    encode(next, record);
    if (tlv->store_tag(context, HUB_OUTPUT_STORE_TAG, record, sizeof(record)) != 0)
        return fail(store, HUB_OUTPUT_STORE_NAME " write failed");
    if (tlv->get_tag(context, HUB_OUTPUT_STORE_TAG, NULL, 0) != sizeof(record)
        || tlv->get_tag(context, HUB_OUTPUT_STORE_TAG, readback, sizeof(readback)) != sizeof(readback)
        || memcmp(record, readback, sizeof(record))) return fail(store, HUB_OUTPUT_STORE_NAME " readback failed");
    *store = *next;
    return true;
}

int hub_output_store_claim(hub_output_store_t *store, uint8_t type,
                           const uint8_t address[6]) {
    if (!store->loaded || store->fault || !hub_output_identity_valid(type, address)) return -1;
    int existing = hub_output_store_find(store, type, address);
    if (existing >= 0) return existing;
    for (unsigned i = 0; i < HUB_OUTPUT_STORE_SLOTS; ++i) {
        if (store->identities[i].reserved) continue;
        hub_output_store_t next = *store;
        next.identities[i].reserved = true;
        next.identities[i].type = type;
        memcpy(next.identities[i].address, address, 6);
        if (!commit(store, &next)) return -1;
        printf("[output-store] reserved PC%u identity (verified flash write)\n", i + 1);
        return (int)i;
    }
    return -1;
}

bool hub_output_store_select(hub_output_store_t *store, unsigned slot) {
    if (!store->loaded || store->fault || slot >= HUB_OUTPUT_STORE_SLOTS
        || !store->identities[slot].reserved) return false;
    if (store->selected == slot) return true;
    hub_output_store_t next = *store;
    next.selected = slot;
    if (!commit(store, &next)) return false;
    printf("[output-store] saved selected PC%u (verified flash write)\n", slot + 1);
    return true;
}
