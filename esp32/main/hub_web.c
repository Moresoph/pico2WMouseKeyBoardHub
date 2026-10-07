#include "hub_web.h"
#include "ap_password_policy.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_hid_common.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"

static const char *TAG = "hub-web";
static char ssid[32], password[64];
static httpd_handle_t server;
static SemaphoreHandle_t control;
static esp_err_t initialization_error = ESP_ERR_INVALID_STATE;
static bool running;
static bool (*read_snapshot)(hub_web_snapshot_t *);
static esp_err_t (*apply_command)(const hub_web_command_t *);
extern const uint8_t index_start[] asm("_binary_index_html_start");
extern const uint8_t index_end[] asm("_binary_index_html_end");
extern const uint8_t css_start[] asm("_binary_style_css_start");
extern const uint8_t css_end[] asm("_binary_style_css_end");
extern const uint8_t js_start[] asm("_binary_app_js_start");
extern const uint8_t js_end[] asm("_binary_app_js_end");

static void response_headers(httpd_req_t *request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
}

static esp_err_t asset(httpd_req_t *request) {
    response_headers(request);
    const uint8_t *start, *end;
    if (!strncmp(request->uri, "/app.js", 7)) {
        start = js_start; end = js_end;
        httpd_resp_set_type(request, "application/javascript; charset=utf-8");
    } else if (!strncmp(request->uri, "/style.css", 10)) {
        start = css_start; end = css_end;
        httpd_resp_set_type(request, "text/css; charset=utf-8");
    } else {
        start = index_start; end = index_end;
        httpd_resp_set_type(request, "text/html; charset=utf-8");
    }
    return httpd_resp_send(request, (const char *)start, end - start);
}

static void address_string(const uint8_t *address, char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
        address[0], address[1], address[2], address[3], address[4], address[5]);
}

