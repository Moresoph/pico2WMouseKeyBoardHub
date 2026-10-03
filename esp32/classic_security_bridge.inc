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
    uint32_t generation;
} hub_classic_close_request_t;

static void hub_classic_close_on_btu(void *context) {
    hub_classic_close_request_t *request = context;
    if (hub_esp_classic_recovery_current(request->address, request->handle, request->generation) &&
        BTM_IsAclConnectionUp(request->address, BT_TRANSPORT_BR_EDR) &&
        BTM_GetHCIConnHandle(request->address, BT_TRANSPORT_BR_EDR) == request->handle) {
        tBTM_STATUS status = btm_remove_acl(request->address, BT_TRANSPORT_BR_EDR);
        if (status != BTM_SUCCESS) ESP_LOGW("classic-recovery", "ACL cleanup status=%u", status);
    }
    osi_free(request);
}

esp_err_t hub_esp_classic_close_acl(const uint8_t address[6], uint16_t handle, uint32_t generation) {
    hub_classic_close_request_t *request = osi_malloc(sizeof(*request));
    if (!request) return ESP_ERR_NO_MEM;
    memcpy(request->address, address, 6);
    request->handle = handle;
    request->generation = generation;
    if (!osi_thread_post(btu_thread, hub_classic_close_on_btu, request, 0, 0)) {
        osi_free(request);
        return ESP_FAIL;
    }
    return ESP_OK;
}
