#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "hub_web.h"
#include "auto_input_policy.h"

#define strcasecmp _stricmp
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_BD_ADDR_STR "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(a) (a)[0],(a)[1],(a)[2],(a)[3],(a)[4],(a)[5]
enum { ESP_HID_TRANSPORT_BT=0, ESP_HID_TRANSPORT_BLE=1 };
enum { SCAN_NONE, SCAN_CLASSIC, SCAN_BLE };
enum { AUTH_CONFIRM=1, AUTH_PASSKEY, AUTH_PIN, AUTH_NONE=0 };
enum { INPUT_PAIRING_WINDOW_MS=120000, INPUT_RECONNECT_TIMEOUT_MS=40000 };
enum { ESP_BT_INQ_MODE_GENERAL_INQUIRY, BLE_SCAN_TYPE_ACTIVE, BLE_ADDR_TYPE_PUBLIC,
       BLE_SCAN_FILTER_ALLOW_ALL, BLE_SCAN_DUPLICATE_ENABLE, pdTRUE=1 };
typedef struct {
    int scan_type, own_addr_type, scan_filter_policy, scan_interval, scan_window, scan_duplicate;
} esp_ble_scan_params_t;
typedef struct {
    bool selected, paused, mapped, secure, pending, recovering, open_running, classic_acl_connected, allow_open, bonded;
    void *dev;
    uint8_t transport, address_type, address[6], auth, reconnect_failures;
    uint32_t acl_generation, pair_until;
    char name[256];
    struct {uint8_t epoch;} input;
    struct {uint8_t roles;} hid;
} peer_t;
static peer_t peers[2];
static hub_input_records_t saved_inputs;
static bool records_fault,scanning,scan_ble_pending,scan_cancelled,record_names_dirty;
static bool auto_enrollment,auto_scan_complete,auto_committed;
static uint8_t auto_stage;
static uint32_t auto_retry_at;
static uint8_t requested_scan,active_scan,candidate_count,pc_attempts;
static uint32_t scan_requested_at,scan_started_at,management_until,led_ack_until,clock_ms,nonce=1,session=2;
static uint16_t pc_sequence;
static char management_token[17],operation[192];
static hub_web_candidate_t candidates[16];
static hub_link_output_cache_t pico_output;
static hub_link_frame_t pc_request;
static hub_web_command_t deferred_delete;
static bool deletion_pending,stop_scan_immediately=true;
static uint32_t delete_requested_at;
typedef struct {
    uint8_t phase,kind,role,record;
    bool abort,was_bonded;
    hub_saved_input_t target;
} input_trial_t;
static input_trial_t trial;
static unsigned states,scans,removes,errors;
static uint8_t removed[6],flash[HUB_INPUT_RECORD_BYTES],staged[HUB_INPUT_RECORD_BYTES];
static bool fail_write,flash_valid,queued;
static hub_web_command_t queued_command;
static int store,management_commands;