#define JSON_ADD(expression) do { if (!(expression)) goto oom; } while (0)
static esp_err_t unavailable(httpd_req_t *request, const char *message) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_api(httpd_req_t *request) {
    response_headers(request);
    hub_web_snapshot_t snapshot;
    if (!read_snapshot(&snapshot)) {
        ESP_LOGW(TAG, "status snapshot busy; returning 503 instead of stale success");
        return unavailable(request, "Status busy; retry");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) goto oom;
    JSON_ADD(cJSON_AddNumberToObject(root, "schema", 1));
    JSON_ADD(cJSON_AddStringToObject(root, "firmware", HUB_WEB_FIRMWARE));
    JSON_ADD(cJSON_AddNumberToObject(root, "uptime_ms", snapshot.uptime_ms));
    JSON_ADD(cJSON_AddStringToObject(root, "ssid", ssid));
    JSON_ADD(cJSON_AddStringToObject(root, "ip", "192.168.4.1"));
    wifi_sta_list_t stations;
    esp_err_t wifi_error = esp_wifi_ap_get_sta_list(&stations);
    if (wifi_error != ESP_OK) {
        ESP_LOGE(TAG, "read AP clients failed: %s", esp_err_to_name(wifi_error));
        cJSON_Delete(root);
        return unavailable(request, "Wi-Fi status unavailable");
    }
    JSON_ADD(cJSON_AddNumberToObject(root, "clients", stations.num));
    JSON_ADD(cJSON_AddBoolToObject(root, "uart_online", snapshot.uart_online));
    JSON_ADD(cJSON_AddNumberToObject(root, "uart_packets", snapshot.packets));
    JSON_ADD(cJSON_AddNumberToObject(root, "uart_overflows", snapshot.overflows));
    cJSON *events=cJSON_AddArrayToObject(root,"input_events");JSON_ADD(events);
    for(unsigned i=0;i<snapshot.event_count;++i){
        const input_event_t *event=&snapshot.events[i];cJSON *entry=cJSON_CreateObject();
        if(!entry||!cJSON_AddItemToArray(events,entry)){cJSON_Delete(entry);goto oom;}
        JSON_ADD(cJSON_AddNumberToObject(entry,"at_ms",event->at));
        JSON_ADD(cJSON_AddNumberToObject(entry,"age_ms",snapshot.uptime_ms-event->at));
        JSON_ADD(cJSON_AddNumberToObject(entry,"slot",event->slot));
        JSON_ADD(cJSON_AddStringToObject(entry,"phase",input_link_phase_name(event->phase)));
        JSON_ADD(cJSON_AddStringToObject(entry,"event",input_event_name(event->kind)));
        JSON_ADD(cJSON_AddNumberToObject(entry,"detail",event->detail));
    }
    JSON_ADD(cJSON_AddBoolToObject(root, "management_open", snapshot.management_open));
    JSON_ADD(cJSON_AddBoolToObject(root, "auto_enrollment", snapshot.auto_enrollment));
    JSON_ADD(cJSON_AddBoolToObject(root, "deletion_pending", snapshot.deletion_pending));
    JSON_ADD(cJSON_AddBoolToObject(root, "scanning", snapshot.scanning));
    JSON_ADD(cJSON_AddBoolToObject(root, "scan_waiting", snapshot.scan_waiting));
    JSON_ADD(cJSON_AddBoolToObject(root, "trial", snapshot.trial));
    JSON_ADD(cJSON_AddNumberToObject(root, "trial_role", snapshot.trial_role));
    JSON_ADD(cJSON_AddNumberToObject(root, "trial_phase", snapshot.trial_phase));
    char trial_address[18];address_string(snapshot.trial_address,trial_address);
    JSON_ADD(cJSON_AddStringToObject(root,"trial_address",snapshot.trial?trial_address:""));
    JSON_ADD(cJSON_AddBoolToObject(root, "input_storage_fault", snapshot.records_fault));
    JSON_ADD(cJSON_AddNumberToObject(root, "remaining_ms", snapshot.remaining_ms));
    JSON_ADD(cJSON_AddStringToObject(root, "token", snapshot.token));
    JSON_ADD(cJSON_AddStringToObject(root, "operation", snapshot.operation));
    JSON_ADD(cJSON_AddNumberToObject(root, "auth", snapshot.auth));
    JSON_ADD(cJSON_AddNumberToObject(root, "auth_slot", snapshot.auth_slot));
    if (snapshot.auth == 1) JSON_ADD(cJSON_AddNumberToObject(root, "auth_number", snapshot.auth_number));
    cJSON *records = cJSON_AddArrayToObject(root, "records");
    JSON_ADD(records);
    for (unsigned i=0;i<HUB_SAVED_INPUTS;++i) {
        const hub_saved_input_t *s=&snapshot.records.slots[i];
        cJSON *e=cJSON_CreateObject();
        if(!e || !cJSON_AddItemToArray(records,e)){cJSON_Delete(e);goto oom;}
        char a[18];address_string(s->address,a);
        JSON_ADD(cJSON_AddNumberToObject(e,"slot",i+1));
        JSON_ADD(cJSON_AddBoolToObject(e,"saved",s->used));
        JSON_ADD(cJSON_AddNumberToObject(e,"role",s->role));
        JSON_ADD(cJSON_AddNumberToObject(e,"order",s->order));
        JSON_ADD(cJSON_AddStringToObject(e,"name",s->name));
        JSON_ADD(cJSON_AddStringToObject(e,"address",s->used?a:""));
        JSON_ADD(cJSON_AddStringToObject(e,"transport",s->transport==ESP_HID_TRANSPORT_BT?"Classic":"BLE"));
        JSON_ADD(cJSON_AddBoolToObject(e,"active",snapshot.records.active[0]==i || snapshot.records.active[1]==i));
    }
    cJSON *candidates=cJSON_AddArrayToObject(root,"candidates");JSON_ADD(candidates);
    if(snapshot.management_open)for(unsigned i=0;i<snapshot.candidate_count;++i){
        const hub_web_candidate_t *s=&snapshot.candidates[i];
        cJSON *e=cJSON_CreateObject();
        if(!e || !cJSON_AddItemToArray(candidates,e)){cJSON_Delete(e);goto oom;}
        char a[18];address_string(s->address,a);
        JSON_ADD(cJSON_AddNumberToObject(e,"id",i));
        JSON_ADD(cJSON_AddStringToObject(e,"name",s->name));
        JSON_ADD(cJSON_AddStringToObject(e,"address",a));
        JSON_ADD(cJSON_AddStringToObject(e,"transport",s->transport==ESP_HID_TRANSPORT_BT?"Classic":"BLE"));
        JSON_ADD(cJSON_AddBoolToObject(e,"hid",s->hid));
        JSON_ADD(cJSON_AddNumberToObject(e,"roles",s->roles));
    }
    cJSON *inputs = cJSON_AddArrayToObject(root, "inputs");
    JSON_ADD(inputs);
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        const hub_web_input_t *input = &snapshot.inputs[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry || !cJSON_AddItemToArray(inputs, entry)) { cJSON_Delete(entry); goto oom; }
        char address[18];
        address_string(input->address, address);
        JSON_ADD(cJSON_AddNumberToObject(entry, "slot", i + 1));
        JSON_ADD(cJSON_AddNumberToObject(entry, "record_slot", input->record_slot));
        JSON_ADD(cJSON_AddStringToObject(entry, "name", input->name));
        JSON_ADD(cJSON_AddStringToObject(entry, "address", input->saved ? address : ""));
        JSON_ADD(cJSON_AddStringToObject(entry, "transport", input->transport == ESP_HID_TRANSPORT_BT ? "Classic" : "BLE"));
        JSON_ADD(cJSON_AddBoolToObject(entry, "saved", input->saved));
        JSON_ADD(cJSON_AddBoolToObject(entry, "bonded", input->bonded));
        JSON_ADD(cJSON_AddBoolToObject(entry, "connected", input->connected));
        JSON_ADD(cJSON_AddBoolToObject(entry, "encrypted", input->encrypted));
        JSON_ADD(cJSON_AddBoolToObject(entry, "hid", input->hid));
        JSON_ADD(cJSON_AddBoolToObject(entry, "ready", input->ready));
        JSON_ADD(cJSON_AddBoolToObject(entry, "paused", input->paused));
        JSON_ADD(cJSON_AddBoolToObject(entry, "pending", input->pending));
        JSON_ADD(cJSON_AddNumberToObject(entry, "roles", input->roles));
        JSON_ADD(cJSON_AddNumberToObject(entry, "reports", input->reports));
        JSON_ADD(cJSON_AddStringToObject(entry,"phase",input_link_phase_name(input->phase)));
        JSON_ADD(cJSON_AddNumberToObject(entry,"phase_age_ms",input->phase_age_ms));
        JSON_ADD(cJSON_AddNumberToObject(entry,"attempt_age_ms",input->attempt_age_ms));
        JSON_ADD(cJSON_AddNumberToObject(entry,"last_error",input->last_error));
        JSON_ADD(cJSON_AddNumberToObject(entry,"pause_reason",input->pause_reason));
        JSON_ADD(cJSON_AddBoolToObject(entry,"cleanup_pending",input->cleanup_pending));
        JSON_ADD(cJSON_AddNumberToObject(entry,"cleanup_result",input->cleanup_result));
        JSON_ADD(cJSON_AddBoolToObject(entry,"report_seen",input->report_seen));
        if(input->report_seen)JSON_ADD(cJSON_AddNumberToObject(entry,"last_report_age_ms",input->last_report_age_ms));
        else JSON_ADD(cJSON_AddNullToObject(entry,"last_report_age_ms"));
        if (input->connected && input->interval)
            JSON_ADD(cJSON_AddNumberToObject(entry, "interval_ms", input->interval * 1.25));
        else JSON_ADD(cJSON_AddNullToObject(entry, "interval_ms"));
    }
    JSON_ADD(cJSON_AddBoolToObject(root, "pico_fresh", snapshot.pico_fresh));
    if (snapshot.pico_received) JSON_ADD(cJSON_AddNumberToObject(root, "pico_age_ms", snapshot.pico_age_ms));
    else JSON_ADD(cJSON_AddNullToObject(root, "pico_age_ms"));
    if (snapshot.pico_fresh) {
        const hub_link_output_t *output = &snapshot.output;
        char version[24];
        snprintf(version, sizeof(version), "%u.%u.%u", output->version[0], output->version[1], output->version[2]);
        JSON_ADD(cJSON_AddStringToObject(root, "pico_firmware", version));
        JSON_ADD(cJSON_AddNumberToObject(root, "selected_pc", output->selected));
        JSON_ADD(cJSON_AddBoolToObject(root, "switching", output->flags & HUB_LINK_OUTPUT_SWITCHING));
        JSON_ADD(cJSON_AddBoolToObject(root, "mouse_buffer", output->flags & HUB_LINK_OUTPUT_MOUSE_BUFFER));
        JSON_ADD(cJSON_AddBoolToObject(root, "storage_fault", output->flags & HUB_LINK_OUTPUT_STORAGE_FAULT));
        cJSON *computers = cJSON_AddArrayToObject(root, "computers");
        JSON_ADD(computers);
        for (unsigned i = 0; i < HUB_LINK_PC_SLOTS; ++i) {
            const hub_link_pc_t *pc = &output->computers[i];
            cJSON *entry = cJSON_CreateObject();
            if (!entry || !cJSON_AddItemToArray(computers, entry)) { cJSON_Delete(entry); goto oom; }
            char address[18];
            address_string(pc->address, address);
            JSON_ADD(cJSON_AddNumberToObject(entry, "slot", i + 1));
            JSON_ADD(cJSON_AddBoolToObject(entry, "saved", pc->flags & HUB_LINK_PC_RESERVED));
            JSON_ADD(cJSON_AddBoolToObject(entry, "connected", pc->flags & HUB_LINK_PC_CONNECTED));
            JSON_ADD(cJSON_AddBoolToObject(entry, "encrypted", pc->flags & HUB_LINK_PC_ENCRYPTED));
            JSON_ADD(cJSON_AddBoolToObject(entry, "ready", pc->flags & HUB_LINK_PC_READY));
            JSON_ADD(cJSON_AddBoolToObject(entry, "suspended", pc->flags & HUB_LINK_PC_SUSPENDED));
            JSON_ADD(cJSON_AddStringToObject(entry, "address", pc->flags & HUB_LINK_PC_RESERVED ? address : ""));
            if (pc->interval) JSON_ADD(cJSON_AddNumberToObject(entry, "interval_ms", pc->interval * 1.25));
            else JSON_ADD(cJSON_AddNullToObject(entry, "interval_ms"));
        }
    } else {
        JSON_ADD(cJSON_AddNullToObject(root, "computers"));
        JSON_ADD(cJSON_AddNullToObject(root, "selected_pc"));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        ESP_LOGE(TAG, "cannot serialize status JSON: out of memory");
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    esp_err_t result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return result;
oom:
    cJSON_Delete(root);
    ESP_LOGE(TAG, "cannot build status JSON: out of memory");
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
}
#undef JSON_ADD
static bool copy_json_string(cJSON *root,const char *name,char *out,size_t size) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(root,name);
    if(!cJSON_IsString(v) || strlen(v->valuestring)>=size)return false;
    snprintf(out,size,"%s",v->valuestring);return true;
}
static esp_err_t command_api(httpd_req_t *req) {
    response_headers(req);
    char type[48];
    if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK ||
       strcmp(type,"application/json") || req->content_len<=0 || req->content_len>384)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected bounded JSON");
    char origin[96],host[80];
    if(httpd_req_get_hdr_value_len(req,"Origin")) {
        char expected[96];
        if(httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK ||
           httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK)
            return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Origin rejected");
        snprintf(expected,sizeof(expected),"http://%s",host);
        if(strcmp(origin,expected))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Origin rejected");
    }
    char body[385];int count=0;
    while(count<req->content_len){
        int n=httpd_req_recv(req,body+count,req->content_len-count);
        if(n<=0)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete request");
        count+=n;
    }
    body[count]=0;
    cJSON *root=cJSON_ParseWithLength(body,count+1);hub_web_command_t command={0};
    bool valid=root && copy_json_string(root,"action",command.action,sizeof(command.action)) &&
        copy_json_string(root,"token",command.token,sizeof(command.token));
    cJSON *v=root?cJSON_GetObjectItemCaseSensitive(root,"slot"):NULL;
    if(v){valid&=cJSON_IsNumber(v)&&v->valuedouble==v->valueint&&v->valueint>=1&&v->valueint<=7;command.slot=v->valueint;}
    v=root?cJSON_GetObjectItemCaseSensitive(root,"candidate"):NULL;
    if(v){valid&=cJSON_IsNumber(v)&&v->valuedouble==v->valueint&&v->valueint>=0&&v->valueint<16;command.candidate=v->valueint;}
    if(root && cJSON_GetObjectItemCaseSensitive(root,"value"))
        valid&=copy_json_string(root,"value",command.value,sizeof(command.value));
    if(root && cJSON_GetObjectItemCaseSensitive(root,"address"))
        valid&=copy_json_string(root,"address",command.address,sizeof(command.address));
    if(root && cJSON_GetObjectItemCaseSensitive(root,"transport"))
        valid&=copy_json_string(root,"transport",command.transport,sizeof(command.transport));
    if(root && cJSON_GetObjectItemCaseSensitive(root,"name"))
        valid&=copy_json_string(root,"name",command.name,sizeof(command.name));
    command.replace=root&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"replace"));
    cJSON_Delete(root);
    if(!valid)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid fields");
    esp_err_t error=apply_command(&command);
    if(error!=ESP_OK) {
        ESP_LOGW(TAG,"management action %s rejected: %s",command.action,esp_err_to_name(error));
        return httpd_resp_send_err(req,error==ESP_ERR_INVALID_STATE?HTTPD_403_FORBIDDEN:HTTPD_400_BAD_REQUEST,esp_err_to_name(error));
    }
    httpd_resp_set_status(req,"202 Accepted");httpd_resp_set_type(req,"application/json");
    return httpd_resp_sendstr(req,"{\"accepted\":true}");
}

