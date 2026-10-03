#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "btstack_tlv.h"
#include "hub_output_store.h"
static uint8_t flash[128];
static int length;
static bool fail_write;
static int get(void *c,uint32_t tag,uint8_t *data,uint32_t size){
    (void)c;assert(tag==HUB_OUTPUT_STORE_TAG);
    if(data){unsigned n=(unsigned)length<size?(unsigned)length:size;memcpy(data,flash,n);}
    return length;
}
static int set(void *c,uint32_t tag,const uint8_t *data,uint32_t size){
    (void)c;assert(tag==HUB_OUTPUT_STORE_TAG);if(fail_write)return -1;
    memcpy(flash,data,size);length=size;return 0;
}
static void del(void *c,uint32_t tag){(void)c;(void)tag;assert(0);}
static const btstack_tlv_t tlv={get,set,del};
void btstack_tlv_get_instance(const btstack_tlv_t **out,void **context){*out=&tlv;*context=NULL;}
#include "..\src\hub_output_store.c"
int main(void){
    assert(HUB_OUTPUT_STORE_SLOTS==7 && HUB_OUTPUT_STORE_BYTES==75);
    /* Migrate a real v1 four-slot record without reallocating its identities/selection. */
    memset(flash,0,sizeof(flash));memcpy(flash,"PHO1",4);flash[4]=1;flash[6]=4;
    flash[8]=1;flash[9]=0;flash[10]=1;flash[15]=2;
    uint32_t crc=record_crc(flash,40);for(unsigned i=0;i<4;++i)flash[40+i]=(uint8_t)(crc>>(8*i));length=44;
    hub_output_store_t s;assert(hub_output_store_load(&s));
    assert(s.identities[0].reserved && s.identities[0].order==1 && !s.identities[4].reserved);
    for(unsigned i=1;i<7;++i){uint8_t address[6]={1,2,3,4,5,(uint8_t)i};
        assert(hub_output_store_claim(&s,0,address)==(int)i);}
    assert(length==75);
    uint8_t newest[6]={9,8,7,6,5,4};
    assert(hub_output_store_claim(&s,0,newest)==0 && s.identities[0].order==7);
    assert(hub_output_store_oldest(&s)==1);
    assert(hub_output_store_select(&s,2));
    hub_output_store_t restored;assert(hub_output_store_load(&restored));
    assert(restored.selected==2 && hub_output_store_oldest(&restored)==1);
    assert(hub_output_store_forget(&restored,2));
    assert(restored.selected==1 && !restored.identities[2].reserved);
    fail_write=true;
    uint8_t fail[6]={2,2,2,2,2,2};
    assert(hub_output_store_claim(&restored,0,fail)<0 && !restored.identities[2].reserved && restored.fault);
    puts("7-PC persisted FIFO, four-slot migration, selection/delete and failed-write protection passed");
    return 0;
}
