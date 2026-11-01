#include "app.h"
#include "network_settings.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "esp_log.h"
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <atomic>

static NetworkConfig config;
static char hostname[33]="ter01",deviceId[32];
static std::atomic<bool> saved{false};
struct NetworkStored { uint32_t magic=0x4e455431,format=2; NetworkConfig config; uint32_t crc=0; };
bool network_settings_saved() { return saved.load(); }
const NetworkConfig &network_settings() { return config; }
const char *network_hostname() { return hostname; }
const char *network_device_id() { return deviceId; }
const char *network_ap_password() { return config.ap_password; }
const char *network_topic() { return config.mqtt_topic; }
static bool read(nvs_handle_t h,NetworkStored &value) {
    size_t size=sizeof(value);
    return nvs_get_blob(h,"config2",&value,&size)==ESP_OK && size==sizeof(value) && value.magic==0x4e455431 && value.format==2 &&
        value.crc==crc32(&value,offsetof(NetworkStored,crc)) && network_valid(value.config);
}
static bool write(nvs_handle_t h,const NetworkConfig &next) {
    NetworkStored value{}; value.config=next; value.crc=crc32(&value,offsetof(NetworkStored,crc));
    if(nvs_set_blob(h,"config2",&value,sizeof(value))!=ESP_OK || nvs_commit(h)!=ESP_OK) return false;
    NetworkStored check{}; return read(h,check) && !memcmp(&value,&check,sizeof(value));
}
bool network_hostname_save(const char *name) {
    if(!hostname_valid(name)) return false;
    nvs_handle_t h; if(nvs_open("solo_network",NVS_READWRITE,&h)!=ESP_OK) return false;
    bool ok=nvs_set_str(h,"hostname",name)==ESP_OK && nvs_commit(h)==ESP_OK;
    char check[33]{}; size_t size=sizeof(check);
    ok=ok && nvs_get_str(h,"hostname",check,&size)==ESP_OK && !strcmp(check,name); nvs_close(h); return ok;
}
void network_settings_init() {
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    snprintf(deviceId,sizeof(deviceId),"termostat_%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(config.ap_ssid,sizeof(config.ap_ssid),"TERMO-%02X%02X%02X",mac[3],mac[4],mac[5]);
    snprintf(config.mqtt_client_id,sizeof(config.mqtt_client_id),"%s",deviceId);
    snprintf(config.mqtt_topic,sizeof(config.mqtt_topic),"wifi2mqtt/%s",deviceId);
    static const char alphabet[]="abcdefghijkmnpqrstuvwxyz23456789";
    // RF is not running yet and this board does not use ADC: enable hardware entropy.
    bootloader_random_enable();
    for(unsigned i=0;i<12;++i) config.ap_password[i]=alphabet[esp_random()%(sizeof(alphabet)-1)];
    bootloader_random_disable();
    nvs_handle_t h;
    if(nvs_open("solo_network",NVS_READWRITE,&h)!=ESP_OK) { ESP_LOGE("network","NVS unavailable; temporary recovery hotspot"); return; }
    char name[33]{}; size_t size=sizeof(name);
    if(nvs_get_str(h,"hostname",name,&size)==ESP_OK && hostname_valid(name)) strcpy(hostname,name);
    NetworkStored stored{};
    if(read(h,stored)) { config=stored.config; saved=true; }
    else {
        size_t present=0;
        if(nvs_get_blob(h,"config2",nullptr,&present)!=ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); ESP_LOGE("network","Invalid v2 record; previous NVS retained"); return; }
        struct Previous { uint32_t magic,format; NetworkConfigV1 config; uint32_t crc; } previous{};
        size_t previousSize=sizeof(previous);
        esp_err_t oldResult=nvs_get_blob(h,"config",&previous,&previousSize);
        if(oldResult!=ESP_ERR_NVS_NOT_FOUND) {
            if(oldResult!=ESP_OK || previousSize!=sizeof(previous) || previous.magic!=0x4e455431 || previous.format!=1 || previous.crc!=crc32(&previous,offsetof(Previous,crc)) || !network_migrate(previous.config,config)) {
                nvs_close(h); ESP_LOGE("network","Invalid v1 record; previous NVS retained"); return;
            }
            saved=write(h,config); nvs_close(h); return;
        }
        char legacy[600]{}; size=sizeof(legacy);
        if(nvs_get_str(h,"json",legacy,&size)==ESP_OK) {
            auto doc=cJSON_Parse(legacy); NetworkConfig migrated=config;
            strcpy(migrated.mqtt_topic,"wifi2mqtt/termostat-solo");
            if(network_legacy(doc,migrated)) { config=migrated; ESP_LOGI("network","Previous Wi-Fi/MQTT configuration migrated"); }
            else {
                ESP_LOGE("network","Migration failed; retaining previous NVS and recovery hotspot");
                cJSON_Delete(doc); nvs_close(h); return;
            }
            cJSON_Delete(doc);
        }
        saved=write(h,config);
    }
    nvs_close(h);
}
cJSON *network_config_json() {
    auto doc=network_redacted(config); if(!doc) return nullptr;
    cJSON_AddStringToObject(doc,"device_id",deviceId); cJSON_AddBoolToObject(doc,"saved",saved.load());
    return doc;
}
int network_save_json(const cJSON *doc) {
    NetworkConfig next; int status=network_parse(doc,config,next); if(status!=200) return status;
    nvs_handle_t h; if(nvs_open("solo_network",NVS_READWRITE,&h)!=ESP_OK) return 500;
    NetworkStored previous{};
    if(read(h,previous) && previous.config.revision!=config.revision) { nvs_close(h); return 409; }
    bool ok=write(h,next); nvs_close(h);
    if(!ok) return 500;
    network_before_restart(next); return 200;
}
bool network_save(const char *json) {
    auto doc=cJSON_Parse(json); NetworkConfig next=config; bool valid=network_legacy(doc,next); cJSON_Delete(doc);
    if(!valid || !next.revision || next.revision==UINT32_MAX) return false;
    next.revision++; nvs_handle_t h;
    if(nvs_open("solo_network",NVS_READWRITE,&h)!=ESP_OK) return false;
    bool ok=write(h,next); nvs_close(h); if(ok) network_before_restart(next); return ok;
}
