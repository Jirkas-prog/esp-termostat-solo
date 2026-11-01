#include "network_config.h"
#include "mqtt_discovery.h"
#include "cJSON.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <set>
using namespace solo;
static NetworkConfig fixture() {
    NetworkConfig c;
    strcpy(c.ap_ssid,"TERMO-TEST"); strcpy(c.ap_password,"fixture-ap-secret");
    strcpy(c.mqtt_client_id,"termostat_aabbccddeeff"); strcpy(c.mqtt_host,"192.0.2.10");
    strcpy(c.mqtt_user,"test-user"); strcpy(c.mqtt_password,"fixture-mqtt-secret"); c.mqtt_enabled=1; c.publish_pin=1;
    strcpy(c.wifi[0].ssid,"Test WiFi"); strcpy(c.wifi[0].password,"fixture-wifi-secret"); c.wifi[0].enabled=1;
    return c;
}
static cJSON *request(const NetworkConfig &c) {
    auto doc=network_redacted(c);
    for(int i=0;i<cJSON_GetArraySize(cJSON_GetObjectItem(doc,"wifi"));++i) cJSON_DeleteItemFromObject(cJSON_GetArrayItem(cJSON_GetObjectItem(doc,"wifi"),i),"password_set");
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(doc,"ap"),"password_set");
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(doc,"mqtt"),"password_set");
    return doc;
}
int main(int argc,char **argv) {
    assert(argc==3); auto c=fixture(); assert(network_valid(c)); NetworkConfig out;
    auto publicConfig=network_redacted(c); char *text=cJSON_PrintUnformatted(publicConfig);
    assert(text && !strstr(text,"fixture-") && !strstr(text,"\"password\":"));
    cJSON_AddStringToObject(publicConfig,"device_id","termostat_aabbccddeeff"); cJSON_AddBoolToObject(publicConfig,"saved",true);
    cJSON_free(text); text=cJSON_PrintUnformatted(publicConfig);
    FILE *file=fopen(argv[1],"wb"); assert(file); fputs(text,file); fclose(file); cJSON_free(text); cJSON_Delete(publicConfig);
    auto doc=request(c); assert(network_parse(doc,c,out)==200 && out.revision==2);
    assert(!strcmp(out.wifi[0].password,c.wifi[0].password) && !strcmp(out.ap_password,c.ap_password) && !strcmp(out.mqtt_password,c.mqtt_password));
    cJSON_AddBoolToObject(doc,"extra",true); assert(network_parse(doc,c,out)==400); cJSON_Delete(doc);
    doc=request(c); cJSON_AddNumberToObject(doc,"revision",1); assert(network_parse(doc,c,out)==400); cJSON_Delete(doc);
    doc=request(c); cJSON_SetNumberValue(cJSON_GetObjectItem(doc,"revision"),0); assert(network_parse(doc,c,out)==409); cJSON_Delete(doc);
    doc=request(c); auto wifi=cJSON_GetArrayItem(cJSON_GetObjectItem(doc,"wifi"),0);
    cJSON_SetValuestring(cJSON_GetObjectItem(wifi,"ssid"),"Another network"); assert(network_parse(doc,c,out)==400);
    cJSON_AddStringToObject(wifi,"password","new-test-secret"); assert(network_parse(doc,c,out)==200 && !strcmp(out.wifi[0].password,"new-test-secret"));
    cJSON_SetValuestring(cJSON_GetObjectItem(wifi,"password"),""); assert(network_parse(doc,c,out)==200 && !*out.wifi[0].password); cJSON_Delete(doc);
    doc=request(c); auto mq=cJSON_GetObjectItem(doc,"mqtt"); cJSON_AddStringToObject(mq,"password","");
    assert(network_parse(doc,c,out)==200 && !*out.mqtt_password && !strcmp(out.wifi[0].password,c.wifi[0].password)); cJSON_Delete(doc);
    doc=request(c); auto ap=cJSON_GetObjectItem(doc,"ap"); cJSON_AddStringToObject(ap,"password",""); assert(network_parse(doc,c,out)==400); cJSON_Delete(doc);
    auto bad=c; bad.ap_ssid[0]=0; assert(!network_valid(bad)); bad=c; bad.ap_delay_s=0; assert(!network_valid(bad));
    bad=c; memset(bad.mqtt_host,'x',sizeof(bad.mqtt_host)); assert(!network_valid(bad));
    for(const char *host:{"mqtt://host","host:1883","host/name","bad host","user@host"}) { bad=c; strcpy(bad.mqtt_host,host); assert(!network_valid(bad)); }
    for(const char *topic:{"/bad","bad/","a//b","a/#","a/+","a b"}) { bad=c; strcpy(bad.mqtt_topic,topic); assert(!network_valid(bad)); }
    bad=c; strcpy(bad.birth_topic,"wifi2mqtt/termostat-solo/availability"); assert(!network_valid(bad));
    bad=c; strcpy(bad.wifi[1].ssid,c.wifi[0].ssid); assert(!network_valid(bad));
    bad=c; bad.mqtt_port=0; assert(!network_valid(bad)); bad=c; bad.mqtt_keepalive_s=9; assert(!network_valid(bad));
    bad=c; bad.publish_s=3601; assert(!network_valid(bad)); bad=c; strcpy(bad.mqtt_host,"2001:db8::1"); assert(network_valid(bad));
    auto legacy=cJSON_Parse("{\"ssid\":\"legacy-wifi\",\"password\":\"legacy-secret\",\"mqtt_uri\":\"mqtt://192.0.2.4:1884\",\"mqtt_user\":\"legacy-user\",\"mqtt_password\":\"legacy-mqtt-secret\"}");
    auto migrated=c; assert(network_legacy(legacy,migrated)); assert(!strcmp(migrated.wifi[0].ssid,"legacy-wifi") && migrated.mqtt_port==1884 && !strcmp(migrated.mqtt_password,"legacy-mqtt-secret"));
    assert(!strcmp(migrated.ap_password,c.ap_password) && migrated.mqtt_keepalive_s==15 && migrated.wifi[0].hidden);
    cJSON_SetValuestring(cJSON_GetObjectItem(legacy,"mqtt_uri"),"mqtt://192.0.2.4:1884/"); assert(network_legacy(legacy,migrated) && migrated.mqtt_port==1884);
    cJSON_SetValuestring(cJSON_GetObjectItem(legacy,"mqtt_uri"),"mqtt://[2001:db8::4]:1885/"); assert(network_legacy(legacy,migrated) && migrated.mqtt_port==1885 && !strcmp(migrated.mqtt_host,"2001:db8::4"));
    migrated.mqtt_tls=1;
    cJSON_SetValuestring(cJSON_GetObjectItem(legacy,"mqtt_uri"),"mqtt://192.0.2.4");
    assert(network_legacy(legacy,migrated) && migrated.mqtt_port==1883 && !migrated.mqtt_tls);
    cJSON_Delete(legacy);
    NetworkConfigV1 previous;
    strcpy(previous.ap_ssid,c.ap_ssid); strcpy(previous.ap_password,c.ap_password); strcpy(previous.mqtt_client_id,c.mqtt_client_id);
    previous.wifi[0]=c.wifi[0]; previous.wifi[1]=c.wifi[0]; strcpy(previous.wifi[1].ssid,"Backup"); previous.wifi[1].priority=80;
    assert(network_migrate(previous,out) && out.order[0]==1 && out.publish_s==60 && out.revision==previous.revision);
    assert(!strcmp(out.wifi[0].password,c.wifi[0].password) && !strcmp(out.ap_password,c.ap_password));
    auto choice=out; WifiSeen seen[2]; strcpy(seen[0].ssid,"Test WiFi"); seen[0].rssi=-35; strcpy(seen[1].ssid,"Backup"); seen[1].rssi=-75;
    assert(wifi_choose(choice,seen,2,0)==1); assert(wifi_choose(choice,seen,2,2)==0); assert(wifi_choose(choice,seen,2,3)==-2);
    assert(wifi_choose(choice,seen,1,0)==0); choice.wifi[1].hidden=1; assert(wifi_choose(choice,seen,1,0)==1);
    choice.order[0]=WifiProfiles; choice.order[8]=1; assert(wifi_choose(choice,seen,2,0)==-2);
    choice=out; doc=request(choice); auto order=cJSON_GetObjectItem(doc,"order"); cJSON_ReplaceItemInArray(order,0,cJSON_CreateNumber(0)); cJSON_ReplaceItemInArray(order,1,cJSON_CreateNumber(1));
    assert(network_parse(doc,choice,out)==200 && out.order[0]==0 && !strcmp(out.wifi[1].password,choice.wifi[1].password));
    cJSON_ReplaceItemInArray(order,1,cJSON_CreateNumber(0)); assert(network_parse(doc,choice,out)==400); cJSON_Delete(doc);
    doc=request(c); cJSON_DeleteItemFromArray(cJSON_GetObjectItem(doc,"order"),1); assert(network_parse(doc,c,out)==400); cJSON_Delete(doc);
    doc=request(c); cJSON_DeleteItemFromArray(cJSON_GetObjectItem(doc,"wifi"),0); cJSON_DeleteItemFromArray(cJSON_GetObjectItem(doc,"order"),0);
    assert(network_parse(doc,c,out)==200 && !*out.wifi[0].ssid && !*out.wifi[0].password); cJSON_Delete(doc);
    doc=request(c); mq=cJSON_GetObjectItem(doc,"mqtt"); cJSON_SetNumberValue(cJSON_GetObjectItem(mq,"min_publish_s"),61); assert(network_parse(doc,c,out)==400); cJSON_Delete(doc);
    doc=request(c); mq=cJSON_GetObjectItem(doc,"mqtt"); cJSON_SetNumberValue(cJSON_GetObjectItem(mq,"delta_centi"),0); assert(network_parse(doc,c,out)==400); cJSON_Delete(doc);
    assert(!NetworkConfig{}.publish_pin);
    auto disabled=c; disabled.publish_pin=0; assert(!discovery_enabled(disabled,discovery_count()-1)); assert(!discovery_payload(disabled,"device","host","0.8.0",discovery_count()-1));
    auto list=cJSON_CreateArray(); std::set<std::string> ids,topics;
    for(unsigned i=0;i<discovery_count();++i) {
        char topic[192]; assert(discovery_topic(topic,sizeof(topic),c,"termostat_aabbccddeeff",i)); assert(topics.insert(topic).second);
        auto payload=discovery_payload(c,"termostat_aabbccddeeff","ter01","0.8.0",i); assert(payload);
        assert(ids.insert(cJSON_GetObjectItem(payload,"unique_id")->valuestring).second);
        auto item=cJSON_CreateObject(); cJSON_AddStringToObject(item,"topic",topic); cJSON_AddItemToObject(item,"payload",payload); cJSON_AddItemToArray(list,item);
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(payload,"availability"))==2);
        text=cJSON_PrintUnformatted(payload); assert(text && !strstr(text,"fixture-") && !strstr(text,"mqtt_password")); cJSON_free(text);
    }
    text=cJSON_PrintUnformatted(list); file=fopen(argv[2],"wb"); assert(file); fputs(text,file); fclose(file); cJSON_free(text); cJSON_Delete(list);
    puts("PASS network: migration, credential preservation/redaction, strict validation, stale revisions, Wi-Fi priority/fallback/hidden/blocked selection, stable discovery identities and templates");
}