static esp_err_t start_locked(void) {
    if (initialization_error != ESP_OK) return initialization_error;
    if (running) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start SoftAP");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = 2;
    config.core_id = 1;
    config.stack_size = 8192;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    esp_err_t error = httpd_start(&server, &config);
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = asset},
        {.uri = "/style.css", .method = HTTP_GET, .handler = asset},
        {.uri = "/app.js", .method = HTTP_GET, .handler = asset},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_api},
        {.uri = "/api/manage", .method = HTTP_POST, .handler = command_api}
    };
    for (unsigned i = 0; error == ESP_OK && i < sizeof(routes) / sizeof(routes[0]); ++i)
        error = httpd_register_uri_handler(server, &routes[i]);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "start HTTP server failed: %s", esp_err_to_name(error));
        if (server) {
            esp_err_t stop = httpd_stop(server);
            if (stop != ESP_OK) ESP_LOGE(TAG, "HTTP cleanup failed: %s", esp_err_to_name(stop));
            server = NULL;
        }
        esp_err_t stop = esp_wifi_stop();
        if (stop != ESP_OK) ESP_LOGE(TAG, "SoftAP cleanup failed: %s", esp_err_to_name(stop));
        return error;
    }
    running = true;
    ESP_LOGI(TAG, "HTTP ready with physically gated management: SSID=%s URL=http://192.168.4.1 (2 clients max)", ssid);
    return ESP_OK;
}

