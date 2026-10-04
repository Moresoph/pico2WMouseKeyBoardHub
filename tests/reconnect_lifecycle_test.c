#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "hub_input_records.h"
#include "hub_hid_report.h"
#include "hub_remote_hid.h"
#include "descriptor_retry.h"
#include "hub_link.h"
#include "hub_classic_security.h"
#include "reconnect_policy.h"
#include "input_diagnostics.h"

#define TAG "test"
#define ESP_LOGI(tag,...) ((void)log_event(__VA_ARGS__))
#define ESP_LOGW(tag,...) ((void)log_event(__VA_ARGS__))
#define ESP_LOGE(tag,...) ((void)log_event(__VA_ARGS__))
static void log_event(const char *format,...){(void)format;}
enum { ESP_HID_TRANSPORT_BT, ESP_HID_TRANSPORT_BLE };
typedef enum { PAUSE_NONE,PAUSE_OPERATOR,PAUSE_SECURITY,PAUSE_INVALID_HID,PAUSE_CLEANUP_TIMEOUT } pause_reason_t;
typedef struct {
    bool selected,paused,pending,mapped,secure,pin_replied,interval_pending,encryption_pending;
    bool classic_acl_connected,classic_acl_encrypted,classic_incoming,open_running,recovering,profile_requested;
    bool cleanup_pending,report_seen,allow_open;
    uint8_t address[6],seen_roles,auth,interval_attempts,reconnect_failures,profile_failures,transport;
    uint16_t interval,classic_handle;
    uint32_t reports,retry_at,recovery_at,opened_at,pair_until,last_error;
    uint32_t last_report_at,invalid;
    uint32_t acl_generation,cleanup_token,cleanup_request_at,cleanup_retry_at;
    uint8_t cleanup_result;
    void *dev;
    char name[256];
    hub_remote_hid_t hid;
    input_descriptor_retry_t descriptor_retry;
    pause_reason_t pause_reason;
    input_link_clock_t clock;
    hub_link_input_t input;
} peer_t;
typedef struct {
    uint8_t address[6],status;
    uint16_t handle;
    uint32_t generation,token;
    hub_cleanup_result_t result;
    esp_err_t submission_error;
} cleanup_event_t;
static peer_t peers[2];
static uint32_t time_ms,cleanup_token;
static bool scanning,bonded=true;
static unsigned requested_scan,closes,submissions,states,btu_posts;
static bool lock_owned=true,fail_queue;
static esp_err_t post_error;
enum { cleanup_submissions=1,cleanup_events,pdTRUE=1 };
static cleanup_event_t requests[8],completion;
static unsigned request_count;
#define configASSERT assert
static struct {unsigned phase;} trial;
static input_history_t history;
static cleanup_event_t reply;
typedef void esp_hidh_dev_t;
typedef const char *esp_event_base_t;
enum { ESP_HIDH_START_EVENT,ESP_HIDH_OPEN_EVENT,ESP_HIDH_CLOSE_EVENT,ESP_HIDH_INPUT_EVENT };
typedef struct {
    struct {esp_err_t status;} start;
    struct {esp_err_t status;esp_hidh_dev_t *dev;} open;
    struct {int reason;esp_hidh_dev_t *dev;} close;
    struct {unsigned map_index,report_id,length;const uint8_t *data;esp_hidh_dev_t *dev;} input;
} esp_hidh_event_data_t;
static bool classic_ready;
static void lock(void){lock_owned=true;}
static void unlock(void){lock_owned=false;}
static const uint8_t *esp_hidh_dev_bda_get(esp_hidh_dev_t *dev){(void)dev;return peers[0].address;}
static unsigned esp_hidh_dev_transport_get(esp_hidh_dev_t *dev){(void)dev;return peers[0].transport;}
static const char *esp_hidh_dev_name_get(esp_hidh_dev_t *dev){(void)dev;return NULL;}
const char *esp_err_to_name(esp_err_t status){(void)status;return "test-status";}
static bool classic_admitted(const peer_t *p){return p&&p->selected&&!p->paused&&bonded;}
static void begin_classic_incoming(peer_t *p){p->pending=true;input_link_enter(&p->clock,INPUT_PHASE_HID,time_ms);}
static void update_saved_name(const uint8_t *address,const char *name){(void)address;(void)name;}
static void request_mouse_interval(peer_t *p){(void)p;}
static esp_err_t esp_ble_set_encryption(uint8_t *address,unsigned mode){(void)address;(void)mode;return ESP_OK;}
enum { ESP_BLE_SEC_ENCRYPT=1 };
#define ESP_BD_ADDR_STR "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(a) (a)[0],(a)[1],(a)[2],(a)[3],(a)[4],(a)[5]
typedef struct {const uint8_t *data;uint16_t len;} esp_hid_raw_report_map_t;
static esp_hid_raw_report_map_t sdk_maps[HUB_REMOTE_MAPS];
static size_t sdk_count;
static esp_err_t sdk_map_status;
static bool sdk_null_maps;
static esp_err_t esp_hidh_dev_report_maps_get(esp_hidh_dev_t *dev,size_t *count,esp_hid_raw_report_map_t **maps){
    assert(dev);*count=sdk_count;*maps=sdk_null_maps?NULL:sdk_maps;return sdk_map_status;
}

