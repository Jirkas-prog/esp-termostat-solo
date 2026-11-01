#include "app.h"
#include "board.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include <cstring>
#include <cstdio>
#include <cmath>

static bool nvsReady=false;
static uint32_t migrationFailureRevision=0;
bool storage_init(Stored &s) {
    nvsReady=nvs_flash_init()==ESP_OK;
    if(!nvsReady) { ESP_LOGE("storage","NVS unavailable; never erased automatically"); return false; }
    nvs_handle_t h;
    if(nvs_open("solo",NVS_READWRITE,&h)!=ESP_OK) return false;
    uint8_t bytes[sizeof(Stored)]; size_t size=sizeof(bytes);
    esp_err_t err=nvs_get_blob(h,"settings",bytes,&size);
    bool ok=err==ESP_OK && decode_stored(bytes,size,s);
    if(ok && size!=sizeof(Stored)) {
        // Persist the validated migration before reporting settings as saved.
        bool written=nvs_set_blob(h,"settings",&s,sizeof(s))==ESP_OK && nvs_commit(h)==ESP_OK;
        Stored check; size_t n=sizeof(check);
        written=written && nvs_get_blob(h,"settings",&check,&n)==ESP_OK && n==sizeof(check) && !memcmp(&s,&check,n);
        if(written) ESP_LOGI("storage","Migrated settings to v3; thermostat, display and calibration preserved");
        else migrationFailureRevision=s.settings.revision; // retain old regulation, report failed persistence
    }
    nvs_close(h);
    if(ok) return true;
    ESP_LOGW("storage","New/invalid configuration: output NONE; defaults will be saved"); return false;
}
static void storage_task(void *) {
    Stored s;
    for(;;) if(xQueueReceive(saveQueue,&s,portMAX_DELAY)==pdTRUE) {
        seal(s); nvs_handle_t h; bool ok=false;
        if(nvsReady && nvs_open("solo",NVS_READWRITE,&h)==ESP_OK) {
            ok=nvs_set_blob(h,"settings",&s,sizeof(s))==ESP_OK && nvs_commit(h)==ESP_OK;
            if(ok) { Stored read; size_t n=sizeof(read); ok=nvs_get_blob(h,"settings",&read,&n)==ESP_OK && n==sizeof(read) && !memcmp(&s,&read,n); }
            nvs_close(h);
        }
        SaveResult result{s.settings.revision,ok}; xQueueOverwrite(savedQueue,&result);
        ESP_LOGI("storage","Settings r%lu: %s",(unsigned long)s.settings.revision,ok?"saved":"FAILED");
    }
}
void storage_start() {
    if(migrationFailureRevision) { SaveResult result{migrationFailureRevision,false}; xQueueOverwrite(savedQueue,&result); }
    configASSERT(xTaskCreate(storage_task,"settings",4096,nullptr,2,nullptr)==pdPASS);
}
static bool integer(const cJSON *v,int low,int high,int &out) {
    if(!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble<low || v->valuedouble>high || std::floor(v->valuedouble)!=v->valuedouble) return false;
    out=int(v->valuedouble); return true;
}
static bool enumeration(const cJSON *p,bool source,uint8_t &out) {
    if(!cJSON_IsString(p)) return false;
    for(uint8_t i=0;i<=(source?uint8_t(Maximum):uint8_t(Latching));++i)
        if(!strcmp(p->valuestring,source?source_name(i):relay_name(i))) { out=i; return true; }
    return false;
}
bool settings_patch(const cJSON *v,Settings &s) {
    if(!cJSON_IsObject(v) || !v->child) return false;
    for(const cJSON *p=v->child;p;p=p->next) {
        // Reject duplicate JSON keys rather than silently accepting the last one.
        for(const cJSON *q=v->child;q!=p;q=q->next) if(!strcmp(p->string,q->string)) return false;
        if(!strcmp(p->string,"on_source")) { if(!enumeration(p,true,s.on_source)) return false; }
        else if(!strcmp(p->string,"off_source")) { if(!enumeration(p,true,s.off_source)) return false; }
        else if(!strcmp(p->string,"relay_mode")) { if(!enumeration(p,false,s.relay)) return false; }
        else if(!strcmp(p->string,"display_source")) { if(!enumeration(p,true,s.display.source)) return false; }
        else if(!strcmp(p->string,"display_night_enabled") || !strcmp(p->string,"display_temp_off_enabled")) {
            if(!cJSON_IsBool(p)) return false;
            if(!strcmp(p->string,"display_night_enabled")) s.display.night_enabled=cJSON_IsTrue(p);
            else s.display.temp_off_enabled=cJSON_IsTrue(p);
        }
        else {
            int n; if(!integer(p,-500,12500,n)) return false;
            if(!strcmp(p->string,"on_centi")) s.on=int16_t(n);
            else if(!strcmp(p->string,"off_centi")) s.off=int16_t(n);
            else if(!strcmp(p->string,"force_centi")) s.force=int16_t(n);
            else if(!strcmp(p->string,"calibration1_centi")) s.calibration[0]=int16_t(n);
            else if(!strcmp(p->string,"calibration2_centi")) s.calibration[1]=int16_t(n);
            else if(!strcmp(p->string,"min_on_s") && n>=0) s.min_on_s=uint16_t(n);
            else if(!strcmp(p->string,"manual_s") && n>=0) s.manual_s=uint16_t(n);
            else if(!strcmp(p->string,"pulse_ms") && n>=0) s.pulse_ms=uint16_t(n);
            else if(!strcmp(p->string,"display_off_centi")) s.display.off_below=int16_t(n);
            else if(!strcmp(p->string,"display_day_brightness") && n>=1 && n<=100) s.display.day_brightness=uint8_t(n);
            else if(!strcmp(p->string,"display_night_brightness") && n>=1 && n<=100) s.display.night_brightness=uint8_t(n);
            else if(!strcmp(p->string,"display_night_start") && n>=0 && n<1440) s.display.night_start=uint16_t(n);
            else if(!strcmp(p->string,"display_night_end") && n>=0 && n<1440) s.display.night_end=uint16_t(n);
            else if(!strcmp(p->string,"display_rotation") && (n==0 || n==90 || n==180 || n==270)) s.display.rotation=uint16_t(n);
            else if(!strcmp(p->string,"outlet_sensor") && n>=1 && n<=2) s.outlet_sensor=uint8_t(n);
            else return false;
        }
    }
    return valid(s);
}
static void console_line(const char *line) {
    cJSON *doc=cJSON_ParseWithOpts(line,nullptr,true);
    if(!cJSON_IsObject(doc) || cJSON_GetArraySize(doc)!=1) { puts("ERR: one JSON command required"); cJSON_Delete(doc); return; }
    const cJSON *v=doc->child; bool ok=false; Command c{};
    if(!strcmp(v->string,"status") && cJSON_IsTrue(v)) {
        char *json=snapshot_json(snapshot()); if(json) { puts(json); cJSON_free(json); } ok=true;
    } else if(!strcmp(v->string,"network") && cJSON_IsObject(v)) {
        char *json=cJSON_PrintUnformatted(v); if(json) { ok=network_save(json); cJSON_free(json); }
        if(ok) puts("Network stored; restart to apply.");
    } else if(!strcmp(v->string,"manual") && cJSON_IsTrue(v)) { c.type=CommandType::Manual; ok=command(c); }
    else if(!strcmp(v->string,"commission") && cJSON_IsTrue(v)) { c.type=CommandType::Commission; ok=command(c); }
    else if(!strcmp(v->string,"settings")) {
        c.type=CommandType::Settings; c.settings=snapshot().settings;
        ok=settings_patch(v,c.settings) && command(c);
    }
    puts(ok?"OK: accepted; check saved and active_relay_mode":"ERR: rejected"); cJSON_Delete(doc);
}
static void console_task(void *) {
    char line[768]; size_t used=0; bool overflow=false;
    for(;;) {
        int c=getchar();
        if(c==EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if(c=='\n' || c=='\r') {
            if(used && !overflow) { line[used]=0; console_line(line); }
            else if(overflow) puts("ERR: command too long");
            used=0; overflow=false;
        } else if(c>=32 && c<127) { if(used<sizeof(line)-1) line[used++]=char(c); else overflow=true; }
    }
}
void console_start() {
    // Buffer complete JSON commands: the hardware FIFO alone loses long USB writes.
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0,2048,0,0,nullptr,0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    configASSERT(xTaskCreate(console_task,"console",6144,nullptr,1,nullptr)==pdPASS);
}
static void temperature(cJSON *j,const char *key,int16_t t) {
    if(t==InvalidTemp) cJSON_AddNullToObject(j,key); else cJSON_AddNumberToObject(j,key,t/100.0);
}
char *snapshot_json(const State &s) {
    cJSON *j=cJSON_CreateObject(); if(!j) return nullptr;
    cJSON_AddStringToObject(j,"firmware","solo-0.8.5");
    cJSON_AddNumberToObject(j,"sequence",s.sequence); cJSON_AddNumberToObject(j,"boot",s.boot);
    cJSON_AddNumberToObject(j,"uptime_s",s.uptime_ms/1000);
    temperature(j,"inlet",s.values.inlet); temperature(j,"outlet",s.values.outlet);
    temperature(j,"average",s.values.average); temperature(j,"minimum",s.values.minimum);
    temperature(j,"maximum",s.values.maximum); temperature(j,"difference",s.values.difference);
    for(unsigned i=0;i<2;++i) {
        cJSON *probe=cJSON_AddObjectToObject(j,i?"sensor2":"sensor1");
        cJSON_AddBoolToObject(probe,"valid",s.values.valid[i]); temperature(probe,"raw",s.raw[i]);
        temperature(probe,"temperature",s.values.temperature[i]);
        cJSON_AddStringToObject(probe,"role",i+1==s.settings.outlet_sensor?"outlet":"inlet");
        cJSON_AddNumberToObject(probe,"calibration",s.settings.calibration[i]/100.0);
        if(s.sample_age_ms[i]==UINT32_MAX) cJSON_AddNullToObject(probe,"age_ms");
        else cJSON_AddNumberToObject(probe,"age_ms",s.sample_age_ms[i]);
        auto d=sensor_diagnostics(i); auto diag=cJSON_AddObjectToObject(probe,"diagnostics");
        cJSON_AddNumberToObject(diag,"gpio",SensorPins[i]);
        cJSON_AddStringToObject(diag,"status",d.status);
        cJSON_AddStringToObject(diag,"error",esp_err_to_name(d.error));
        cJSON_AddBoolToObject(diag,"presence",d.presence);
        cJSON_AddNumberToObject(diag,"devices",d.devices);
        cJSON_AddNumberToObject(diag,"good_samples",d.good_samples);
        cJSON_AddNumberToObject(diag,"attempts",d.attempts);
        cJSON_AddNumberToObject(diag,"failures",d.failures);
        char hex[27];
        if(d.rom) { snprintf(hex,sizeof(hex),"%016llX",(unsigned long long)d.rom); cJSON_AddStringToObject(diag,"rom",hex); }
        else cJSON_AddNullToObject(diag,"rom");
        if(d.has_scratchpad) {
            for(unsigned b=0;b<9;++b) snprintf(hex+b*3,sizeof(hex)-b*3,b==8?"%02X":"%02X ",d.scratchpad[b]);
            cJSON_AddStringToObject(diag,"scratchpad",hex);
        } else cJSON_AddNullToObject(diag,"scratchpad");
    }
    cJSON_AddBoolToObject(j,"pump_request",s.demand);
    cJSON_AddBoolToObject(j,"output_enabled",s.active_relay!=None);
    cJSON_AddStringToObject(j,"active_relay_mode",relay_name(s.active_relay));
    if(s.active_relay==None) cJSON_AddNullToObject(j,"command_sent"); else cJSON_AddBoolToObject(j,"command_sent",s.command_sent);
    cJSON_AddBoolToObject(j,"saved",s.saved); cJSON_AddBoolToObject(j,"link",s.link);
    cJSON_AddBoolToObject(j,"ha_delivery",s.delivered);
    cJSON_AddNumberToObject(j,"manual_remaining_s",s.manual_remaining_s);
    cJSON_AddNumberToObject(j,"faults",s.faults); cJSON_AddNumberToObject(j,"fault_events",s.fault_events);
    cJSON_AddNumberToObject(j,"last_fault",s.last_fault);
    cJSON *cfg=cJSON_AddObjectToObject(j,"settings");
    const auto &v=s.settings;
    cJSON_AddNumberToObject(cfg,"revision",v.revision);
    cJSON_AddNumberToObject(cfg,"outlet_sensor",v.outlet_sensor);
    cJSON_AddNumberToObject(cfg,"display_rotation",v.display.rotation);
    cJSON_AddStringToObject(cfg,"on_source",source_name(v.on_source));
    cJSON_AddStringToObject(cfg,"off_source",source_name(v.off_source));
    cJSON_AddNumberToObject(cfg,"on",v.on/100.0); cJSON_AddNumberToObject(cfg,"off",v.off/100.0);
    cJSON_AddNumberToObject(cfg,"force",v.force/100.0);
    cJSON_AddNumberToObject(cfg,"min_on_s",v.min_on_s); cJSON_AddNumberToObject(cfg,"manual_s",v.manual_s);
    cJSON_AddStringToObject(cfg,"relay_mode",relay_name(v.relay));
    cJSON_AddNumberToObject(cfg,"pulse_ms",v.pulse_ms); cJSON_AddBoolToObject(cfg,"commissioned",v.commissioned);
    const auto &d=v.display;
    cJSON_AddStringToObject(cfg,"display_source",source_name(d.source));
    cJSON_AddNumberToObject(cfg,"display_off",d.off_below/100.0);
    cJSON_AddBoolToObject(cfg,"display_temp_off_enabled",d.temp_off_enabled);
    cJSON_AddBoolToObject(cfg,"display_night_enabled",d.night_enabled);
    cJSON_AddNumberToObject(cfg,"display_night_start",d.night_start);
    cJSON_AddNumberToObject(cfg,"display_night_end",d.night_end);
    cJSON_AddNumberToObject(cfg,"display_day_brightness",d.day_brightness);
    cJSON_AddNumberToObject(cfg,"display_night_brightness",d.night_brightness);
    auto network=network_info(); int minute=local_minute();
    auto net=cJSON_AddObjectToObject(j,"network");
    cJSON_AddStringToObject(net,"ssid",network.ssid); cJSON_AddStringToObject(net,"station_ip",network.station_ip);
    cJSON_AddStringToObject(net,"ap_ip",network.ap_ip); cJSON_AddNumberToObject(net,"profile",network.profile);
    cJSON_AddNumberToObject(net,"rssi",network.rssi); cJSON_AddNumberToObject(net,"wifi_reason",network.wifi_reason);
    cJSON_AddNumberToObject(net,"mqtt_error",network.mqtt_error); cJSON_AddNumberToObject(net,"discovery_published",network.discovery_published);

    auto view=display_view(s,network.configured,network.ap,minute,false,display_network_grace());
    cJSON *display=cJSON_AddObjectToObject(j,"display");
    temperature(display,"temperature",view.temperature);
    temperature(display,"threshold_temperature",view.threshold_temperature);
    cJSON_AddNumberToObject(display,"rotation",v.display.rotation);
    cJSON_AddBoolToObject(display,"night",view.night);
    cJSON_AddBoolToObject(display,"diagnostics",view.diagnostics);
    cJSON_AddBoolToObject(display,"clock_valid",minute>=0);
    if(minute>=0) cJSON_AddNumberToObject(display,"local_minute",minute); else cJSON_AddNullToObject(display,"local_minute");
    cJSON_AddNumberToObject(display,"brightness",display_brightness());
    cJSON_AddBoolToObject(display,"ready",display_ready());
    cJSON_AddBoolToObject(j,"mqtt_connected",network.mqtt);
    cJSON_AddBoolToObject(j,"ap_active",network.ap);
    cJSON_AddStringToObject(j,"ip",network.ip);
    cJSON_AddItemToObject(j,"analysis",analysis_json(s));
    char *json=cJSON_PrintUnformatted(j); cJSON_Delete(j); return json;
}