static esp_err_t initialize(void) {
    nvs_handle_t settings;
    ESP_RETURN_ON_ERROR(nvs_open("hub_web", NVS_READWRITE, &settings), TAG, "open AP settings");
    size_t length = sizeof(password);
    esp_err_t error = nvs_get_str(settings, "ap_password", password, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (unsigned i = 0; i < sizeof(random); ++i) password[i] = alphabet[random[i] & 31];
        password[16] = 0;
        error = nvs_set_str(settings, "ap_password", password);
        if (error == ESP_OK) error = nvs_commit(settings);
    }
    nvs_close(settings);
    ESP_RETURN_ON_ERROR(error, TAG, "read/persist AP password");
    if (strlen(password) < 8 || strlen(password) > 63) {
        ESP_LOGE(TAG, "invalid saved AP password; refusing open-network fallback");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "read AP identity");
    snprintf(ssid, sizeof(ssid), "PicoHub-%02X%02X", mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create network event loop");
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (!netif) return ESP_ERR_NO_MEM;
    esp_err_t dhcp = esp_netif_dhcps_stop(netif);
    if (dhcp != ESP_OK && dhcp != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "stop DHCP before static AP address failed: %s", esp_err_to_name(dhcp));
        return dhcp;
    }
    esp_netif_ip_info_t ip;
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    ip.gw = ip.ip;
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip), TAG, "set fixed AP address");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(netif), TAG, "start AP DHCP");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "keep Wi-Fi config out of default NVS");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "select AP-only mode");
    wifi_config_t config = {0};
    memcpy(config.ap.ssid, ssid, strlen(ssid));
    config.ap.ssid_len = strlen(ssid);
    memcpy(config.ap.password, password, strlen(password));
    config.ap.channel = 6;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 2;
    config.ap.beacon_interval = 100;
    config.ap.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "configure protected hotspot");
    return esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
}

