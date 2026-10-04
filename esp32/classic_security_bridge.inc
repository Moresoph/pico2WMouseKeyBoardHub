#include "hub_classic_security.h"
#ifndef HUB_CLASSIC_SECURITY_TEST
#include <string.h>
#include "osi/allocator.h"
#include "osi/thread.h"
#include "stack/btm_api.h"
#include "btm_int.h"
#include "esp_log.h"
#endif

extern osi_thread_t *btu_thread;
typedef struct {
    uint8_t address[6];
    uint32_t generation;
} hub_classic_encrypt_request_t;

static void hub_classic_encrypt_complete(BD_ADDR address, tBT_TRANSPORT transport,
                                          void *context, tBTM_STATUS status) {
    hub_classic_encrypt_request_t *request = context;
    uint8_t flags = 0;
    bool connected = transport == BT_TRANSPORT_BR_EDR &&
        !memcmp(address, request->address, 6) &&
        BTM_IsAclConnectionUp(request->address, BT_TRANSPORT_BR_EDR);
    bool encrypted = connected &&
        BTM_GetSecurityFlagsByTransport(request->address, &flags, BT_TRANSPORT_BR_EDR) &&
        (flags & BTM_SEC_FLAG_ENCRYPTED);
    bool security_failure = status == BTM_NOT_AUTHORIZED || status == BTM_FAILED_ON_SECURITY ||
        status == BTM_REPEATED_ATTEMPTS;
    hub_esp_classic_encryption_result(request->address, request->generation, connected, encrypted,
                                      security_failure, status, flags);
    osi_free(request);
}

static void hub_classic_encrypt_on_btu(void *context) {
    hub_classic_encrypt_request_t *request = context;
    /* Pinned BTM calls completion for synchronous results as well as pending requests. */
    BTM_SetEncryption(request->address, BT_TRANSPORT_BR_EDR, hub_classic_encrypt_complete, request);
}

esp_err_t hub_esp_classic_encrypt(const uint8_t address[6], uint32_t generation) {
    hub_classic_encrypt_request_t *request = osi_malloc(sizeof(*request));
    if (!request) return ESP_ERR_NO_MEM;
    memcpy(request->address, address, 6);
    request->generation = generation;
    if (!osi_thread_post(btu_thread, hub_classic_encrypt_on_btu, request, 0, 0)) {
        osi_free(request);
        return ESP_FAIL;
    }
    return ESP_OK;
}

typedef struct {
    uint8_t address[6];
    uint16_t handle;
    uint32_t generation, token;
} hub_classic_close_request_t;

static void hub_classic_close_on_btu(void *context) {
    hub_classic_close_request_t *request = context;
    hub_cleanup_guard_t guard = hub_esp_classic_recovery_check(request->address, request->handle,
                                                             request->generation, request->token);
    hub_cleanup_result_t result = HUB_CLEANUP_OBSOLETE;
    tBTM_STATUS status = BTM_SUCCESS;
    if (guard == HUB_CLEANUP_DEFERRED) result = HUB_CLEANUP_LOCK_BUSY;
    else if (guard == HUB_CLEANUP_CURRENT) {
        if (!BTM_IsAclConnectionUp(request->address, BT_TRANSPORT_BR_EDR)) result = HUB_CLEANUP_ABSENT;
        else if (BTM_GetHCIConnHandle(request->address, BT_TRANSPORT_BR_EDR) == request->handle) {
            status = btm_remove_acl(request->address, BT_TRANSPORT_BR_EDR);
            result = status == BTM_SUCCESS ? HUB_CLEANUP_SENT : HUB_CLEANUP_FAILED;
        }
    }
    hub_esp_classic_cleanup_result(request->address, request->handle, request->generation,
                                   request->token, result, status);
    osi_free(request);
}

esp_err_t hub_esp_classic_close_acl(const uint8_t address[6], uint16_t handle, uint32_t generation,
                                   uint32_t token) {
    hub_classic_close_request_t *request = osi_malloc(sizeof(*request));
    if (!request) return ESP_ERR_NO_MEM;
    memcpy(request->address, address, 6);
    request->handle = handle;
    request->generation = generation;
    request->token = token;
    if (!osi_thread_post(btu_thread, hub_classic_close_on_btu, request, 0, 0)) {
        osi_free(request);
        return ESP_FAIL;
    }
    return ESP_OK;
}
