#pragma once
#include <cstdint>
#include <cstddef>
struct cJSON;
namespace solo {
constexpr unsigned WifiProfiles=8;
struct WifiProfile {
    char ssid[33]{},password[65]{};
    uint8_t enabled=0,hidden=0,priority=50;
};
struct NetworkConfigV1 {
    uint32_t revision=1;
    WifiProfile wifi[4];
    char ap_ssid[33]{},ap_password[64]{};
    uint16_t ap_delay_s=30;
    uint8_t ap_always=0,ap_channel=1;
    char mqtt_host[129]{},mqtt_user[65]{},mqtt_password[97]{},mqtt_client_id[65]{};
    char mqtt_topic[97]="wifi2mqtt/termostat-solo";
    char device_name[65]="Termostat ter01",discovery_prefix[65]="homeassistant",birth_topic[97]="homeassistant/status";
    uint16_t mqtt_port=1883,mqtt_keepalive_s=30,publish_s=5;
    uint8_t mqtt_enabled=0,mqtt_tls=0,discovery=1;
};
// Version 2: stable profile IDs; AP is the ordered fallback.
struct NetworkConfig {
    uint32_t revision=1;
    WifiProfile wifi[WifiProfiles];
    char ap_ssid[33]{},ap_password[64]{};
    uint16_t ap_delay_s=30;
    uint8_t ap_always=0,ap_channel=1;
    char mqtt_host[129]{},mqtt_user[65]{},mqtt_password[97]{},mqtt_client_id[65]{};
    char mqtt_topic[97]="wifi2mqtt/termostat-solo";
    char device_name[65]="Termostat ter01",discovery_prefix[65]="homeassistant",birth_topic[97]="homeassistant/status";
    uint16_t mqtt_port=1883,mqtt_keepalive_s=30,publish_s=60;
    uint8_t mqtt_enabled=0,mqtt_tls=0,discovery=1;
    uint8_t order[WifiProfiles+1]={0,1,2,3,4,5,6,7,WifiProfiles};
    uint16_t publish_min_s=1,publish_delta_centi=20;
    uint8_t publish_pin=0;
};
bool network_migrate(const NetworkConfigV1 &old,NetworkConfig &next);
bool network_valid(const NetworkConfig &c);
bool network_legacy(const cJSON *doc,NetworkConfig &c);
// Full configuration, omitted passwords preserve matching profile credentials.
// 200 valid; 400 bad payload; 409 stale revision. No changes on rejection.
int network_parse(const cJSON *doc,const NetworkConfig &current,NetworkConfig &next);
cJSON *network_redacted(const NetworkConfig &c);
struct WifiSeen { char ssid[33]{}; int rssi=-127; };
int wifi_choose(const NetworkConfig &c,const WifiSeen *seen,size_t count,unsigned blocked);
}