esp_err_t hub_web_init(bool (*snapshot)(hub_web_snapshot_t *),esp_err_t (*command)(const hub_web_command_t *)) {
    read_snapshot = snapshot;
    apply_command = command;
    control = xSemaphoreCreateMutex();
    if (!control) return ESP_ERR_NO_MEM;
    initialization_error = initialize();
    if (initialization_error != ESP_OK) return initialization_error;
    ESP_LOGI(TAG, "Wi-Fi/HTTP initialized but OFF at boot; double-click BOOT to enable");
    return ESP_OK;
}
bool hub_web_enabled(void){return running;}
static esp_err_t stop_locked(void) {
    if (!running) return ESP_OK;
    if (server) {
        esp_err_t error = httpd_stop(server);
        if (error != ESP_OK) return error;
        server = NULL;
    }
    esp_err_t error = esp_wifi_stop();
    if (error == ESP_OK) running = false;
    return error;
}

static esp_err_t change_password_locked(const char *value) {
    if (initialization_error != ESP_OK) return initialization_error;
    if (!hub_ap_password_valid(value)) return ESP_ERR_INVALID_ARG;
    if (!strcmp(value, password)) return ESP_OK;
    nvs_handle_t settings;
    ESP_RETURN_ON_ERROR(nvs_open("hub_web", NVS_READWRITE, &settings), TAG, "open AP password settings");
    esp_err_t error = nvs_set_str(settings, "ap_password", value);
    if (error == ESP_OK) error = nvs_commit(settings);
    char verified[64] = {0};
    size_t length = sizeof(verified);
    if (error == ESP_OK) error = nvs_get_str(settings, "ap_password", verified, &length);
    if (error == ESP_OK && (length != strlen(value) + 1 || strcmp(verified, value)))
        error = ESP_ERR_INVALID_RESPONSE;
    nvs_close(settings);
    ESP_RETURN_ON_ERROR(error, TAG, "save/verify AP password");
    memcpy(password, verified, sizeof(password));
    bool resume = running;
    ESP_RETURN_ON_ERROR(stop_locked(), TAG, "password saved; stopping old hotspot failed");
    wifi_config_t config;
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_AP, &config), TAG, "password saved; read AP config failed");
    memset(config.ap.password, 0, sizeof(config.ap.password));
    memcpy(config.ap.password, password, strlen(password));
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "password saved; apply AP config failed");
    if (resume) ESP_RETURN_ON_ERROR(start_locked(), TAG, "password saved; restarting hotspot failed");
    ESP_LOGI(TAG, "hotspot password saved and applied; reconnect clients with new password (Bluetooth unchanged)");
    return ESP_OK;
}

