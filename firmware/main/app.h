#pragma once
#include "core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
using namespace solo;
inline uint32_t now_ms() { return uint32_t(esp_timer_get_time()/1000); }
enum class CommandType { Settings, Manual, Commission };
struct Command { CommandType type; Settings settings; };
struct SaveResult { uint32_t revision; bool success; };
extern QueueHandle_t measurementQueue[2],commandQueue,saveQueue,savedQueue;
State snapshot();
bool command(const Command &c);
void route_link(bool connected);
void route_interval(uint16_t seconds);
void route_sent(uint32_t sequence);
void route_ack(uint32_t sequence);
void sensor_start();
bool sensors_pause_for_update();
void sensors_resume();
struct SensorDiagnostics {
    const char *status="starting";
    int32_t error=0;
    uint64_t rom=0;
    uint32_t attempts=0,failures=0;
    uint8_t devices=0,good_samples=0,scratchpad[9]{};
    bool presence=false,has_scratchpad=false;
};
SensorDiagnostics sensor_diagnostics(unsigned index);
void ui_start();
void transport_start();
void relay_early_init();
void relay_start(uint8_t mode);
void relay_tick(bool on,uint16_t pulse_ms,uint32_t now);
bool relay_fault();
bool relay_command_sent();
bool storage_init(Stored &settings);
void storage_start();
bool network_save(const char *json);
const char *network_hostname();
bool network_hostname_save(const char *name);
struct cJSON;
cJSON *network_config_json();
int network_save_json(const cJSON *doc);
const char *network_topic();
bool settings_patch(const cJSON *value,Settings &settings);
bool web_start();
void ota_verify_start();
struct NetworkInfo {
    bool configured=false,mqtt=false,ap=false;
    char ip[16]{},station_ip[16]{},ap_ip[16]{},ssid[33]{};
    int profile=-1,rssi=0,wifi_reason=0,mqtt_error=0;
    unsigned discovery_published=0;
};
NetworkInfo network_info();
int local_minute();
int display_brightness();
bool display_network_grace();
bool display_ready();
void analysis_start();
cJSON *analysis_config_json();
cJSON *analysis_json(const State &state);
int analysis_save_json(const cJSON *doc);
void console_start();
char *snapshot_json(const State &s); // caller frees with cJSON_free