static uint32_t now_ms(void){return time_ms;}
static bool due(uint32_t now,uint32_t at){return (int32_t)(now-at)>=0;}
static void check_result(esp_err_t error,const char *operation){(void)error;(void)operation;}
static void queue_state(void){++states;}
static void input_phase(peer_t *p,input_link_phase_t phase){input_link_enter(&p->clock,phase,time_ms);}
static void note_input(peer_t *p,input_event_kind_t kind,uint32_t detail){
    input_event_t event={.at=time_ms,.slot=(uint8_t)(p-peers)+1,.phase=p->clock.phase,.kind=kind,.detail=detail};
    input_history_add(&history,&event);
}
static bool forwarding_allowed(const peer_t *p){return p->secure||p->allow_open;}
static bool pairing_window_open(const peer_t *p){return p->pair_until && !due(time_ms,p->pair_until);}
static bool classic_bonded(const uint8_t *address){(void)address;return bonded;}
static bool ble_bonded(const uint8_t *address){(void)address;return bonded;}
static void reject_auth(peer_t *p){p->auth=0;}
static peer_t *peer_for(const uint8_t *address,unsigned transport){
    for(unsigned i=0;i<2;++i)if(peers[i].selected && peers[i].transport==transport &&
        !memcmp(address,peers[i].address,6))return &peers[i];
    return NULL;
}
static esp_err_t esp_hidh_dev_close(void *dev){assert(dev);++closes;return ESP_OK;}
esp_err_t hub_esp_classic_close_acl(const uint8_t address[6],uint16_t handle,uint32_t generation,uint32_t token){
    (void)address;(void)handle;(void)generation;(void)token;
    assert(!lock_owned);++btu_posts;return post_error;
}
static int xQueueSend(unsigned queue,const void *data,unsigned ticks){
    (void)ticks;const cleanup_event_t *event=data;
    if(queue==cleanup_events){completion=*event;return pdTRUE;}
    assert(queue==cleanup_submissions && lock_owned);
    if(fail_queue)return 0;
    assert(request_count<8);requests[request_count++]=*event;
    reply=*event;++submissions;return pdTRUE;
}
static int xQueueReceive(unsigned queue,void *data,unsigned ticks){
    (void)ticks;assert(queue==cleanup_submissions);
    if(!request_count)return 0;
    *(cleanup_event_t *)data=requests[0];
    memmove(requests,requests+1,--request_count*sizeof(requests[0]));return pdTRUE;
}
#include "reconnect_lifecycle_generated.inc"

