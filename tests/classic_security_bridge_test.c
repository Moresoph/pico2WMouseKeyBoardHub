#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "hub_classic_security.h"
#include "input_policy.h"

typedef uint8_t BD_ADDR[6];
typedef uint8_t tBT_TRANSPORT;
typedef uint8_t tBTM_STATUS;
typedef struct { unsigned unused; } osi_thread_t;
typedef void tBTM_SEC_CBACK(BD_ADDR, tBT_TRANSPORT, void *, tBTM_STATUS);
enum { BT_TRANSPORT_BR_EDR = 1, BTM_SUCCESS = 0, BTM_CMD_STARTED = 1, BTM_WRONG_MODE = 6,
       BTM_NOT_AUTHORIZED = 11, BTM_FAILED_ON_SECURITY = 18, BTM_REPEATED_ATTEMPTS = 19 };
#define BTM_SEC_FLAG_ENCRYPTED 4u
static osi_thread_t thread;
osi_thread_t *btu_thread = &thread;
static bool allocate = true, post = true, asynchronous, link_up = true, record_found = true;
static uint8_t security_flags, completion_status;
static unsigned outstanding, completions;
static uint32_t reply_generation;
static bool reply_connected, reply_encrypted, reply_security;
static uint16_t acl_handle = 0x80;
static unsigned acl_closes;
static uint32_t acl_generation = 1;
static uint32_t cleanup_token=1;
static bool guard_busy;
static hub_cleanup_result_t close_result;
static uint32_t close_token;
static unsigned close_completions;
static uint8_t remove_status;
static void (*work)(void *);
static void *work_context, *pending_context;
static tBTM_SEC_CBACK *pending_callback;
static BD_ADDR pending_address;

static void *osi_malloc(size_t size) {
    if (!allocate) return NULL;
    void *p = malloc(size);
    assert(p);
    ++outstanding;
    return p;
}
static void osi_free(void *p) { assert(outstanding); --outstanding; free(p); }
static bool osi_thread_post(osi_thread_t *t, void (*fn)(void *), void *context, int queue, uint32_t timeout) {
    assert(t == &thread && queue == 0 && timeout == 0);
    if (!post) return false;
    work = fn; work_context = context;
    return true;
}
static bool BTM_IsAclConnectionUp(BD_ADDR address, tBT_TRANSPORT transport) {
    assert(address[0] == 1 && transport == BT_TRANSPORT_BR_EDR);
    return link_up;
}
static bool BTM_GetSecurityFlagsByTransport(BD_ADDR address, uint8_t *flags, tBT_TRANSPORT transport) {
    assert(address[0] == 1 && transport == BT_TRANSPORT_BR_EDR);
    *flags = security_flags;
    return record_found;
}
static uint16_t BTM_GetHCIConnHandle(BD_ADDR address, tBT_TRANSPORT transport) {
    assert(address[0] == 1 && transport == BT_TRANSPORT_BR_EDR);
    return acl_handle;
}
hub_cleanup_guard_t hub_esp_classic_recovery_check(const uint8_t address[6], uint16_t handle,
                                                  uint32_t generation,uint32_t token) {
    assert(address[0] == 1);
    if(guard_busy)return HUB_CLEANUP_DEFERRED;
    return handle == acl_handle && generation == acl_generation && token==cleanup_token?
        HUB_CLEANUP_CURRENT:HUB_CLEANUP_STALE;
}
void hub_esp_classic_cleanup_result(const uint8_t address[6],uint16_t handle,uint32_t generation,
                                    uint32_t token,hub_cleanup_result_t result,uint8_t status){
    (void)handle;(void)generation;(void)status;assert(address[0]==1);
    close_result=result;close_token=token;++close_completions;
}
static tBTM_STATUS btm_remove_acl(BD_ADDR address, tBT_TRANSPORT transport) {
    assert(address[0] == 1 && transport == BT_TRANSPORT_BR_EDR);
    ++acl_closes;
    return remove_status;
}
static tBTM_STATUS BTM_SetEncryption(BD_ADDR address, tBT_TRANSPORT transport,
                                    tBTM_SEC_CBACK *callback, void *context) {
    assert(transport == BT_TRANSPORT_BR_EDR);
    if (asynchronous) {
        pending_callback = callback; pending_context = context;
        memcpy(pending_address, address, 6);
        return BTM_CMD_STARTED;
    }
    callback(address, transport, context, completion_status);
    return completion_status;
}
void hub_esp_classic_encryption_result(const uint8_t address[6], uint32_t generation,
                                      bool connected, bool encrypted, bool security_failure,
                                      uint8_t status, uint8_t flags) {
    assert(address[0] == 1 && status == completion_status);
    (void)flags;
    ++completions;
    reply_generation = generation; reply_connected = connected; reply_encrypted = encrypted;
    reply_security = security_failure;
}
#define HUB_CLASSIC_SECURITY_TEST 1
#define ESP_LOGW(tag, ...) ((void)(tag))
#include "..\esp32\classic_security_bridge.inc"