static uint32_t now_ms(void){return clock_ms;}
static bool management_open(void){return management_until && (int32_t)(clock_ms-management_until)<0;}
static uint32_t random_nonzero(void){static uint32_t value=10;return ++value;}
static void lock(void){}
static void unlock(void){}
static void check_result(esp_err_t error,const char *what){(void)what;if(error!=ESP_OK)++errors;}
static bool pairing_allowed(const peer_t *p){return management_open() && p->pending && !p->paused && p->pair_until>clock_ms;}
static bool old_address(const uint8_t *a){return a[0]==1 || a[0]==2;}
static void update_saved_name(const uint8_t address[6],const char *name);
static bool classic_bonded(const uint8_t *a){return old_address(a);}
static bool ble_bonded(const uint8_t *a){return old_address(a);}
static void queue_state(void){++states;}
static void reject_auth(peer_t *p){p->auth=AUTH_NONE;}
static void recover_peer(peer_t *p,const char *reason){
    (void)reason;
    p->mapped=p->secure=false;p->recovering=true;
    if(!p->dev && !p->open_running && !p->classic_acl_connected)p->recovering=false;
}
static esp_err_t nvs_set_blob(int s,const char *key,const void *data,size_t length){
    (void)s;assert(!strcmp(key,"records5") && length==sizeof(staged));
    if(fail_write)return ESP_FAIL;
    memcpy(staged,data,length);return ESP_OK;
}
static esp_err_t nvs_commit(int s){(void)s;memcpy(flash,staged,sizeof(flash));flash_valid=true;return ESP_OK;}
static esp_err_t nvs_get_blob(int s,const char *key,void *data,size_t *length){
    (void)s;assert(!strcmp(key,"records5") && *length==sizeof(flash));
    if(!flash_valid)return ESP_ERR_NOT_FOUND;
    memcpy(data,flash,sizeof(flash));return ESP_OK;
}
static esp_err_t esp_bt_gap_cancel_discovery(void){if(stop_scan_immediately)scanning=false;return ESP_OK;}
static esp_err_t esp_ble_gap_stop_scanning(void){if(stop_scan_immediately)scanning=false;return ESP_OK;}
static esp_err_t esp_bt_gap_start_discovery(int mode,int duration,int count){
    (void)mode;(void)duration;(void)count;++scans;return ESP_OK;
}
static esp_err_t esp_ble_gap_set_scan_params(const esp_ble_scan_params_t *params){
    assert(params->scan_window==0x30);++scans;return ESP_OK;
}
static esp_err_t esp_bt_gap_remove_bond_device(uint8_t *a){memcpy(removed,a,6);++removes;return ESP_OK;}
static esp_err_t esp_ble_remove_bond_device(uint8_t *a){return esp_bt_gap_remove_bond_device(a);}
static esp_err_t esp_bt_gap_ssp_confirm_reply(uint8_t *a,bool yes){(void)a;(void)yes;return ESP_OK;}
static esp_err_t esp_ble_confirm_reply(uint8_t *a,bool yes){return esp_bt_gap_ssp_confirm_reply(a,yes);}
static esp_err_t esp_bt_gap_ssp_passkey_reply(uint8_t *a,bool yes,uint32_t value){(void)value;return esp_bt_gap_ssp_confirm_reply(a,yes);}
static esp_err_t esp_ble_passkey_reply(uint8_t *a,bool yes,uint32_t value){return esp_bt_gap_ssp_passkey_reply(a,yes,value);}
const char *esp_err_to_name(esp_err_t error){return error==ESP_OK?"ESP_OK":"TEST_ERROR";}
static int xQueueSend(int q,const void *command,int wait){
    (void)q;(void)wait;if(queued)return 0;
    queued_command=*(const hub_web_command_t *)command;queued=true;return pdTRUE;
}
static int xQueueReceive(int q,void *command,int wait){
    (void)q;(void)wait;if(!queued)return 0;
    *(hub_web_command_t *)command=queued_command;queued=false;return pdTRUE;
}
#include "input_management_generated.inc"

