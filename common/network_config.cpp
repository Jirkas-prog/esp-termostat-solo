#include "network_config.h"
#include "cJSON.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <initializer_list>
#include <algorithm>
namespace solo {
static const cJSON *get(const cJSON *v,const char *key) { return cJSON_GetObjectItemCaseSensitive(v,key); }
static bool keys(const cJSON *obj,std::initializer_list<const char *> required,std::initializer_list<const char *> optional={}) {
    if(!cJSON_IsObject(obj)) return false;
    for(auto name:required) if(!get(obj,name)) return false;
    for(auto item=obj->child;item;item=item->next) {
        if(!item->string) return false;
        bool known=false;
        for(auto name:required) if(!strcmp(item->string,name)) known=true;
        for(auto name:optional) if(!strcmp(item->string,name)) known=true;
        if(!known) return false;
        for(auto prev=obj->child;prev!=item;prev=prev->next) if(!strcmp(prev->string,item->string)) return false;
    }
    return true;
}
template<size_t N> static bool str(const cJSON *v,char (&dest)[N]) {
    if(!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring)>=N) return false;
    for(auto p=reinterpret_cast<const unsigned char *>(v->valuestring);*p;++p) if(*p<32 || *p==127) return false;
    memset(dest,0,N); strcpy(dest,v->valuestring); return true;
}
template<class T> static bool number(const cJSON *v,T &dest,unsigned max) {
    if(!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble<0 || v->valuedouble>max || floor(v->valuedouble)!=v->valuedouble) return false;
    dest=static_cast<T>(v->valuedouble); return true;
}
static bool bit(const cJSON *v,uint8_t &dest) { if(!cJSON_IsBool(v)) return false; dest=cJSON_IsTrue(v)?1:0; return true; }
template<size_t N> static bool safe(const char (&s)[N]) {
    if(!memchr(s,0,N)) return false;
    for(auto p=reinterpret_cast<const unsigned char *>(s);*p;++p) if(*p<32 || *p==127) return false;
    return true;
}
static bool topic(const char *s) { return *s && s[0]!='/' && s[strlen(s)-1]!='/' && !strpbrk(s,"+# ") && !strstr(s,"//"); }
bool network_migrate(const NetworkConfigV1 &old,NetworkConfig &next) {
    NetworkConfig n;
    n.revision=old.revision;
    for(unsigned i=0;i<4;++i) n.wifi[i]=old.wifi[i];
    // The suffix has the original layout in both versions; copy fields explicitly.
    memcpy(n.ap_ssid,old.ap_ssid,sizeof(n.ap_ssid));
    memcpy(n.ap_password,old.ap_password,sizeof(n.ap_password));
    memcpy(n.mqtt_host,old.mqtt_host,sizeof(n.mqtt_host));
    memcpy(n.mqtt_user,old.mqtt_user,sizeof(n.mqtt_user));
    memcpy(n.mqtt_password,old.mqtt_password,sizeof(n.mqtt_password));
    memcpy(n.mqtt_client_id,old.mqtt_client_id,sizeof(n.mqtt_client_id));
    memcpy(n.mqtt_topic,old.mqtt_topic,sizeof(n.mqtt_topic));
    memcpy(n.device_name,old.device_name,sizeof(n.device_name));
    memcpy(n.discovery_prefix,old.discovery_prefix,sizeof(n.discovery_prefix));
    memcpy(n.birth_topic,old.birth_topic,sizeof(n.birth_topic));
    n.ap_delay_s=old.ap_delay_s;
    n.ap_always=old.ap_always;
    n.ap_channel=old.ap_channel;
    n.mqtt_port=old.mqtt_port;
    n.mqtt_keepalive_s=old.mqtt_keepalive_s;
    n.mqtt_enabled=old.mqtt_enabled;
    n.mqtt_tls=old.mqtt_tls;
    n.discovery=old.discovery;
    n.publish_s=old.publish_s==5?60:old.publish_s;
    n.publish_pin=1; // Existing 0.7 deployment: owner explicitly requested MQTT PIN.
    std::stable_sort(n.order,n.order+4,[&](uint8_t a,uint8_t b){return n.wifi[a].priority>n.wifi[b].priority;});
    if(!network_valid(n)) return false;
    next=n; return true;
}
bool network_valid(const NetworkConfig &c) {
    unsigned mask=0;
    for(auto id:c.order) { if(id>WifiProfiles || (mask&(1u<<id))) return false; mask|=1u<<id; }
    if(c.publish_s<1 || c.publish_s>3600 || c.publish_min_s<1 || c.publish_min_s>60 || c.publish_min_s>c.publish_s || c.publish_delta_centi<1 || c.publish_delta_centi>1000 || c.publish_pin>1) return false;
    if(!c.revision || !safe(c.ap_ssid) || !*c.ap_ssid || !safe(c.ap_password) || strlen(c.ap_password)<8 || c.ap_always>1 || c.ap_channel<1 || c.ap_channel>11 || c.ap_delay_s<10 || c.ap_delay_s>300) return false;
    for(unsigned i=0;i<WifiProfiles;++i) {
        const auto &w=c.wifi[i];
        if(!safe(w.ssid) || !safe(w.password) || w.enabled>1 || w.hidden>1 || w.priority>100 || (w.enabled && !*w.ssid)) return false;
        size_t n=strlen(w.password);
        if(n && n<8) return false;
        if(n==64 && strspn(w.password,"0123456789abcdefABCDEF")!=64) return false;
        for(unsigned j=0;j<i;++j) if(*w.ssid && !strcmp(w.ssid,c.wifi[j].ssid)) return false;
    }
    if(!safe(c.mqtt_host) || !safe(c.mqtt_user) || !safe(c.mqtt_password) || !safe(c.mqtt_client_id) || !safe(c.mqtt_topic) || !safe(c.device_name) || !safe(c.discovery_prefix) || !safe(c.birth_topic)) return false;
    if(c.mqtt_enabled>1 || c.mqtt_tls>1 || c.discovery>1 || (c.mqtt_enabled && !*c.mqtt_host) || !c.mqtt_port || c.mqtt_keepalive_s<10 || c.mqtt_keepalive_s>300) return false;
    if(strpbrk(c.mqtt_host," /@?#[]\\") || !*c.mqtt_client_id || strpbrk(c.mqtt_client_id," /+#") || !*c.device_name || !topic(c.mqtt_topic) || !topic(c.discovery_prefix) || !topic(c.birth_topic)) return false;
    if(const char *colon=strchr(c.mqtt_host,':')) {
        if(!strchr(colon+1,':') || strspn(c.mqtt_host,"0123456789abcdefABCDEF:.")!=strlen(c.mqtt_host)) return false;
    }
    // Prevent our own availability/state from acting as a HA birth trigger.
    if(!strncmp(c.birth_topic,c.mqtt_topic,strlen(c.mqtt_topic)) && c.birth_topic[strlen(c.mqtt_topic)]=='/') return false;
    return true;
}
bool network_legacy(const cJSON *doc,NetworkConfig &c) {
    if(!keys(doc,{"ssid","password","mqtt_uri","mqtt_user","mqtt_password"})) return false;
    NetworkConfig n=c; n.mqtt_port=1883; n.mqtt_tls=0; char uri[129];
    if(!str(get(doc,"ssid"),n.wifi[0].ssid) || !str(get(doc,"password"),n.wifi[0].password) || !str(get(doc,"mqtt_uri"),uri) || !str(get(doc,"mqtt_user"),n.mqtt_user) || !str(get(doc,"mqtt_password"),n.mqtt_password)) return false;
    if(strncmp(uri,"mqtt://",7)) return false;
    char *host=uri+7,*port=nullptr;
    if(char *path=strpbrk(host,"/?#")) *path=0;
    if(*host=='[') {
        ++host; char *end=strchr(host,']'); if(!end) return false;
        if(end[1]==':') port=end+2; else if(end[1]) return false;
        *end=0;
    } else if(char *separator=strrchr(host,':')) { *separator=0; port=separator+1; }
    if(port) { char *end=nullptr; unsigned long value=strtoul(port,&end,10); if(!*port || *end || !value || value>65535) return false; n.mqtt_port=uint16_t(value); }
    if(!*host || strlen(host)>=sizeof(n.mqtt_host)) return false;
    strcpy(n.mqtt_host,host); n.mqtt_enabled=1; n.wifi[0].enabled=1; n.wifi[0].hidden=1;
    strcpy(n.mqtt_client_id,"kamna-termostat-solo"); n.mqtt_keepalive_s=15;
    if(!network_valid(n)) return false;
    c=n; return true;
}
int network_parse(const cJSON *doc,const NetworkConfig &current,NetworkConfig &next) {
    if(!keys(doc,{"schema","revision","wifi","order","ap","mqtt"})) return 400;
    uint32_t revision=0; unsigned schema=0;
    if(!number(get(doc,"schema"),schema,2) || schema!=2 || !number(get(doc,"revision"),revision,UINT32_MAX)) return 400;
    if(revision!=current.revision || revision==UINT32_MAX) return 409;
    auto wifi=get(doc,"wifi"),ap=get(doc,"ap"),mq=get(doc,"mqtt"),order=get(doc,"order"); NetworkConfig n=current;
    if(!cJSON_IsArray(wifi) || cJSON_GetArraySize(wifi)>int(WifiProfiles) || !cJSON_IsArray(order) || cJSON_GetArraySize(order)!=cJSON_GetArraySize(wifi)+1) return 400;
    unsigned used=0,ordered=0,pos=0;
    for(auto &w:n.wifi) w=WifiProfile{};
    for(auto w=wifi->child;w;w=w->next) {
        unsigned id=0;
        if(!keys(w,{"id","ssid","enabled","hidden"},{"password"}) || !number(get(w,"id"),id,WifiProfiles-1) || (used&(1u<<id))) return 400;
        used|=1u<<id; auto &dest=n.wifi[id]; dest=current.wifi[id];
        if(!str(get(w,"ssid"),dest.ssid) || !*dest.ssid || !bit(get(w,"enabled"),dest.enabled) || !bit(get(w,"hidden"),dest.hidden)) return 400;
        if(auto password=get(w,"password")) { if(!str(password,dest.password)) return 400; }
        else if(strcmp(dest.ssid,current.wifi[id].ssid)) return 400;
    }
    for(auto entry=order->child;entry;entry=entry->next) {
        unsigned id=WifiProfiles;
        if(!(cJSON_IsString(entry) && !strcmp(entry->valuestring,"ap"))) {
            if(!number(entry,id,WifiProfiles-1) || !(used&(1u<<id))) return 400;
        }
        if(ordered&(1u<<id)) return 400;
        ordered|=1u<<id; n.order[pos++]=uint8_t(id);
    }
    if(ordered!=(used|(1u<<WifiProfiles))) return 400;
    for(unsigned id=0;id<WifiProfiles;++id) if(!(used&(1u<<id))) n.order[pos++]=uint8_t(id);
    if(!keys(ap,{"ssid","always","channel","delay_s"},{"password"}) || !str(get(ap,"ssid"),n.ap_ssid) || !bit(get(ap,"always"),n.ap_always) || !number(get(ap,"channel"),n.ap_channel,11) || !number(get(ap,"delay_s"),n.ap_delay_s,300)) return 400;
    if(auto password=get(ap,"password")) if(!str(password,n.ap_password)) return 400;
    if(!keys(mq,{"enabled","host","port","tls","user","client_id","topic","device_name","discovery","discovery_prefix","birth_topic","keepalive_s","publish_s","min_publish_s","delta_centi","publish_pin"},{"password"}) ||
       !bit(get(mq,"enabled"),n.mqtt_enabled) || !str(get(mq,"host"),n.mqtt_host) || !number(get(mq,"port"),n.mqtt_port,65535) || !bit(get(mq,"tls"),n.mqtt_tls) ||
       !str(get(mq,"user"),n.mqtt_user) || !str(get(mq,"client_id"),n.mqtt_client_id) || !str(get(mq,"topic"),n.mqtt_topic) || !str(get(mq,"device_name"),n.device_name) ||
       !bit(get(mq,"discovery"),n.discovery) || !str(get(mq,"discovery_prefix"),n.discovery_prefix) || !str(get(mq,"birth_topic"),n.birth_topic) || !number(get(mq,"keepalive_s"),n.mqtt_keepalive_s,300) || !number(get(mq,"publish_s"),n.publish_s,3600) || !number(get(mq,"min_publish_s"),n.publish_min_s,60) || !number(get(mq,"delta_centi"),n.publish_delta_centi,1000) || !bit(get(mq,"publish_pin"),n.publish_pin)) return 400;
    if(auto password=get(mq,"password")) if(!str(password,n.mqtt_password)) return 400;
    if(!network_valid(n)) return 400;
    n.revision++; next=n; return 200;
}
cJSON *network_redacted(const NetworkConfig &c) {
    auto doc=cJSON_CreateObject(); if(!doc) return nullptr;
    cJSON_AddNumberToObject(doc,"schema",2); cJSON_AddNumberToObject(doc,"revision",c.revision);
    auto wifi=cJSON_AddArrayToObject(doc,"wifi");
    for(unsigned id=0;id<WifiProfiles;++id) {
        const auto &w=c.wifi[id]; if(!*w.ssid) continue;
        auto p=cJSON_CreateObject(); cJSON_AddItemToArray(wifi,p);
        cJSON_AddStringToObject(p,"ssid",w.ssid); cJSON_AddBoolToObject(p,"enabled",w.enabled); cJSON_AddBoolToObject(p,"hidden",w.hidden);
        cJSON_AddNumberToObject(p,"id",id); cJSON_AddBoolToObject(p,"password_set",*w.password);
    }
    auto order=cJSON_AddArrayToObject(doc,"order");
    for(auto id:c.order) { if(id==WifiProfiles) cJSON_AddItemToArray(order,cJSON_CreateString("ap")); else if(*c.wifi[id].ssid) cJSON_AddItemToArray(order,cJSON_CreateNumber(id)); }
    auto ap=cJSON_AddObjectToObject(doc,"ap"); cJSON_AddStringToObject(ap,"ssid",c.ap_ssid); cJSON_AddBoolToObject(ap,"password_set",*c.ap_password);
    cJSON_AddBoolToObject(ap,"always",c.ap_always); cJSON_AddNumberToObject(ap,"channel",c.ap_channel); cJSON_AddNumberToObject(ap,"delay_s",c.ap_delay_s);
    auto mq=cJSON_AddObjectToObject(doc,"mqtt");
    cJSON_AddBoolToObject(mq,"enabled",c.mqtt_enabled); cJSON_AddStringToObject(mq,"host",c.mqtt_host); cJSON_AddNumberToObject(mq,"port",c.mqtt_port); cJSON_AddBoolToObject(mq,"tls",c.mqtt_tls);
    cJSON_AddStringToObject(mq,"user",c.mqtt_user); cJSON_AddBoolToObject(mq,"password_set",*c.mqtt_password); cJSON_AddStringToObject(mq,"client_id",c.mqtt_client_id);
    cJSON_AddStringToObject(mq,"topic",c.mqtt_topic); cJSON_AddStringToObject(mq,"device_name",c.device_name); cJSON_AddBoolToObject(mq,"discovery",c.discovery);
    cJSON_AddStringToObject(mq,"discovery_prefix",c.discovery_prefix); cJSON_AddStringToObject(mq,"birth_topic",c.birth_topic);
    cJSON_AddNumberToObject(mq,"keepalive_s",c.mqtt_keepalive_s); cJSON_AddNumberToObject(mq,"publish_s",c.publish_s);
    cJSON_AddNumberToObject(mq,"min_publish_s",c.publish_min_s); cJSON_AddNumberToObject(mq,"delta_centi",c.publish_delta_centi); cJSON_AddBoolToObject(mq,"publish_pin",c.publish_pin);
    return doc;
}
int wifi_choose(const NetworkConfig &c,const WifiSeen *seen,size_t count,unsigned blocked) {
    for(auto id:c.order) {
        if(id==WifiProfiles) return -2; // AP ends automatic selection; lower profiles are parked.
        const auto &w=c.wifi[id]; if(!w.enabled || !*w.ssid || (blocked&(1u<<id))) continue;
        if(w.hidden) return int(id);
        for(size_t j=0;j<count;++j) if(!strcmp(w.ssid,seen[j].ssid)) return int(id);
    }
    return -2;
}
}