int main(void) {
    const uint8_t address[6] = {1,2,3,4,5,6};
    assert(hub_esp_classic_encrypt(address, 7) == ESP_OK && outstanding == 1);
    work(work_context);
    assert(!outstanding && completions == 1 && reply_generation == 7 && !reply_encrypted);
    security_flags = BTM_SEC_FLAG_ENCRYPTED;
    assert(hub_esp_classic_encrypt(address, 8) == ESP_OK);
    work(work_context);
    assert(!outstanding && reply_connected && reply_encrypted && reply_generation == 8);
    link_up = false;
    assert(hub_esp_classic_encrypt(address, 9) == ESP_OK);
    work(work_context);
    assert(!outstanding && !reply_connected && !reply_encrypted);
    link_up = true; record_found = false;
    assert(hub_esp_classic_encrypt(address, 10) == ESP_OK);
    work(work_context);
    assert(!outstanding && !reply_encrypted);
    record_found = true; asynchronous = true;
    assert(hub_esp_classic_encrypt(address, 11) == ESP_OK);
    work(work_context);
    assert(outstanding == 1);
    pending_callback(pending_address, BT_TRANSPORT_BR_EDR, pending_context, completion_status);
    assert(!outstanding && reply_encrypted && reply_generation == 11);
    asynchronous = false; security_flags = 0; completion_status = BTM_WRONG_MODE;
    assert(hub_esp_classic_encrypt(address, 12) == ESP_OK);
    work(work_context);
    assert(!reply_security && !reply_encrypted);
    completion_status = BTM_FAILED_ON_SECURITY;
    assert(hub_esp_classic_encrypt(address, 13) == ESP_OK);
    work(work_context);
    assert(reply_security && !reply_encrypted && !outstanding);
    assert(hub_esp_classic_close_acl(address, 0x80, 1,1) == ESP_OK);
    work(work_context);
    assert(acl_closes == 1 && !outstanding && close_result==HUB_CLEANUP_SENT);
    assert(hub_esp_classic_close_acl(address, 0x80, 1,1) == ESP_OK);
    acl_handle = 0x81;
    work(work_context);
    assert(acl_closes == 1 && !outstanding && close_result==HUB_CLEANUP_OBSOLETE);
    acl_handle = 0x80;
    assert(hub_esp_classic_close_acl(address, 0x80, 1,1) == ESP_OK);
    acl_generation = 2; /* Even if the controller reuses the same numeric handle. */
    work(work_context);
    assert(acl_closes == 1 && !outstanding);
    link_up = false;
    assert(hub_esp_classic_close_acl(address, 0x80, 2,1) == ESP_OK);
    work(work_context);
    assert(acl_closes == 1 && !outstanding && close_result==HUB_CLEANUP_ABSENT);
    link_up=true;guard_busy=true;
    assert(hub_esp_classic_close_acl(address,0x80,2,1)==ESP_OK);work(work_context);
    assert(close_result==HUB_CLEANUP_LOCK_BUSY && acl_closes==1 && !outstanding);
    guard_busy=false;cleanup_token=2;
    assert(hub_esp_classic_close_acl(address,0x80,2,1)==ESP_OK);work(work_context);
    assert(close_result==HUB_CLEANUP_OBSOLETE && acl_closes==1 && close_token==1);
    remove_status=3;
    assert(hub_esp_classic_close_acl(address,0x80,2,2)==ESP_OK);work(work_context);
    assert(close_result==HUB_CLEANUP_FAILED && acl_closes==2 && !outstanding);
    assert(close_completions==7);
    post = false;
    assert(hub_esp_classic_encrypt(address, 12) == ESP_FAIL && !outstanding);
    assert(hub_esp_classic_close_acl(address, 0x80, 2,2) == ESP_FAIL && !outstanding);
    allocate = false;
    assert(hub_esp_classic_encrypt(address, 13) == ESP_ERR_NO_MEM && !outstanding);
    assert(hub_esp_classic_close_acl(address, 0x80, 2,2) == ESP_ERR_NO_MEM && !outstanding);
    for (unsigned bits = 0; bits < 32; ++bits) {
        bool selected = (bits & 1) != 0, paused = (bits & 2) != 0, connected = (bits & 4) != 0;
        bool mapped = (bits & 8) != 0, requested = (bits & 16) != 0;
        assert(input_classic_encryption_current(selected, paused, connected, mapped, requested, 7, 7) ==
               (selected && !paused && connected && mapped && requested));
        assert(!input_classic_encryption_current(selected, paused, connected, mapped, requested, 8, 7));
    }
    puts("Classic encryption bridge: actual encrypted flag, live ACL, sync/async lifetime, allocation/post failures and stale-generation guards passed");
    return 0;
}