static void reset(void){
    memset(peers,0,sizeof(peers));hub_records_init(&saved_inputs);
    hub_saved_input_t keyboard={.used=true,.role=1,.transport=ESP_HID_TRANSPORT_BT,.address={1,1,1,1,1,1}};
    hub_saved_input_t old_mouse={.used=true,.role=2,.transport=ESP_HID_TRANSPORT_BLE,.type=1,.address={2,2,2,2,2,2}};
    assert(hub_records_insert(&saved_inputs,&keyboard)==0);
    assert(hub_records_insert(&saved_inputs,&old_mouse)==1);
    configure_peer(1,&keyboard,false);configure_peer(2,&old_mouse,false);
    peers[0].mapped=peers[0].secure=true;peers[0].dev=&peers[0];peers[0].hid.roles=1;
    clock_ms=1000;states=scans=removes=errors=0;flash_valid=fail_write=queued=false;
    records_fault=scanning=scan_ble_pending=scan_cancelled=record_names_dirty=deletion_pending=false;
    stop_scan_immediately=true;requested_scan=active_scan=SCAN_NONE;pc_request.type=0;
    memset(&trial,0,sizeof(trial));memset(&pico_output,0,sizeof(pico_output));
    open_management();
    assert(management_open());
    auto_enrollment=false;auto_stage=0;requested_scan=SCAN_NONE;
}
static hub_web_command_t cmd(const char *action){
    hub_web_command_t c={0};snprintf(c.action,sizeof(c.action),"%s",action);
    memcpy(c.token,management_token,sizeof(c.token));return c;
}
static void new_mouse(void){
    uint8_t address[]={3,3,3,3,3,3};const uint8_t name[]="New independent mouse";
    remember_candidate(address,ESP_HID_TRANSPORT_BLE,1,name,sizeof(name)-1,true,HUB_LINK_MOUSE);
}
static void close_old(void){
    peers[1].open_running=peers[1].classic_acl_connected=peers[1].recovering=false;
    peers[1].pending=false;peers[1].dev=NULL;
}
static void admit(void){
    new_mouse();hub_web_command_t c=cmd("connect");c.slot=2;c.candidate=0;
    snprintf(c.address,sizeof(c.address),"03:03:03:03:03:03");
    assert(enqueue_management(&c)==ESP_OK);manage_tick();
    assert(trial.phase==2 && peers[1].address[0]==3);
    assert(saved_inputs.slots[1].address[0]==2 && saved_inputs.active[1]==1 && !removes);
}
int main(void){
    reset();
    peers[1].open_running=peers[1].pending=true;
    hub_web_command_t c=cmd("scan");strcpy(c.transport,"BLE");
    assert(enqueue_management(&c)==ESP_OK);
    manage_tick();
    assert(requested_scan==SCAN_BLE && !scans && !scanning);
    clock_ms+=30000;manage_tick();assert(!scans);
    close_old();manage_tick();assert(scans==1 && scanning && requested_scan==SCAN_NONE);
    scanning=scan_ble_pending=false;active_scan=SCAN_NONE;
    /* An unfamiliar mouse is admitted while an old keyboard/mouse record exists. */
    admit();
    assert(peers[0].dev==&peers[0] && peers[0].secure);
    peers[1].mapped=true;peers[1].hid.roles=2;peers[1].dev=&peers[1];
    manage_tick();assert(trial.phase==2 && !flash_valid);
    peers[1].secure=true;manage_tick();
    assert(!trial.phase && saved_inputs.active[1]==2 && saved_inputs.slots[1].used && saved_inputs.slots[2].address[0]==3);
    assert(flash_valid && !removes && !management_open());
    hub_input_records_t restored;assert(hub_records_decode(&restored,flash));
    assert(restored.active[1]==2 && restored.slots[1].address[0]==2);
    reset();admit();
    peers[1].reconnect_failures=3;manage_tick();assert(trial.abort);
    close_old();manage_tick();
    assert(!trial.phase && peers[1].address[0]==2 && saved_inputs.active[1]==1 && !flash_valid);
    assert(!management_open()); /* Rollback must resume old-target reconnect, not wait behind management. */
    assert(removes==1 && removed[0]==3);
    reset();admit();c=cmd("cancel");assert(enqueue_management(&c)==ESP_OK);manage_tick();
    assert(!trial.phase && peers[1].address[0]==2 && saved_inputs.slots[1].used);
    reset();admit();clock_ms=management_until;manage_tick();
    assert(!trial.phase && peers[1].address[0]==2 && saved_inputs.active[1]==1 && !flash_valid);
    reset();admit();peers[1].mapped=peers[1].secure=true;peers[1].hid.roles=1;
    manage_tick();assert(trial.abort);close_old();manage_tick();assert(peers[1].address[0]==2 && !flash_valid);
    reset();admit();peers[1].mapped=peers[1].secure=true;peers[1].hid.roles=2;fail_write=true;
    manage_tick();assert(records_fault && trial.abort && saved_inputs.slots[1].used && !saved_inputs.slots[2].used);
    close_old();manage_tick();assert(!trial.phase && peers[1].address[0]==2 && removed[0]==3);
    reset();peers[1].open_running=true;assert(scan_inputs(false)==ESP_OK);
    close_management();close_old();manage_tick();assert(!scans && !requested_scan && !trial.phase);
    reset();peers[1].open_running=true;assert(scan_inputs(false)==ESP_OK);
    clock_ms+=50000;manage_tick();assert(!scans && !requested_scan);
    reset();
    for(unsigned i=0;i<16;++i){
        uint8_t a[]={9,8,7,6,5,(uint8_t)i};const uint8_t n[]="Unrelated beacon";
        remember_candidate(a,ESP_HID_TRANSPORT_BLE,0,n,sizeof(n)-1,false,0);
    }
    assert(candidate_count==16);new_mouse();assert(candidate_count==16 && candidates[0].hid && candidates[0].address[0]==3);
    c=cmd("connect");c.slot=2;c.candidate=0;strcpy(c.address,"09:08:07:06:05:00");
    assert(handle_management(&c)==ESP_ERR_INVALID_ARG && !trial.phase);
    reset();c=cmd("scan");strcpy(c.transport,"BLE");strcpy(c.token,"expired");
    assert(enqueue_management(&c)==ESP_ERR_INVALID_STATE && !requested_scan);
    reset();peers[1].dev=&peers[1];peers[1].open_running=true;new_mouse();
    c=cmd("connect");c.slot=2;strcpy(c.address,"03:03:03:03:03:03");
    assert(handle_management(&c)==ESP_OK);manage_tick();assert(trial.phase==1 && peers[1].address[0]==2);
    clock_ms+=30000;close_old();manage_tick();
    assert(trial.phase==2 && peers[1].address[0]==3 && peers[1].pair_until-clock_ms==120000);
    c=cmd("passkey");c.slot=2;strcpy(c.value,"456789");
    peers[1].pending=true;peers[1].auth=AUTH_PASSKEY;
    assert(handle_management(&c)==ESP_OK && peers[1].auth==AUTH_NONE);
    reset();admit();peers[1].mapped=peers[1].secure=true;peers[1].hid.roles=2;
    manage_tick();assert(saved_inputs.active[1]==2);
    open_management();c=cmd("manual");assert(handle_management(&c)==ESP_OK);
    c=cmd("activate");c.slot=2;strcpy(c.address,"02:02:02:02:02:02");
    assert(handle_management(&c)==ESP_OK);manage_tick();
    assert(peers[1].address[0]==2 && trial.phase==2 && !peers[1].pair_until);
    peers[1].mapped=peers[1].secure=true;peers[1].hid.roles=2;manage_tick();
    assert(!trial.phase && saved_inputs.active[1]==1 && saved_inputs.slots[2].used && saved_inputs.slots[2].address[0]==3);
    assert(!strcmp(saved_inputs.slots[2].name,"New independent mouse"));
    /* Long-press's production entry automatically scans both transports, then admits a new mouse. */
    reset();open_management();
    assert(auto_enrollment && requested_scan==SCAN_BLE);
    manage_tick();assert(scanning && scans==1);
    new_mouse();
    uint8_t pico[]={8,8,8,8,8,8};const uint8_t pico_name[]="Pico2W Keyboard Mouse Hub";
    remember_candidate(pico,ESP_HID_TRANSPORT_BLE,0,pico_name,sizeof(pico_name)-1,true,3);
    assert(candidate_count==1);
    scanning=scan_ble_pending=false;active_scan=SCAN_NONE;auto_scan_complete=true;
    manage_tick();assert(auto_stage==2 && scanning && scans==2 && candidate_count==1 && !trial.phase);
    scanning=false;active_scan=SCAN_NONE;auto_scan_complete=true;
    manage_tick();assert(trial.phase==2 && peers[1].address[0]==3 && auto_enrollment);
    assert(saved_inputs.active[1]==1 && !flash_valid);
    peers[1].mapped=peers[1].secure=true;peers[1].hid.roles=2;
    manage_tick();assert(saved_inputs.active[1]==2 && saved_inputs.slots[1].used);
    assert(!strcmp(saved_inputs.slots[2].name,"New independent mouse"));
    manage_tick();assert(!management_open() && !auto_enrollment);
    /* Deletion can be confirmed directly while automatic discovery is waiting/running. */
    reset();open_management();
    c=cmd("delete");c.slot=2;strcpy(c.address,"02:02:02:02:02:02");
    assert(handle_management(&c)==ESP_OK && deletion_pending && !requested_scan && !auto_enrollment);
    manage_tick();
    assert(!deletion_pending && !trial.phase && !saved_inputs.slots[1].used && !peers[1].selected);
    assert(saved_inputs.slots[0].used && peers[0].mapped && removes==1 && removed[0]==2);
    reset();open_management();manage_tick();
    assert(scanning && scan_ble_pending);
    scan_ble_pending=false;stop_scan_immediately=false;
    c=cmd("delete");c.slot=2;strcpy(c.address,"02:02:02:02:02:02");
    assert(handle_management(&c)==ESP_OK && deletion_pending && scanning && scan_cancelled);
    manage_tick();assert(deletion_pending && saved_inputs.slots[1].used && !removes);
    clock_ms+=5999;manage_tick();assert(deletion_pending && saved_inputs.slots[1].used);
    scanning=false;active_scan=SCAN_NONE;manage_tick();
    assert(!deletion_pending && !saved_inputs.slots[1].used && saved_inputs.slots[0].used);
    reset();open_management();manage_tick();scan_ble_pending=false;stop_scan_immediately=false;
    c=cmd("delete");c.slot=2;strcpy(c.address,"02:02:02:02:02:02");assert(handle_management(&c)==ESP_OK);
    clock_ms+=6000;manage_tick();
    assert(!deletion_pending && saved_inputs.slots[1].used && !removes && !flash_valid);
    reset();open_management();manage_tick();scan_ble_pending=false;stop_scan_immediately=false;
    c=cmd("delete");c.slot=2;strcpy(c.address,"02:02:02:02:02:02");assert(handle_management(&c)==ESP_OK);
    c=cmd("cancel");assert(handle_management(&c)==ESP_OK);scanning=false;manage_tick();
    assert(!deletion_pending && saved_inputs.slots[1].used && !removes);
    reset();open_management();manage_tick();scan_ble_pending=false;stop_scan_immediately=false;
    c=cmd("delete");c.slot=2;strcpy(c.address,"02:02:02:02:02:02");assert(handle_management(&c)==ESP_OK);
    saved_inputs.slots[1].address[0]=9;scanning=false;manage_tick();
    assert(!deletion_pending && saved_inputs.slots[1].used && !removes);
    /* Multiple fresh mice never pick by order/RSSI; generic hubs/known peers are excluded. */
    reset();open_management();requested_scan=SCAN_NONE;auto_stage=3;new_mouse();
    uint8_t second_mouse[]={4,4,4,4,4,4};const uint8_t mouse_name[]="Second new mouse";
    remember_candidate(second_mouse,ESP_HID_TRANSPORT_BLE,1,mouse_name,sizeof(mouse_name)-1,true,2);
    manage_tick();assert(!auto_enrollment && !trial.phase && saved_inputs.active[1]==1);
    reset();open_management();c=cmd("manual");
    assert(enqueue_management(&c)==ESP_OK);manage_tick();
    assert(management_open() && !auto_enrollment && !requested_scan && !trial.phase);
    /* Names learned for old records are persisted without selecting or reconnecting the device. */
    const uint8_t known[]={2,2,2,2,2,2};const uint8_t known_name[]="Old saved mouse";
    remember_candidate(known,ESP_HID_TRANSPORT_BLE,1,known_name,sizeof(known_name)-1,true,2);
    assert(record_names_dirty);
    manage_tick();assert(!record_names_dirty && !strcmp(saved_inputs.slots[1].name,"Old saved mouse"));
    assert(hub_records_decode(&restored,flash) && !strcmp(restored.slots[1].name,"Old saved mouse"));
    c=cmd("rename");c.slot=2;strcpy(c.address,"02:02:02:02:02:02");strcpy(c.name,"Desk mouse");
    assert(handle_management(&c)==ESP_OK && !strcmp(saved_inputs.slots[1].name,"Desk mouse"));
    assert(!strcmp(peers[1].name,"Desk mouse"));
    c.address[0]='9';assert(handle_management(&c)==ESP_ERR_INVALID_ARG);
    reset();open_management();requested_scan=SCAN_NONE;auto_stage=3;
    const uint8_t saved_mouse_name[]="Previously enrolled";
    remember_candidate(known,ESP_HID_TRANSPORT_BLE,1,saved_mouse_name,sizeof(saved_mouse_name)-1,true,2);
    uint8_t generic[]={5,5,5,5,5,5};const uint8_t generic_name[]="Generic HID";
    remember_candidate(generic,ESP_HID_TRANSPORT_BLE,0,generic_name,sizeof(generic_name)-1,true,0);
    manage_tick();assert(!trial.phase && requested_scan==SCAN_NONE && scanning);
    close_management();scanning=scan_ble_pending=false;active_scan=SCAN_NONE;
    /* A mouse and keyboard can be uniquely admitted in the same physical window. */
    reset();open_management();requested_scan=SCAN_NONE;auto_stage=3;new_mouse();
    uint8_t new_keyboard[]={6,6,6,6,6,6};const uint8_t keyboard_name[]="New keyboard";
    remember_candidate(new_keyboard,ESP_HID_TRANSPORT_BT,0,keyboard_name,sizeof(keyboard_name)-1,true,1);
    manage_tick();assert(trial.phase==2 && trial.role==2);
    peers[1].mapped=peers[1].secure=true;peers[1].hid.roles=2;manage_tick();
    assert(auto_enrollment && saved_inputs.active[1]==2);
    peers[0].dev=NULL;peers[0].mapped=peers[0].secure=false;
    manage_tick();assert(trial.phase==2 && trial.role==1 && peers[0].address[0]==6);
    peers[0].mapped=peers[0].secure=true;peers[0].hid.roles=1;manage_tick();
    assert(saved_inputs.active[0]==3 && saved_inputs.slots[0].used && saved_inputs.slots[1].used);
    assert(!strcmp(saved_inputs.slots[3].name,"New keyboard"));
    manage_tick();assert(!management_open() && !auto_enrollment);
    puts("Production input enrollment: queued discovery, unfamiliar mouse admission, encrypted-role commit, old-record preservation, cancel/failure/write rollback, stale identity/token, candidate priority and fresh pairing deadline passed");
    return 0;
}
