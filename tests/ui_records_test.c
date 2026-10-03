#include <assert.h>
#include <stdio.h>
#include "hub_ui.h"
#include "hub_input_records.h"

static hub_ui_action_t press(hub_ui_button_t *b,uint32_t time,bool down) {
    assert(hub_ui_tick(b,down,time)==HUB_UI_NONE);
    return hub_ui_tick(b,down,time+50);
}
int main(void) {
    hub_ui_button_t b={0};
    assert(hub_ui_tick(&b,true,0)==HUB_UI_NONE);
    assert(press(&b,10,false)==HUB_UI_NONE); /* Held during boot cannot act. */
    assert(press(&b,100,true)==HUB_UI_NONE);
    assert(press(&b,250,false)==HUB_UI_NONE);
    assert(hub_ui_tick(&b,false,649)==HUB_UI_NONE);
    assert(hub_ui_tick(&b,false,650)==HUB_UI_SHORT);
    assert(press(&b,700,true)==HUB_UI_NONE);
    assert(press(&b,800,false)==HUB_UI_NONE);
    assert(press(&b,950,true)==HUB_UI_NONE);
    assert(press(&b,1000,false)==HUB_UI_DOUBLE);
    assert(press(&b,1200,true)==HUB_UI_NONE);
    assert(hub_ui_tick(&b,true,4250)==HUB_UI_NONE);
    assert(press(&b,4300,false)==HUB_UI_LONG);
    assert(hub_ui_tick(&b,false,5000)==HUB_UI_NONE);
    assert(!hub_ui_led(HUB_LED_OFF,0) && !hub_ui_led(HUB_LED_OFF,500));
    assert(hub_ui_led(HUB_LED_PAIR,499) && !hub_ui_led(HUB_LED_PAIR,500));
    for(unsigned time=0;time<12000;++time) {
        unsigned phase=time%4000;
        bool expected=phase<2000 || ((phase-2000)/100)%2==0;
        assert(hub_ui_led(HUB_LED_FAULT,time)==expected);
    }
    hub_ui_fault_clock_t clock={0};
    assert(hub_ui_fault_tick(&clock,true,3725));
    assert(hub_ui_fault_tick(&clock,true,5724)); /* Last millisecond of the solid phase. */
    assert(hub_ui_fault_tick(&clock,true,5725)); /* First fast-flash ON. */
    assert(!hub_ui_fault_tick(&clock,true,5825));
    assert(!hub_ui_fault_tick(&clock,true,7724));
    assert(hub_ui_fault_tick(&clock,true,7725)); /* Next four-second cycle. */
    assert(!hub_ui_fault_tick(&clock,false,8000));
    assert(hub_ui_fault_tick(&clock,true,8900)); /* A new fault starts solid again. */
    clock=(hub_ui_fault_clock_t){0};
    assert(hub_ui_fault_tick(&clock,true,0xfffffff0u));
    assert(hub_ui_fault_tick(&clock,true,0x7bfu));
    assert(!hub_ui_fault_tick(&clock,true,0x824u));
    hub_input_records_t r;hub_records_init(&r);
    for(unsigned i=0;i<5;++i){
        hub_saved_input_t input={.used=true,.role=(i%2)+1,.transport=HUB_RECORD_BLE,.type=1,.address={1,2,3,4,5,(uint8_t)i}};
        assert(hub_records_insert(&r,&input)==(int)i);
    }
    assert(hub_records_oldest(&r)==0 && r.active[0]==4 && r.active[1]==3);
    hub_saved_input_t replacement={.used=true,.role=1,.transport=HUB_RECORD_CLASSIC,.address={9,8,7,6,5,4}};
    hub_input_records_t provisional=r;
    assert(hub_records_insert(&provisional,&replacement)==0);
    assert(r.slots[0].address[0]==1); /* Failed trial must not mutate persistent registry. */
    assert(provisional.slots[0].order==5 && hub_records_oldest(&provisional)==1);
    uint8_t bytes[HUB_INPUT_RECORD_BYTES];hub_records_encode(&provisional,bytes);
    hub_input_records_t loaded;hub_records_init(&loaded);
    assert(hub_records_decode(&loaded,bytes));
    assert(loaded.slots[0].order==5 && loaded.active[0]==0);
    bytes[5]^=1;assert(!hub_records_decode(&loaded,bytes));
    assert(hub_records_forget(&provisional,3) && provisional.active[1]==255);
    assert(!hub_records_forget(&provisional,3));
    assert(hub_records_insert(&provisional,&r.slots[2])==2 && provisional.active[0]==2);
    assert(provisional.slots[2].order==2); /* Activating is not a new admission/LRU age. */
    hub_input_records_t migration;hub_records_init(&migration);
    hub_saved_input_t keyboard={.used=true,.role=1,.transport=HUB_RECORD_CLASSIC,.address={1,2,3,4,5,6}};
    hub_saved_input_t mouse={.used=true,.role=2,.transport=HUB_RECORD_BLE,.type=1,.address={7,8,9,10,11,12}};
    assert(hub_records_insert(&migration,&keyboard)==0);
    assert(hub_records_insert(&migration,&mouse)==1);
    assert(migration.active[0]==0 && migration.active[1]==1);
    hub_records_encode(&migration,bytes);assert(hub_records_decode(&loaded,bytes));
    assert(loaded.slots[0].transport==0 && loaded.slots[1].transport==1);
    char long_name[100];memset(long_name,'x',sizeof(long_name));long_name[99]=0;
    hub_input_name_copy(migration.slots[1].name,long_name);
    assert(strlen(migration.slots[1].name)==63);
    hub_records_encode(&migration,bytes);assert(hub_records_decode(&loaded,bytes));
    assert(strlen(loaded.slots[1].name)==63);
    uint8_t legacy[HUB_INPUT_RECORD_V1_BYTES]={1,0,1,0};
    for(unsigned i=0;i<2;++i){
        uint8_t *p=legacy+4+i*12;const hub_saved_input_t *s=&migration.slots[i];
        p[0]=1;p[1]=s->role;p[2]=s->transport;p[3]=s->type;
        memcpy(p+5,s->address,6);p[11]=s->order;
    }
    uint32_t crc=hub_records_crc(legacy,64);
    for(unsigned i=0;i<4;++i)legacy[64+i]=(uint8_t)(crc>>(8*i));
    assert(hub_records_decode_sized(&loaded,legacy,sizeof(legacy)));
    assert(loaded.active[0]==0 && loaded.active[1]==1 && !loaded.slots[1].name[0]);
    assert(!hub_records_decode_sized(&loaded,legacy,sizeof(legacy)-1));
    hub_records_encode(&migration,bytes);
    memset(bytes+4+12,'x',64);
    crc=hub_records_crc(bytes,HUB_INPUT_RECORD_BYTES-4);
    for(unsigned i=0;i<4;++i)bytes[HUB_INPUT_RECORD_BYTES-4+i]=(uint8_t)(crc>>(8*i));
    assert(!hub_records_decode(&loaded,bytes)); /* Persistent names must be terminated. */
    puts("UI release/hold/double/debounce, quiet LED, 5-input FIFO, rollback-copy, CRC and active deletion passed");
    return 0;
}