esp_err_t hub_web_set_enabled(bool enabled) {
    if(!control)return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(control,portMAX_DELAY);
    esp_err_t error=ESP_OK;
    if(enabled)error=start_locked();
    else error=stop_locked();
    xSemaphoreGive(control);return error;
}

int hub_web_command(int argc, char **argv) {
    if (!control) { ESP_LOGE(TAG, "web initialization unavailable"); return 1; }
    xSemaphoreTake(control, portMAX_DELAY);
    esp_err_t error = ESP_OK;
    if (argc == 2 && !strcmp(argv[1], "on")) {
        ESP_LOGW(TAG, "hotspot enable requires a physical BOOT double-click");
        error = ESP_ERR_INVALID_STATE;
    }
    else if (argc == 2 && !strcmp(argv[1], "off")) error = stop_locked();
    else if (argc == 3 && !strcmp(argv[1], "password")) error = change_password_locked(argv[2]);
    else if (argc != 1) error = ESP_ERR_INVALID_ARG;
    if (error != ESP_OK) ESP_LOGE(TAG, "web command failed: %s", esp_err_to_name(error));
    printf("Web: %s initialization=%s SSID=%s URL=http://192.168.4.1 free-heap=%" PRIu32 "\n",
        running ? "ON" : "OFF", esp_err_to_name(initialization_error), ssid, esp_get_free_heap_size());
    if (initialization_error == ESP_OK) printf("AP password (USB console only): %s\n", password);
    xSemaphoreGive(control);
    return error != ESP_OK;
}
