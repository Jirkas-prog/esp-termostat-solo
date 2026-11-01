#include "mqtt_discovery.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
namespace solo {
struct Entity { const char *id,*name,*path,*unit,*deviceClass; bool binary=false,diagnostic=false; };
static const Entity entities[]={
    {"outlet","Výstup z kamen","outlet","°C","temperature"},
    {"inlet","Vstup do kamen","inlet","°C","temperature"},
    {"average","Průměr teplot","average","°C","temperature"},
    {"maximum","Maximum teplot","maximum","°C","temperature"},
    {"minimum","Minimum teplot","minimum","°C","temperature"},
    {"delta","Rozdíl výstup − vstup","difference","°C",nullptr},
    {"probe1","Senzor 1 GPIO27","sensor1.temperature","°C","temperature",false,true},
    {"probe2","Senzor 2 GPIO26","sensor2.temperature","°C","temperature",false,true},
    {"demand","Požadavek oběhu regulátoru","pump_request",nullptr,nullptr,true},
    {"output","Reléový výstup povolen","output_enabled",nullptr,nullptr,true,true},
    {"probe1_valid","Senzor 1 platné měření","sensor1.valid",nullptr,nullptr,true,true},
    {"probe2_valid","Senzor 2 platné měření","sensor2.valid",nullptr,nullptr,true,true},
    {"faults","Chybová maska","faults",nullptr,nullptr,false,true},
    {"uptime","Doba běhu","uptime_s","s","duration",false,true},
    {"stove","Provozní stav kamen","analysis.stove_state",nullptr,nullptr},
    {"thermal","Vývoj teploty vody","analysis.thermal_state",nullptr,nullptr},
    {"circulation","Odhad cirkulace","analysis.circulation",nullptr,nullptr},
    {"relative","Relativní tepelný výkon odhad","analysis.relative_output_pct","%",nullptr},
    {"source_kw","Výkon zdroje odhad","analysis.source_to_water_kw_est","kW","power"},
    {"export_kw","Výkon do okruhu odhad","analysis.water_export_kw_est","kW","power"},
    {"inlet_rate","Změna vstupní teploty","analysis.inlet_rate_c_min","°C/min",nullptr},
    {"outlet_rate","Změna výstupní teploty","analysis.outlet_rate_c_min","°C/min",nullptr},
    {"power_trend","Trend výkonu odhad","analysis.power_trend",nullptr,nullptr},
    {"advice","Vyhodnocení kamen","analysis.advice",nullptr,nullptr},
    {"quality","Kvalita tepelné analýzy","analysis.quality",nullptr,nullptr,false,true},
    {"mixing_minimum","Dno poklesu po změně oběhu odhad","analysis.mixing_minimum_c","°C","temperature"},
    {"mixing_eta","Čas do dna poklesu odhad","analysis.mixing_eta_min","min","duration"},
    {"verification_pin","Ověřovací PIN","verification_pin",nullptr,nullptr,false,true}
};
unsigned discovery_count() { return sizeof(entities)/sizeof(entities[0]); }
bool discovery_enabled(const NetworkConfig &c,unsigned index) { return index<discovery_count() && (strcmp(entities[index].id,"verification_pin") || c.publish_pin); }
bool discovery_topic(char *out,size_t size,const NetworkConfig &c,const char *device,unsigned index) {
    if(index>=discovery_count()) return false;
    const auto &e=entities[index]; int n=snprintf(out,size,"%s/%s/%s/%s/config",c.discovery_prefix,e.binary?"binary_sensor":"sensor",device,e.id);
    return n>0 && size_t(n)<size;
}
cJSON *discovery_payload(const NetworkConfig &c,const char *device,const char *hostname,const char *version,unsigned index) {
    if(!discovery_enabled(c,index)) return nullptr;
    const auto &e=entities[index]; auto doc=cJSON_CreateObject(); if(!doc) return nullptr;
    bool pin=!strcmp(e.id,"verification_pin");
    char buf[384],state[128],availability[128];
    snprintf(state,sizeof(state),"%s/%s",c.mqtt_topic,pin?"access":"state"); snprintf(availability,sizeof(availability),"%s/availability",c.mqtt_topic);
    cJSON_AddStringToObject(doc,"name",e.name);
    snprintf(buf,sizeof(buf),"%s_%s",device,e.id); cJSON_AddStringToObject(doc,"unique_id",buf);
    cJSON_AddStringToObject(doc,"state_topic",state); cJSON_AddNumberToObject(doc,"qos",1);
    if(!pin) cJSON_AddNumberToObject(doc,"expire_after",3*c.publish_s+10);
    else cJSON_AddStringToObject(doc,"icon","mdi:form-textbox-password");
    if(e.binary) snprintf(buf,sizeof(buf),"{{ 'ON' if value_json.%s else 'OFF' }}",e.path);
    else snprintf(buf,sizeof(buf),"{{ value_json.%s if value_json.%s is defined and value_json.%s is not none else none }}",e.path,e.path,e.path);
    cJSON_AddStringToObject(doc,"value_template",buf);
    auto av=cJSON_AddArrayToObject(doc,"availability"),a=cJSON_CreateObject(),b=cJSON_CreateObject();
    cJSON_AddItemToArray(av,a); cJSON_AddItemToArray(av,b);
    cJSON_AddStringToObject(a,"topic",availability); cJSON_AddStringToObject(b,"topic",state);
    snprintf(buf,sizeof(buf),"{{ 'online' if value_json.%s is defined and value_json.%s is not none else 'offline' }}",e.path,e.path);
    cJSON_AddStringToObject(b,"value_template",buf); cJSON_AddStringToObject(doc,"availability_mode","all");
    if(e.unit) { cJSON_AddStringToObject(doc,"unit_of_measurement",e.unit); cJSON_AddStringToObject(doc,"state_class","measurement"); }
    if(e.deviceClass) cJSON_AddStringToObject(doc,"device_class",e.deviceClass);
    if(e.diagnostic) cJSON_AddStringToObject(doc,"entity_category","diagnostic");
    auto dev=cJSON_AddObjectToObject(doc,"device"),ids=cJSON_AddArrayToObject(dev,"identifiers"); cJSON_AddItemToArray(ids,cJSON_CreateString(device));
    cJSON_AddStringToObject(dev,"name",c.device_name); cJSON_AddStringToObject(dev,"manufacturer","ESP TERMOSTAT");
    cJSON_AddStringToObject(dev,"model","SOLO / LILYGO TTGO T-Display ESP32"); cJSON_AddStringToObject(dev,"sw_version",version);
    snprintf(buf,sizeof(buf),"http://%s.local/#nastaveni/mqtt",hostname); cJSON_AddStringToObject(dev,"configuration_url",buf);
    return doc;
}
}
