#include "app.h"
#include "network_settings.h"
#include "mqtt_discovery.h"
#include "telemetry.h"
#include "web_access.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mdns.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>

static esp_mqtt_client_handle_t mqtt;
static std::atomic<bool> initialized{false},wifiUp{false},mqttUp{false},apUp{false},scanDone{false},connectFailed{false},announce{false},stopping{false};
static std::atomic<int> activeProfile{-1},wifiReason{0},mqttError{0};
static std::atomic<unsigned> announced{0};
static char stateTopic[128],eventTopic[128],availabilityTopic[128],ackTopic[128],accessTopic[128];
static WifiSeen seen[WifiProfiles];
static void ip_address(const char *key,char *out,size_t capacity) {
    auto netif=esp_netif_get_handle_from_ifkey(key); esp_netif_ip_info_t ip{};
    if(netif && esp_netif_get_ip_info(netif,&ip)==ESP_OK && ip.ip.addr) snprintf(out,capacity,IPSTR,IP2STR(&ip.ip));
}
NetworkInfo network_info() {
    NetworkInfo info; if(!initialized) return info;
    const auto &c=network_settings(); for(const auto &w:c.wifi) if(w.enabled) info.configured=true;
    info.mqtt=mqttUp; info.ap=apUp; info.profile=activeProfile; info.wifi_reason=wifiReason; info.mqtt_error=mqttError; info.discovery_published=announced;
    if(wifiUp) {
        ip_address("WIFI_STA_DEF",info.station_ip,sizeof(info.station_ip));
        wifi_ap_record_t ap{};
        if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK) { memcpy(info.ssid,ap.ssid,32); info.rssi=ap.rssi; }
    }
    if(info.ap) ip_address("WIFI_AP_DEF",info.ap_ip,sizeof(info.ap_ip));
    snprintf(info.ip,sizeof(info.ip),"%s",*info.station_ip?info.station_ip:info.ap_ip);
    return info;
}
int local_minute() {
    time_t now=time(nullptr); if(now<1704067200) return -1;
    tm local{}; if(!localtime_r(&now,&local)) return -1;
    return local.tm_hour*60+local.tm_min;
}
static void wifi_event(void *,esp_event_base_t base,int32_t id,void *data) {
    if(base==WIFI_EVENT) {
        if(id==WIFI_EVENT_SCAN_DONE) scanDone=true;
        if(id==WIFI_EVENT_AP_START) apUp=true;
        if(id==WIFI_EVENT_AP_STOP) apUp=false;
        if(id==WIFI_EVENT_STA_DISCONNECTED) {
            wifiUp=false; route_link(false); connectFailed=true;
            wifiReason=static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
        }
    } else if(base==IP_EVENT && id==IP_EVENT_STA_LOST_IP) {
        wifiUp=false; route_link(false); esp_wifi_disconnect();
    } else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        wifiUp=true; route_link(true); wifiReason=0;
        if(esp_netif_sntp_start()!=ESP_OK) ESP_LOGW("network","NTP start deferred");
    }
}
static bool same_topic(esp_mqtt_event_handle_t e,const char *topic) { return e->topic_len==int(strlen(topic)) && !memcmp(e->topic,topic,e->topic_len); }
static void mqtt_event(void *,esp_event_base_t,int32_t id,void *data) {
    auto e=static_cast<esp_mqtt_event_handle_t>(data);
    if(id==MQTT_EVENT_CONNECTED) {
        mqttUp=true; mqttError=0; announced=0; announce=true;
        esp_mqtt_client_subscribe(mqtt,ackTopic,1);
        if(network_settings().discovery) esp_mqtt_client_subscribe(mqtt,network_settings().birth_topic,1);
        esp_mqtt_client_enqueue(mqtt,availabilityTopic,"online",0,1,1,true);
    } else if(id==MQTT_EVENT_DISCONNECTED) mqttUp=false;
    else if(id==MQTT_EVENT_ERROR && e->error_handle) {
        mqttError=e->error_handle->connect_return_code?int(e->error_handle->connect_return_code):int(e->error_handle->error_type);
    } else if(id==MQTT_EVENT_DATA && e->current_data_offset==0 && e->data_len==e->total_data_len) {
        if(same_topic(e,network_settings().birth_topic) && e->data_len==6 && !memcmp(e->data,"online",6)) announce=true;
        if(same_topic(e,ackTopic) && !e->retain && e->data_len>0 && e->data_len<=10) {
            uint64_t number=0; bool ok=true;
            for(int i=0;i<e->data_len;++i) { if(e->data[i]<'0' || e->data[i]>'9') { ok=false; break; } number=number*10+e->data[i]-'0'; }
            if(ok && number<=UINT32_MAX) route_ack(uint32_t(number));
        }
    }
}
void network_before_restart(const NetworkConfig &next) {
    stopping=true;
    if(!mqtt || !mqttUp) return;
    const auto &old=network_settings();
    bool remove=old.discovery && (!next.mqtt_enabled || !next.discovery || strcmp(old.discovery_prefix,next.discovery_prefix) || strcmp(old.mqtt_host,next.mqtt_host) || old.mqtt_port!=next.mqtt_port || old.mqtt_tls!=next.mqtt_tls);
    bool clearPin=!next.mqtt_enabled || !next.publish_pin || strcmp(old.mqtt_host,next.mqtt_host) || old.mqtt_port!=next.mqtt_port || old.mqtt_tls!=next.mqtt_tls || strcmp(old.mqtt_topic,next.mqtt_topic);
    if(clearPin) esp_mqtt_client_enqueue(mqtt,accessTopic,"",0,1,1,true);
    if(!remove && old.discovery) for(unsigned i=0;i<discovery_count();++i) if(discovery_enabled(old,i) && !discovery_enabled(next,i)) {
        char topic[192]; if(discovery_topic(topic,sizeof(topic),old,network_device_id(),i)) esp_mqtt_client_enqueue(mqtt,topic,"",0,1,1,true);
    }
    if(remove) for(unsigned i=0;i<discovery_count();++i) {
        char topic[192]; if(discovery_topic(topic,sizeof(topic),old,network_device_id(),i)) esp_mqtt_client_enqueue(mqtt,topic,"",0,1,1,true);
    }
    esp_mqtt_client_enqueue(mqtt,availabilityTopic,"offline",0,1,1,true);
}
static bool access_point(bool on) {
    wifi_mode_t mode; if(esp_wifi_get_mode(&mode)!=ESP_OK) return false;
    bool active=mode==WIFI_MODE_APSTA;
    if(active==on) return true;
    return esp_wifi_set_mode(on?WIFI_MODE_APSTA:WIFI_MODE_STA)==ESP_OK;
}
static void network_task(void *) {
    network_settings_init();
    const auto &c=network_settings();
    route_interval(c.publish_s);
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    auto sta=esp_netif_create_default_wifi_sta(); auto ap=esp_netif_create_default_wifi_ap(); configASSERT(sta && ap);
    esp_netif_set_hostname(sta,network_hostname()); esp_netif_set_hostname(ap,network_hostname());
    esp_sntp_config_t clock=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org"); clock.wait_for_sync=false; clock.start=false;
    if(esp_netif_sntp_init(&clock)!=ESP_OK) ESP_LOGW("network","NTP unavailable");
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Configure WPA2 before the radio starts; never briefly advertise an open AP.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t hotspot{}; memcpy(hotspot.ap.ssid,c.ap_ssid,strlen(c.ap_ssid)); hotspot.ap.ssid_len=uint8_t(strlen(c.ap_ssid));
    memcpy(hotspot.ap.password,c.ap_password,strlen(c.ap_password)); hotspot.ap.authmode=WIFI_AUTH_WPA2_PSK;
    hotspot.ap.channel=c.ap_channel; hotspot.ap.max_connection=3; hotspot.ap.pmf_cfg.capable=true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&hotspot));
    bool any=false; for(auto id:c.order) { if(id==WifiProfiles) break; if(c.wifi[id].enabled) any=true; }
    if(any && !c.ap_always) ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start()); esp_wifi_set_ps(WIFI_PS_NONE); initialized=true;
    if(web_start() && mdns_init()==ESP_OK) {
        mdns_hostname_set(network_hostname()); mdns_instance_name_set(c.device_name); mdns_service_add(nullptr,"_http","_tcp",80,nullptr,0);
    }
    snprintf(stateTopic,sizeof(stateTopic),"%s/state",c.mqtt_topic); snprintf(eventTopic,sizeof(eventTopic),"%s/event",c.mqtt_topic);
    snprintf(availabilityTopic,sizeof(availabilityTopic),"%s/availability",c.mqtt_topic); snprintf(ackTopic,sizeof(ackTopic),"%s/ack",c.mqtt_topic);
    snprintf(accessTopic,sizeof(accessTopic),"%s/access",c.mqtt_topic);
    if(c.mqtt_enabled) {
        esp_mqtt_client_config_t mc{};
        mc.broker.address.hostname=c.mqtt_host; mc.broker.address.port=c.mqtt_port;
        mc.broker.address.transport=c.mqtt_tls?MQTT_TRANSPORT_OVER_SSL:MQTT_TRANSPORT_OVER_TCP;
        if(c.mqtt_tls) mc.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;
        mc.credentials.username=*c.mqtt_user?c.mqtt_user:nullptr; mc.credentials.authentication.password=*c.mqtt_password?c.mqtt_password:nullptr;
        mc.credentials.client_id=c.mqtt_client_id; mc.session.keepalive=c.mqtt_keepalive_s;
        mc.session.last_will.topic=availabilityTopic; mc.session.last_will.msg="offline"; mc.session.last_will.qos=1; mc.session.last_will.retain=1;
        mc.network.reconnect_timeout_ms=5000; mc.network.timeout_ms=4000;
        mc.buffer.size=2048; mc.task.priority=4; mc.task.stack_size=6144; mc.outbox.limit=8192;
        mqtt=esp_mqtt_client_init(&mc); configASSERT(mqtt);
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt,MQTT_EVENT_ANY,mqtt_event,nullptr)); ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
    }
    TelemetryGate telemetry;
    char publishedPin[5]{};
    uint32_t now=now_ms(),lastScan=now-10000,offlineAt=now,onlineAt=now,attemptAt=now,scanAt=now;
    bool attempting=false,scanning=false,wasOnline=false,apSelected=!any,pinPending=true; unsigned blocked=0,discoveryIndex=discovery_count();
    uint32_t failedAt[WifiProfiles]{},faults=UINT32_MAX;
    for(;;) {
        now=now_ms(); bool up=wifiUp;
        if(up) { if(!wasOnline) onlineAt=now; attempting=false; blocked=0; }
        else if(wasOnline) { offlineAt=now; lastScan=now-10000; }
        wasOnline=up;
        if(!stopping) {
            if(!up && (!any || (apSelected && age(now,offlineAt)>=uint32_t(c.ap_delay_s)*1000))) access_point(true);
            if(up && !c.ap_always && apUp && age(now,onlineAt)>=60000) access_point(false);
            for(unsigned i=0;i<WifiProfiles;++i) if((blocked&(1u<<i)) && age(now,failedAt[i])>=60000) blocked&=~(1u<<i);
            if(!up && attempting && (connectFailed.exchange(false) || age(now,attemptAt)>=20000)) {
                int index=activeProfile; if(index>=0) { blocked|=1u<<index; failedAt[index]=now; }
                esp_wifi_disconnect(); attempting=false; lastScan=now-10000;
            }
            if(scanning && scanDone.exchange(false)) {
                scanning=false; for(auto &entry:seen) entry=WifiSeen{};
                wifi_ap_record_t record{};
                while(esp_wifi_scan_get_ap_record(&record)==ESP_OK) {
                    for(unsigned i=0;i<WifiProfiles;++i) if(!strncmp(c.wifi[i].ssid,reinterpret_cast<const char *>(record.ssid),33) && record.rssi>seen[i].rssi) {
                        memcpy(seen[i].ssid,record.ssid,32); seen[i].rssi=record.rssi;
                    }
                }
                esp_wifi_clear_ap_list();
                int index=wifi_choose(c,seen,WifiProfiles,blocked);
                if(!up) apSelected=index==-2;
                if(!up && index>=0) {
                    wifi_config_t w{}; const auto &p=c.wifi[index]; memcpy(w.sta.ssid,p.ssid,strlen(p.ssid)); memcpy(w.sta.password,p.password,strlen(p.password));
                    w.sta.threshold.authmode=*p.password?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN; w.sta.pmf_cfg.capable=true;
                    activeProfile=index; connectFailed=false;
                    if(esp_wifi_set_config(WIFI_IF_STA,&w)==ESP_OK && esp_wifi_connect()==ESP_OK) { attempting=true; attemptAt=now; }
                    else { blocked|=1u<<index; failedAt[index]=now; }
                }
            }
            if(scanning && age(now,scanAt)>15000) { esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); scanning=false; }
            if(!up && any && !attempting && !scanning && age(now,lastScan)>=10000) {
                wifi_scan_config_t scan{}; scan.show_hidden=true; scanDone=false;
                if(esp_wifi_scan_start(&scan,false)==ESP_OK) { scanning=true; scanAt=now; }
                lastScan=now;
            }
            if(announce.exchange(false)) { discoveryIndex=0; announced=0; telemetry.sent=false; pinPending=true; }
            // Read even with MQTT disabled/offline so the hourly PIN rotates locally.
            char pin[5]{}; bool pinReady=web_mqtt_pin(pin);
            if(c.publish_pin && pinReady && strcmp(pin,publishedPin)) pinPending=true;
            if(mqttUp && pinPending && (!c.publish_pin || pinReady) && esp_mqtt_client_get_outbox_size(mqtt)<2048) {
                char message[40]{}; if(c.publish_pin) snprintf(message,sizeof(message),"{\"verification_pin\":\"%s\"}",pin);
                if(esp_mqtt_client_enqueue(mqtt,accessTopic,message,0,1,1,true)>=0) {
                    pinPending=false; memcpy(publishedPin,pin,sizeof(publishedPin));
                }
            }
            if(mqttUp && c.discovery && discoveryIndex<discovery_count() && esp_mqtt_client_get_outbox_size(mqtt)<2048) {
                bool enabled=discovery_enabled(c,discoveryIndex);
                char topic[192]; auto doc=discovery_payload(c,network_device_id(),network_hostname(),esp_app_get_description()->version,discoveryIndex);
                char *text=doc?cJSON_PrintUnformatted(doc):nullptr; cJSON_Delete(doc);
                if((text || !enabled) && discovery_topic(topic,sizeof(topic),c,network_device_id(),discoveryIndex) && esp_mqtt_client_enqueue(mqtt,topic,text?text:"",0,1,1,true)>=0) { discoveryIndex++; if(enabled) announced++; }
                cJSON_free(text);
            }
            auto s=snapshot();
            if(mqttUp && telemetry.due(s,c,now) && esp_mqtt_client_get_outbox_size(mqtt)<4096) {
                char *text=snapshot_json(s);
                if(text) {
                    route_sent(s.sequence);
                    if(esp_mqtt_client_enqueue(mqtt,stateTopic,text,0,1,0,false)>=0) {
                        if(s.faults!=faults) esp_mqtt_client_enqueue(mqtt,eventTopic,text,0,1,0,false);
                        telemetry.published(s,now); faults=s.faults;
                    }
                    cJSON_free(text);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
void transport_start() {
    setenv("TZ","CET-1CEST,M3.5.0/2,M10.5.0/3",1); tzset();
    configASSERT(xTaskCreate(network_task,"network",16384,nullptr,4,nullptr)==pdPASS);
}