static peer_t *fresh(void){
    for(unsigned i=0;i<2;++i)input_descriptor_reset(&peers[i].descriptor_retry);
    memset(peers,0,sizeof(peers));memset(&history,0,sizeof(history));
    memset(sdk_maps,0,sizeof(sdk_maps));sdk_count=0;sdk_map_status=ESP_OK;sdk_null_maps=false;
    time_ms=1000;closes=submissions=states=btu_posts=request_count=0;
    scanning=fail_queue=false;lock_owned=true;post_error=ESP_OK;requested_scan=0;trial.phase=0;bonded=true;
    peer_t *p=&peers[0];p->selected=true;p->address[0]=1;p->transport=ESP_HID_TRANSPORT_BT;
    return p;
}
static void ready(peer_t *p){
    input_phase(p,INPUT_PHASE_PAGE);p->pending=p->profile_requested=true;
    input_acl_up(p,0x80);p->dev=p;p->mapped=p->secure=p->classic_acl_encrypted=true;
    maintain_peer(p);assert(p->clock.phase==INPUT_PHASE_READY&&!p->pending);
}
int main(void){
    peer_t *p=fresh();ready(p);p->profile_requested=true;
    time_ms+=10;input_hid_closed(p);
    assert(!p->dev&&!p->mapped&&!p->profile_requested&&p->classic_acl_connected&&p->pending&&p->recovering);
    input_reconnect_view_t view={.selected=true,.classic=true,.pending=p->pending,.acl=true,
        .recovering=p->recovering,.profile_requested=p->profile_requested,.started_at=p->opened_at};
    assert(input_reconnect_plan(&view,time_ms+999,false)==INPUT_WAIT);
    assert(input_reconnect_plan(&view,time_ms+1000,false)==INPUT_WAIT);
    assert(input_reconnect_plan(&view,time_ms+40000,false)==INPUT_WAIT);
    assert(input_acl_down(p,0x80,8)&&!p->pending&&!p->recovering&&p->clock.phase==INPUT_PHASE_IDLE);
    p=fresh();ready(p);assert(input_acl_down(p,0x80,8));
    assert(p->recovering && p->dev && p->clock.phase==INPUT_PHASE_CLEANUP);
    input_hid_closed(p);assert(!p->recovering&&!p->pending&&!p->dev);
    p=fresh();p->pending=p->profile_requested=true;input_phase(p,INPUT_PHASE_HID);
    input_acl_up(p,0x80);
    input_open_failed(p,ESP_FAIL);
    assert(p->recovering&&!p->paused&&p->cleanup_pending&&p->classic_acl_connected);
    uint32_t backoff=p->retry_at;
    retry_peer(p);assert(p->recovering&&p->retry_at==backoff); /* Cannot begin while ACL owns resources. */
    assert(input_acl_down(p,0x80,0x16)&&!p->recovering&&p->retry_at==time_ms+1000);
    p=fresh();p->open_running=true;input_open_failed(p,ESP_FAIL);
    maintain_peer(p);assert(p->recovering&&p->open_running);
    input_open_finished(p,true);assert(!p->pending&&!p->recovering&&p->retry_at>time_ms);
    p=fresh();ready(p);p->open_running=true;
    input_open_finished(p,false);
    assert(!p->open_running&&p->recovering&&p->dev&&p->classic_acl_connected&&!p->mapped);
    input_hid_closed(p);assert(input_acl_down(p,0x80,0x16)&&!p->recovering);
    p=fresh();ready(p);p->open_running=true;
    input_open_finished(p,true);assert(!p->open_running&&p->mapped&&p->secure&&!p->recovering);
    p=fresh();ready(p);assert(!input_acl_down(p,0x81,8)&&p->mapped&&p->secure);
    uint32_t generation=p->acl_generation;input_acl_up(p,0x81);
    assert(p->acl_generation==generation+1 && !p->secure && p->recovering);
    assert(!input_acl_down(p,0x80,8) && p->classic_acl_connected);
    p=fresh();ready(p);
    recover_peer(p,"test lock boundary");assert(submissions==1&&btu_posts==0);
    lock_owned=false;submit_cleanups();lock_owned=true;assert(btu_posts==1);
    p=fresh();ready(p);post_error=ESP_FAIL;
    recover_peer(p,"test post failure");lock_owned=false;submit_cleanups();lock_owned=true;
    assert(completion.result==HUB_CLEANUP_FAILED&&completion.submission_error==ESP_FAIL);
    handle_cleanup_event(&completion);assert(!p->cleanup_pending&&p->last_error==(uint32_t)ESP_FAIL);
    p=fresh();ready(p);fail_queue=true;recover_peer(p,"test queue full");
    assert(!p->cleanup_pending&&p->last_error==ESP_ERR_NO_MEM&&btu_posts==0);
    p=fresh();ready(p);
    /* Lock contention is explicitly acknowledged and requeued in 100ms, not silently lost for 5s. */
    recover_peer(p,"test");assert(p->cleanup_pending&&submissions==1);
    reply.result=HUB_CLEANUP_LOCK_BUSY;handle_cleanup_event(&reply);
    assert(!p->cleanup_pending&&p->cleanup_result==HUB_CLEANUP_LOCK_BUSY);
    maintain_peer(p);assert(submissions==1);
    time_ms+=100;maintain_peer(p);assert(submissions==2&&p->cleanup_pending);
    cleanup_event_t stale=reply;stale.token--;
    handle_cleanup_event(&stale);assert(p->cleanup_pending);
    reply.result=HUB_CLEANUP_SENT;handle_cleanup_event(&reply);
    assert(!p->cleanup_pending&&p->classic_acl_connected&&p->recovering);
    assert(input_acl_down(p,0x80,0));input_hid_closed(p);assert(!p->recovering);
    /* Lost reply is detected without authorizing stale cleanup tokens. */
    p=fresh();ready(p);recover_peer(p,"test");
    time_ms+=2000;maintain_peer(p);assert(submissions==2&&p->cleanup_pending);
    time_ms=1000+INPUT_CLEANUP_TIMEOUT_MS;maintain_peer(p);
    assert(p->paused && p->pause_reason==PAUSE_CLEANUP_TIMEOUT && !p->recovering && p->dev);
    assert(!p->mapped&&!p->secure&&!p->pending); /* Never free SDK-owned resource to fake recovery. */
    assert(retry_input(1)==ESP_OK && !p->paused&&p->recovering);
    assert(p->clock.phase_at==time_ms && p->pair_until==0);
    p=fresh();p->open_running=p->pending=true;input_phase(p,INPUT_PHASE_PAGE);
    time_ms+=INPUT_RECONNECT_TIMEOUT_MS;maintain_peer(p);assert(p->recovering);
    time_ms+=INPUT_CLEANUP_TIMEOUT_MS;maintain_peer(p);
    assert(p->paused&&p->open_running); /* Missing BLE/SDK callback is bounded by an explicit fault. */
    p=fresh();p->pending=true;input_phase(p,INPUT_PHASE_PAGE);
    time_ms+=39000;input_acl_up(p,0x80);assert(p->clock.attempt_at==1000);
    time_ms+=14000;input_phase(p,INPUT_PHASE_ENCRYPT);
    time_ms=66000;maintain_peer(p);assert(p->recovering); /* Progress cannot extend overall 65s. */
    p=fresh();p->pending=true;input_phase(p,INPUT_PHASE_ENCRYPT);p->pair_until=time_ms+120000;
    time_ms+=119999;maintain_peer(p);assert(!p->recovering);
    ++time_ms;maintain_peer(p);assert(!p->pending); /* No owned resources: release then automatic retry. */
    p=fresh();ready(p);time_ms+=3600000;maintain_peer(p);assert(!p->recovering&&!p->paused);
    /* Normal idle is never a reason to reset an encrypted keyboard. */
    assert(!input_scan_blocks_transport(true,2,2,true));
    assert(input_scan_blocks_transport(false,2,2,true));
    assert(input_scan_blocks_transport(true,1,0,false));
    assert(!input_scan_blocks_transport(false,1,1,true));
    p=fresh();p->reconnect_failures=5;p->retry_at=time_ms+15000;
    assert(retry_input(1)==ESP_OK&&p->retry_at==time_ms&&!p->pair_until&&!p->reconnect_failures);
    p->pause_reason=PAUSE_SECURITY;p->paused=true;assert(retry_input(1)!=ESP_OK&&p->paused);
    p->pause_reason=PAUSE_INVALID_HID;assert(retry_input(1)!=ESP_OK);
    p=fresh();ready(p);p->paused=true;p->pause_reason=PAUSE_SECURITY;recover_peer(p,"authentication failure");
    time_ms+=INPUT_CLEANUP_TIMEOUT_MS;maintain_peer(p);
    assert(p->pause_reason==PAUSE_SECURITY && retry_input(1)!=ESP_OK);
    p=fresh();
    p->pause_reason=PAUSE_NONE;bonded=false;assert(retry_input(1)!=ESP_OK);
    /* Missing metadata, failed reads, null/empty maps never become "unsupported". */
    for(unsigned failure=0;failure<5;++failure){
        p=fresh();p->dev=p;p->pending=true;input_acl_up(p,0x80);
        if(failure==1)sdk_map_status=ESP_FAIL;
        if(failure>=2)sdk_count=1;
        if(failure==2)sdk_null_maps=true;
        if(failure==3)sdk_maps[0].len=1;
        if(failure==4)sdk_maps[0].data=hub_hid_report_descriptor;
        assert(!input_validate_hid(p,p));
        assert(!p->paused&&p->recovering&&!p->mapped&&p->descriptor_retry.failures==0);
        assert(input_acl_down(p,0x80,0x16));input_hid_closed(p);
        assert(!p->recovering&&p->retry_at>time_ms&&!p->paused);
    }
    /* Count identical rejected byte sequences only across completed independent attempts. */
    const uint8_t unsupported_a[]={0xa4}; /* Push is unsupported by the existing bounded parser. */
    const uint8_t unsupported_b[]={0xb4};
    p=fresh();
    for(unsigned attempt=0;attempt<3;++attempt){
        time_ms+=2000;p->dev=p;p->pending=true;input_acl_up(p,(uint16_t)(0x80+attempt));
        sdk_count=1;sdk_maps[0]=(esp_hid_raw_report_map_t){unsupported_a,sizeof(unsupported_a)};
        assert(!input_validate_hid(p,p));
        assert(p->descriptor_retry.failures==attempt+1&&p->paused==(attempt==2)&&p->recovering);
        input_hid_closed(p);assert(p->classic_acl_connected&&p->recovering);
        assert(input_acl_down(p,(uint16_t)(0x80+attempt),0x16));
        assert(!p->recovering&&!p->dev&&!p->classic_acl_connected);
    }
    assert(p->paused&&p->pause_reason==PAUSE_INVALID_HID&&bonded);
    p=fresh();p->dev=p;input_acl_up(p,0x80);sdk_count=1;
    sdk_maps[0]=(esp_hid_raw_report_map_t){unsupported_a,sizeof(unsupported_a)};
    assert(!input_validate_hid(p,p));assert(input_acl_down(p,0x80,0));input_hid_closed(p);
    p->dev=p;input_acl_up(p,0x81);sdk_maps[0].data=unsupported_b;
    assert(!input_validate_hid(p,p)&&p->descriptor_retry.failures==1&&!p->paused);
    assert(input_acl_down(p,0x81,0));input_hid_closed(p);
    p->dev=p;input_acl_up(p,0x82);
    sdk_maps[0]=(esp_hid_raw_report_map_t){hub_hid_report_descriptor,sizeof(hub_hid_report_descriptor)};
    assert(input_validate_hid(p,p)&&p->mapped&&p->hid.roles==3&&!p->descriptor_retry.bytes);
    uint8_t key[8]={0,0,4};
    assert(hub_remote_hid_report(&p->hid,0,1,key,sizeof(key),&p->input)&&p->input.keyboard[2]==4);
    memset(key,0,sizeof(key));
    assert(hub_remote_hid_report(&p->hid,0,1,key,sizeof(key),&p->input)&&!p->input.keyboard[2]);
    assert(input_validate_hid(p,p)); /* Duplicate ready OPEN is not counted as a new attempt. */
    input_hid_closed(p);
    assert(!p->input.keyboard[2]&&p->recovering); /* Host release is part of full connection exit. */
    /* Exercise the real callback, including failed OPEN racing an incoming encrypted ACL. */
    p=fresh();p->pending=p->profile_requested=true;p->open_running=true;input_acl_up(p,0x80);
    esp_hidh_event_data_t event={.open={.status=ESP_FAIL,.dev=p}};
    hid_event(NULL,NULL,ESP_HIDH_OPEN_EVENT,&event);
    assert(p->recovering&&p->classic_acl_connected&&p->open_running&&!p->dev);
    p->open_running=false;assert(input_acl_down(p,0x80,0x16)&&!p->recovering);
    input_acl_up(p,0x81);p->classic_acl_encrypted=true;
    event.open.status=ESP_OK;hid_event(NULL,NULL,ESP_HIDH_OPEN_EVENT,&event);
    assert(!p->paused&&p->recovering&&p->descriptor_retry.failures==0); /* Zero-map SDK OPEN is transient. */
    sdk_count=1;sdk_maps[0]=(esp_hid_raw_report_map_t){hub_hid_report_descriptor,sizeof(hub_hid_report_descriptor)};
    hid_event(NULL,NULL,ESP_HIDH_OPEN_EVENT,&event);
    assert(p->recovering&&!p->mapped&&!p->paused); /* Late/duplicate OPEN cannot resurrect failed attempt. */
    event.close.dev=p;hid_event(NULL,NULL,ESP_HIDH_CLOSE_EVENT,&event);
    assert(input_acl_down(p,0x81,0x16)&&!p->recovering);
    time_ms=p->retry_at;input_acl_up(p,0x82);p->classic_acl_encrypted=true;
    hid_event(NULL,NULL,ESP_HIDH_OPEN_EVENT,&event);maintain_peer(p);
    assert(p->mapped&&p->secure&&!p->pending&&!p->paused);
    event.input.dev=p;event.input.map_index=0;event.input.report_id=1;
    event.input.length=8;event.input.data=key;key[2]=4;
    hid_event(NULL,NULL,ESP_HIDH_INPUT_EVENT,&event);
    assert(p->input.keyboard[2]==4&&p->reports==1);
    key[2]=0;hid_event(NULL,NULL,ESP_HIDH_INPUT_EVENT,&event);
    assert(!p->input.keyboard[2]&&p->reports==2);
    void *other_dev=&peers[1];
    event.close.dev=other_dev;hid_event(NULL,NULL,ESP_HIDH_CLOSE_EVENT,&event);
    assert(p->dev==p&&p->mapped); /* Stale CLOSE must not release the new live device. */
    event.close.dev=p;hid_event(NULL,NULL,ESP_HIDH_CLOSE_EVENT,&event);
    assert(p->recovering&&!p->mapped&&!p->input.keyboard[2]);
    assert(input_acl_down(p,0x82,0x16)&&!p->recovering);
    p=fresh();ready(p);recover_peer(p,"test late complete");
    time_ms+=INPUT_CLEANUP_TIMEOUT_MS;maintain_peer(p);assert(p->paused);
    input_hid_closed(p);assert(p->recovering&&p->paused&&p->classic_acl_connected);
    assert(input_acl_down(p,0x80,0x16));
    assert(!p->paused&&!p->recovering&&p->pause_reason==PAUSE_NONE);
    /* Bounded diagnostics roll over and contain metadata only. */
    memset(&history,0,sizeof(history));
    for(unsigned i=0;i<30;++i){time_ms=i;note_input(p,INPUT_EVENT_OPEN,i);}
    input_event_t events[HUB_INPUT_HISTORY];input_history_snapshot(&history,events);
    assert(history.count==24&&events[0].detail==6&&events[23].detail==29);
    input_link_clock_t clock={0};input_link_enter(&clock,INPUT_PHASE_HID,0xfffffff0u);
    assert(!input_link_expired(&clock,0x3a87u,false));
    assert(input_link_expired(&clock,0x3a88u,false));
    fresh();
    puts("Scoped clean-retry: full HID/ACL/worker exit, both callback orders, missing metadata retries, repeated exact descriptor stop, original bonds and report release preserved");
    return 0;
}
