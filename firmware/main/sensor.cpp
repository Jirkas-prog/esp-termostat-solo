#include "app.h"
#include "board.h"
#include "onewire_bus.h"
#include "onewire_device.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <cstring>
#include <atomic>

static uint8_t crc8(const uint8_t *p,size_t n) {
    uint8_t crc=0;
    while (n--) { uint8_t v=*p++; for (int i=0;i<8;++i) { bool bit=(crc^v)&1; crc>>=1; if (bit) crc^=0x8c; v>>=1; } }
    return crc;
}
static bool select(onewire_bus_handle_t bus,uint64_t address) {
    uint8_t cmd[9]={0x55}; memcpy(cmd+1,&address,8);
    return onewire_bus_reset(bus)==ESP_OK && onewire_bus_write_bytes(bus,cmd,sizeof(cmd))==ESP_OK;
}
static portMUX_TYPE diagnosticMux=portMUX_INITIALIZER_UNLOCKED;
static SensorDiagnostics diagnostics[2];
static std::atomic<bool> pauseRequested{false},paused[2]{};
void sensors_resume() { pauseRequested=false; }
bool sensors_pause_for_update() {
    pauseRequested=true;
    for(unsigned i=0;i<150;++i) {
        if(paused[0] && paused[1]) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    sensors_resume(); return false;
}
SensorDiagnostics sensor_diagnostics(unsigned index) {
    portENTER_CRITICAL(&diagnosticMux); auto d=diagnostics[index<2?index:0]; portEXIT_CRITICAL(&diagnosticMux); return d;
}
static bool failure(SensorDiagnostics &d,const char *stage,esp_err_t error=ESP_FAIL) {
    d.status=stage; d.error=error; return false;
}
static bool discover(onewire_bus_handle_t bus,uint64_t &address,SensorDiagnostics &d) {
    esp_err_t err=onewire_bus_reset(bus);
    d.presence=err==ESP_OK;
    if(err!=ESP_OK) return failure(d,err==ESP_ERR_NOT_FOUND?"no_presence":"bus_reset",err);
    onewire_device_iter_handle_t it=nullptr;
    err=onewire_new_device_iter(bus,&it);
    if(err!=ESP_OK) return failure(d,"search_error",err);
    onewire_device_t dev{}; unsigned count=0; esp_err_t result;
    while ((result=onewire_device_iter_get_next(it,&dev))==ESP_OK) { ++count; address=dev.address; if(count>1) break; }
    onewire_del_device_iter(it); d.devices=uint8_t(count); if(count) d.rom=address;
    if(count>1) return failure(d,"multiple_devices");
    if(result!=ESP_ERR_NOT_FOUND) return failure(d,"search_error",result);
    if(count!=1) return failure(d,"no_device",result);
    if(uint8_t(address)!=0x28) return failure(d,"unsupported_family");
    if(crc8(reinterpret_cast<uint8_t *>(&address),8)) return failure(d,"rom_crc",ESP_ERR_INVALID_CRC);
    // Accept any DS18B20 ROM on either input; calibration belongs to the GPIO.
    // Volatile TH/TL marker disappears if the sensor loses power. No EEPROM writes.
    uint8_t cfg[]={0x4e,0x5a,0xa5,0x7f};
    if(!select(bus,address)) return failure(d,"select_config");
    err=onewire_bus_write_bytes(bus,cfg,sizeof(cfg));
    return err==ESP_OK || failure(d,"write_config",err);
}
static void publish_diagnostics(unsigned index,const SensorDiagnostics &d) {
    portENTER_CRITICAL(&diagnosticMux); diagnostics[index]=d; portEXIT_CRITICAL(&diagnosticMux);
}
static void sensor_task(void *arg) {
    unsigned index=*static_cast<const unsigned *>(arg);
    onewire_bus_handle_t bus=nullptr;
    onewire_bus_config_t cfg{}; cfg.bus_gpio_num=SensorPins[index];
    cfg.flags.en_pull_up=true; // Enabled on request; external 4.7k to 3V3 may remain in parallel.
    onewire_bus_rmt_config_t rmt{}; rmt.max_rx_bytes=10;
    esp_err_t err=onewire_new_bus_rmt(&cfg,&rmt,&bus);
    if(err!=ESP_OK) {
        SensorDiagnostics d; failure(d,"bus_init",err); publish_diagnostics(index,d);
        paused[index]=true; // No bus task is running; it is safe to write flash.
        ESP_LOGE("sensor","GPIO%d bus init: %s",SensorPins[index],esp_err_to_name(err)); vTaskDelete(nullptr); return;
    }
    uint64_t address=0; bool found=false; unsigned good=0; uint32_t attempts=0,failures=0,lastLog=0;
    const char *lastStatus="starting";
    for(;;) {
        if(pauseRequested) {
            // Flash erases stall the RMT ISR. Acknowledge only between completed transactions.
            found=false; good=0;
            auto d=sensor_diagnostics(index); d.status="updating"; d.error=ESP_OK;
            d.good_samples=0; d.has_scratchpad=false; publish_diagnostics(index,d);
            Reading unavailable{InvalidTemp,false,now_ms()}; xQueueOverwrite(measurementQueue[index],&unavailable);
            if(bus) { onewire_bus_del(bus); bus=nullptr; }
            gpio_set_direction(gpio_num_t(SensorPins[index]),GPIO_MODE_INPUT);
            gpio_set_pull_mode(gpio_num_t(SensorPins[index]),GPIO_PULLUP_ONLY);
            ESP_LOGI("sensor","GPIO%d OTA pause; RMT released",SensorPins[index]);
            paused[index]=true;
            while(pauseRequested) vTaskDelay(pdMS_TO_TICKS(20));
            paused[index]=false;
        }
        if(!bus) {
            err=onewire_new_bus_rmt(&cfg,&rmt,&bus);
            if(err!=ESP_OK) {
                SensorDiagnostics d; failure(d,"bus_init",err); publish_diagnostics(index,d);
                vTaskDelay(pdMS_TO_TICKS(1000)); continue;
            }
            ESP_LOGI("sensor","GPIO%d measurement resumed",SensorPins[index]);
        }
        SensorDiagnostics d; d.attempts=++attempts;
        // When already discovered, keep its verified identity in the diagnostic snapshot.
        if(found) { d.rom=address; d.devices=1; d.presence=true; }
        if(!found) { found=discover(bus,address,d); good=0; }
        bool ok=found;
        if(ok && !select(bus,address)) ok=failure(d,"select_convert");
        uint8_t convert=0x44;
        if(ok) { err=onewire_bus_write_bytes(bus,&convert,1); if(err!=ESP_OK) ok=failure(d,"convert",err); }
        // Blocking only this low-priority sensor task. Control/LCD run separately.
        vTaskDelay(pdMS_TO_TICKS(800));
        if(ok && !select(bus,address)) ok=failure(d,"select_read");
        uint8_t read=0xbe;
        if(ok) { err=onewire_bus_write_bytes(bus,&read,1); if(err!=ESP_OK) ok=failure(d,"read_command",err); }
        if(ok) {
            err=onewire_bus_read_bytes(bus,d.scratchpad,sizeof(d.scratchpad));
            if(err!=ESP_OK) ok=failure(d,"read_scratchpad",err); else d.has_scratchpad=true;
        }
        const auto *data=d.scratchpad;
        if(ok && crc8(data,9)) ok=failure(d,"scratchpad_crc",ESP_ERR_INVALID_CRC);
        if(ok && (data[2]!=0x5a || data[3]!=0xa5)) ok=failure(d,"sensor_reset");
        if(ok && data[4]!=0x7f) ok=failure(d,"configuration");
        int16_t raw=InvalidTemp;
        if(ok) {
            int16_t sixteenths=int16_t(uint16_t(data[0]) | (uint16_t(data[1])<<8));
            int value=int(sixteenths)*100/16;
            if(value < -5500 || value > 12500) ok=failure(d,"temperature_range"); else raw=int16_t(value);
        }
        if(ok) { if(good<3) ++good; d.status=good>=3?"ok":"warming_up"; }
        else { good=0; found=false; ++failures; }
        d.good_samples=uint8_t(good); d.failures=failures; publish_diagnostics(index,d);
        if(strcmp(lastStatus,d.status) || (!ok && age(now_ms(),lastLog)>=10000)) {
            ESP_LOGI("sensor","GPIO%d %s err=%s presence=%u devices=%u ROM=%016llX data=%02X %02X %02X %02X %02X %02X %02X %02X %02X",
                SensorPins[index],d.status,esp_err_to_name(d.error),d.presence,d.devices,
                (unsigned long long)d.rom,
                data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7],data[8]);
            lastStatus=d.status; lastLog=now_ms();
        }
        Reading m{raw,ok && good>=3,now_ms()}; xQueueOverwrite(measurementQueue[index],&m);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
void sensor_start() {
    static unsigned index[2]={0,1};
    for(unsigned i=0;i<2;++i) configASSERT(xTaskCreate(sensor_task,i?"outlet":"inlet",4096,&index[i],5,nullptr)==pdPASS);
}
