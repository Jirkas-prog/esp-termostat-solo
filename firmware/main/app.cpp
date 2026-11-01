#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"
#include <atomic>

QueueHandle_t measurementQueue[2],commandQueue,saveQueue,savedQueue;
static portMUX_TYPE stateMux=portMUX_INITIALIZER_UNLOCKED;
static State current;
static std::atomic<bool> connected{false},acknowledged{false};
static std::atomic<uint32_t> lastAck{0},pendingAck{0},sentAt{0};
static std::atomic<uint32_t> deliveryLifetime{45000};
void route_interval(uint16_t seconds) { deliveryLifetime=uint32_t(seconds)*1000+15000; }
static Stored initial;
static bool loaded=false;
State snapshot() { portENTER_CRITICAL(&stateMux); State s=current; portEXIT_CRITICAL(&stateMux); return s; }
bool command(const Command &c) { return xQueueSend(commandQueue,&c,0)==pdTRUE; }
void route_link(bool value) { connected=value; }
void route_sent(uint32_t seq) { sentAt=now_ms(); pendingAck=seq; }
void route_ack(uint32_t seq) {
    if(seq && age(now_ms(),sentAt.load())<15000 && pendingAck.compare_exchange_strong(seq,0)) {
        lastAck=now_ms(); acknowledged=true;
    }
}
static void control_task(void *) {
    Controller ctl(initial,loaded,esp_random());
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    TickType_t wake=xTaskGetTickCount(); uint32_t oldFaults=UINT32_MAX,wdAt=0; bool wd=false;
    for(;;) {
        uint32_t now=now_ms(); Reading r; SaveResult saved;
        for(unsigned i=0;i<2;++i) if(xQueueReceive(measurementQueue[i],&r,0)==pdTRUE) ctl.sample(i,r);
        if(xQueueReceive(savedQueue,&saved,0)==pdTRUE) ctl.persisted(saved.revision,saved.success);
        Command c;
        for(int i=0;i<4 && xQueueReceive(commandQueue,&c,0)==pdTRUE;++i) {
            if(c.type==CommandType::Settings) ctl.change(c.settings);
            else if(c.type==CommandType::Manual) ctl.manual(now);
            else { auto cfg=ctl.store.settings; cfg.commissioned=1; ctl.change(cfg); }
        }
        if(ctl.dirty) { xQueueOverwrite(saveQueue,&ctl.store); ctl.dirty=false; }
        bool delivered=connected && acknowledged && age(now,lastAck.load())<deliveryLifetime.load();
        State s=ctl.tick(now,connected,delivered,relay_fault(),false);
        relay_tick(s.demand,s.settings.pulse_ms,now);
        s.command_sent=relay_command_sent();
        portENTER_CRITICAL(&stateMux); current=s; portEXIT_CRITICAL(&stateMux);
        if(age(now,wdAt)>=100) { wd=!wd; gpio_set_level(gpio_num_t(WatchdogPin),wd); wdAt=now; }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        if(s.faults!=oldFaults) { ESP_LOGW("control","faults=0x%lx demand=%u output=%s",(unsigned long)s.faults,s.demand,relay_name(s.active_relay)); oldFaults=s.faults; }
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(20));
    }
}
extern "C" void app_main() {
    relay_early_init(); loaded=storage_init(initial); relay_start(initial.settings.relay);
    for(auto &q:measurementQueue) { q=xQueueCreate(1,sizeof(Reading)); configASSERT(q); }
    commandQueue=xQueueCreate(8,sizeof(Command)); saveQueue=xQueueCreate(1,sizeof(Stored)); savedQueue=xQueueCreate(1,sizeof(SaveResult));
    configASSERT(commandQueue && saveQueue && savedQueue);
    ESP_LOGI("boot","ESP TERMOSTAT SOLO 0.8.5 / %s / reset=%d",BoardName,int(esp_reset_reason()));
    configASSERT(xTaskCreate(control_task,"control",6144,nullptr,12,nullptr)==pdPASS);
    storage_start(); sensor_start(); ui_start(); console_start(); analysis_start(); transport_start(); ota_verify_start();
}
