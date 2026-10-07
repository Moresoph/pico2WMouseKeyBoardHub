#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_err.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef struct {unsigned unused;} hub_web_snapshot_t;
typedef struct {unsigned unused;} hub_web_command_t;
static const char *TAG="test";
static bool (*read_snapshot)(hub_web_snapshot_t *);
static esp_err_t (*apply_command)(const hub_web_command_t *);
static unsigned control;
static bool running,allocation_failed,locked;
static esp_err_t initialization_error=ESP_ERR_INVALID_STATE,initialize_result,enable_result,disable_result,password_result;
static unsigned initializations,starts,stops,changes;
static char ssid[32]="test",password[64]="test-only";
enum {portMAX_DELAY=1};
static unsigned xSemaphoreCreateMutex(void){return allocation_failed?0:1;}
static void xSemaphoreTake(unsigned mutex,unsigned wait){assert(mutex&&wait==portMAX_DELAY&&!locked);locked=true;}
static void xSemaphoreGive(unsigned mutex){assert(mutex&&locked);locked=false;}
static esp_err_t initialize(void){++initializations;return initialize_result;}
static esp_err_t start_locked(void){assert(locked);++starts;if(enable_result==ESP_OK)running=true;return enable_result;}
static esp_err_t stop_locked(void){assert(locked);++stops;if(disable_result==ESP_OK)running=false;return disable_result;}
static esp_err_t change_password_locked(const char *value){
    assert(locked&&!strcmp(value,"new-test-only"));++changes;return password_result;
}
static uint32_t esp_get_free_heap_size(void){return 40000;}
const char *esp_err_to_name(esp_err_t error){(void)error;return "test";}
#include "web_startup_generated.inc"

static bool snapshot(hub_web_snapshot_t *out){(void)out;return true;}
static esp_err_t command(const hub_web_command_t *in){(void)in;return ESP_OK;}
int main(void){
    (void)TAG;
    assert(hub_web_init(snapshot,command)==ESP_OK);
    assert(read_snapshot==snapshot&&apply_command==command&&initializations==1);
    assert(!running&&!hub_web_enabled()&&!starts&&!stops);
    char *on[]={"web","on"},*off[]={"web","off"},*status[]={"web"};
    char *rename[]={"web","password","new-test-only"};
    assert(hub_web_command(2,on)==1&&!running&&!starts&&!locked);
    assert(hub_web_command(1,status)==0&&!running&&!starts&&!locked);
    assert(hub_web_command(3,rename)==0&&changes==1&&!running&&!starts);
    assert(hub_web_set_enabled(true)==ESP_OK&&hub_web_enabled()&&starts==1); /* Explicit physical toggle. */
    assert(hub_web_command(2,off)==0&&!hub_web_enabled()&&stops==1);
    assert(hub_web_set_enabled(true)==ESP_OK&&starts==2);
    assert(hub_web_set_enabled(false)==ESP_OK&&!running&&stops==2);
    enable_result=ESP_FAIL;assert(hub_web_set_enabled(true)==ESP_FAIL&&!running);
    allocation_failed=true;assert(hub_web_init(snapshot,command)==ESP_ERR_NO_MEM&&!running);
    assert(hub_web_set_enabled(true)==ESP_ERR_INVALID_STATE);
    allocation_failed=false;initialize_result=ESP_FAIL;
    assert(hub_web_init(snapshot,command)==ESP_FAIL&&!running&&starts==3);
    assert(!locked);
    puts("Production web startup/control: boot OFF, explicit toggle ON/OFF, USB on rejected, query/password never start radio, initialization/control errors preserved");
    return 0;
}
